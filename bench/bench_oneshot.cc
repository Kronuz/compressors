// One-shot zstd and deflate: compress_* / decompress_* against the stream classes they bypass
// (same bytes) and against the codecs' own one-call APIs (ZSTD_compress and ZSTD_decompress,
// zlib's compress2 and uncompress), the floor to compare with. Prints the best of several
// runs per case, in ns per call. Build through CMake (-DCOMPRESSORS_BENCH=ON).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>

#include <zlib.h>
#include <zstd.h>

#include "compressor_deflate.h"
#include "compressor_zstd.h"

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

// The stream classes are the reference the one-shots must match byte for byte.
template <class Codec>
static std::string via_stream(std::string_view s) {
	std::string out;
	Codec c(s.data(), s.size());
	for (auto it = c.begin(); it; ++it) { out.append(*it); }
	return out;
}

int main() {
	std::printf("zstd %s (compression level %d), zlib %s (default level); ns per call, best of 15\n", ZSTD_versionString(), int(ZSTD_COMPRESS_LEVEL), zlibVersion());
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
			const int iters = n >= 65536 ? 1 : 100;

			// zstd
			const std::string zs = compress_zstd(s);
			std::string zdirect(ZSTD_compressBound(n), '\0');
			const std::size_t zn = ZSTD_compress(zdirect.data(), zdirect.size(), s.data(), n, ZSTD_COMPRESS_LEVEL);
			if (zs != via_stream<ZstdCompressData>(s) || zs != std::string_view(zdirect.data(), zn) || decompress_zstd(zs) != s) {
				std::printf("MISMATCH zstd\n");
				return 1;
			}
			std::string back(n, '\0');
			volatile std::size_t sink = 0;
			const double zc_stream = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = via_stream<ZstdCompressData>(s).size(); } }) / iters;
			const double zc_oneshot = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = compress_zstd(s).size(); } }) / iters;
			const double zc_direct = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = ZSTD_compress(zdirect.data(), zdirect.size(), s.data(), n, ZSTD_COMPRESS_LEVEL); } }) / iters;
			const double zd_stream = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = via_stream<ZstdDecompressData>(zs).size(); } }) / iters;
			const double zd_oneshot = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = decompress_zstd(zs).size(); } }) / iters;
			const double zd_bounded = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = decompress_zstd(zs, n).size(); } }) / iters;
			const double zd_direct = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = ZSTD_decompress(back.data(), n, zs.data(), zs.size()); } }) / iters;
			std::printf("%-6s %8zu B  zstd compress: stream %9.0f one-shot %9.0f direct %9.0f | decompress: stream %9.0f one-shot %9.0f bounded %9.0f direct %9.0f ns\n",
				kind, n, zc_stream, zc_oneshot, zc_direct, zd_stream, zd_oneshot, zd_bounded, zd_direct);

			// deflate
			const std::string ds = compress_deflate(s);
			std::string ddirect(compressBound(static_cast<uLong>(n)), '\0');
			uLongf dn = ddirect.size();
			compress2(reinterpret_cast<Bytef*>(ddirect.data()), &dn, reinterpret_cast<const Bytef*>(s.data()), static_cast<uLong>(n), Z_DEFAULT_COMPRESSION);
			if (ds != via_stream<DeflateCompressData>(s) || ds != std::string_view(ddirect.data(), dn) || decompress_deflate(ds) != s) {
				std::printf("MISMATCH deflate\n");
				return 1;
			}
			const int diters = n >= 65536 ? 1 : 20;
			const double dc_stream = best_ns([&] { for (int i = 0; i < diters; ++i) { sink = via_stream<DeflateCompressData>(s).size(); } }) / diters;
			const double dc_oneshot = best_ns([&] { for (int i = 0; i < diters; ++i) { sink = compress_deflate(s).size(); } }) / diters;
			const double dc_direct = best_ns([&] {
				for (int i = 0; i < diters; ++i) {
					uLongf l = ddirect.size();
					sink = compress2(reinterpret_cast<Bytef*>(ddirect.data()), &l, reinterpret_cast<const Bytef*>(s.data()), static_cast<uLong>(n), Z_DEFAULT_COMPRESSION);
				}
			}) / diters;
			const double dd_stream = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = via_stream<DeflateDecompressData>(ds).size(); } }) / iters;
			const double dd_oneshot = best_ns([&] { for (int i = 0; i < iters; ++i) { sink = decompress_deflate(ds).size(); } }) / iters;
			const double dd_direct = best_ns([&] {
				for (int i = 0; i < iters; ++i) {
					uLongf l = n;
					sink = uncompress(reinterpret_cast<Bytef*>(back.data()), &l, reinterpret_cast<const Bytef*>(ds.data()), static_cast<uLong>(ds.size()));
				}
			}) / iters;
			std::printf("%-6s %8zu B  deflate compress: stream %9.0f one-shot %9.0f direct %9.0f | decompress: stream %9.0f one-shot %9.0f direct %9.0f ns\n",
				kind, n, dc_stream, dc_oneshot, dc_direct, dd_stream, dd_oneshot, dd_direct);
		}
	}
}
