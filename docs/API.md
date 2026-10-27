# compressors API reference

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

## The interface

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

`decompress_zstd(compressed, max_size)` decompresses one frame whose recorded content
size is at most `max_size` and throws otherwise, for input that may be corrupt or
hostile: the one-argument form allocates whatever size the frame claims.

The CMake options `COMPRESSORS_DEFLATE`, `COMPRESSORS_LZ4`, and `COMPRESSORS_ZSTD`
(all `ON`) leave a backend and its dependency out of the build.

## One-shot blocks into the caller's buffer

For a storage layer that keeps each block's uncompressed size itself (a block index), LZ4
and zstd also compress and decompress one block straight into a buffer the caller owns,
with contexts reused per thread, so a steady stream of blocks allocates nothing:

```cpp
std::size_t lz4_block_bound(std::size_t n) noexcept;
std::size_t compress_lz4_block(std::string_view src, char* dst, std::size_t capacity) noexcept;
bool decompress_lz4_block(std::string_view src, char* dst, std::size_t dst_size) noexcept;

std::size_t zstd_frame_bound(std::size_t n) noexcept;
std::size_t compress_zstd_into(std::string_view src, char* dst, std::size_t capacity, int level);
bool decompress_zstd_into(std::string_view src, char* dst, std::size_t dst_size) noexcept;
```

- `compress_lz4_block` writes the raw LZ4 block format, the bytes `LZ4_compress_default`
  writes, not `compress_lz4`'s record framing. `compress_zstd_into` writes a standard frame
  recording its content size, the bytes `ZSTD_compress` writes at that level (at level 3,
  `compress_zstd`'s). Both return the bytes written, or 0 when the capacity is too small;
  size the buffer with the bound.
- Both decoders take the exact uncompressed size and return false for a malformed block or
  one that decodes to any other size. They never read past `src` or write past `dst_size`,
  so they are safe on corrupt or hostile input.
- The LZ4 compression state and the zstd contexts are created on a thread's first call and
  freed when the thread ends.

`decompress_deflate(bytes, maximum_output, gzip=false)` bounds cumulative decoded bytes before allocating each returned block. Pass `true` for gzip. `DeflateDecompressData(data, size, gzip, maximum_output)` exposes the same bound for iteration. `reset(data, size, gzip)` preserves that bound; its four-argument overload replaces it explicitly. Existing constructors retain an unlimited output default and use the same decoder. Its blocks contain at most `DEFLATE_BLOCK_SIZE` bytes (16 KiB).

The decoder requires complete checksummed streams. It rejects truncation, invalid checksums and trailing nonmember data. Gzip accepts concatenated members under one cumulative output bound, as specified by [RFC 1952, section 2.2](https://www.rfc-editor.org/rfc/rfc1952#section-2.2); zlib mode accepts one stream. The decoder retains a fixed output block and uses zlib's fixed 15-bit window, independently of the total decoded length. The caller still owns the encoded input, and a buffered result retains up to `maximum_output` decoded bytes.

Adding the optional bound changes `DeflateDecompressData` layout and its constructor symbol. Rebuild C++ dependents together. Source calls and compressed output remain compatible. Truncated data and silently ignored trailing data now fail explicitly; iterator block boundaries may differ. The file decoder is unchanged.

## Dependencies and build options

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

### Build options

- `COMPRESSORS_DEFLATE`, `COMPRESSORS_LZ4`, `COMPRESSORS_ZSTD` (all `ON`) leave a
  backend and its dependency out of the build. At least one must stay on; configuring with
  all three off stops with an error. The round-trip test covers whichever backends are
  built; the dispatch example needs all three.
- `COMPRESSORS_ZSTD_PINNED` (`OFF`) builds zstd from its pinned release (`v1.5.7`)
  instead of the system's. Different zstd versions can compress the same input to
  different valid bytes, so a consumer that needs byte-identical output on every
  platform, such as strata's segment fixtures, turns it on.
- `COMPRESSORS_LZ4_PINNED` (`OFF`) does the same for LZ4, building its pinned release
  (`v1.10.0`, the static library only) instead of the system's.
