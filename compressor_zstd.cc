/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "compressor_zstd.h"

#include <zstd_errors.h>  // for ZSTD_getErrorCode, which older zstd.h headers leave out

#include "compressor_scratch.h"  // for compressors::detail::Scratch


/*
 * Constructor compress with data
 */
ZstdCompressData::ZstdCompressData(const char* data_, size_t data_size_, int level_)
	: ZstdData(data_, data_size_),
	  ZstdBlockStreaming(level_) { }


ZstdCompressData::~ZstdCompressData() = default;


std::string
ZstdCompressData::init()
{
	// One-shot compress of the whole buffer. ZSTD_compress writes a frame whose
	// header records the content size, so decompression can recover the exact
	// original length. An empty input still produces a valid (small) frame.
	//
	// The block-streaming iterator yields what init() returns first, then calls
	// next() once more, which ends the stream. So init() leaves state at INIT
	// (the iterator's operator bool stays true for this one block) and next()
	// flips it to END, mirroring the deflate/lz4 backends where the payload is
	// emitted before the stream terminates.
	size_t bound = ZSTD_compressBound(data_size);
	std::string result;
	result.resize(bound);

	size_t cmp_size = ZSTD_compress(result.data(), bound, data, data_size, level);
	if (ZSTD_isError(cmp_size) != 0u) {
		THROW(ZstdException, "ZSTD_compress failed: {}", ZSTD_getErrorName(cmp_size));
	}
	result.resize(cmp_size);

	state = ZstdState::INIT;
	return result;
}


std::string
ZstdCompressData::next()
{
	// The whole frame was already produced by init(); terminate the stream.
	state = ZstdState::END;
	return std::string();
}


/*
 * Constructor decompress with data
 */
ZstdDecompressData::ZstdDecompressData(const char* data_, size_t data_size_, int level_)
	: ZstdData(data_, data_size_),
	  ZstdBlockStreaming(level_) { }


ZstdDecompressData::~ZstdDecompressData() = default;


std::string
ZstdDecompressData::init()
{
	// One-shot decompress of the whole frame, emitted as a single block. As in
	// the compress path, init() returns the payload with state left at INIT and
	// next() ends the stream, so the iterator yields the decompressed buffer
	// before terminating.
	unsigned long long const content_size = ZSTD_getFrameContentSize(data, data_size);
	if (content_size == ZSTD_CONTENTSIZE_ERROR) {
		THROW(ZstdCorruptVolume, "Data is not a valid zstd frame");
	}
	if (content_size == ZSTD_CONTENTSIZE_UNKNOWN) {
		// The frames written by ZstdCompressData always carry the content size,
		// so this should not happen for data produced by this library.
		THROW(ZstdCorruptVolume, "zstd frame does not record its content size");
	}

	// A 128 KiB block is at best a 4-byte run, so a frame cannot hold more than 32768 bytes per
	// compressed byte; a header claiming more is damaged and must not size an allocation.
	if (content_size / 32768 > data_size) {
		THROW(ZstdCorruptVolume, "zstd frame claims more content than it can hold");
	}

	std::string result;
	result.resize(static_cast<size_t>(content_size));

	size_t dec_size = ZSTD_decompress(result.data(), result.size(), data, data_size);
	if (ZSTD_isError(dec_size) != 0u) {
		THROW(ZstdException, "ZSTD_decompress failed: {}", ZSTD_getErrorName(dec_size));
	}
	result.resize(dec_size);

	state = ZstdState::INIT;
	return result;
}


std::string
ZstdDecompressData::next()
{
	// The whole frame was already decompressed by init(); terminate the stream.
	state = ZstdState::END;
	return std::string();
}


namespace {

// This thread's contexts, created on first use and freed when the thread ends. The one-shot
// functions below reuse them, so a steady stream of calls creates none.
struct zstd_contexts {
	ZSTD_CCtx* c = nullptr;
	ZSTD_DCtx* d = nullptr;
	~zstd_contexts() {
		ZSTD_freeCCtx(c);
		ZSTD_freeDCtx(d);
	}
};
thread_local zstd_contexts contexts;

ZSTD_CCtx* compression_context() noexcept {
	if (!contexts.c) {
		contexts.c = ZSTD_createCCtx();
	}
	return contexts.c;
}

ZSTD_DCtx* decompression_context() noexcept {
	if (!contexts.d) {
		contexts.d = ZSTD_createDCtx();
	}
	return contexts.d;
}

// ZSTD_decompress on this thread's context: the same bytes, without creating a context per call.
size_t decompress_frame(char* dst, size_t dst_size, std::string_view src) {
	ZSTD_DCtx* const ctx = decompression_context();
	if (!ctx) {
		THROW(ZstdException, "ZSTD_createDCtx failed");
	}
	return ZSTD_decompressDCtx(ctx, dst, dst_size, src.data(), src.size());
}

}  // namespace


std::string
decompress_zstd(std::string_view compressed, std::size_t max_size)
{
	unsigned long long const content_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
	if (content_size == ZSTD_CONTENTSIZE_ERROR || content_size == ZSTD_CONTENTSIZE_UNKNOWN || content_size > max_size ||
	    content_size / 32768 > compressed.size()) {
		THROW(ZstdCorruptVolume, "zstd frame is invalid or larger than allowed");
	}
	std::string result(static_cast<size_t>(content_size), '\0');
	size_t dec_size = decompress_frame(result.data(), result.size(), compressed);
	if (ZSTD_isError(dec_size) != 0u || dec_size != result.size()) {
		THROW(ZstdCorruptVolume, "zstd frame is corrupt");
	}
	return result;
}


std::string
decompress_zstd(std::string_view compressed)
{
	std::string result;
	decompress_zstd(compressed, result);
	return result;
}


void
decompress_zstd(std::string_view compressed, std::string& out)
{
	// The checks and the exceptions of ZstdDecompressData::init(), without the copy through the
	// block iterator.
	unsigned long long const content_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
	if (content_size == ZSTD_CONTENTSIZE_ERROR) {
		THROW(ZstdCorruptVolume, "Data is not a valid zstd frame");
	}
	if (content_size == ZSTD_CONTENTSIZE_UNKNOWN) {
		THROW(ZstdCorruptVolume, "zstd frame does not record its content size");
	}
	if (content_size / 32768 > compressed.size()) {
		THROW(ZstdCorruptVolume, "zstd frame claims more content than it can hold");
	}

	out.resize(static_cast<size_t>(content_size));
	size_t dec_size = decompress_frame(out.data(), out.size(), compressed);
	if (ZSTD_isError(dec_size) != 0u) {
		THROW(ZstdException, "ZSTD_decompress failed: {}", ZSTD_getErrorName(dec_size));
	}
	out.resize(dec_size);
}


std::string
compress_zstd(std::string_view uncompressed)
{
	// The frame goes into scratch memory and is copied out at its exact size: a string sized for
	// the worst case would be zero-filled first and keep that capacity.
	const size_t bound = ZSTD_compressBound(uncompressed.size());
	if (ZSTD_isError(bound) != 0u) {
		THROW(ZstdException, "ZSTD_compress failed: {}", ZSTD_getErrorName(bound));
	}
	ZSTD_CCtx* const ctx = compression_context();
	if (!ctx) {
		THROW(ZstdException, "ZSTD_createCCtx failed");
	}
	compressors::detail::Scratch out(bound);
	const size_t n = ZSTD_compressCCtx(ctx, out.data(), bound, uncompressed.data(), uncompressed.size(), ZSTD_COMPRESS_LEVEL);
	if (ZSTD_isError(n) != 0u) {
		THROW(ZstdException, "ZSTD_compress failed: {}", ZSTD_getErrorName(n));
	}
	return std::string(out.data(), n);
}


std::size_t
zstd_frame_bound(std::size_t n) noexcept
{
	return ZSTD_compressBound(n);
}


std::size_t
compress_zstd_into(std::string_view src, char* dst, std::size_t capacity, int level)
{
	ZSTD_CCtx* const ctx = compression_context();
	if (!ctx) {
		THROW(ZstdException, "ZSTD_createCCtx failed");
	}
	const size_t n = ZSTD_compressCCtx(ctx, dst, capacity, src.data(), src.size(), level);
	if (ZSTD_isError(n) != 0u) {
		if (ZSTD_getErrorCode(n) == ZSTD_error_dstSize_tooSmall) {
			return 0;
		}
		THROW(ZstdException, "ZSTD_compressCCtx failed: {}", ZSTD_getErrorName(n));
	}
	return n;
}


bool
decompress_zstd_into(std::string_view src, char* dst, std::size_t dst_size) noexcept
{
	ZSTD_DCtx* const ctx = decompression_context();
	if (!ctx) {
		return false;
	}
	const size_t n = ZSTD_decompressDCtx(ctx, dst, dst_size, src.data(), src.size());
	return ZSTD_isError(n) == 0u && n == dst_size;
}
