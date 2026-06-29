# Architecture

This document describes the internals of `compressors`: the CRTP block-streaming
base each backend shares, the buffer-core vs fd-streaming split that drove the
extraction, the per-backend specifics, and the decoupling done to lift the code
out of Xapiand.

## Overview

There are three backends, one per file pair:

- `compressor_deflate.{h,cc}` — zlib deflate/inflate.
- `compressor_lz4.{h,cc}` — lz4 ring-buffer streaming.
- `compressor_zstd.{h,cc}` — Zstandard one-shot frame (the new backend).

Each is built the same way: a CRTP base `<Backend>BlockStreaming<Impl>` provides
a block iterator, a `<Backend>Data` base holds the input buffer pointer, and two
leaf classes `<Backend>CompressData` / `<Backend>DecompressData` implement
`init()` and `next()`. Two free functions `compress_<backend>` /
`decompress_<backend>` drive the iterator and return a `std::string`.

The value is in the uniform shape: three different compression libraries behind
one interface, so a caller can pick a backend by name and swap it later.

## The CRTP block-streaming base

`<Backend>BlockStreaming<Impl>` (for example `DeflateBlockStreaming`) is a
template parameterized on the leaf class. It does not know how to compress; it
knows how to *iterate*. It exposes `begin()` / `end()` returning an `iterator`
whose `operator++` calls back into the leaf's `next()` (via `_next()`, which is
`static_cast<Impl*>(this)->next()`), and an `operator bool` that reports whether
the stream is still producing.

The iterator contract is the load-bearing part, and it is subtle:

1. `begin()` calls the leaf's `init()` and seeds the iterator with whatever
   `init()` returns. That first value is the first block.
2. The driving loop is `for (auto it = obj.begin(); it; ++it) out.append(*it);`.
   `operator bool` is checked *before* the body, so `init()`'s return value is
   appended first.
3. `++it` calls `next()`, which produces the next block or signals the end.

So a backend must return its first block from `init()` while leaving the stream
"not yet ended", and only signal the end on a later `next()` call. Getting this
wrong (ending the stream in the same call that produces the only block) makes the
loop drop that block, because `operator bool` is false on the first check. This
is exactly the trap the zstd backend had to navigate (below).

### The iterator typedefs

The original Xapiand iterators derived from `std::iterator<...>`, which C++17
deprecated and C++20 removed from libc++ in practice. Each iterator here instead
declares the five member typedefs by hand (`iterator_category`, `value_type`,
`difference_type`, `pointer`, `reference`). This is a pure modernization; the
behavior is identical.

## Buffer-core vs fd-streaming (why the `*File` classes are gone)

In Xapiand each header carried **two** leaf families on the same CRTP base:

- **buffer-core**: `<Backend>Data`, `<Backend>CompressData`,
  `<Backend>DecompressData`. These compress/decompress an in-memory buffer. They
  depend only on the compression library, a couple of small std-only utils, and
  the exception base. This is the reusable part.
- **fd-streaming**: `<Backend>File`, `<Backend>CompressFile`,
  `<Backend>DecompressFile`. These stream to and from file descriptors. They
  included `io.hh`, which pulled in Xapiand's `opts.h` and the rest of the app.
  This is app glue, not a general-purpose library.

The extraction kept the buffer-core and **dropped the fd-streaming layer
entirely**. The CRTP base is shared, so it came along; the `*File` classes, the
`#include "io.hh"`, the `<fcntl.h>` / `<sys/stat.h>` includes, and the
`stringified.hh` include (used only by the `*File::open` path) were all removed.
The fd-streaming classes stay in Xapiand, where the `io::` layer lives.

One consequence worth noting: `stringified.hh` was *only* used by the dropped
`*File` classes (for null-terminating a filename before `io::open`). The
buffer-core never touches it, so it was not vendored.

## Per-backend specifics

### deflate (`compressor_deflate.cc`)

A thin wrapper over zlib's streaming `deflate`/`inflate`. `init()` calls
`deflateInit2` / `inflateInit2` (with `15 + 16` window bits for gzip, plain `15`
otherwise, and a `-15` raw fallback on inflate) and returns the first 16 KB
block. `next()` feeds successive `DEFLATE_BLOCK_SIZE` (16384) chunks and appends
the deflated output, ending when `data_offset` passes `data_size`. The leaf owns
a `z_stream` and tears it down (`deflateEnd` / `inflateEnd`) in its destructor.
Errors go through `THROW(DeflateException, zerr(stream))`.

### lz4 (`compressor_lz4.cc`)

Uses lz4's *streaming* API (`LZ4_compress_fast_continue` /
`LZ4_decompress_safe_continue`) over a 256 KB ring buffer, so back-references
carry across 2 KB blocks. Each emitted block is `[uint16 compressed length][LZ4
bytes]`, which the decompressor reads back to size each block. The leaf owns an
`LZ4_stream_t` / `LZ4_streamDecode_t`. A running XXH32 digest is maintained over
the bytes and exposed via `get_digest()`; it is **auxiliary** — not embedded in
the stream, not needed for a round-trip. The `read_partial_*` helpers and the
mid-block-boundary handling from Xapiand belonged to the `*File` path and were
dropped; the buffer-core only needs `read_uint16` and `read_bin`.

### zstd (`compressor_zstd.cc`) — the new backend

Zstandard's simple API (`ZSTD_compress` / `ZSTD_decompress`) already operates on
a whole in-memory buffer, which is exactly the buffer-core contract, so this
backend does the work in one shot rather than chunking. `init()` compresses (or
decompresses) the entire buffer and returns it as a single block; `next()` then
ends the stream. The compress path sizes the output with `ZSTD_compressBound`;
the decompress path recovers the exact original length from the frame header via
`ZSTD_getFrameContentSize` (the frames `ZSTD_compress` writes always record it).

The one place this backend differs structurally from deflate/lz4 is that the
whole payload is one block, which makes the iterator contract above easy to get
wrong: the first cut ended the stream inside `init()`, so the single block was
dropped by the driving loop and decompress saw empty input. The fix is to leave
`state` at `INIT` after `init()` produces the block and flip it to `END` only in
the subsequent `next()`. With that, the iterator yields the block, then
terminates, matching deflate/lz4.

## Vendored utilities

Three small headers were vendored so the library is self-contained, each keeping
its license/attribution:

- **`exception.h`** — Xapiand throws compressor errors with
  `THROW(SomeException, "fmt", ...)`, where the exceptions derive from `Error`,
  which derives from a located `BaseException` that records function/file/line.
  That full machinery now lives in the standalone
  [located-exception](https://github.com/Kronuz/located-exception) library, which
  needs its own `.cc` and CMake target. The compressors only ever throw on a
  backend failure and read `.what()`; they never inspect the located fields. So
  this vendors a **trimmed, header-only** `Error` (a `std::runtime_error` that
  records the call site for diagnostics) plus the `THROW` macro spelled exactly as
  the Xapiand call sites expect, using `std::format` for interpolation. No
  external dependency, same call-site spelling.
- **`likely.h`** — the `likely()` / `unlikely()` branch hints, vendored verbatim
  from the threadpool extraction's self-contained copy (it detects
  `__builtin_expect` via `__has_builtin`, no `config.h`). Only the macro-guard
  prefix is renamed to `COMPRESSORS_*`.
- **`xxhash.h`** — a minimal, self-contained XXH32 streaming implementation
  (`XXH32_state_t`, `XXH32_reset` / `XXH32_update` / `XXH32_digest`) for lz4's
  `get_digest()`. Xapiand pulled in the full xxHash distribution; since the digest
  is a non-load-bearing auxiliary checksum, vendoring only the XXH32 surface
  (canonical algorithm, so digests match upstream) avoids adding a fourth
  third-party dependency. Carries the upstream BSD-2-Clause header.

## Third-party discovery (CMake)

The library links three real compression libraries; discovery is layered so it
works whether they are installed system-wide, via Homebrew (keg-only), or not at
all:

- **zlib** via `find_package(ZLIB REQUIRED)` (CMake's bundled finder).
- **lz4** via `find_path` + `find_library` (lz4 ships no CMake config), falling
  back to `FetchContent` of `lz4/lz4`. Normalized to an imported `lz4::lz4`.
- **zstd** via `find_package(zstd CONFIG)` (zstd does ship a config), then
  `find_path`/`find_library`, then `FetchContent` of `facebook/zstd`. Normalized
  to `zstd::libzstd`.

The library is `STATIC` (three `.cc` files), include dir is the repo root,
`cxx_std_20`, aliased `compressors::compressors`, and the test block is gated on
`CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR` so consumers pulling this in
via FetchContent do not build it.

## Standalone vs. Xapiand

This is a partial extraction: the buffer-core of `compressor_deflate` and
`compressor_lz4` from [Xapiand](https://github.com/Kronuz/Xapiand), plus a new
zstd backend. The decoupling was:

1. Drop the `*File` fd-streaming classes and their `io.hh` / `fcntl.h` /
   `stringified.hh` includes (they depend on Xapiand's `opts.h`).
2. Replace the `exception.h` that wraps located-exception with a trimmed
   header-only `Error` + `THROW`.
3. Vendor `likely.h` and `xxhash.h` self-contained.
4. Modernize the iterators off the removed `std::iterator` base.

Changes to the buffer-core logic itself were kept minimal so they can be
reconciled with upstream Xapiand. The zstd backend has no upstream counterpart;
it is new code in the same shape.
