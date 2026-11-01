// The one-shot zstd and deflate functions against the stream classes they bypass. The classes are
// the reference and are unchanged: compress_zstd / compress_deflate must write the same bytes,
// decompress_zstd / decompress_deflate must return the same bytes or fail the same way (the same
// exception type and message) on damaged input, and the one-shots must not pay for per-call
// scratch (a second copy of the result, a zero-filled block, a context per call).
//
// CHECK aborts on failure even when NDEBUG removes assert().
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if COMPRESSORS_TEST_DEFLATE
#include "compressor_deflate.h"
#endif
#if COMPRESSORS_TEST_ZSTD
#include "compressor_zstd.h"
#endif

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

// Counts C++ allocations (not the codecs' own malloc) so a call's allocation budget can be checked.
// TSan's runtime defines operator new itself, so the budget checks are skipped under it.
#if defined(__SANITIZE_THREAD__)
#  define ONESHOT_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define ONESHOT_TSAN 1
#  endif
#endif
#ifndef ONESHOT_TSAN
static std::atomic<std::size_t> g_allocs{0};
static std::atomic<std::size_t> g_bytes{0};
void* operator new(std::size_t n) {
	g_allocs.fetch_add(1, std::memory_order_relaxed);
	g_bytes.fetch_add(n, std::memory_order_relaxed);
	if (void* p = std::malloc(n ? n : 1)) { return p; }
	throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

static std::mt19937_64 rng(2026);

static std::string random_bytes(std::size_t n) {
	std::string s(n, '\0');
	for (auto& c : s) { c = static_cast<char>(rng()); }
	return s;
}

static std::string text(std::size_t n) {
	static const char* const words[] = {"alpha ", "beta ", "gamma ", "delta ", "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog\n", "0123456789 ", "storage ", "record "};
	std::string s;
	while (s.size() < n) { s += words[rng() % 15]; if (rng() % 7 == 0) { s += std::to_string(rng() % 100000); } }
	s.resize(n);
	return s;
}

static std::string mixed(std::size_t n) {
	std::string s;
	while (s.size() < n) {
		const std::size_t part = 1 + rng() % 20000;
		switch (rng() % 3) {
			case 0: s += random_bytes(part); break;
			case 1: s += text(part); break;
			default: s += std::string(part, static_cast<char>(rng())); break;
		}
	}
	s.resize(n);
	return s;
}

// What a decoder did with an input: returned bytes (kind 0), or threw the codec's base error (1)
// or its corrupt-volume error (2), with the message up to the "(in function at file:line)" suffix,
// which names the throwing function.
struct Verdict {
	int kind = 0;
	std::string what;
	std::string bytes;
	bool operator==(const Verdict& o) const { return kind == o.kind && what == o.what && bytes == o.bytes; }
};

static std::string message(const char* w) {
	std::string s = w;
	const auto at = s.find(" (in ");
	return at == std::string::npos ? s : s.substr(0, at);
}

template <class Codec>
static std::string via_stream(const std::string& s) {
	std::string out;
	Codec c(s.data(), s.size());
	for (auto it = c.begin(); it; ++it) { out.append(*it); }
	return out;
}

static std::string slice(const std::string& s, std::size_t at, std::size_t n) { return s.substr(at, n); }

// Damaged variants of a valid stream: truncated, bit flips, random bytes, trailing garbage, another
// stream appended, the first bytes only.
static std::string damage(const std::string& good, int kind) {
	std::string c = good;
	if (c.empty()) { return c; }
	switch (kind % 7) {
		case 0: c.resize(rng() % c.size()); break;
		case 1: c[rng() % c.size()] ^= static_cast<char>(1 + rng() % 255); break;
		case 2: for (int k = 0; k < 8; ++k) { c[rng() % c.size()] = static_cast<char>(rng()); } break;
		case 3: c += random_bytes(1 + rng() % 40); break;
		case 4: c += good; break;
		case 5: c = slice(c, 0, 1 + rng() % std::min<std::size_t>(c.size(), 24)); break;
		default: for (std::size_t i = 0; i < c.size(); i += 1 + rng() % 8) { if (rng() % 4 == 0) { c[i] ^= static_cast<char>(1 << (rng() % 8)); } } break;
	}
	return c;
}

#ifdef ONESHOT_TSAN
#  define BUDGET(label, allocs_max, bytes_max, expr) do { (void)(label); (void)(allocs_max); (void)(bytes_max); (void)(expr); } while (0)
#else
// Runs expr once to warm the thread's scratch, then again with the allocations counted. A call over
// budget is reported and counted, so one run names every function that is over.
static int g_budget_failures = 0;
#  define BUDGET(label, allocs_max, bytes_max, expr) do { \
	{ auto warm_ = (expr); (void)warm_; } \
	g_allocs = 0; g_bytes = 0; \
	{ auto got_ = (expr); (void)got_; \
	  const std::size_t a_ = g_allocs, b_ = g_bytes; \
	  if (a_ > (allocs_max) || b_ > (bytes_max)) { \
		std::fprintf(stderr, "%s:%d: %s allocated %zu times, %zu bytes (budget %zu times, %zu bytes)\n", __FILE__, __LINE__, label, a_, b_, std::size_t(allocs_max), std::size_t(bytes_max)); \
		++g_budget_failures; } } \
} while (0)
#endif

static const std::vector<std::size_t> sizes = {0, 1, 2, 3, 15, 16, 17, 100, 255, 256, 257, 1000, 4095, 4096, 4097, 16383, 16384, 16385,
	32767, 32768, 32769, 65535, 65536, 65537, 131072, 262144, 262145, 1 << 20, (1 << 20) + 7, 3 * (1 << 20) + 11};

static std::vector<std::string> make_inputs(std::size_t n) {
	return {random_bytes(n), text(n), std::string(n, 'a'), mixed(n)};
}


#if COMPRESSORS_TEST_ZSTD

static Verdict zstd_ref_decompress(const std::string& s) {
	Verdict v;
	try {
		v.bytes = via_stream<ZstdDecompressData>(s);
	} catch (const ZstdCorruptVolume& e) {
		v.kind = 2; v.what = message(e.what());
	} catch (const ZstdException& e) {
		v.kind = 1; v.what = message(e.what());
	}
	return v;
}

static Verdict zstd_decompress(const std::string& s) {
	Verdict v;
	try {
		v.bytes = decompress_zstd(s);
	} catch (const ZstdCorruptVolume& e) {
		v.kind = 2; v.what = message(e.what());
	} catch (const ZstdException& e) {
		v.kind = 1; v.what = message(e.what());
	}
	return v;
}

// decompress_zstd(compressed, max_size) as it was written before it reused the thread's context.
static std::string zstd_old_bounded(std::string_view compressed, std::size_t max_size) {
	unsigned long long const content_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
	if (content_size == ZSTD_CONTENTSIZE_ERROR || content_size == ZSTD_CONTENTSIZE_UNKNOWN || content_size > max_size ||
	    content_size / 32768 > compressed.size()) {
		THROW(ZstdCorruptVolume, "zstd frame is invalid or larger than allowed");
	}
	std::string result(static_cast<size_t>(content_size), '\0');
	size_t dec_size = ZSTD_decompress(result.data(), result.size(), compressed.data(), compressed.size());
	if (ZSTD_isError(dec_size) != 0u || dec_size != result.size()) {
		THROW(ZstdCorruptVolume, "zstd frame is corrupt");
	}
	return result;
}

template <class F>
static Verdict zstd_verdict(F&& f) {
	Verdict v;
	try {
		v.bytes = f();
	} catch (const ZstdCorruptVolume& e) {
		v.kind = 2; v.what = message(e.what());
	} catch (const ZstdException& e) {
		v.kind = 1; v.what = message(e.what());
	}
	return v;
}

static void check_zstd_valid(const std::string& in) {
	// Compressed bytes: the stream class, and ZSTD_compress on a fresh context, which the thread's
	// reused context must not change whatever it compressed before.
	const std::string expect = via_stream<ZstdCompressData>(in);
	const std::string got = compress_zstd(in);
	CHECK(got == expect);
	std::string direct(ZSTD_compressBound(in.size()), '\0');
	const std::size_t dn = ZSTD_compress(direct.data(), direct.size(), in.data(), in.size(), ZSTD_COMPRESS_LEVEL);
	CHECK(!ZSTD_isError(dn) && std::string_view(direct.data(), dn) == got);

	CHECK(decompress_zstd(got) == in);
	CHECK(decompress_zstd(got, in.size()) == in);
	CHECK(zstd_ref_decompress(got) == zstd_decompress(got));
	if (!in.empty()) {
		CHECK(zstd_verdict([&] { return decompress_zstd(got, in.size() - 1); }) == zstd_verdict([&] { return zstd_old_bounded(got, in.size() - 1); }));
		CHECK(zstd_verdict([&] { return decompress_zstd(got, in.size() - 1); }).kind == 2);
	}
}

static void check_zstd_damaged(const std::string& in) {
	const std::string good = via_stream<ZstdCompressData>(in);
	for (int k = 0; k < 14; ++k) {
		const std::string bad = damage(good, k);
		// A damaged header can claim gigabytes; the stream class would allocate them too, so skip those.
		const auto claim = ZSTD_getFrameContentSize(bad.data(), bad.size());
		if (claim != ZSTD_CONTENTSIZE_ERROR && claim != ZSTD_CONTENTSIZE_UNKNOWN && claim > (16u << 20)) { continue; }
		const Verdict ref = zstd_ref_decompress(bad);
		const Verdict got = zstd_decompress(bad);
		if (!(ref == got)) {
			std::fprintf(stderr, "zstd verdict differs: input %zu B, damage %d: ref kind %d '%s' %zu B, got kind %d '%s' %zu B\n", in.size(), k, ref.kind, ref.what.c_str(), ref.bytes.size(), got.kind, got.what.c_str(), got.bytes.size());
			std::abort();
		}
		for (std::size_t max : {std::size_t(0), in.size(), in.size() * 2 + 1, std::size_t(1) << 20}) {
			CHECK(zstd_verdict([&] { return decompress_zstd(bad, max); }) == zstd_verdict([&] { return zstd_old_bounded(bad, max); }));
		}
		// The thread's context survives a failure: a valid frame still decodes right after.
		CHECK(decompress_zstd(good) == in);
	}
}

static void check_zstd_allocations() {
#ifndef ONESHOT_TSAN
	// One allocation, the result, whatever the size: no bound-sized zero-filled string and no
	// second copy through the block iterator.
	for (std::size_t n : {std::size_t(100), std::size_t(1000), std::size_t(4096), std::size_t(60000), std::size_t(300000)}) {
		for (const std::string& in : {random_bytes(n), text(n)}) {
			const std::string packed = compress_zstd(in);
			const std::size_t bound = ZSTD_compressBound(n);
			if (bound <= (1u << 20)) {
				BUDGET("compress_zstd", 1, packed.size() + 64, compress_zstd(in));
			}
			BUDGET("decompress_zstd", 1, n + 64, decompress_zstd(packed));
			BUDGET("decompress_zstd bounded", 1, n + 64, decompress_zstd(packed, n));
			const std::string one = compress_zstd(in);
			CHECK(one.capacity() <= one.size() + 64);
		}
	}
#endif
}

static void check_zstd() {
	for (std::size_t n : sizes) {
		for (const std::string& in : make_inputs(n)) { check_zstd_valid(in); }
	}
	// The context is reused: sizes in shuffled order, so a large frame is followed by a small one.
	std::vector<std::string> pool;
	for (std::size_t n : {std::size_t(0), std::size_t(7), std::size_t(300), std::size_t(5000), std::size_t(70000), std::size_t(500000)}) {
		for (auto& s : make_inputs(n)) { pool.push_back(std::move(s)); }
	}
	for (int i = 0; i < 300; ++i) {
		const std::string& in = pool[rng() % pool.size()];
		std::string direct(ZSTD_compressBound(in.size()), '\0');
		const std::size_t dn = ZSTD_compress(direct.data(), direct.size(), in.data(), in.size(), ZSTD_COMPRESS_LEVEL);
		CHECK(compress_zstd(in) == std::string_view(direct.data(), dn));
	}
	for (int i = 0; i < 600; ++i) {
		const std::size_t n = i % 3 == 0 ? rng() % 60000 : rng() % 3000;
		check_zstd_damaged(i % 2 ? text(n) : mixed(n));
	}
	// Edge inputs.
	CHECK(zstd_ref_decompress("") == zstd_decompress(""));
	CHECK(zstd_decompress("").kind == 2);
	CHECK(zstd_ref_decompress("not a frame at all") == zstd_decompress("not a frame at all"));
	check_zstd_allocations();
}

#endif  // COMPRESSORS_TEST_ZSTD


#if COMPRESSORS_TEST_DEFLATE

static Verdict deflate_ref_decompress(const std::string& s) {
	Verdict v;
	try {
		v.bytes = via_stream<DeflateDecompressData>(s);
	} catch (const DeflateException& e) {
		v.kind = 1; v.what = message(e.what());
	}
	return v;
}

static Verdict deflate_decompress(const std::string& s) {
	Verdict v;
	try {
		v.bytes = decompress_deflate(s);
	} catch (const DeflateException& e) {
		v.kind = 1; v.what = message(e.what());
	}
	return v;
}

static void check_deflate_valid(const std::string& in) {
	const std::string packed = compress_deflate(in);
	CHECK(packed == via_stream<DeflateCompressData>(in));
	CHECK(decompress_deflate(packed) == in);
	CHECK(deflate_ref_decompress(packed) == deflate_decompress(packed));
}

static void check_deflate_damaged(const std::string& in) {
	const std::string good = compress_deflate(in);
	for (int k = 0; k < 14; ++k) {
		const std::string bad = damage(good, k);
		const Verdict ref = deflate_ref_decompress(bad);
		const Verdict got = deflate_decompress(bad);
		if (!(ref == got)) {
			std::fprintf(stderr, "deflate verdict differs: input %zu B, damage %d: ref kind %d '%s' %zu B, got kind %d '%s' %zu B\n", in.size(), k, ref.kind, ref.what.c_str(), ref.bytes.size(), got.kind, got.what.c_str(), got.bytes.size());
			std::abort();
		}
		CHECK(decompress_deflate(good) == in);
	}
}

static void check_deflate_allocations() {
#ifndef ONESHOT_TSAN
	// Output under one block: one allocation, the result (none when it fits the string itself).
	for (std::size_t n : {std::size_t(100), std::size_t(1000), std::size_t(4096), std::size_t(16000)}) {
		for (const std::string& in : {random_bytes(n), text(n)}) {
			const std::string packed = compress_deflate(in);
			BUDGET("decompress_deflate", 1, n + 64, decompress_deflate(packed));
		}
	}
	// Longer output: the result sized from the input and doubled, not a string per block.
	for (std::size_t n : {std::size_t(100000), std::size_t(1) << 20}) {
		const std::string in = random_bytes(n);
		const std::string packed = compress_deflate(in);
		BUDGET("decompress_deflate (stored)", 1, packed.size() + (64u << 10), decompress_deflate(packed));
	}
#endif
}

static void check_deflate() {
	for (std::size_t n : sizes) {
		for (const std::string& in : make_inputs(n)) { check_deflate_valid(in); }
	}
	for (int i = 0; i < 600; ++i) {
		const std::size_t n = i % 3 == 0 ? rng() % 90000 : rng() % 3000;
		check_deflate_damaged(i % 2 ? text(n) : mixed(n));
	}
	// Edge inputs: empty (no stream, so no output and no error) and not a stream at all.
	CHECK(deflate_ref_decompress("") == deflate_decompress(""));
	CHECK(deflate_decompress("").kind == 0 && deflate_decompress("").bytes.empty());
	CHECK(deflate_ref_decompress("not a stream at all") == deflate_decompress("not a stream at all"));
	CHECK(deflate_decompress("not a stream at all").kind == 1);
	// A stream followed by anything is read up to its end.
	const std::string in = text(50000);
	const std::string packed = compress_deflate(in);
	CHECK(decompress_deflate(packed + "trailing bytes") == in);
	CHECK(decompress_deflate(packed + packed) == in);
	check_deflate_allocations();
}

#endif  // COMPRESSORS_TEST_DEFLATE


// The same inputs from several threads at once: each thread has its own contexts and scratch.
static void check_threads() {
	std::vector<std::thread> threads;
	std::atomic<int> failures{0};
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([t, &failures] {
			std::mt19937_64 local(100 + t);
			for (int i = 0; i < 60; ++i) {
				std::string in(local() % 40000, '\0');
				for (auto& c : in) { c = static_cast<char>('a' + local() % (t + 2)); }
#if COMPRESSORS_TEST_ZSTD
				if (decompress_zstd(compress_zstd(in)) != in) { ++failures; }
#endif
#if COMPRESSORS_TEST_DEFLATE
				if (decompress_deflate(compress_deflate(in)) != in) { ++failures; }
#endif
			}
		});
	}
	for (auto& t : threads) { t.join(); }
	CHECK(failures == 0);
}


int main() {
#if COMPRESSORS_TEST_ZSTD
	check_zstd();
#endif
#if COMPRESSORS_TEST_DEFLATE
	check_deflate();
#endif
	check_threads();
#ifndef ONESHOT_TSAN
	CHECK(g_budget_failures == 0);
#endif
	std::printf("all one-shot tests passed\n");
	return 0;
}
