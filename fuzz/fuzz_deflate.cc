// libFuzzer target for the deflate decoders.
//
// Input: [u8 mode][u8 a][u8 b] then bytes.
//   - mode&3 == 0: the bytes as a zlib (a&1 == 0) or gzip (a&1) stream through
//     DeflateDecompressData and DeflateDecompressFile. Each decodes or throws DeflateException.
//   - mode&3 == 1: the same through decompress_deflate.
//   - mode&3 == 2: the bytes compressed and decompressed come back equal.
//   - mode&3 == 3: the same, with one byte of the stream XORed (a picks the position among the
//     first 64 bytes, b the mask): it decodes or throws DeflateException.
#include "compressor_deflate.h"

#include "fuzz_util.h"

#include <algorithm>
#include <cstdint>
#include <string>

static void decode_data(std::string_view in, bool gzip) {
	try {
		DeflateDecompressData d(in.data(), in.size(), gzip);
		for (auto it = d.begin(); it; ++it) { }
	} catch (const DeflateException&) {
	}
}

static void decode_file(std::string_view in, bool gzip) {
	const int fd = fuzz_fd(in);
	try {
		DeflateDecompressFile d(fd, -1, -1, gzip);
		for (auto it = d.begin(); it; ++it) { }
	} catch (const DeflateException&) {
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	// Inflate expands at most about 1032:1.
	if (size < 3 || size > (1u << 14)) { return 0; }
	const unsigned mode = data[0] & 3, a = data[1], b = data[2];
	const std::string_view rest(reinterpret_cast<const char*>(data) + 3, size - 3);

	switch (mode) {
		case 0:
			decode_data(rest, a & 1);
			decode_file(rest, a & 1);
			break;
		case 1:
			try { (void)decompress_deflate(rest); } catch (const DeflateException&) { }
			break;
		default: {
			std::string packed = compress_deflate(rest);
			if (mode == 2) {
				FUZZ_CHECK(decompress_deflate(packed) == rest);
				break;
			}
			if (!packed.empty()) { packed[a % std::min<size_t>(packed.size(), 64)] ^= static_cast<char>(b ? b : 1); }
			decode_data(packed, false);
			decode_file(packed, false);
		}
	}
	return 0;
}
