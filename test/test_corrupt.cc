// Damaged compressed input: every decoder either decodes it or throws its library's error,
// and never reads or writes out of bounds (run it under ASan and UBSan).
//
// CHECK aborts on failure even when NDEBUG removes assert().
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <string>
#include <unistd.h>

#if COMPRESSORS_TEST_DEFLATE
#include "compressor_deflate.h"
#endif
#if COMPRESSORS_TEST_LZ4
#include "compressor_lz4.h"
#endif
#if COMPRESSORS_TEST_ZSTD
#include "compressor_zstd.h"
#endif

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

// Runs `f` and reports whether it threw E (and nothing else).
template <typename E, typename F>
static bool throws(F&& f) {
	try { f(); } catch (const E&) { return true; }
	return false;
}

static std::string sample(std::size_t n, unsigned seed) {
	std::mt19937 rng(seed);
	std::string s;
	while (s.size() < n) {
		s += (rng() % 3) ? std::string("the quick brown fox jumps over the lazy dog. ") : std::to_string(rng());
	}
	s.resize(n);
	return s;
}

static std::string write_temp_file(const std::string& bytes) {
	static unsigned counter = 0;
	std::string path;
	int fd = -1;
	do {
		path = "compressors_corrupt_" + std::to_string(static_cast<long long>(::getpid())) + "_" + std::to_string(counter++);
		fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	} while (fd == -1 && errno == EEXIST);
	CHECK(fd != -1);
	CHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
	::close(fd);
	return path;
}

#if COMPRESSORS_TEST_LZ4
static std::string lz4_record(std::uint16_t cmp_bytes, std::size_t body) {
	std::string s(reinterpret_cast<const char*>(&cmp_bytes), sizeof(cmp_bytes));
	return s + sample(body, 7);
}

static void lz4_decode_data(const std::string& s) {
	LZ4DecompressData d(s.data(), s.size());
	for (auto it = d.begin(); it; ++it) { }
}

static void lz4_decode_file(const std::string& s) {
	const std::string path = write_temp_file(s);
	try {
		LZ4DecompressFile d{std::string_view(path)};
		for (auto it = d.begin(); it; ++it) { }
	} catch (...) {
		::unlink(path.c_str());
		throw;
	}
	::unlink(path.c_str());
}

static void test_lz4_lengths() {
	constexpr std::size_t bound = LZ4_COMPRESSBOUND(LZ4_BLOCK_SIZE);
	for (auto decode : {lz4_decode_data, lz4_decode_file}) {
		// A record longer than any block can compress to overflowed the compressed-block buffer.
		for (std::size_t n : {bound + 1, bound + 2, std::size_t(4096), std::size_t(30000), std::size_t(65535)}) {
			CHECK(throws<LZ4CorruptVolume>([&] { decode(lz4_record(static_cast<std::uint16_t>(n), n)); }));
		}
		// A zero length and lengths the data does not hold.
		CHECK(throws<LZ4CorruptVolume>([&] { decode(lz4_record(0, 100)); }));
		CHECK(throws<LZ4CorruptVolume>([&] { decode(lz4_record(65535, 100)); }));
		CHECK(throws<LZ4CorruptVolume>([&] { decode(std::string("\x01", 1)); }));
		// The largest legal length with bytes that are not LZ4: an LZ4 error, not a crash.
		CHECK(throws<LZ4Exception>([&] { decode(lz4_record(static_cast<std::uint16_t>(bound), bound)); }));
	}
	// A bad length after good records, with the next record straddling a file read.
	const std::string good = compress_lz4(sample(100000, 3));
	for (auto decode : {lz4_decode_data, lz4_decode_file}) {
		CHECK(throws<LZ4CorruptVolume>([&] { decode(good + lz4_record(65535, 65535)); }));
		CHECK(throws<LZ4CorruptVolume>([&] { decode(good + lz4_record(static_cast<std::uint16_t>(bound + 1), bound + 1)); }));
	}
}

// Every single-bit flip in the head of a multi-block stream decodes or throws an LZ4Exception.
static void test_lz4_bitflips() {
	const std::string good = compress_lz4(sample(20000, 5));
	for (auto decode : {lz4_decode_data, lz4_decode_file}) {
		std::size_t threw = 0;
		for (std::size_t at = 0; at < 400; ++at) {
			for (int bit = 0; bit < 8; ++bit) {
				std::string bad = good;
				bad[at] = static_cast<char>(bad[at] ^ (1 << bit));
				try { decode(bad); } catch (const LZ4Exception&) { ++threw; }
			}
		}
		CHECK(threw > 0);
		for (std::size_t cut : {std::size_t(1), std::size_t(2), std::size_t(3), good.size() / 2, good.size() - 1}) {
			try { decode(good.substr(0, cut)); } catch (const LZ4Exception&) { }
		}
	}
}
#endif

#if COMPRESSORS_TEST_ZSTD
static void test_zstd_lengths() {
	// A frame header claiming 1 TiB of content in 13 bytes.
	std::string huge("\x28\xb5\x2f\xfd\xe0", 5);
	huge += std::string("\x00\x00\x00\x00\x00\x01\x00\x00", 8);
	CHECK(throws<ZstdCorruptVolume>([&] { (void)decompress_zstd(huge); }));
	CHECK(throws<ZstdCorruptVolume>([&] { (void)decompress_zstd(huge, std::size_t(1) << 20); }));
	CHECK(throws<ZstdCorruptVolume>([&] { (void)decompress_zstd(huge, SIZE_MAX); }));
	std::string unbounded("\x28\xb5\x2f\xfd\xe0", 5);
	unbounded += std::string(8, '\xff');
	CHECK(throws<ZstdCorruptVolume>([&] { (void)decompress_zstd(unbounded); }));
	CHECK(throws<ZstdCorruptVolume>([&] { (void)decompress_zstd(std::string()); }));
	CHECK(throws<ZstdCorruptVolume>([&] { (void)decompress_zstd(std::string("\x28\xb5\x2f", 3)); }));

	const std::string good = compress_zstd(sample(20000, 9));
	std::size_t threw = 0;
	for (std::size_t at = 0; at < good.size() && at < 300; ++at) {
		for (int bit = 0; bit < 8; ++bit) {
			std::string bad = good;
			bad[at] = static_cast<char>(bad[at] ^ (1 << bit));
			try { (void)decompress_zstd(bad); } catch (const ZstdException&) { ++threw; }
			char out[64];
			(void)decompress_zstd_into(bad, out, sizeof(out));
		}
	}
	CHECK(threw > 0);
	for (std::size_t cut = 0; cut < good.size(); cut += 7) {
		CHECK(throws<ZstdException>([&] { (void)decompress_zstd(good.substr(0, cut)); }));
	}
}
#endif

#if COMPRESSORS_TEST_DEFLATE
static void test_deflate_flips() {
	// A zlib header with the preset-dictionary flag: an error, not an empty result.
	CHECK(throws<DeflateException>([] { (void)decompress_deflate(std::string("\x78\xbb\x00\x00\x00\x01\x03\x00\x00\x00\x00\x01", 12)); }));
	const std::string good = compress_deflate(sample(20000, 11));
	std::size_t threw = 0;
	for (std::size_t at = 0; at < good.size() && at < 300; ++at) {
		for (int bit = 0; bit < 8; ++bit) {
			std::string bad = good;
			bad[at] = static_cast<char>(bad[at] ^ (1 << bit));
			try { (void)decompress_deflate(bad); } catch (const DeflateException&) { ++threw; }
		}
	}
	CHECK(threw > 0);
	for (std::size_t cut = 0; cut < good.size(); cut += 13) {
		try { (void)decompress_deflate(good.substr(0, cut)); } catch (const DeflateException&) { }
	}
}
#endif

int main() {
#if COMPRESSORS_TEST_LZ4
	test_lz4_lengths();
	test_lz4_bitflips();
	{
		std::string out(100, '\0');
		CHECK(!decompress_lz4_block(std::string(300, '\xff'), out.data(), out.size()));
		CHECK(!decompress_lz4_block(std::string(), out.data(), out.size()));
	}
#endif
#if COMPRESSORS_TEST_ZSTD
	test_zstd_lengths();
#endif
#if COMPRESSORS_TEST_DEFLATE
	test_deflate_flips();
#endif
	std::printf("compressors corrupt-input test passed\n");
	return 0;
}
