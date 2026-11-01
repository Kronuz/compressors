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

inline uint32_t round(uint32_t acc, uint32_t input) noexcept {
	acc += input * PRIME32_2;
	acc = rotl32(acc, 13);
	acc *= PRIME32_1;
#if (defined(__aarch64__) || defined(__SSE2__)) && (defined(__clang__) || defined(__GNUC__))
	// Keeps the four lanes in scalar registers, as upstream xxHash does on these targets. Left
	// alone, the compiler packs them into one vector (NEON, SSE2 and later), whose multiply has
	// a longer latency than the scalar one, and the dependency chain through each lane (not the
	// throughput) then sets the speed: 1.4-2.1x slower on x86, 2x on AArch64.
	__asm__("" : "+r"(acc));
#endif
	return acc;
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
	uint32_t v1, v2, v3, v4;
	uint32_t mem32[4];        // up to 16 buffered bytes
	uint32_t memsize;         // buffered byte count
};


inline int XXH32_reset(XXH32_state_t* state, uint32_t seed) noexcept {
	using compressors_xxhash_detail::PRIME32_1, compressors_xxhash_detail::PRIME32_2;
	state->total_len = 0;
	state->large_len = 0;
	state->v1 = seed + PRIME32_1 + PRIME32_2;
	state->v2 = seed + PRIME32_2;
	state->v3 = seed + 0;
	state->v4 = seed - PRIME32_1;
	state->mem32[0] = state->mem32[1] = state->mem32[2] = state->mem32[3] = 0;
	state->memsize = 0;
	return 0;
}


// The kernels, for a host that loads four bytes as Host does (NativeHost is the machine's own).
// XXH32_update and XXH32_digest below are these on the native host; the tests run them on an
// emulated big-endian one.
template <class Host>
inline int XXH32_update_as(XXH32_state_t* state, const void* input, size_t len) noexcept {
	using compressors_xxhash_detail::round;
	if (input == nullptr) {
		return 0;
	}

	const auto* p = static_cast<const uint8_t*>(input);
	const uint8_t* const bEnd = p + len;

	state->total_len += static_cast<uint32_t>(len);
	if (state->total_len >= 16) {
		state->large_len = 1;
	}

	// Fill the carry buffer first if it holds a partial 16-byte block.
	if (state->memsize + len < 16) {
		memcpy(reinterpret_cast<uint8_t*>(state->mem32) + state->memsize, input, len);
		state->memsize += static_cast<uint32_t>(len);
		return 0;
	}

	if (state->memsize != 0) {
		memcpy(reinterpret_cast<uint8_t*>(state->mem32) + state->memsize, input, 16 - state->memsize);
		const uint32_t* m = state->mem32;
		state->v1 = round(state->v1, compressors_xxhash_detail::read32_le<Host>(m + 0));
		state->v2 = round(state->v2, compressors_xxhash_detail::read32_le<Host>(m + 1));
		state->v3 = round(state->v3, compressors_xxhash_detail::read32_le<Host>(m + 2));
		state->v4 = round(state->v4, compressors_xxhash_detail::read32_le<Host>(m + 3));
		p += 16 - state->memsize;
		state->memsize = 0;
	}

	if (p <= bEnd - 16) {
		const uint8_t* const limit = bEnd - 16;
		uint32_t v1 = state->v1;
		uint32_t v2 = state->v2;
		uint32_t v3 = state->v3;
		uint32_t v4 = state->v4;
		do {
			v1 = round(v1, compressors_xxhash_detail::read32_le<Host>(p)); p += 4;
			v2 = round(v2, compressors_xxhash_detail::read32_le<Host>(p)); p += 4;
			v3 = round(v3, compressors_xxhash_detail::read32_le<Host>(p)); p += 4;
			v4 = round(v4, compressors_xxhash_detail::read32_le<Host>(p)); p += 4;
		} while (p <= limit);
		state->v1 = v1;
		state->v2 = v2;
		state->v3 = v3;
		state->v4 = v4;
	}

	if (p < bEnd) {
		memcpy(state->mem32, p, static_cast<size_t>(bEnd - p));
		state->memsize = static_cast<uint32_t>(bEnd - p);
	}

	return 0;
}


template <class Host>
inline uint32_t XXH32_digest_as(const XXH32_state_t* state) noexcept {
	using compressors_xxhash_detail::PRIME32_1, compressors_xxhash_detail::PRIME32_2, compressors_xxhash_detail::PRIME32_3,
		compressors_xxhash_detail::PRIME32_4, compressors_xxhash_detail::PRIME32_5, compressors_xxhash_detail::rotl32;
	uint32_t h32;

	if (state->large_len) {
		h32 = rotl32(state->v1, 1) + rotl32(state->v2, 7) + rotl32(state->v3, 12) + rotl32(state->v4, 18);
	} else {
		h32 = state->v3 + PRIME32_5;
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
inline int XXH32_update(XXH32_state_t* state, const void* input, size_t len) noexcept {
	return XXH32_update_as<compressors_xxhash_detail::NativeHost>(state, input, len);
}


inline uint32_t XXH32_digest(const XXH32_state_t* state) noexcept {
	return XXH32_digest_as<compressors_xxhash_detail::NativeHost>(state);
}

}  // namespace compressors
