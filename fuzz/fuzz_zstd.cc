// libFuzzer target for the zstd decoders.
//
// Input: [u8 mode][u8 a][u8 b] then bytes.
//   - mode&3 == 0: the bytes as a frame through decompress_zstd, bounded by a*256+b and unbounded.
//     Each decodes or throws ZstdException (the bounded form throws ZstdCorruptVolume over the
//     bound); when both decode they agree.
//   - mode&3 == 1: the same frame into a buffer of a*256+b bytes (never out of bounds).
//   - mode&3 == 2: the bytes compressed and decompressed come back equal.
//   - mode&3 == 3: the same, with one byte of the frame XORed (a picks the position, b the mask):
//     it decodes or throws ZstdException.
#include "compressor_zstd.h"

#include "fuzz_util.h"

#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	// A frame header may claim up to 32768 bytes per input byte, so the input stays small.
	if (size < 3 || size > 2048) { return 0; }
	const unsigned mode = data[0] & 3, a = data[1], b = data[2];
	const std::string_view rest(reinterpret_cast<const char*>(data) + 3, size - 3);

	switch (mode) {
		case 0: {
			std::string x, y;
			bool xok = true, yok = true;
			try { x = decompress_zstd(rest, a * 256u + b); } catch (const ZstdException&) { xok = false; }
			try { y = decompress_zstd(rest); } catch (const ZstdException&) { yok = false; }
			if (xok) { FUZZ_CHECK(yok && x == y && x.size() <= a * 256u + b); }
			break;
		}
		case 1: {
			std::string out(a * 256u + b, '\0');
			(void)decompress_zstd_into(rest, out.data(), out.size());
			break;
		}
		default: {
			std::string packed = compress_zstd(rest);
			if (mode == 2) {
				FUZZ_CHECK(decompress_zstd(packed) == rest);
				break;
			}
			packed[a % packed.size()] ^= static_cast<char>(b ? b : 1);
			try { (void)decompress_zstd(packed); } catch (const ZstdException&) { }
		}
	}
	return 0;
}
