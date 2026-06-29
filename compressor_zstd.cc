/*
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
