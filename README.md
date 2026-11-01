# compressors

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/compressors/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/compressors/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

Compression library for C++20: deflate, LZ4 and zstd behind one function shape, with fd
streaming for deflate and LZ4. `compress_zstd(bytes)` returns a string, and swapping the name
switches the codec. Reach for it in storage and network layers that want a codec choice
without three different APIs.

## ✨ Highlights

- **One shape, three codecs.** `compress_<codec>` and `decompress_<codec>` take a
  `std::string_view` and return a `std::string`.
- **One allocation per call.** `compress_zstd`, `decompress_zstd` and `decompress_deflate` run
  on the thread's reused contexts and scratch memory, and `decompress_zstd(compressed, out)`
  decodes into a string you keep from record to record.
- **Blocks into your buffer.** `compress_lz4_block` and `compress_zstd_into`, and their
  decoders, write one block into a buffer you own and allocate nothing per block.
- **Safe on hostile input.** `decompress_zstd(bytes, max_size)` refuses frames that claim more
  than `max_size`, and `decompress_lz4` sizes its buffers by what the blocks decode to, so a
  hostile 98 KB stream allocates under 1 MB.
- **Streams and file descriptors.** Iterate the compressed output block by block, reuse a
  compressor's buffers with `reset()`, and stream deflate and LZ4 to and from file descriptors
  with EINTR-safe I/O.
- **Reproducible bytes.** `COMPRESSORS_ZSTD_PINNED` and `COMPRESSORS_LZ4_PINNED` build pinned
  releases, so output is byte-identical on every platform.
- **Finds or fetches its dependencies.** zlib comes from the system; LZ4 and zstd from the
  system when CMake finds them, and from a pinned release otherwise.

## 🚀 Quick start

```cpp
#include "compressor_lz4.h"
#include "compressor_zstd.h"

#include <cstdio>
#include <string>

int main() {
	std::string payload(100'000, 'x');

	std::string packed = compress_zstd(payload);                   // or compress_lz4, compress_deflate
	std::string back = decompress_zstd(packed, payload.size());    // bounded for untrusted input
	std::puts(back == payload ? "zstd round trip" : "mismatch");    // zstd round trip

	back = decompress_lz4(compress_lz4(payload));
	std::puts(back == payload ? "lz4 round trip" : "mismatch");     // lz4 round trip
}
```

[`examples/roundtrip.cc`](examples/roundtrip.cc) shows codec dispatch, block streaming and
edge inputs.

## 📦 Installation

Requires a C++20 compiler, CMake 3.14 or newer, and zlib. CMake uses the system's LZ4 and zstd
when it finds them and fetches pinned releases (LZ4 1.10.0, zstd 1.5.7) otherwise; on macOS,
pass `-DCMAKE_PREFIX_PATH="$(brew --prefix)"` so it finds Homebrew's packages. CI builds it on
Linux (x86_64, arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan, UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(compressors
  GIT_REPOSITORY https://github.com/Kronuz/compressors.git
  GIT_TAG        fe0e7127512ac14c57e9083733dcd48d2b701de8)  # main tracks the tip
FetchContent_MakeAvailable(compressors)

target_link_libraries(my_app PRIVATE compressors::compressors)
```

Then `#include "compressor_zstd.h"`, `"compressor_lz4.h"` or `"compressor_deflate.h"`.

## 🎯 When to use it

Use it to compress records, pages or messages in memory, with the codec chosen per use: zstd
for ratio, LZ4 for speed, deflate for gzip interop.

Not for:

- Files written by other tools: the LZ4 backend uses its own record framing, and
  `decompress_zstd` needs frames that record their content size, which a `.zst` streamed
  from a pipe may not.
- Streaming zstd: only deflate and LZ4 stream to and from file descriptors; zstd works on
  whole in-memory buffers.
- Untrusted zstd input through the one-argument `decompress_zstd`: it allocates the size the
  frame claims, up to 32,768 times the frame's own size. Pass `max_size` instead.

## ⚡ Performance

Nanoseconds per call on random bytes, from `compressors_bench_oneshot` (best of 15 runs, Apple
clang 17, `-O3`), against the codecs the library wraps: [zstd](https://github.com/facebook/zstd)
1.5.7 at level 3 and [zlib](https://zlib.net) 1.2.12 (macOS's) at its default level. Each machine
has two columns: *library* is the library's call (`compress_zstd`, `decompress_zstd`,
`decompress_deflate`, each returning a `std::string`) and *codec* is the codec's own one-call API
on the same bytes (`ZSTD_compress`, `ZSTD_decompress`, zlib's `uncompress`), which writes into a
buffer the benchmark reuses and so includes no allocation. Before timing, the benchmark checks that
each compressed output equals the codec's byte for byte and that every decoder returns its input.

| Call | M4 Pro: library | M4 Pro: codec | i9-9980HK: library | i9-9980HK: codec |
|---|---:|---:|---:|---:|
| `compress_zstd`, 256 B | 621 | 695 | 1,901 | 2,043 |
| `compress_zstd`, 64 KB | 8,833 | 9,458 | 20,530 | 24,709 |
| `decompress_zstd`, 256 B | 51 | 91 | 228 | 240 |
| `decompress_deflate`, 256 B | 108 | 54 | 269 | 131 |
| `decompress_deflate`, 1 MB | 75,500 | 68,625 | 102,797 | 84,366 |

An Apple M4 Pro (macOS 26, arm64) and an Intel Core i9-9980HK (macOS 15, x86-64). The XXH32 that
checks LZ4 streams is the library's own; against [xxHash](https://github.com/Cyan4973/xxHash)
0.8.3 it takes 124 µs per MiB against 121 on the i9-9980HK (`compressors_bench_xxh32`, which also
checks that the digests agree).

```sh
cmake -S . -B build -DCOMPRESSORS_BENCH=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/compressors_bench_oneshot
build/compressors_bench_xxh32
```

## 📚 Documentation

- [docs/API.md](docs/API.md): the three backends and their formats, the interface, the
  block calls, how dependencies are found, and the build options.
- [ARCHITECTURE.md](ARCHITECTURE.md): the CRTP block-streaming bases, the one-shots, and the
  buffer and fd families.
- [CHANGELOG.md](CHANGELOG.md): changes by date, with their measurements.
- [AGENTS.md](AGENTS.md): repository map and conventions.
- [Catalog entry](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#compressors):
  include lines, target, dependencies and key API.

## 🛠️ Building and testing

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

On macOS add `-DCMAKE_PREFIX_PATH="$(brew --prefix)"` to the configure line.
`-DCOMPRESSORS_BENCH=ON` builds the benchmarks (the XXH32 one fetches upstream xxHash for
the benchmark only), and `-DCOMPRESSORS_FUZZ=ON` the libFuzzer targets (Clang).

## 🤝 Contributing

Read [AGENTS.md](AGENTS.md) and the catalog's
[rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
before opening a pull request; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo and (c) 2015-2020 Dubalu LLC. See
[LICENSE](LICENSE). The vendored `xxh32_stream.h` carries its own BSD 2-Clause license (Yann
Collet). Derived from [Xapiand](https://github.com/Kronuz/Xapiand)'s compressors; the zstd
backend is this library's own.
