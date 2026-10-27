/*
 * roundtrip: a runnable, self-checking example for the compressors library.
 *
 * It mirrors the compressor usage that Xapiand and the storage library rely on:
 *   - one-shot helpers for stored records and WAL payloads,
 *   - block-iterator streaming for response compression,
 *   - codec dispatch with LZ4 as the default and explicit zstd / deflate paths.
 *
 * It doubles as a REGRESSION TEST: every step asserts through CHECK(), and main()
 * returns non-zero on any failure, so CMake registers it with ctest.
 */

#include "compressor_deflate.h"
#include "compressor_lz4.h"
#include "compressor_zstd.h"

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)


enum class Codec {
	lz4,
	zstd,
	deflate,
};


constexpr Codec DEFAULT_CODEC = Codec::lz4;


static const char*
codec_name(Codec codec)
{
	switch (codec) {
		case Codec::zstd:    return "zstd";
		case Codec::deflate: return "deflate";
		case Codec::lz4:
		default:             return "lz4";
	}
}


template <typename Stream>
static std::string
collect_blocks(Stream& stream)
{
	std::string out;
	for (auto it = stream.begin(); it; ++it) {
		out.append(*it);
	}
	return out;
}


static std::string
compress_one_shot(Codec codec, std::string_view input)
{
	switch (codec) {
		case Codec::zstd:    return compress_zstd(input);
		case Codec::deflate: return compress_deflate(input);
		case Codec::lz4:
		default:             return compress_lz4(input);
	}
}


static std::string
decompress_one_shot(Codec codec, std::string_view input)
{
	switch (codec) {
		case Codec::zstd:    return decompress_zstd(input);
		case Codec::deflate: return decompress_deflate(input);
		case Codec::lz4:
		default:             return decompress_lz4(input);
	}
}


static std::string
compress_streaming(Codec codec, std::string_view input)
{
	switch (codec) {
		case Codec::zstd: {
			ZstdCompressData compressor(input.data(), input.size());
			return collect_blocks(compressor);
		}
		case Codec::deflate: {
			DeflateCompressData compressor(input.data(), input.size());
			return collect_blocks(compressor);
		}
		case Codec::lz4:
		default: {
			LZ4CompressData compressor(input.data(), input.size());
			return collect_blocks(compressor);
		}
	}
}


static std::string
decompress_streaming(Codec codec, std::string_view input)
{
	switch (codec) {
		case Codec::zstd: {
			ZstdDecompressData decompressor(input.data(), input.size());
			return collect_blocks(decompressor);
		}
		case Codec::deflate: {
			DeflateDecompressData decompressor(input.data(), input.size());
			return collect_blocks(decompressor);
		}
		case Codec::lz4:
		default: {
			LZ4DecompressData decompressor(input.data(), input.size());
			return collect_blocks(decompressor);
		}
	}
}


static std::string
realistic_payload()
{
	std::string payload;
	payload.reserve(256 * 1024);

	for (int i = 0; i < 4096; ++i) {
		payload += "wal:";
		payload += std::to_string(i);
		payload += " op=put index=products shard=";
		payload += std::to_string(i % 16);
		payload += " field=title term=compressor value=\"the quick brown fox jumps over the lazy compressor\"";
		payload += " body={\"sku\":\"";
		payload += std::to_string(i % 128);
		payload += "\",\"category\":\"storage\",\"text\":\"compression keeps hot records small\"}\n";
	}

	return payload;
}


static std::vector<std::pair<const char*, std::string>>
edge_inputs()
{
	std::vector<std::pair<const char*, std::string>> inputs;
	inputs.emplace_back("empty", std::string());
	inputs.emplace_back("one-byte", std::string(1, 'x'));
	inputs.emplace_back("binary-nul", std::string("xapiand\0compressors\0edge", 24));
	inputs.emplace_back("lz4-block-exact", std::string(LZ4_BLOCK_SIZE, 'l'));
	inputs.emplace_back("lz4-block-plus-one", std::string(LZ4_BLOCK_SIZE + 1, 'm'));
	inputs.emplace_back("deflate-block-plus-one", std::string(DEFLATE_BLOCK_SIZE + 1, 'd'));
	return inputs;
}


static void
check_round_trip(Codec codec, const char* label, const std::string& original)
{
	std::string one_shot = compress_one_shot(codec, original);
	std::string one_shot_restored = decompress_one_shot(codec, one_shot);
	CHECK(one_shot_restored == original);
	if (codec == Codec::deflate) CHECK(decompress_deflate(one_shot, original.size()) == original);

	std::string streamed = compress_streaming(codec, original);
	std::string streamed_restored = decompress_streaming(codec, streamed);
	CHECK(streamed_restored == original);

	CHECK(decompress_streaming(codec, one_shot) == original);
	CHECK(decompress_one_shot(codec, streamed) == original);

	if (codec == Codec::lz4) {
		CHECK(streamed == one_shot);
	}

	std::printf("  %-8s %-22s %8zu -> %8zu one-shot, %8zu streaming\n",
		codec_name(codec), label, original.size(), one_shot.size(), streamed.size());
}


static void
test_codec(Codec codec)
{
	std::string payload = realistic_payload();
	std::string one_shot = compress_one_shot(codec, payload);
	std::string streamed = compress_streaming(codec, payload);

	CHECK(!one_shot.empty());
	CHECK(!streamed.empty());
	CHECK(one_shot.size() < payload.size());
	CHECK(streamed.size() < payload.size());

	check_round_trip(codec, "realistic-records", payload);
	for (const auto& [label, input] : edge_inputs()) {
		check_round_trip(codec, label, input);
	}

	std::printf("%s OK: one-shot, streaming, cross-API round-trips passed\n\n", codec_name(codec));
}


static void
test_default_dispatch()
{
	std::string payload = realistic_payload();
	std::string dispatched = compress_one_shot(DEFAULT_CODEC, payload);
	std::string direct_lz4 = compress_lz4(payload);

	CHECK(dispatched == direct_lz4);
	CHECK(decompress_one_shot(DEFAULT_CODEC, dispatched) == payload);
	std::printf("dispatch OK: default codec is LZ4\n\n");
}


int
main()
{
	try {
		std::printf("compressors round-trip example\n\n");

		test_default_dispatch();
		test_codec(Codec::lz4);
		test_codec(Codec::zstd);
		test_codec(Codec::deflate);

		if (failures == 0) {
			std::puts("compressors_roundtrip: all checks passed");
		}
	} catch (const std::exception& exc) {
		std::printf("FAIL %s:%d: unexpected exception: %s\n", __FILE__, __LINE__, exc.what());
		++failures;
	}

	return failures == 0 ? 0 : 1;
}
