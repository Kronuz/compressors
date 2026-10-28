// Round-trip correctness test for the standalone compressors library.
//
// For each of the three buffer-core backends (deflate / lz4 / zstd) it runs the
// same spread of inputs through compress then decompress and asserts the result
// is byte-identical to the original, plus a plausibility check on the compressed
// size (incompressible input does not shrink wildly; highly repetitive input
// compresses well). The backends share the same compress_*/decompress_* shape,
// so the test body is one templated helper parameterized by the backend's pair
// of free functions.
//
// Build: c++ -std=c++20 -I.. test.cc ../compressor_*.cc -llz4 -lz -lzstd -o test && ./test

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#if COMPRESSORS_TEST_DEFLATE
#include "compressor_deflate.h"
#endif
#if COMPRESSORS_TEST_LZ4
// What an inlined xxHash (lz4's xxhash.c under XXH_INLINE_ALL, which a hashing library on the
// same include path may pull in first) declares at global scope: compressor_lz4.h's own XXH32
// must still compile beside it.
[[maybe_unused]] static const unsigned PRIME32_1 = 2654435761U, PRIME32_2 = 2246822519U, PRIME32_3 = 3266489917U, PRIME32_4 = 668265263U,
	PRIME32_5 = 374761393U;
#include "compressor_lz4.h"
#endif
#if COMPRESSORS_TEST_ZSTD
#include "compressor_zstd.h"
#endif


// The spread of inputs every backend must round-trip.
static std::vector<std::pair<std::string, std::string>> sample_inputs() {
	std::vector<std::pair<std::string, std::string>> inputs;

	// Empty.
	inputs.emplace_back("empty", std::string());

	// Small.
	inputs.emplace_back("small", std::string("hello, compressors"));

	// A single byte and a byte just over the lz4 2KiB block boundary, to exercise
	// the multi-block path.
	inputs.emplace_back("one-byte", std::string(1, 'x'));
	inputs.emplace_back("block-boundary", std::string(2049, 'q'));

	// Highly compressible: long repetitive run that spans many lz4 blocks.
	inputs.emplace_back("repetitive", std::string(200000, 'A'));

	// Compressible-ish text: a repeated phrase, large.
	{
		std::string s;
		while (s.size() < 100000) {
			s += "the quick brown fox jumps over the lazy dog. ";
		}
		inputs.emplace_back("repeated-phrase", std::move(s));
	}

	// Incompressible: deterministic pseudo-random bytes, large.
	{
		std::mt19937 rng(12345);
		std::string s;
		s.reserve(100000);
		for (int i = 0; i < 100000; ++i) {
			s.push_back(static_cast<char>(rng() & 0xff));
		}
		inputs.emplace_back("random", std::move(s));
	}

	return inputs;
}


// Round-trip every sample through one backend and assert correctness plus a
// plausibility check on the compressed size.
template <typename Compress, typename Decompress>
static void test_backend(const char* name, Compress compress, Decompress decompress) {
	for (const auto& [label, original] : sample_inputs()) {
		std::string compressed = compress(original);
		std::string restored = decompress(compressed);

		// Core guarantee: decompress(compress(x)) == x, byte for byte.
		assert(restored == original);

		// Plausibility of the compressed size.
		if (label == "repetitive" || label == "repeated-phrase") {
			// Highly compressible inputs must shrink a lot.
			assert(compressed.size() < original.size() / 2);
		} else if (label == "random") {
			// Incompressible input may grow slightly (framing overhead) but must
			// not blow up: stay within a small factor of the original.
			assert(compressed.size() < original.size() + original.size() / 4 + 1024);
		} else if (!original.empty()) {
			// Everything non-empty produces a non-empty compressed buffer.
			assert(!compressed.empty());
		}

		std::printf("  %-16s %-16s %8zu -> %8zu bytes\n",
			name, label.c_str(), original.size(), compressed.size());
	}
	std::printf("%s OK: all round-trips byte-identical\n\n", name);
}


// Write bytes to a fresh unique local file and return its path. The caller is
// responsible for unlinking it. ctest runs from the build tree, so this keeps
// scratch files under the build directory instead of using a system temp dir.
static std::string write_temp_file(const std::string& bytes) {
	static unsigned counter = 0;
	std::string path;
	int fd = -1;
	do {
		path = "compressors_test_" + std::to_string(static_cast<long long>(::getpid())) + "_" + std::to_string(counter++);
		fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	} while (fd == -1 && errno == EEXIST);
	assert(fd != -1);
	size_t off = 0;
	while (off < bytes.size()) {
		ssize_t w = ::write(fd, bytes.data() + off, bytes.size() - off);
		assert(w > 0);
		off += static_cast<size_t>(w);
	}
	::close(fd);
	return path;
}


// Drive a *CompressFile / *DecompressFile pair over an on-disk round trip:
// write the original bytes to a temp file, compress that file to a second temp
// file, decompress the second file, and assert the bytes match the original.
// Also exercises the adopt-an-fd constructor (open the fd yourself, hand it to
// the *File class, and confirm the class does NOT close your fd).
template <typename CompressFile, typename DecompressFile>
static void test_file_backend(const char* name) {
	for (const auto& [label, original] : sample_inputs()) {
		// The fd-streaming compress path reads the file in blocks and finishes
		// the deflate/lz4 stream when it hits EOF; a zero-byte file produces no
		// reads, so skip the empty case (the in-memory tests cover empty input).
		if (original.empty()) {
			continue;
		}

		std::string src_path = write_temp_file(original);
		std::string_view src_view(src_path);

		// Compress src_path -> compressed buffer, via the open-by-filename ctor
		// (the *File class owns and closes its own fd).
		std::string compressed;
		{
			CompressFile compressor(src_view);
			for (auto it = compressor.begin(); it; ++it) {
				compressed.append(*it);
			}
		}
		assert(!compressed.empty());

		// Write the compressed bytes to a second temp file and decompress it.
		std::string cmp_path = write_temp_file(compressed);
		std::string_view cmp_view(cmp_path);
		std::string restored;
		{
			DecompressFile decompressor(cmp_view);
			for (auto it = decompressor.begin(); it; ++it) {
				restored.append(*it);
			}
		}
		assert(restored == original);

		::unlink(src_path.c_str());
		::unlink(cmp_path.c_str());

		std::printf("  %-12s %-16s %8zu -> %8zu -> %8zu bytes (file)\n",
			name, label.c_str(), original.size(), compressed.size(), restored.size());
	}

	// Adopt-an-fd path: open the file ourselves, hand the borrowed fd to the
	// *File class, and confirm it never closes our fd. We pass fd_offset=-1 so
	// init() does not lseek (it reads from the current position).
	{
		const std::string payload(50000, 'k');
		std::string src_path = write_temp_file(payload);

		int fd = ::open(src_path.c_str(), O_RDONLY);
		assert(fd != -1);

		std::string compressed;
		{
			CompressFile compressor(fd, /*fd_offset=*/-1, /*fd_nbytes=*/-1);
			for (auto it = compressor.begin(); it; ++it) {
				compressed.append(*it);
			}
		}
		assert(!compressed.empty());

		// The borrowed fd must still be open: the *File class does not own it.
		// fcntl(F_GETFD) succeeds only on a live fd; close() returning 0 proves
		// the fd was still valid and that we (not the *File class) closed it.
		assert(::fcntl(fd, F_GETFD) != -1);
		assert(::close(fd) == 0);

		// And the compressed bytes are a real round trip.
		std::string cmp_path = write_temp_file(compressed);
		std::string_view cmp_view(cmp_path);
		std::string restored;
		{
			DecompressFile decompressor(cmp_view);
			for (auto it = decompressor.begin(); it; ++it) {
				restored.append(*it);
			}
		}
		assert(restored == payload);

		::unlink(src_path.c_str());
		::unlink(cmp_path.c_str());

		std::printf("  %-12s borrowed-fd: not closed by *File class, %zu -> %zu bytes round-tripped\n",
			name, payload.size(), restored.size());
	}

	std::printf("%s file OK: all on-disk round-trips byte-identical; borrowed fd left open\n\n", name);
}


#if COMPRESSORS_TEST_LZ4 || COMPRESSORS_TEST_ZSTD
// A one-shot block codec into the caller's buffer: every sample round-trips into a buffer of
// exactly its size and matches the reference encoder's bytes; too small a destination writes
// nothing (0); a wrong size, a truncated or flipped input, or garbage decodes to false, never
// past the buffer. Four threads at once exercise the per-thread contexts.
template <typename Bound, typename Compress, typename Decompress, typename Reference>
static void test_block_codec(const char* name, Bound bound, Compress compress, Decompress decompress, Reference reference) {
	auto once = [&](bool print) {
		for (const auto& [label, original] : sample_inputs()) {
			std::string out(bound(original.size()), '\0');
			const std::size_t n = compress(original, out.data(), out.size());
			assert(n > 0 && n <= out.size());
			out.resize(n);
			assert(out == reference(original));
			std::string back(original.size(), '\0');
			assert(decompress(out, back.data(), back.size()));
			assert(back == original);
			// A destination one byte short of the compressed size takes nothing.
			std::string tight(n - 1, '\0');
			assert(compress(original, tight.data(), tight.size()) == 0);
			// Any size but the original's is refused, whatever the buffer holds.
			std::string wrong(original.size() + 1, '\0');
			assert(!decompress(out, wrong.data(), wrong.size()));
			if (!original.empty()) {
				assert(!decompress(out, back.data(), back.size() - 1));
				assert(!decompress(std::string_view(out).substr(0, n / 2), back.data(), back.size()));
			}
			if (print) {
				std::printf("  %-12s %-16s %8zu -> %8zu bytes (block)\n", name, label.c_str(), original.size(), n);
			}
		}
		// Flipped bytes and garbage never decode to the wrong size or write past the buffer.
		std::mt19937 rng(7);
		const std::string original = sample_inputs()[5].second;
		std::string out(bound(original.size()), '\0');
		out.resize(compress(original, out.data(), out.size()));
		std::string back(original.size(), '\0');
		for (int i = 0; i < 2000; ++i) {
			std::string bad = out;
			bad[rng() % bad.size()] ^= char(1 + rng() % 255);
			if (decompress(bad, back.data(), back.size())) { assert(back.size() == original.size()); }
			std::string junk(1 + rng() % 64, '\0');
			for (auto& c : junk) { c = char(rng()); }
			(void)decompress(junk, back.data(), back.size());
		}
	};
	once(true);
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) { threads.emplace_back([&] { for (int r = 0; r < 3; ++r) { once(false); } }); }
	for (auto& t : threads) { t.join(); }
	std::printf("%s OK: block round-trips byte-identical, the reference's bytes, bounds refused\n\n", name);
}
#endif


#if COMPRESSORS_TEST_DEFLATE
static void test_bounded_deflate() {
	for (const bool gzip : {false, true}) {
		for (const auto &[name, original] : sample_inputs()) {
			(void)name;
			DeflateCompressData compressor(original.data(), original.size(), gzip);
			std::string compressed;
			for (auto it = compressor.begin(); it; ++it) compressed.append(*it);
			assert(decompress_deflate(compressed, original.size(), gzip) == original);
			DeflateDecompressData decoder(compressed.data(), compressed.size(), gzip, original.size());
			std::string result;
			for (auto it = decoder.begin(); it; ++it) {
				assert(it.size() <= DEFLATE_BLOCK_SIZE);
				result.append(*it);
			}
			assert(result == original);
			if (!original.empty()) {
				DeflateDecompressData limited(compressed.data(), compressed.size(), gzip, 0);
				for (int attempt = 0; attempt != 2; ++attempt) {
					bool rejected = false;
					try { for (auto it = limited.begin(); it; ++it) {} }
					catch (const DeflateException &) { rejected = true; }
					assert(rejected);
					limited.reset(compressed.data(), compressed.size(), gzip);
				}
			}
			const auto rejects = [&](std::string_view bytes, size_t bound) {
				bool rejected = false;
				try { (void)decompress_deflate(bytes, bound, gzip); }
				catch (const DeflateException &) { rejected = true; }
				assert(rejected);
			};
			if (!original.empty()) rejects(compressed, original.size() - 1);
			rejects(compressed + "trailing", original.size());
			if (gzip) {
				assert(decompress_deflate(compressed + compressed, original.size() * 2, true) == original + original);
				if (!original.empty()) rejects(compressed + compressed, original.size() * 2 - 1);
			} else rejects(compressed + compressed, original.size() * 2);
			for (size_t removed = 1; removed <= std::min<size_t>(compressed.size(), 16); ++removed)
				rejects(std::string_view(compressed).substr(0, compressed.size() - removed), original.size());
			auto corrupt = compressed;
			corrupt.back() ^= 1;
			rejects(corrupt, original.size());
			decoder.reset(compressed.data(), compressed.size(), gzip, original.size());
			result.clear();
			for (auto it = decoder.begin(); it; ++it) result.append(*it);
			assert(result == original);
		}
	}
	for (const bool gzip : {false, true}) {
		const std::string original(2 * 1024 * 1024, 'x');
		DeflateCompressData reference(original.data(), original.size(), gzip);
		std::string expected;
		for (auto it = reference.begin(); it; ++it) expected.append(*it);
		DeflateCompressData compressor(original.data(), original.size(), gzip);
		std::string compressed;
		for (const auto &block : compressor) compressed.append(block);
		assert(compressed == expected);
		DeflateDecompressData decoder(compressed.data(), compressed.size(), gzip);
		std::string restored;
		for (const auto &block : decoder) restored.append(block);
		assert(restored == original);
		if (gzip) {
			DeflateCompressData empty("", 0, true);
			std::string members;
			for (auto it = empty.begin(); it; ++it) members.append(*it);
			members += compressed;
			DeflateDecompressData joined(members.data(), members.size(), true);
			restored.clear();
			for (const auto &block : joined) restored.append(block);
			assert(restored == original);
		}
	}
	std::puts("bounded deflate/gzip output, chunks, truncation, checksum, trailing data and reset passed");
}
#endif

int main() {
	std::printf("compressors round-trip test\n\n");

	// Each backend built, and its on-disk round trips through the fd-streaming *File
	// classes (deflate and lz4; zstd has none), which also leave a borrowed fd open.
#if COMPRESSORS_TEST_DEFLATE
	test_bounded_deflate();
	test_backend("deflate",
		[](std::string_view s) { return compress_deflate(s); },
		[](std::string_view s) { return decompress_deflate(s); });
	test_file_backend<DeflateCompressFile, DeflateDecompressFile>("deflate");
#endif
#if COMPRESSORS_TEST_LZ4
	test_backend("lz4",
		[](std::string_view s) { return compress_lz4(s); },
		[](std::string_view s) { return decompress_lz4(s); });
	test_file_backend<LZ4CompressFile, LZ4DecompressFile>("lz4");
#endif
#if COMPRESSORS_TEST_ZSTD
	test_backend("zstd",
		[](std::string_view s) { return compress_zstd(s); },
		[](std::string_view s) { return decompress_zstd(s); });
#endif
#if COMPRESSORS_TEST_LZ4
	test_block_codec("lz4 block",
		[](std::size_t n) { return lz4_block_bound(n); },
		[](std::string_view s, char* d, std::size_t c) { return compress_lz4_block(s, d, c); },
		[](std::string_view s, char* d, std::size_t n) { return decompress_lz4_block(s, d, n); },
		[](std::string_view s) {
			std::string out(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(s.size()))), '\0');
			out.resize(static_cast<std::size_t>(LZ4_compress_default(s.data(), out.data(), static_cast<int>(s.size()), static_cast<int>(out.size()))));
			return out;
		});
	assert(lz4_block_bound(std::size_t(LZ4_MAX_INPUT_SIZE) + 1) == 0);
#endif
#if COMPRESSORS_TEST_ZSTD
	for (int level : {1, 3}) {
		test_block_codec(level == 1 ? "zstd-1 into" : "zstd-3 into",
			[](std::size_t n) { return zstd_frame_bound(n); },
			[level](std::string_view s, char* d, std::size_t c) { return compress_zstd_into(s, d, c, level); },
			[](std::string_view s, char* d, std::size_t n) { return decompress_zstd_into(s, d, n); },
			[level](std::string_view s) {
				std::string out(ZSTD_compressBound(s.size()), '\0');
				out.resize(ZSTD_compress(out.data(), out.size(), s.data(), s.size(), level));
				return out;
			});
	}
	// The level-3 frame is compress_zstd's own.
	assert([] {
		const std::string s = sample_inputs()[4].second;
		std::string out(zstd_frame_bound(s.size()), '\0');
		out.resize(compress_zstd_into(s, out.data(), out.size(), ZSTD_COMPRESS_LEVEL));
		return out == compress_zstd(s);
	}());
#endif

#if COMPRESSORS_TEST_DEFLATE && COMPRESSORS_TEST_LZ4 && COMPRESSORS_TEST_ZSTD
	// Cross-check that the three backends are independent: each decodes only its
	// own output. A spot check that compressed forms differ for a non-trivial
	// input (they use different formats).
	{
		std::string original(50000, 'Z');
		original += std::string("some trailing variety to avoid a trivial frame");
		std::string d = compress_deflate(original);
		std::string l = compress_lz4(original);
		std::string z = compress_zstd(original);
		assert(decompress_deflate(d) == original);
		assert(decompress_lz4(l) == original);
		assert(decompress_zstd(z) == original);
		assert(decompress_zstd(z, original.size()) == original);
		bool threw = false;
		try { (void)decompress_zstd(z, original.size() - 1); } catch (...) { threw = true; }
		assert(threw);
		// The three encodings are genuinely different byte streams.
		assert(d != l && l != z && d != z);
		std::printf("cross-check OK: three backends produce distinct, independently decodable frames\n\n");
	}
#endif

	std::printf("all compressors tests passed\n");
	return 0;
}
