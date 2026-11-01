// XXH32 throughput: the library's streaming implementation against upstream xxHash.
// Build through CMake (-DCOMPRESSORS_BENCH=ON); it prints one line per case and checks
// that the digests agree. Per input size, three ways to call it:
//   2KB chunks    reset, update in 2 KB pieces (as the lz4 stream feeds it), digest;
//   one update    reset, one update over the whole buffer, digest; the calls do not depend
//                 on each other, so the processor overlaps them (a short input costs its
//                 throughput, not its latency);
//   dependent     the same, but each call seeds from the digest of the one before, so the
//                 next cannot start until this one ends (a short input costs its latency).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"
#include "xxh32_stream.h"

template <typename F>
static double best_ns(F&& f, int reps = 25) {
	double best = 1e18;
	for (int r = 0; r < reps; ++r) {
		auto t0 = std::chrono::steady_clock::now();
		f();
		auto t1 = std::chrono::steady_clock::now();
		best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count());
	}
	return best;
}

int main() {
	std::mt19937_64 rng(1);
	for (std::size_t n : {std::size_t(16), std::size_t(256), std::size_t(2048), std::size_t(65536), std::size_t(1) << 20}) {
		std::string s(n, '\0');
		for (auto& c : s) { c = static_cast<char>(rng()); }
		volatile uint32_t sink = 0;
		const int iters = n >= 65536 ? 1 : n <= 256 ? 5000 : 200;
		uint32_t a = 0, b = 0, c = 0, d = 0, e = 0;
		double lib = best_ns([&] {  // streaming, 2 KB chunks, as the lz4 stream feeds it
			for (int i = 0; i < iters; ++i) {
				compressors::XXH32_state_t st;
				compressors::XXH32_reset(&st, 0);
				for (std::size_t o = 0; o < n; o += 2048) { compressors::XXH32_update(&st, s.data() + o, std::min<std::size_t>(2048, n - o)); }
				a = compressors::XXH32_digest(&st);
				sink = a;
			}
		}) / iters;
		double up = best_ns([&] { for (int i = 0; i < iters; ++i) { b = XXH32(s.data(), n, 0); sink = b; } }) / iters;
		double lib1 = best_ns([&] {  // one update over the whole buffer
			for (int i = 0; i < iters; ++i) {
				compressors::XXH32_state_t st;
				compressors::XXH32_reset(&st, 0);
				compressors::XXH32_update(&st, s.data(), n);
				c = compressors::XXH32_digest(&st);
				sink = c;
			}
		}) / iters;
		double dep = best_ns([&] {  // one update, each call seeded by the digest before it
			uint32_t h = 0;
			for (int i = 0; i < iters; ++i) {
				compressors::XXH32_state_t st;
				compressors::XXH32_reset(&st, h);
				compressors::XXH32_update(&st, s.data(), n);
				h = compressors::XXH32_digest(&st);
			}
			sink = d = h;
		}) / iters;
		double updep = best_ns([&] {
			uint32_t h = 0;
			for (int i = 0; i < iters; ++i) { h = XXH32(s.data(), n, h); }
			sink = e = h;
		}) / iters;
		if (a != b || c != b) { std::printf("DIGEST MISMATCH n=%zu\n", n); return 1; }
		if (d != e) { std::printf("DIGEST MISMATCH (dependent) n=%zu\n", n); return 1; }
		std::printf("n=%-8zu lib(2KB chunks) %9.1f ns  lib(one update) %9.1f ns  upstream %9.1f ns  ratio %.2f  dependent: lib %9.1f ns  upstream %9.1f ns\n", n, lib, lib1, up, lib / up, dep, updep);
	}
}
