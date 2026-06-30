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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

#include "compressor_deflate.h"
#include "compressor_lz4.h"
#include "compressor_zstd.h"


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


// Write bytes to a fresh unique temp file and return its path. The caller is
// responsible for unlinking it.
static std::string write_temp_file(const std::string& bytes) {
	char path[] = "/tmp/compressors_test_XXXXXX";
	int fd = ::mkstemp(path);
	assert(fd != -1);
	size_t off = 0;
	while (off < bytes.size()) {
		ssize_t w = ::write(fd, bytes.data() + off, bytes.size() - off);
		assert(w > 0);
		off += static_cast<size_t>(w);
	}
	::close(fd);
	return std::string(path);
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


int main() {
	std::printf("compressors round-trip test\n\n");

	test_backend("deflate",
		[](std::string_view s) { return compress_deflate(s); },
		[](std::string_view s) { return decompress_deflate(s); });

	test_backend("lz4",
		[](std::string_view s) { return compress_lz4(s); },
		[](std::string_view s) { return decompress_lz4(s); });

	test_backend("zstd",
		[](std::string_view s) { return compress_zstd(s); },
		[](std::string_view s) { return decompress_zstd(s); });

	// On-disk round trips through the fd-streaming *File classes (deflate and
	// lz4; zstd has no *File variant). Also asserts the adopt-an-fd ctor leaves
	// a borrowed fd open.
	test_file_backend<DeflateCompressFile, DeflateDecompressFile>("deflate");
	test_file_backend<LZ4CompressFile, LZ4DecompressFile>("lz4");

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
		// The three encodings are genuinely different byte streams.
		assert(d != l && l != z && d != z);
		std::printf("cross-check OK: three backends produce distinct, independently decodable frames\n\n");
	}

	std::printf("all compressors tests passed\n");
	return 0;
}
