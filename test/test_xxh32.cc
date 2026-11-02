// XXH32 against its definition, on every byte order. The digest of a byte sequence is fixed by the
// algorithm (little-endian reads of the input), so it must not depend on the host: storage persists
// it. Checked here against published values, an independent one-shot reference written from the
// specification, and the kernel run on an emulated big-endian host, which loads four bytes the way
// a big-endian machine does and so exercises the byte swap on any host.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "xxh32_stream.h"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

namespace {

using compressors_xxhash_detail::EmulatedBigEndianHost;
using compressors_xxhash_detail::Kernel;
using compressors_xxhash_detail::NativeHost;

// Every kernel a host can run, as a template-argument loop: the library picks one per update, the
// tests run each of them on every input.
template <class Host, class F>
void for_each_kernel(F&& f) {
	f.template operator()<Host, Kernel::automatic>();
	f.template operator()<Host, Kernel::generic>();
	f.template operator()<Host, Kernel::start>();
}

// XXH32 from the specification: one shot, a plain loop, bytes assembled little-endian one at a time,
// so no host byte order and no vectorizable lane structure.
std::uint32_t rotl(std::uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

std::uint32_t reference(const unsigned char* p, std::size_t n, std::uint32_t seed) {
	const std::uint32_t P1 = 0x9E3779B1U, P2 = 0x85EBCA77U, P3 = 0xC2B2AE3DU, P4 = 0x27D4EB2FU, P5 = 0x165667B1U;
	auto rd = [](const unsigned char* q) { return std::uint32_t(q[0]) | std::uint32_t(q[1]) << 8 | std::uint32_t(q[2]) << 16 | std::uint32_t(q[3]) << 24; };
	const unsigned char* const end = p + n;
	std::uint32_t h;
	if (n >= 16) {
		std::uint32_t v[4] = {seed + P1 + P2, seed + P2, seed, seed - P1};
		do {
			for (auto& lane : v) { lane = rotl(lane + rd(p) * P2, 13) * P1; p += 4; }
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

// The digest of data fed in pieces of the given sizes (cycled), through the kernel on Host.
template <class Host, Kernel K = Kernel::automatic>
std::uint32_t streamed(const unsigned char* d, std::size_t n, std::uint32_t seed, const std::vector<std::size_t>& pieces) {
	compressors::XXH32_state_t st;
	compressors::XXH32_reset(&st, seed);
	std::size_t o = 0;
	for (std::size_t k = 0; o < n; ++k) {
		const std::size_t c = std::min(pieces[k % pieces.size()], n - o);
		compressors::XXH32_update_as<Host, K>(&st, d + o, c);
		o += c;
	}
	return compressors::XXH32_digest_as<Host>(&st);
}

template <class Host, Kernel K>
void check_vectors() {
	struct Vector { const char* text; std::uint32_t seed; std::uint32_t digest; };
	// Published values (the xxHash documentation and its sanity test): 0x02CC5D05 is XXH32 of the
	// empty input at seed 0.
	static const Vector vectors[] = {
		{"", 0, 0x02CC5D05U},
		{"", 0x9E3779B1U, 0x36B78AE7U},
		{"a", 0, 0x550D7456U},
		{"abc", 0, 0x32D153FFU},
		{"Nobody inspects the spammish repetition", 0, 0xE2293B2FU},
	};
	for (const auto& v : vectors) {
		const std::string s = v.text;
		const auto* d = reinterpret_cast<const unsigned char*>(s.data());
		CHECK(reference(d, s.size(), v.seed) == v.digest);
		CHECK((streamed<Host, K>(d, s.size(), v.seed, {1}) == v.digest));
		CHECK((streamed<Host, K>(d, s.size(), v.seed, {1000}) == v.digest));
	}
}

// Every length around the 4-byte and 16-byte steps, several seeds, several ways of cutting the input.
template <class Host, Kernel K>
void check_lengths(std::mt19937_64& rng) {
	std::string buf(700, '\0');
	for (auto& c : buf) { c = static_cast<char>(rng()); }
	const auto* d = reinterpret_cast<const unsigned char*>(buf.data());
	const std::vector<std::vector<std::size_t>> cuts = {{1000}, {1}, {3}, {7}, {15}, {16}, {17}, {64}, {5, 11, 16, 1, 0, 33}};
	for (std::size_t n = 0; n <= buf.size(); ++n) {
		for (std::uint32_t seed : {0U, 1U, 0x9E3779B1U, 0xFFFFFFFFU}) {
			const std::uint32_t want = reference(d, n, seed);
			for (const auto& cut : cuts) {
				CHECK((streamed<Host, K>(d, n, seed, cut) == want));
			}
		}
	}
}

template <class Host, Kernel K>
void check_random(std::mt19937_64& rng) {
	for (int i = 0; i < 400; ++i) {
		const std::size_t n = i < 40 ? 100000 + rng() % 1000 : rng() % 20000;
		std::string s(n, '\0');
		for (auto& c : s) { c = static_cast<char>(rng()); }
		const auto* d = reinterpret_cast<const unsigned char*>(s.data());
		const auto seed = static_cast<std::uint32_t>(rng());
		std::vector<std::size_t> cut;
		for (int k = 0; k < 8; ++k) { cut.push_back(rng() % 3000); }
		cut.push_back(1);
		CHECK((streamed<Host, K>(d, n, seed, cut) == reference(d, n, seed)));
	}
}

// The word at p, read as little-endian, whatever the host: this is the line that differs on a
// big-endian machine.
template <class Host>
void check_read32() {
	const unsigned char b[4] = {0x01, 0x02, 0x03, 0x04};
	CHECK(compressors_xxhash_detail::read32_le<Host>(b) == 0x04030201U);
	const unsigned char c[4] = {0xFF, 0x00, 0x80, 0x7F};
	CHECK(compressors_xxhash_detail::read32_le<Host>(c) == 0x7F8000FFU);
}

}  // namespace


int main() {
	static_assert(compressors_xxhash_detail::bswap32(0x01020304U) == 0x04030201U);
	CHECK(NativeHost::big_endian == (std::endian::native == std::endian::big));
	CHECK(EmulatedBigEndianHost::big_endian);

	// The two loads disagree on the same four bytes (unless the host is big-endian itself, where they agree).
	const unsigned char probe[4] = {0x01, 0x02, 0x03, 0x04};
	CHECK((NativeHost::load32(probe) == EmulatedBigEndianHost::load32(probe)) == NativeHost::big_endian);

	std::mt19937_64 rng(32);
	check_read32<NativeHost>();
	check_read32<EmulatedBigEndianHost>();
	const auto all = [&]<class Host>() {
		for_each_kernel<Host>([&]<class H, Kernel K>() {
			check_vectors<H, K>();
			check_lengths<H, K>(rng);
			check_random<H, K>(rng);
		});
	};
	all.template operator()<NativeHost>();
	all.template operator()<EmulatedBigEndianHost>();

	// The public entry points are the native kernel.
	for (int i = 0; i < 200; ++i) {
		std::string s(rng() % 5000, '\0');
		for (auto& c : s) { c = static_cast<char>(rng()); }
		const auto* d = reinterpret_cast<const unsigned char*>(s.data());
		compressors::XXH32_state_t st;
		compressors::XXH32_reset(&st, 7);
		compressors::XXH32_update(&st, nullptr, 0);
		compressors::XXH32_update(&st, d, s.size());
		CHECK(compressors::XXH32_digest(&st) == reference(d, s.size(), 7));
	}
	std::printf("all xxh32 tests passed\n");
	return 0;
}
