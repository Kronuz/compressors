// XXH32 against upstream xxHash (fetched by CMake for this test only; the library never depends
// on it): every length from 0 to 4096 at several seeds, alignments and ways of cutting the input,
// on every kernel, then random lengths up to 1 MiB, and on Linux one update of 4 GiB + 5 bytes
// (the length counter wraps; upstream still knows the input was long) where sanitizers are off.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

// clang defines no __SANITIZE_ADDRESS__: its sanitizers announce themselves through __has_feature.
#ifdef __has_feature
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define COMPRESSORS_TEST_SANITIZED 1
#endif
#endif
#ifndef COMPRESSORS_TEST_SANITIZED
#define COMPRESSORS_TEST_SANITIZED 0
#endif

#if defined(__linux__)
#include <sys/mman.h>
#endif

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "xxh32_stream.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

namespace {

using compressors_xxhash_detail::Kernel;
using compressors_xxhash_detail::NativeHost;

// The digest of data fed in pieces of the given sizes (cycled), through kernel K.
template <Kernel K>
std::uint32_t ours(const unsigned char* d, std::size_t n, std::uint32_t seed, const std::vector<std::size_t>& pieces) {
	compressors::XXH32_state_t st;
	compressors::XXH32_reset(&st, seed);
	std::size_t o = 0;
	for (std::size_t k = 0; o < n; ++k) {
		const std::size_t c = std::min(pieces[k % pieces.size()], n - o);
		compressors::XXH32_update_as<NativeHost, K>(&st, d + o, c);
		o += c;
	}
	return compressors::XXH32_digest(&st);
}

// Upstream's own streaming API over the same pieces, so the two streams are cut alike.
std::uint32_t upstream_streamed(const unsigned char* d, std::size_t n, std::uint32_t seed, const std::vector<std::size_t>& pieces) {
	XXH32_state_t st;
	XXH32_reset(&st, seed);
	std::size_t o = 0;
	for (std::size_t k = 0; o < n; ++k) {
		const std::size_t c = std::min(pieces[k % pieces.size()], n - o);
		XXH32_update(&st, d + o, c);
		o += c;
	}
	return XXH32_digest(&st);
}

template <class F>
void for_each_kernel(F&& f) {
	f.template operator()<Kernel::automatic>();
	f.template operator()<Kernel::generic>();
	f.template operator()<Kernel::start>();
}

}  // namespace


int main() {
	std::mt19937_64 rng(0x58584833);
	std::vector<unsigned char> buf(4096 + 16);
	for (auto& c : buf) { c = static_cast<unsigned char>(rng()); }

	// Every length 0..4096, one update, at the alignments of a carried-over tail (0..3 and 15), seeds
	// including the edges, and the digest of upstream's one-shot as the oracle.
	const std::uint32_t seeds[] = {0U, 1U, 0x9E3779B1U, 0x85EBCA77U, 0xFFFFFFFFU, static_cast<std::uint32_t>(rng())};
	for_each_kernel([&]<Kernel K>() {
		for (std::size_t n = 0; n <= 4096; ++n) {
			for (std::size_t off : {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(3), std::size_t(15)}) {
				if (off && (n % 7) != off % 7) { continue; }  // the odd alignments on a sample of lengths
				const unsigned char* d = buf.data() + off;
				for (std::uint32_t seed : seeds) {
					CHECK((ours<K>(d, n, seed, {n + 1}) == XXH32(d, n, seed)));
				}
			}
		}
	});

	// Cut in pieces: the carry buffer, a piece shorter than 16 bytes, the boundaries between kernels.
	const std::vector<std::vector<std::size_t>> cuts = {{1}, {3}, {7}, {15}, {16}, {17}, {31}, {64}, {255}, {256}, {257}, {1023}, {1024}, {1025}, {2048}, {5, 11, 16, 1, 0, 33}, {4096, 1, 15, 16, 17}};
	for_each_kernel([&]<Kernel K>() {
		for (std::size_t n : {std::size_t(0), std::size_t(1), std::size_t(15), std::size_t(16), std::size_t(17), std::size_t(31), std::size_t(32), std::size_t(33), std::size_t(255), std::size_t(256), std::size_t(257), std::size_t(1000), std::size_t(2047), std::size_t(2048), std::size_t(2049), std::size_t(4096)}) {
			for (std::uint32_t seed : seeds) {
				const std::uint32_t want = XXH32(buf.data(), n, seed);
				for (const auto& cut : cuts) {
					CHECK((ours<K>(buf.data(), n, seed, cut) == want));
					CHECK(upstream_streamed(buf.data(), n, seed, cut) == want);
				}
			}
		}
	});

	// Random lengths, seeds and cuts, longer inputs included.
	std::vector<unsigned char> big(1u << 20);
	for (auto& c : big) { c = static_cast<unsigned char>(rng()); }
	for_each_kernel([&]<Kernel K>() {
		for (int i = 0; i < 600; ++i) {
			const std::size_t n = i < 30 ? (std::size_t(1) << 19) + rng() % (std::size_t(1) << 19) : rng() % 20000;
			const std::size_t off = rng() % 64;
			const auto seed = static_cast<std::uint32_t>(rng());
			std::vector<std::size_t> cut;
			for (int k = 0; k < 6; ++k) { cut.push_back(rng() % 3000); }
			cut.push_back(1);
			const unsigned char* d = big.data() + off;
			if (off + n > big.size()) { continue; }
			CHECK((ours<K>(d, n, seed, cut) == XXH32(d, n, seed)));
		}
	});

#if defined(__linux__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && !COMPRESSORS_TEST_SANITIZED
	// One update of 4 GiB + 5 bytes: the 32-bit length wraps to 5, and the digest is still the one of
	// a long input. The pages are never written, so they cost nothing. (Skipped under the
	// sanitizers, where hashing 4 GiB takes minutes and the check is of arithmetic, not of memory.)
	if (sizeof(std::size_t) >= 8) {
		const std::size_t n = (std::size_t(1) << 32) + 5;
		void* m = mmap(nullptr, n, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (m != MAP_FAILED) {
			const auto* d = static_cast<const unsigned char*>(m);
			CHECK((ours<Kernel::automatic>(d, n, 3, {n}) == XXH32(d, n, 3)));
			munmap(m, n);
		}
	}
#endif

	std::printf("all xxh32 upstream tests passed\n");
	return 0;
}
