/*
 * xxHash - Extremely Fast Hash algorithm
 * Header File
 * Copyright (C) 2012-2021 Yann Collet
 *
 * BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following disclaimer
 *     in the documentation and/or other materials provided with the
 *     distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * You can contact the author at:
 *   - xxHash homepage: https://www.xxhash.com
 *   - xxHash source repository: https://github.com/Cyan4973/xxHash
 */

#pragma once

// Minimal, self-contained XXH32 streaming implementation.
//
// The LZ4 buffer-core keeps a running XXH32 digest over the bytes it sees and
// exposes it through LZ4BlockStreaming::get_digest(), and storage persists XXH32
// checksums of its records. Xapiand pulled in the full xxHash distribution; here
// we vendor only the XXH32 reset/update/digest surface, implementing the canonical
// XXH32 algorithm so the digest of a byte sequence is the one upstream xxHash
// computes, on every host byte order. This avoids adding a fourth third-party
// dependency.

#include <bit>         // for std::endian
#include <cstddef>     // for size_t
#include <cstdint>     // for uint32_t
#include <cstring>     // for memcpy

// On AArch64 an update shorter than this many bytes runs the update of 17873b2 as it was (the lanes
// unpinned, inlined into the caller); a longer one runs them pinned in scalar registers. Define it
// to 0 to pin them at every length.
#ifndef COMPRESSORS_XXH32_PLAIN_BELOW
#define COMPRESSORS_XXH32_PLAIN_BELOW 1024
#endif


namespace compressors_xxhash_detail {

static constexpr uint32_t PRIME32_1 = 0x9E3779B1U;
static constexpr uint32_t PRIME32_2 = 0x85EBCA77U;
static constexpr uint32_t PRIME32_3 = 0xC2B2AE3DU;
static constexpr uint32_t PRIME32_4 = 0x27D4EB2FU;
static constexpr uint32_t PRIME32_5 = 0x165667B1U;

inline uint32_t rotl32(uint32_t x, int r) noexcept {
	return (x << r) | (x >> (32 - r));
}

constexpr uint32_t bswap32(uint32_t x) noexcept {
	return (x >> 24) | ((x >> 8) & 0x0000FF00U) | ((x << 8) & 0x00FF0000U) | (x << 24);
}

// How a host loads four bytes as one integer. The kernel reads through one of these, so the
// big-endian path runs, and is tested, on any host: EmulatedBigEndianHost loads the way a
// big-endian machine does, and read32_le swaps that back to the little-endian value XXH32 defines.
struct NativeHost {
	static constexpr bool big_endian = std::endian::native == std::endian::big;
	static uint32_t load32(const void* p) noexcept {
		uint32_t v;
		memcpy(&v, p, sizeof(v));
		return v;
	}
};

struct EmulatedBigEndianHost {
	static constexpr bool big_endian = true;
	static uint32_t load32(const void* p) noexcept {
		const auto* b = static_cast<const uint8_t*>(p);
		return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | uint32_t(b[3]);
	}
};

// The four bytes at p as a little-endian integer, on a host that loads them as Host does.
template <class Host>
inline uint32_t read32_le(const void* p) noexcept {
	const uint32_t v = Host::load32(p);
	if constexpr (Host::big_endian) {
		return bswap32(v);
	} else {
		return v;
	}
}

// n < 16 bytes from src to dst without a call: copies of fixed size, two of them overlapping when n
// is not a power of two. A tail or a carried block is short, and a libc memcpy of unknown size costs
// more than the hash of the 16 bytes next to it.
inline void copy_small(uint8_t* dst, const uint8_t* src, size_t n) noexcept {
	if (n >= 8) {
		uint64_t a, b;
		memcpy(&a, src, 8);
		memcpy(&b, src + n - 8, 8);
		memcpy(dst, &a, 8);
		memcpy(dst + n - 8, &b, 8);
	} else if (n >= 4) {
		uint32_t a, b;
		memcpy(&a, src, 4);
		memcpy(&b, src + n - 4, 4);
		memcpy(dst, &a, 4);
		memcpy(dst + n - 4, &b, 4);
	} else if (n != 0) {
		const uint8_t a = src[0], b = src[n >> 1], c = src[n - 1];
		dst[0] = a;
		dst[n >> 1] = b;
		dst[n - 1] = c;
	}
}

// Keeps x in a scalar register. Left alone, the compiler packs the four lanes into one vector
// (NEON, SSE2 and later), whose multiply has a longer latency than the scalar one; the
// dependency chain through each lane (not the throughput) then sets the speed on a long input:
// 1.4-2.1x slower on x86, 2x on AArch64.
inline void pin(uint32_t& x) noexcept {
#if (defined(__aarch64__) || defined(__SSE2__)) && (defined(__clang__) || defined(__GNUC__))
	__asm__("" : "+r"(x));
#else
	(void)x;
#endif
}

// The ways to run an update; they compute the same digest.
enum class Kernel {
	automatic,  // what the host runs fastest for the update's length
	generic,    // the lanes pinned in scalar registers: the chain through a lane sets the speed
	start,      // the update of 17873b2: the lanes unpinned, whatever the compiler makes of them
};

// The lanes after the blocks at p, p + 16, ... up to and including limit (p <= limit).
template <class Host>
inline const uint8_t* blocks_generic(uint32_t* v, const uint8_t* p, const uint8_t* limit) noexcept {
	const auto round = [](uint32_t acc, uint32_t input) noexcept {
		acc += input * PRIME32_2;
		acc = rotl32(acc, 13);
		acc *= PRIME32_1;
		pin(acc);
		return acc;
	};
	uint32_t v1 = v[0], v2 = v[1], v3 = v[2], v4 = v[3];
	do {
		v1 = round(v1, read32_le<Host>(p));
		v2 = round(v2, read32_le<Host>(p + 4));
		v3 = round(v3, read32_le<Host>(p + 8));
		v4 = round(v4, read32_le<Host>(p + 12));
		p += 16;
	} while (p <= limit);
	v[0] = v1; v[1] = v2; v[2] = v3; v[3] = v4;
	return p;
}

// The lanes after the blocks, pinned in scalar registers.
template <class Host>
inline const uint8_t* blocks(uint32_t* v, const uint8_t* p, const uint8_t* limit) noexcept {
	return blocks_generic<Host>(v, p, limit);
}

} // namespace compressors_xxhash_detail


// In a namespace so these never collide with a real xxHash (XXH32_state_t/
// XXH32_reset/...) the host may also have on its include path (e.g. lz4's xxhash.h
// pulled in by the hashes sibling). Renamed off "xxhash.h" for the same reason. The
// functions name the constants through using-declarations, which hide an inlined
// xxHash's global PRIME32_* where a using-directive would make them ambiguous.
namespace compressors {

struct XXH32_state_t {
	uint32_t total_len;
	uint32_t large_len;       // non-zero once total_len >= 16
	uint32_t v[4];            // the four lanes
	uint32_t mem32[4];        // up to 16 buffered bytes
	uint32_t memsize;         // buffered byte count
};


inline int XXH32_reset(XXH32_state_t* state, uint32_t seed) noexcept {
	using compressors_xxhash_detail::PRIME32_1, compressors_xxhash_detail::PRIME32_2;
	state->total_len = 0;
	state->large_len = 0;
	state->v[0] = seed + PRIME32_1 + PRIME32_2;
	state->v[1] = seed + PRIME32_2;
	state->v[2] = seed + 0;
	state->v[3] = seed - PRIME32_1;
	state->mem32[0] = state->mem32[1] = state->mem32[2] = state->mem32[3] = 0;
	state->memsize = 0;
	return 0;
}


namespace detail {

// On x86 a compiler leaves the update out of line once it holds the tail copies, and the call (the
// state through memory, no constant folding of a fresh state) takes 16 B from 7 ns to 17 on an
// i9-9980HK; the update before the tail copies was inline there, as upstream's streaming one is.
// The update and the functions that wrap it are forced inline on x86.
#if (defined(__x86_64__) || defined(__aarch64__)) && (defined(__clang__) || defined(__GNUC__))
#define COMPRESSORS_XXH32_UPDATE_INLINE __attribute__((always_inline))
#else
#define COMPRESSORS_XXH32_UPDATE_INLINE
#endif

// The update with the lanes pinned.
template <class Host>
COMPRESSORS_XXH32_UPDATE_INLINE inline int update_pinned(XXH32_state_t* state, const void* input, size_t len) noexcept {
	namespace x = compressors_xxhash_detail;
	const auto* p = static_cast<const uint8_t*>(input);
	size_t rest = len;  // the bytes from p on

	// Fill the carry buffer first if it holds a partial 16-byte block.
	if (state->memsize != 0) {
		const size_t take = 16 - state->memsize;
		x::copy_small(reinterpret_cast<uint8_t*>(state->mem32) + state->memsize, p, take);
		const auto* m = reinterpret_cast<const uint8_t*>(state->mem32);
		x::blocks<Host>(state->v, m, m);
		p += take;
		rest -= take;
		state->memsize = 0;
	}

	if (rest >= 16) {
		p = x::blocks<Host>(state->v, p, p + (rest - 16));  // up to the last whole block
		rest = static_cast<size_t>(len - static_cast<size_t>(p - static_cast<const uint8_t*>(input)));
	}

	if (rest != 0) {
		x::copy_small(reinterpret_cast<uint8_t*>(state->mem32), p, rest);
		state->memsize = static_cast<uint32_t>(rest);
	}

	return 0;
}

// The update of 17873b2, with its libc memcpy calls and the plain round: on AArch64 a short update inlined into
// its caller gets the code that was measured then, whatever the compiler makes of the lanes in that context.
template <class Host>
COMPRESSORS_XXH32_UPDATE_INLINE inline int update_start(XXH32_state_t* state, const void* input, size_t len) noexcept {
	using compressors_xxhash_detail::PRIME32_1, compressors_xxhash_detail::PRIME32_2, compressors_xxhash_detail::rotl32;
	const auto round = [](uint32_t acc, uint32_t in) noexcept { acc += in * PRIME32_2; acc = rotl32(acc, 13); return acc * PRIME32_1; };
	const auto* p = static_cast<const uint8_t*>(input);
	size_t rest = len;
	if (state->memsize != 0) {
		const size_t take = 16 - state->memsize;
		memcpy(reinterpret_cast<uint8_t*>(state->mem32) + state->memsize, input, take);
		const auto* m = reinterpret_cast<const uint8_t*>(state->mem32);
		state->v[0] = round(state->v[0], compressors_xxhash_detail::read32_le<Host>(m));
		state->v[1] = round(state->v[1], compressors_xxhash_detail::read32_le<Host>(m + 4));
		state->v[2] = round(state->v[2], compressors_xxhash_detail::read32_le<Host>(m + 8));
		state->v[3] = round(state->v[3], compressors_xxhash_detail::read32_le<Host>(m + 12));
		p += take;
		rest -= take;
		state->memsize = 0;
	}
	if (rest >= 16) {
		const uint8_t* const limit = p + (rest - 16);
		uint32_t v1 = state->v[0], v2 = state->v[1], v3 = state->v[2], v4 = state->v[3];
		do {
			v1 = round(v1, compressors_xxhash_detail::read32_le<Host>(p)); p += 4;
			v2 = round(v2, compressors_xxhash_detail::read32_le<Host>(p)); p += 4;
			v3 = round(v3, compressors_xxhash_detail::read32_le<Host>(p)); p += 4;
			v4 = round(v4, compressors_xxhash_detail::read32_le<Host>(p)); p += 4;
		} while (p <= limit);
		state->v[0] = v1; state->v[1] = v2; state->v[2] = v3; state->v[3] = v4;
		rest = len - static_cast<size_t>(p - static_cast<const uint8_t*>(input));
	}
	if (rest != 0) {
		memcpy(state->mem32, p, rest);
		state->memsize = static_cast<uint32_t>(rest);
	}
	return 0;
}

} // namespace detail


// The kernels, for a host that loads four bytes as Host does (NativeHost is the machine's own) and
// lanes run by K. XXH32_update and XXH32_digest below are these on the native host; the tests run
// them on an emulated big-endian one and on every kernel.
template <class Host, compressors_xxhash_detail::Kernel K = compressors_xxhash_detail::Kernel::automatic>
COMPRESSORS_XXH32_UPDATE_INLINE inline int XXH32_update_as(XXH32_state_t* state, const void* input, size_t len) noexcept {
	using compressors_xxhash_detail::Kernel;
	if (input == nullptr) {
		return 0;
	}

	state->total_len += static_cast<uint32_t>(len);
	if (len >= 16 || state->total_len >= 16) {
		state->large_len = 1;
	}

	if (state->memsize + len < 16) {
		compressors_xxhash_detail::copy_small(reinterpret_cast<uint8_t*>(state->mem32) + state->memsize, static_cast<const uint8_t*>(input), len);
		state->memsize += static_cast<uint32_t>(len);
		return 0;
	}

#if defined(__aarch64__) && (defined(__clang__) || defined(__GNUC__))
	if constexpr (K == Kernel::automatic) {
		if (len < COMPRESSORS_XXH32_PLAIN_BELOW) {
			return detail::update_start<Host>(state, input, len);
		}
	}
#endif
	if constexpr (K == Kernel::start) {
		return detail::update_start<Host>(state, input, len);
	} else {
		return detail::update_pinned<Host>(state, input, len);
	}
}




template <class Host>
inline uint32_t XXH32_digest_as(const XXH32_state_t* state) noexcept {
	using compressors_xxhash_detail::PRIME32_1, compressors_xxhash_detail::PRIME32_2, compressors_xxhash_detail::PRIME32_3,
		compressors_xxhash_detail::PRIME32_4, compressors_xxhash_detail::PRIME32_5, compressors_xxhash_detail::rotl32;
	uint32_t h32;

	if (state->large_len) {
		h32 = rotl32(state->v[0], 1) + rotl32(state->v[1], 7) + rotl32(state->v[2], 12) + rotl32(state->v[3], 18);
	} else {
		h32 = state->v[2] + PRIME32_5;
	}

	h32 += state->total_len;

	const auto* p = reinterpret_cast<const uint8_t*>(state->mem32);
	const uint8_t* const bEnd = p + state->memsize;

	while (p + 4 <= bEnd) {
		h32 += compressors_xxhash_detail::read32_le<Host>(p) * PRIME32_3;
		h32 = rotl32(h32, 17) * PRIME32_4;
		p += 4;
	}
	while (p < bEnd) {
		h32 += (*p) * PRIME32_5;
		h32 = rotl32(h32, 11) * PRIME32_1;
		++p;
	}

	h32 ^= h32 >> 15;
	h32 *= PRIME32_2;
	h32 ^= h32 >> 13;
	h32 *= PRIME32_3;
	h32 ^= h32 >> 16;
	return h32;
}


// XXH32 as upstream xxHash defines it, on every host: the same digest for the same bytes.
COMPRESSORS_XXH32_UPDATE_INLINE inline int XXH32_update(XXH32_state_t* state, const void* input, size_t len) noexcept {
	return XXH32_update_as<compressors_xxhash_detail::NativeHost>(state, input, len);
}


inline uint32_t XXH32_digest(const XXH32_state_t* state) noexcept {
	return XXH32_digest_as<compressors_xxhash_detail::NativeHost>(state);
}

}  // namespace compressors
