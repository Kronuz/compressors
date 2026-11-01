# Changelog

All notable changes to `compressors` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2026-10-31

### Fixed

- `XXH32` reads its input little-endian on every host. It used to load words in host
  order, so a big-endian host computed a different digest than upstream xxHash and than a
  little-endian host. storage persists the digest, so volumes written by a big-endian host
  carried checksums a little-endian host rejected. Digests on little-endian hosts are
  unchanged. The kernel now reads through a host policy, and `test/test_xxh32.cc` runs it on
  an emulated big-endian host against published values and a reference written from the
  specification (it fails with the byte swap removed).
- `decompress_lz4` sizes its buffers by the decoded data instead of the block
  headers. It used to walk the headers and reserve blocks * 2 KB up front,
  capped at 64 MB, before decoding anything: a crafted 98 KB stream of 3-byte
  blocks reserved 64 MB (650x the input), and a failing `new[]` raised
  `bad_alloc` instead of `LZ4CorruptVolume`. The contiguous phase now uses at
  most 256 KB, and the output after the ring wraps grows from a reservation of
  16 times the input. Hostile 98 KB inputs (valid one-byte blocks, zero-length
  blocks, a torn header) allocate under 1 MB, and a 70 MB stream stays under
  twice its size. The special case that sent streams over 64 MB of estimated
  output to the stream class is gone. Bytes and the accept/reject verdict are
  unchanged, checked against `LZ4DecompressData`.

### Changed

- `compress_lz4` / `decompress_lz4` run their own loop instead of going through
  `LZ4CompressData` / `LZ4DecompressData`, which cost a zero-filled 256 KB ring,
  a codec stream, an XXH32 state, and a `std::string` per 2 KB block on every
  call. The output bytes are identical. On an Apple M-series host, against the
  old implementation: 256 B compress 4x and decompress 20-50x faster, 1 MB random
  compress 1.4-1.6x and decompress 3.4x faster, text 1.0-1.5x faster.
- `XXH32` keeps its four lanes in scalar registers on AArch64. The compiler used
  to pack them into a NEON vector and run at half speed; it now matches upstream
  xxHash (about 12 GB/s). Digests are unchanged.
- `XXH32` keeps its four lanes in scalar registers on x86 (SSE2) too, as upstream
  xxHash does. On x86 clang and g++ vectorize the lanes at every level, and the
  lane chain then sets the speed. One 1 MiB buffer fed in 2 KiB updates
  (`compressors_bench_xxh32`, best of 25, per MiB), against upstream xxHash 0.8.3: on an
  Intel i9-9980HK (macOS), Apple clang 17 -O3 198 us to 124 us (upstream 121 us), -O3
  -march=native 199-208 us to 130-148 us (upstream 127-137 us), g++-16 -O3 259 us to
  124 us (upstream 121 us). On GitHub runners: Linux x86_64, g++ 13, 501 us to 83 us
  (upstream 82 us); macOS x86_64, Apple clang 17, 277 us to 153-171 us (139-141 us when
  fed in one update; upstream 138-141 us). The
  AArch64 code is the same instruction for instruction (Linux arm64 g++; only function-label
  numbers differ) and its timing is unchanged (Apple M4 Pro 87-89 us both; Linux arm64
  g++ 110 us both; macOS arm64 runner 155 us both). Digests are
  unchanged. The LZ4 stream classes, which hash every block, are faster by the
  same factor on x86 (1 MB random compress 1.2-1.3x).
- `compress_zstd`, `decompress_zstd` (both overloads) and `decompress_deflate` no
  longer go through the block iterators of `ZstdCompressData`, `ZstdDecompressData`
  and `DeflateDecompressData`. Each iterator path zero-filled a worst-case string,
  copied the result into a second one, and paid a context per call (`ZSTD_compress`
  and `ZSTD_decompress` create and free one) or a 16 KB zeroed buffer and a string
  per block (deflate). The one-shots run on the thread's zstd contexts, compress
  into scratch memory and copy out the exact frame, and decode into the result
  directly, with one allocation. The output bytes, the decoded bytes and the
  exceptions (type and message) are identical, checked against the classes on
  damaged input. They are out of line now (`compress_zstd`, `decompress_zstd` and
  `decompress_deflate` were inline in their headers). Linux x86_64, g++ 13,
  best of 15, ns per call, old path / one-shot / the codec's own call:
  zstd compress 256 B random 1878 / 606 / 1794, 64 KB random 10225 / 6379 / 9113,
  1 MB random 100098 / 87961 / 79608; zstd decompress 256 B random 1190 / 32 / 1133,
  4 KB text 4417 / 3153 / 4279, 1 MB random 36444 / 20390 / 15994; deflate
  decompress 256 B random 240 / 116 / 106, 64 KB random 17776 / 15784 / 15022,
  1 MB random 297503 / 251434 / 245855. On the Intel i9-9980HK (macOS, Apple clang 17,
  -O3): zstd compress 256 B random 2115 / 1901 / 2043, 64 KB random 28987 / 20530 /
  24709; zstd decompress 256 B random 531 / 228 / 240, 1 MB random 102530 / 62853 /
  36612; deflate decompress 256 B random 637 / 269 / 131, 4 KB random 954 / 432 / 316,
  1 MB random 396000 / 102797 / 84366. On Apple M4 Pro: zstd compress 256 B random
  772 / 621 / 695, 64 KB random 12458 / 8833 / 9458; zstd decompress 256 B random 166 /
  51 / 91; deflate decompress 256 B random 269 / 108 / 54, 1 MB random 136375 / 75500 /
  68625. The codec's own call writes into a buffer the benchmark reuses, so the
  decompress rows to its right include no allocation or zero-fill of a result; the
  one-shot's gap to it is the returned string. On macOS x86_64 runners the context
  creation costs about 13 us per call: zstd decompress of 256 B 13626 ns to 255 ns.
  `compress_deflate` is unchanged (it is zlib's `deflateInit` per call, a tie with
  `compress2`).
- The per-thread scratch block the lz4 one-shots use (up to 1 MB) is shared with the
  zstd one-shots (`compressor_scratch.h`).
- The XXH32 kernel reads through a host policy (`NativeHost`, `EmulatedBigEndianHost`);
  `XXH32_update` and `XXH32_digest` keep their signatures and call the native kernel.

### Added

- A differential test of the one-shots against the stream classes (bytes,
  decoded bytes, and verdicts on damaged input, around the 256 KB wrap) and
  against a reference XXH32.
- `decompress_zstd(compressed, out)`: decodes into a caller's string, reusing its capacity.
  storage reads one record after another into one string; with it a read allocates nothing
  for the record. Without it, a 1 MB zstd record read through the returning form allocated a
  fresh 1 MB string per record, and under glibc's default trim threshold the freed top of
  the heap went back to the kernel and was faulted in again: storage's 1 MB zstd read ran at
  430-450 MB/s instead of 740 (Linux x86_64 runner; 760 with trimming disabled).
- `test/test_oneshot.cc`: the zstd and deflate one-shots against the stream classes
  (bytes, verdicts and messages on 8,400 damaged inputs per codec, thread-local
  context reuse across sizes, four threads) and an allocation budget per call (one
  allocation, at the result's size) that fails on the iterator paths.
- `test/test_xxh32.cc`: XXH32 against published values and a reference, on the native
  and an emulated big-endian host.
- `bench/bench_oneshot.cc`: the zstd and deflate one-shots against the stream classes
  and the codecs' own one-call APIs.
- Benchmarks under `COMPRESSORS_BENCH`.

## 2026-10-30

### Added

- Fuzz mode 4 of the LZ4 target builds a one-record stream from a declared
  length and a body cycled to that length, so the fuzzer reaches the
  oversize-length overflow directly. On the unfixed decoder it finds the
  heap-buffer-overflow from an empty corpus in seconds.
- Standalone extraction from Xapiand: the deflate and lz4 buffer-core and
  `*File` classes (decoupled from `io.hh` onto `compressor_io.h`), a trimmed
  header-only `exception.h`, self-contained `likely.h` and `xxh32_stream.h`,
  iterators moved off the removed `std::iterator` base, and a new zstd backend.

### Fixed

- The first zstd backend ended the stream inside `init()`, so the driving loop
  dropped its single block and decompress saw empty input. `init()` now leaves
  `state` at `INIT` and the following `next()` flips it to `END`.
