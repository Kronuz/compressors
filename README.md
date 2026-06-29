# compressors

In-memory buffer compression with three interchangeable backends: **deflate**
(zlib), **lz4**, and **zstd** (Zstandard). A small static C++20 library.

## What it is

Each backend compresses and decompresses a whole in-memory buffer. They share
one shape: a CRTP block-streaming base (`*BlockStreaming<Derived>`), a
`*Data` / `*CompressData` / `*DecompressData` class family, and a pair of
free-function helpers (`compress_<backend>` / `decompress_<backend>`) that take a
`std::string_view` and return a `std::string`. Because the three backends share
that shape, you can swap one for another by changing the function name.

```cpp
#include "compressor_deflate.h"
#include "compressor_lz4.h"
#include "compressor_zstd.h"

std::string packed   = compress_zstd(payload);     // or compress_deflate / compress_lz4
std::string restored = decompress_zstd(packed);    // restored == payload
```

`decompress(compress(x)) == x` holds byte-for-byte for every backend, including
empty and incompressible inputs.

## The three backends

| backend | library | format on the wire | use it for |
| --- | --- | --- | --- |
| `compress_deflate` | zlib | raw deflate (or gzip when `gzip=true`) | broad interop, gzip output |
| `compress_lz4` | lz4 | a sequence of `[uint16 len][LZ4 block]` records, ring-buffer streamed | speed, cheap to compress |
| `compress_zstd` | libzstd | a standard zstd frame (records content size) | best ratio, modern default |

They are genuinely independent formats. Each backend only decodes its own
output; the test cross-checks that the three encodings of the same input are
distinct byte streams.

The numbers from the round-trip test (`test/test.cc`), Homebrew LLVM 22, give a
feel for the ratios:

| input | original | deflate | lz4 | zstd |
| --- | --- | --- | --- | --- |
| 200 KB of one repeated byte | 200000 | 218 | 1861 | 26 |
| 100 KB repeated phrase | 100035 | 361 | 998 | 68 |
| 100 KB random (incompressible) | 100000 | 100041 | 100538 | 100012 |
| 18-byte string | 18 | 26 | 22 | 27 |

zstd wins the ratio on compressible data and adds the least overhead on
incompressible data; lz4 trades ratio for compression speed; deflate sits in
between and gives you gzip interop.

## API

All three backends expose the same surface. Taking deflate as the example
(`lz4` and `zstd` are identical except for the backend-specific tuning
parameter):

```cpp
// Free-function helpers (the common path).
std::string compress_deflate(std::string_view uncompressed);
std::string decompress_deflate(std::string_view compressed);

// The classes underneath, for when you want to iterate the output in blocks.
DeflateCompressData   c(data, size, /*gzip=*/false);
for (auto it = c.begin(); it; ++it) {
    out.append(*it);          // each *it is a compressed block
}
```

The third constructor argument is the backend's tuning knob:

- **deflate**: `bool gzip` — emit a gzip wrapper instead of raw deflate.
- **lz4**: `int seed` — the seed for the running XXH32 digest exposed via
  `get_digest()` (auxiliary, not part of the stream).
- **zstd**: `int level` — the zstd compression level (default 3).

Each class also has a `reset(data, size, ...)` to reuse the object for another
buffer without reallocating its scratch buffers.

## Install

CMake. With `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
  compressors
  GIT_REPOSITORY https://github.com/Kronuz/compressors.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(compressors)

target_link_libraries(your_target PRIVATE compressors::compressors)
```

`compressors` is a `STATIC` library: it compiles the three `.cc` files, puts the
repo root on your include path, requests `cxx_std_20`, and links zlib, lz4, and
zstd.

### How the third-party libraries are found

- **zlib** — `find_package(ZLIB REQUIRED)`, the module finder that ships with
  CMake. Links `ZLIB::ZLIB`.
- **lz4** — lz4 does not install a CMake package config, so discovery is by hand:
  `find_path(lz4.h)` + `find_library(lz4)`. If neither is found it falls back to
  `FetchContent` of `lz4/lz4` (tag `v1.10.0`, its `build/cmake` subdir). Either
  way the dependency is normalized to an imported `lz4::lz4`.
- **zstd** — zstd *does* ship a CMake config, so `find_package(zstd CONFIG)` is
  tried first; failing that, `find_path` + `find_library`; failing that,
  `FetchContent` of `facebook/zstd` (tag `v1.5.7`). Normalized to
  `zstd::libzstd`.

On macOS the three are keg-only Homebrew formulae, so point CMake at them:

```sh
cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix)"
```

## Build & test

```sh
cmake -B build -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++ \
      -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build
ctest --test-dir build
```

`ctest` runs one test, `compressors` (`test/test.cc`): it round-trips a spread of
buffers (empty, one byte, a string, a buffer straddling the lz4 block boundary,
a highly repetitive run, a repeated phrase, and incompressible random bytes)
through each backend, asserts `decompress(compress(x)) == x`, and sanity-checks
the compressed size. It ends with `all compressors tests passed`.

## Provenance

Extracted from [Xapiand](https://github.com/Kronuz/Xapiand), which carried the
deflate and lz4 buffer compressors (`compressor_deflate.{h,cc}`,
`compressor_lz4.{h,cc}`). The **Zstandard backend is new**, written here to
mirror the deflate/lz4 buffer-core so the three are interchangeable.

Xapiand's compressors had two families sharing each CRTP base: a **buffer-core**
(`*Data` classes, in-memory) and an **fd-streaming** layer (`*File` classes that
streamed to and from file descriptors via Xapiand's `io.hh`, which pulled in
`opts.h`). Only the buffer-core was reusable; the fd-streaming layer was app
glue. So the extraction kept the CRTP base and the `*Data` classes and **dropped
the `*File` classes**, which stay in Xapiand. See [ARCHITECTURE.md](ARCHITECTURE.md)
for the split and the decoupling notes, and [AGENTS.md](AGENTS.md) for the repo
map and conventions.

## License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE). The vendored
`xxhash.h` carries its own BSD-2-Clause header (Yann Collet).
