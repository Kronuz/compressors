# Changelog

All notable changes to `compressors` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2026-10-31

### Fixed

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

### Added

- A differential test of the one-shots against the stream classes (bytes,
  decoded bytes, and verdicts on damaged input, around the 256 KB wrap) and
  against a reference XXH32.
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
