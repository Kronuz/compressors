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
// exposes it through LZ4BlockStreaming::get_digest(). It is an auxiliary
// checksum: it is not embedded in the compressed stream and is not needed for a
// round-trip (decompress(compress(x)) == x holds without it). Xapiand pulled in
// the full xxHash distribution; here we vendor only the XXH32 reset/update/digest
// surface the compressor calls, implementing the canonical XXH32 algorithm so
// the digest values match upstream xxHash. This avoids adding a fourth
// third-party dependency for a non-load-bearing checksum.

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

inline uint32_t read32_le(const void* p) noexcept {
	uint32_t v;
	memcpy(&v, p, sizeof(v));
	// The canonical algorithm reads little-endian; assume LE hosts (the only
	// platforms this library targets). On a BE host the digest would differ
	// from upstream, but it is never compared across hosts here.
	return v;
}

inline uint32_t round(uint32_t acc, uint32_t input) noexcept {
	acc += input * PRIME32_2;
	acc = rotl32(acc, 13);
	acc *= PRIME32_1;
	return acc;
}

} // namespace compressors_xxhash_detail


// In a namespace so these never collide with a real xxHash (XXH32_state_t/
// XXH32_reset/...) the host may also have on its include path (e.g. lz4's xxhash.h
// pulled in by the hashes sibling). Renamed off "xxhash.h" for the same reason.
namespace compressors {

struct XXH32_state_t {
	uint32_t total_len;
	uint32_t large_len;       // non-zero once total_len >= 16
	uint32_t v1, v2, v3, v4;
	uint32_t mem32[4];        // up to 16 buffered bytes
	uint32_t memsize;         // buffered byte count
};


inline int XXH32_reset(XXH32_state_t* state, uint32_t seed) noexcept {
	using namespace compressors_xxhash_detail;
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


inline int XXH32_update(XXH32_state_t* state, const void* input, size_t len) noexcept {
	using namespace compressors_xxhash_detail;
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
		state->v1 = round(state->v1, read32_le(m + 0));
		state->v2 = round(state->v2, read32_le(m + 1));
		state->v3 = round(state->v3, read32_le(m + 2));
		state->v4 = round(state->v4, read32_le(m + 3));
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
			v1 = round(v1, read32_le(p)); p += 4;
			v2 = round(v2, read32_le(p)); p += 4;
			v3 = round(v3, read32_le(p)); p += 4;
			v4 = round(v4, read32_le(p)); p += 4;
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


inline uint32_t XXH32_digest(const XXH32_state_t* state) noexcept {
	using namespace compressors_xxhash_detail;
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
		h32 += read32_le(p) * PRIME32_3;
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

}  // namespace compressors
