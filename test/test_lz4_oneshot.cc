// compress_lz4 / decompress_lz4 against the stream classes they replace in the one-shot path:
// the compressed bytes are identical (the framing is an on-disk format), the decoded bytes
// are identical, and damaged input is accepted or rejected the same way. The classes are the
// reference implementation; they are unchanged.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <atomic>
#include <new>

#include "compressor_lz4.h"

// The largest single allocation since the counter was reset: decompress_lz4 must size its
// buffers by what the blocks decode to, not by what the headers claim. (TSan's runtime
// defines operator new itself, so the allocation checks are skipped under it.)
static std::atomic<std::size_t> g_largest_alloc{0};
#if defined(__SANITIZE_THREAD__)
#  define ONESHOT_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define ONESHOT_TSAN 1
#  endif
#endif
#ifndef ONESHOT_TSAN
static void note_alloc(std::size_t n) {
	std::size_t seen = g_largest_alloc.load(std::memory_order_relaxed);
	while (n > seen && !g_largest_alloc.compare_exchange_weak(seen, n, std::memory_order_relaxed)) { }
}
void* operator new(std::size_t n) {
	note_alloc(n);
	if (void* p = std::malloc(n ? n : 1)) { return p; }
	throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

static std::string ref_compress(const std::string& s) {
	std::string out;
	LZ4CompressData c(s.data(), s.size());
	for (auto it = c.begin(); it; ++it) { out.append(*it); }
	return out;
}

// Result of the reference decoder: (threw, bytes).
static std::pair<bool, std::string> ref_decompress(const std::string& s) {
	std::string out;
	try {
		LZ4DecompressData d(s.data(), s.size());
		for (auto it = d.begin(); it; ++it) { out.append(*it); }
	} catch (const LZ4Exception&) {
		return {true, {}};
	}
	return {false, out};
}

static std::pair<bool, std::string> new_decompress(const std::string& s) {
	try {
		return {false, decompress_lz4(s)};
	} catch (const LZ4Exception&) {
		return {true, {}};
	}
}

static std::mt19937_64 rng(12345);

static std::string random_bytes(std::size_t n) {
	std::string s(n, '\0');
	for (auto& c : s) { c = static_cast<char>(rng()); }
	return s;
}

// Text-like: words from a small vocabulary, so matches are near and far.
static std::string text(std::size_t n) {
	static const char* const words[] = {"alpha ", "beta ", "gamma ", "delta ", "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog\n", "0123456789 ", "storage ", "record "};
	std::string s;
	while (s.size() < n) { s += words[rng() % 15]; if (rng() % 7 == 0) { s += std::to_string(rng() % 100000); } }
	s.resize(n);
	return s;
}

// Matches at distances around the 64 KB window and the ring wrap.
static std::string far_repeats(std::size_t n) {
	std::string unit = random_bytes(70000);
	std::string s;
	while (s.size() < n) {
		std::size_t take = 1 + rng() % 70000;
		std::size_t at = rng() % (unit.size() - take + 1);
		s.append(unit, at, take);
		if (rng() % 3 == 0) { s += random_bytes(rng() % 200); }
	}
	s.resize(n);
	return s;
}

static std::string mixed(std::size_t n) {
	std::string s;
	while (s.size() < n) {
		std::size_t part = 1 + rng() % 40000;
		switch (rng() % 3) {
			case 0: s += random_bytes(part); break;
			case 1: s += text(part); break;
			default: s += std::string(part, static_cast<char>(rng())); break;
		}
	}
	s.resize(n);
	return s;
}


// XXH32 as specified, one shot and unvectorizable by construction (a plain loop): the reference
// for the streaming implementation at every length, seed and chunking.
static std::uint32_t rotl(std::uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
static std::uint32_t ref_xxh32(const unsigned char* p, std::size_t n, std::uint32_t seed) {
	const std::uint32_t P1 = 0x9E3779B1U, P2 = 0x85EBCA77U, P3 = 0xC2B2AE3DU, P4 = 0x27D4EB2FU, P5 = 0x165667B1U;
	auto rd = [](const unsigned char* q) { return std::uint32_t(q[0]) | std::uint32_t(q[1]) << 8 | std::uint32_t(q[2]) << 16 | std::uint32_t(q[3]) << 24; };
	const unsigned char* const end = p + n;
	std::uint32_t h;
	if (n >= 16) {
		std::uint32_t v[4] = {seed + P1 + P2, seed + P2, seed, seed - P1};
		do {
			for (auto& l : v) { l = rotl(l + rd(p) * P2, 13) * P1; p += 4; }
		} while (p + 16 <= end);
		h = rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18);
	} else {
		h = seed + P5;
	}
	h += static_cast<std::uint32_t>(n);
	for (; p + 4 <= end; p += 4) { h = rotl(h + rd(p) * P3, 17) * P4; }
	for (; p < end; ++p) { h = rotl(h + *p * P5, 11) * P1; }
	h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
	return h;
}

static void check_xxh32() {
	const unsigned char none = 0;
	CHECK(ref_xxh32(&none, 0, 0) == 0x02CC5D05U);  // published value for the empty input
	for (int i = 0; i < 3000; ++i) {
		const std::size_t n = i < 300 ? i : rng() % 20000;
		const std::string s = random_bytes(n);
		const std::uint32_t seed = i % 3 ? static_cast<std::uint32_t>(rng()) : 0;
		const auto* d = reinterpret_cast<const unsigned char*>(s.data());
		compressors::XXH32_state_t st;
		compressors::XXH32_reset(&st, seed);
		for (std::size_t o = 0; o < n;) {  // random chunking, including empty chunks
			const std::size_t c = std::min<std::size_t>(rng() % 70, n - o);
			compressors::XXH32_update(&st, d + o, c);
			o += c;
		}
		CHECK(compressors::XXH32_digest(&st) == ref_xxh32(d, n, seed));
	}
}

static void check_input(const std::string& in) {
	const std::string expect = ref_compress(in);
	const std::string got = compress_lz4(in);
	CHECK(got == expect);
	CHECK(decompress_lz4(got) == in);
	auto r = ref_decompress(expect);
	CHECK(!r.first && r.second == in);
}

int main() {
	check_xxh32();
	// Sizes around the block (2 KB), the window (64 KB) and the ring wrap (256 KB).
	std::vector<std::size_t> sizes = {0, 1, 2, 3, 11, 12, 13, 255, 256, 2047, 2048, 2049, 4096, 4097, 65535, 65536, 65537,
		131072, 262143, 262144, 262145, 262144 + 2048, 262144 + 2049, 264192, 300000, 524288, 524288 + 1, 524288 + 2048,
		786432 + 17, 1 << 20, (1 << 20) + 12345, 3 * 262144 + 100};
	for (std::size_t n : sizes) {
		check_input(random_bytes(n));
		check_input(text(n));
		check_input(std::string(n, 'a'));
		check_input(far_repeats(n));
		check_input(mixed(n));
	}
	for (int i = 0; i < 60; ++i) {
		const std::size_t n = rng() % 900000;
		check_input(i % 3 == 0 ? mixed(n) : i % 3 == 1 ? far_repeats(n) : text(n));
	}

	// Decoding damaged streams: same verdict and same bytes as the reference.
	for (int i = 0; i < 4000; ++i) {
		std::string in = i % 2 ? text(rng() % 20000) : mixed(rng() % 600000);
		std::string c = ref_compress(in);
		if (c.empty()) { continue; }
		switch (i % 5) {
			case 0: c.resize(rng() % c.size()); break;                         // truncate
			case 1: c[rng() % c.size()] ^= static_cast<char>(1 + rng() % 255); break;  // flip bits
			case 2: for (int k = 0; k < 8; ++k) { c[rng() % c.size()] = static_cast<char>(rng()); } break;
			case 3: c += random_bytes(1 + rng() % 5); break;                   // trailing garbage
			default: c.erase(rng() % c.size(), 1 + rng() % 4); break;          // lose bytes
		}
		auto a = ref_decompress(c);
		auto b = new_decompress(c);
		CHECK(a.first == b.first);
		CHECK(a.second == b.second);
	}
	for (int i = 0; i < 2000; ++i) {  // arbitrary bytes
		std::string c = random_bytes(rng() % 300);
		auto a = ref_decompress(c);
		auto b = new_decompress(c);
		CHECK(a.first == b.first && a.second == b.second);
	}

	// Untrusted headers do not size allocations: 32768 three-byte blocks (98 KB) stay under 1 MB.
	// Oracle: the stream class decodes the same input (to the same bytes, or the same rejection);
	// the allocation bound is the decoded size plus constant buffers, whatever the block count.
	{
		std::string valid;   // each block: header 1, token 0x10 (one literal) 'a'+i: decodes to one byte
		std::string expect_bytes;
		for (int i = 0; i < 32768; ++i) {
			valid.push_back(2); valid.push_back(0);
			valid.push_back('\x10'); valid.push_back(static_cast<char>('a' + i % 26));
			expect_bytes.push_back(static_cast<char>('a' + i % 26));
		}
		std::string zero_len;   // block of one zero byte: decodes to nothing, rejected
		for (int i = 0; i < 32768; ++i) { zero_len.push_back(1); zero_len.push_back(0); zero_len.push_back(0); }
		std::string torn = valid;   // valid blocks, then a header claiming more than is left
		torn.push_back(static_cast<char>(0xff)); torn.push_back(static_cast<char>(0x7f));

		for (const std::string* hostile : {&valid, &zero_len, &torn}) {
			g_largest_alloc.store(0);
			auto b = new_decompress(*hostile);
#ifndef ONESHOT_TSAN
			CHECK(g_largest_alloc.load() <= (1u << 20));
#endif
			auto a = ref_decompress(*hostile);
			CHECK(a.first == b.first && a.second == b.second);
		}
		CHECK(new_decompress(valid).second == expect_bytes);
		CHECK(!new_decompress(valid).first && new_decompress(zero_len).first && new_decompress(torn).first);
	}

	// A large valid stream (70 MB): same bytes in and out, the allocations growing with the data.
	{
		std::string big = text(70u << 20);
		std::string c = compress_lz4(big);
		g_largest_alloc.store(0);
		CHECK(decompress_lz4(c) == big);
#ifndef ONESHOT_TSAN
		CHECK(g_largest_alloc.load() <= 2 * big.size() + (1u << 20));
#endif
	}

	// Threads share nothing: concurrent calls give the same bytes.
	std::string in = mixed(500000);
	const std::string expect = ref_compress(in);
	std::vector<std::thread> ts;
	for (int t = 0; t < 4; ++t) {
		ts.emplace_back([&] { for (int k = 0; k < 20; ++k) { CHECK(compress_lz4(in) == expect); CHECK(decompress_lz4(expect) == in); } });
	}
	for (auto& t : ts) { t.join(); }
	std::puts("lz4 one-shot: OK");
}
