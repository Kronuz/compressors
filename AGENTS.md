# AGENTS.md

Working notes for agents modifying this repository. For the design read
`ARCHITECTURE.md`; for usage and the backend comparison read `README.md`. This
file covers the repo layout, how to build and test, the third-party discovery,
the invariants you must not break, and the traps.

## Repo map

```
compressor_deflate.h / .cc    Deflate (zlib) buffer-core: CRTP base + Deflate{Data,CompressData,DecompressData} + compress/decompress_deflate.
compressor_lz4.h / .cc        LZ4 buffer-core: CRTP base + LZ4{Data,CompressData,DecompressData} + compress/decompress_lz4. Uses xxhash.h for the auxiliary digest.
compressor_zstd.h / .cc       Zstd buffer-core (the new backend): CRTP base + Zstd{Data,CompressData,DecompressData} + compress/decompress_zstd.
exception.h                   Vendored, trimmed: header-only Error + THROW (std::format). Replaces Xapiand's located-exception wrapper.
likely.h                      Vendored verbatim from threadpool: self-contained likely()/unlikely().
xxhash.h                      Vendored, minimal: XXH32 streaming for lz4's get_digest(). BSD-2-Clause.
test/test.cc                  Round-trip test across all three backends. CTest test `compressors`.
CMakeLists.txt                STATIC library compressors + alias compressors::compressors; zlib/lz4/zstd discovery; top-level-only test.
LICENSE                       MIT, Copyright (c) 2015-2019 Dubalu LLC.
README.md                     What it is, the three backends, API, install, third-party discovery.
ARCHITECTURE.md               CRTP base, buffer-core vs fd-streaming split, per-backend specifics, vendored utils, decoupling.
```

The library is three `.cc` files compiled into `libcompressors.a`. The headers
plus the three vendored utils are the public surface.

## Build and run the tests

```sh
cmake -B build -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++ \
      -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build
ctest --test-dir build
```

One CTest test, `compressors` (`test/test.cc`): it round-trips a spread of
buffers through each backend and ends with `all compressors tests passed`. Run
the binary directly (`./build/compressors_test`) to see the per-input
compressed-size table.

On macOS the three compression libraries are keg-only Homebrew formulae, so the
`-DCMAKE_PREFIX_PATH="$(brew --prefix)"` is what lets `find_package`/`find_library`
locate them. `brew install zlib lz4 zstd` if any are missing.

## Third-party discovery

See `CMakeLists.txt`. Each dependency has a layered lookup:

- **zlib**: `find_package(ZLIB REQUIRED)`.
- **lz4**: `find_path`/`find_library` (no CMake config upstream) → `FetchContent`
  of `lz4/lz4` v1.10.0. Imported as `lz4::lz4`.
- **zstd**: `find_package(zstd CONFIG)` → `find_path`/`find_library` →
  `FetchContent` of `facebook/zstd` v1.5.7. Normalized to `zstd::libzstd`.

If you add a backend, follow the same pattern: prefer a CMake config, then
find_library, then FetchContent, and normalize to a single imported target name.

## Conventions

- **C++20.** Double quotes in code; no em dashes in prose.
- **Keep the three backends interchangeable.** Same CRTP base shape, same
  `*Data`/`*CompressData`/`*DecompressData` family, same
  `compress_<backend>`/`decompress_<backend>` free functions taking
  `std::string_view` and returning `std::string`. A new backend should look like
  the existing three.
- **Buffer-core only.** Do not re-add fd-streaming (`*File`) classes or anything
  that needs `io.hh` / file descriptors. That layer belongs in Xapiand. This
  library compresses in-memory buffers.
- **Keep the vendored utils self-contained.** `exception.h`, `likely.h`, and
  `xxhash.h` must not grow a dependency on anything outside std and themselves.
  Keep their license headers.

## Load-bearing invariants

These are the rules the backends' correctness rests on. Breaking one tends to
drop a block or corrupt a round-trip rather than fail to compile.

- **`init()` emits the first block; the stream ends on a later `next()`.** The
  driving loop checks `operator bool` *before* appending, so the value `init()`
  returns is the first block and must be emitted with the stream still "live"
  (state not `END`). Only a subsequent `next()` may signal the end. This is why
  the zstd backend, whose whole payload is one block, leaves `state` at `INIT`
  after `init()` and flips to `END` in `next()`. Ending the stream inside the
  block-producing call silently drops that block.
- **`decompress(compress(x)) == x`, byte for byte, for every input.** Empty,
  one byte, buffers straddling the 2 KB lz4 block boundary, and incompressible
  random bytes all must round-trip. The test pins all of these; extend it for any
  new input class you start handling.
- **The lz4 block format is `[uint16 len][LZ4 block]`.** The decompressor relies
  on the length prefix to size each block. Do not change the framing without
  changing both sides.
- **The lz4 XXH32 digest is auxiliary.** It is not in the stream and not part of
  the round-trip. `get_digest()` is a checksum the caller may read; do not start
  depending on it for correctness.
- **zstd frames carry their content size.** The decompressor uses
  `ZSTD_getFrameContentSize`, which works because `ZSTD_compress` records it. If
  you switch the compress path to a streaming API that omits the content size,
  the decompress path must change too (size the output some other way).

## Traps

- **The iterator block-drop trap.** Covered above; it is the single most likely
  thing to get wrong when adding or editing a backend. If a backend round-trips
  large inputs but loses small/empty ones, this is almost always the cause.
- **Don't vendor more than you need.** `stringified.hh` was *not* vendored: it was
  only used by the dropped `*File` classes. Before vendoring a Xapiand header,
  check the buffer-core actually uses it.
- **lz4 ships no CMake config.** `find_package(lz4)` will not work; use
  `find_path`/`find_library`. zstd *does* ship one. Don't unify them.
- **macOS keg-only libraries.** Without `CMAKE_PREFIX_PATH=$(brew --prefix)`,
  `find_library` will not see Homebrew's zlib/lz4/zstd and configuration fails or
  silently falls through to FetchContent.
- **Always extend the test.** A behavioral change should add an assertion to
  `test/test.cc`. The round-trip equality and the size plausibility checks are the
  contract.

## Standalone vs. Xapiand

The deflate and lz4 backends are extracted from
[Xapiand](https://github.com/Kronuz/Xapiand) (`compressor_deflate.{h,cc}`,
`compressor_lz4.{h,cc}`), keeping the CRTP buffer-core and dropping the
fd-streaming `*File` classes (which stay in Xapiand, behind its `io.hh`). The
**zstd backend is new** and has no upstream counterpart. Keep buffer-core edits
minimal and reconcilable with upstream; the zstd backend is free to evolve here.
See `ARCHITECTURE.md` for the full decoupling.
