// One-shot lz4: compress_lz4 / decompress_lz4 against the stream classes (same bytes) and
// against raw LZ4 blocks (a different, unframed format, so a floor to compare with). Prints the best of several runs per case.
#include <chrono>
#include <cstdio>
#include <random>
#include <string>

#include "compressor_lz4.h"

template <typename F>
static double best_ns(F&& f, int reps = 15) {
	double best = 1e18;
	for (int r = 0; r < reps; ++r) {
		auto t0 = std::chrono::steady_clock::now();
		f();
		auto t1 = std::chrono::steady_clock::now();
		best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count());
	}
	return best;
}

static std::string old_compress(std::string_view s) {
	std::string out;
	LZ4CompressData c(s.data(), s.size());
	for (auto it = c.begin(); it; ++it) { out.append(*it); }
	return out;
}

static std::string old_decompress(std::string_view s) {
	std::string out;
	LZ4DecompressData d(s.data(), s.size());
	for (auto it = d.begin(); it; ++it) { out.append(*it); }
	return out;
}

int main() {
	std::mt19937_64 rng(7);
	static const char* const words[] = {"alpha ", "beta ", "gamma ", "delta ", "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog\n"};
	for (const char* kind : {"random", "text"}) {
		for (std::size_t n : {std::size_t(256), std::size_t(4096), std::size_t(65536), std::size_t(1) << 20}) {
			std::string s;
			if (kind[0] == 'r') {
				s.resize(n);
				for (auto& c : s) { c = static_cast<char>(rng()); }
			} else {
				while (s.size() < n) { s += words[rng() % 12]; }
				s.resize(n);
			}
			const int iters = n >= 65536 ? 1 : 200;
			const std::string packed = compress_lz4(s);
			if (packed != old_compress(s) || decompress_lz4(packed) != s) { std::printf("MISMATCH\n"); return 1; }
			std::string raw(lz4_block_bound(n), '\0');
			const std::size_t raw_n = compress_lz4_block(s, raw.data(), raw.size());
			std::string back(n, '\0');
			volatile std::size_t sink = 0;
			double co = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = old_compress(s).size(); } }) / iters;
			double cn = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = compress_lz4(s).size(); } }) / iters;
			double cr = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = compress_lz4_block(s, raw.data(), raw.size()); } }) / iters;
			double dO = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = old_decompress(packed).size(); } }) / iters;
			double dn = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = decompress_lz4(packed).size(); } }) / iters;
			double dr = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = decompress_lz4_block(std::string_view(raw.data(), raw_n), back.data(), n); } }) / iters;
			std::printf("%-6s %8zu B  compress old %10.0f new %10.0f (%.1fx) raw %10.0f ns | decompress old %10.0f new %10.0f (%.1fx) raw %10.0f ns\n",
				kind, n, co, cn, co / cn, cr, dO, dn, dO / dn, dr);
		}
	}
}
