// libFuzzer target for the LZ4 decoders.
//
// Input: [u8 mode][u8 a][u8 b] then bytes.
//   - mode&3 == 0: the bytes as a block stream, through LZ4DecompressData and LZ4DecompressFile.
//     Each either decodes or throws LZ4Exception; the two agree on the outcome and the bytes.
//   - mode&3 == 1: the bytes as one raw block into a buffer of a*256+b bytes (never out of bounds).
//   - mode&3 == 2: the bytes compressed and decompressed come back equal.
//   - mode&3 == 3: the same, with one byte of the compressed stream XORed (a picks the position
//     among the first 64 bytes, b the mask): the stream decodes or throws LZ4Exception.
#include "compressor_lz4.h"

#include "fuzz_util.h"

#include <algorithm>
#include <cstdint>
#include <string>

struct Decoded {
	bool ok;
	std::string bytes;
	uint32_t digest;
};

static Decoded by_data(std::string_view in) {
	Decoded r{true, {}, 0};
	try {
		LZ4DecompressData d(in.data(), in.size());
		for (auto it = d.begin(); it; ++it) { r.bytes += *it; }
		r.digest = d.get_digest();
	} catch (const LZ4Exception&) {
		r.ok = false;
	}
	return r;
}

static Decoded by_file(std::string_view in) {
	Decoded r{true, {}, 0};
	const int fd = fuzz_fd(in);
	try {
		LZ4DecompressFile d(fd, -1, -1);
		for (auto it = d.begin(); it; ++it) { r.bytes += *it; }
		r.digest = d.get_digest();
	} catch (const LZ4Exception&) {
		r.ok = false;
	}
	return r;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	if (size < 3 || size > (1u << 16)) { return 0; }
	const unsigned mode = data[0] & 3, a = data[1], b = data[2];
	const std::string_view rest(reinterpret_cast<const char*>(data) + 3, size - 3);

	switch (mode) {
		case 0: {
			const Decoded x = by_data(rest), y = by_file(rest);
			FUZZ_CHECK(x.ok == y.ok);
			if (x.ok) { FUZZ_CHECK(x.bytes == y.bytes && x.digest == y.digest); }
			break;
		}
		case 1: {
			std::string out(a * 256u + b, '\0');
			(void)decompress_lz4_block(rest, out.data(), out.size());
			break;
		}
		default: {
			std::string packed = compress_lz4(rest);
			if (mode == 2) {
				FUZZ_CHECK(decompress_lz4(packed) == rest);
				break;
			}
			if (!packed.empty()) { packed[a % std::min<size_t>(packed.size(), 64)] ^= static_cast<char>(b ? b : 1); }
			const Decoded x = by_data(packed), y = by_file(packed);
			FUZZ_CHECK(x.ok == y.ok);
			if (x.ok) { FUZZ_CHECK(x.bytes == y.bytes); }
		}
	}
	return 0;
}
