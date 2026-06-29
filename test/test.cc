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
#include <random>
#include <string>
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
