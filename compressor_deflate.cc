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

#include "compressor_deflate.h"

#include <cassert>               // for assert


std::string zerr(int ret) {
	switch (ret) {
		case Z_ERRNO:
			return "there is an error reading or writing the files";
		case Z_STREAM_ERROR:
			return "invalid compression level";
		case Z_DATA_ERROR:
			return "invalid or incomplete deflate data";
		case Z_MEM_ERROR:
			return "memory could not be allocated for processing (out of memory)";
		case Z_VERSION_ERROR:
			return "zlib version mismatch!";
		default:
			return std::string();
	}
}


int DeflateCompressData::FINISH_COMPRESS = Z_FINISH;


/*
 * Constructor compress with data
 */
DeflateCompressData::DeflateCompressData(const char* data_, size_t data_size_, bool gzip_)
	: DeflateData(data_, data_size_),
	  DeflateBlockStreaming(gzip_) { }


DeflateCompressData::~DeflateCompressData()
{
	if (state != DeflateState::NONE) {
		deflateEnd(&strm);
	}
}


std::string
DeflateCompressData::init()
{
	if (state != DeflateState::NONE) {
		deflateEnd(&strm);
	}

	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.next_in = Z_NULL;

	stream = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + (gzip ? 16 : 0), 8, Z_DEFAULT_STRATEGY);
	if (stream < 0) {
		THROW(DeflateException, zerr(stream));
	}
	state = DeflateState::INIT;

	if (data != nullptr) {
		if (!cmpBuf) {
			cmpBuf = std::make_unique<char[]>(cmpBuf_size);
		}
		return next();
	}
	return std::string();
}


std::string
DeflateCompressData::next(const char* input, size_t input_size, int flush)
{
	std::vector<char> out;
	out.resize(input_size);
	strm.avail_in = input_size;
	strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input));

	std::string result;
	do {
		strm.avail_out = input_size;
		strm.next_out = reinterpret_cast<Bytef*>(out.data());
		stream = deflate(&strm, flush);    // no bad return value
		if (stream < 0) {
			THROW(DeflateException, zerr(stream));
		}
		int compress_size = input_size - strm.avail_out;
		result.append(out.data(), compress_size);
	} while (strm.avail_out == 0);

	return result;
}


std::string
DeflateCompressData::next()
{
	assert(cmpBuf);

	int flush;
	if (data_offset > data_size) {
		state = DeflateState::END;
		return std::string();
	}

	if (data_offset < data_size) {
		strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data + data_offset));
	}

	auto remain_size = data_size - data_offset;
	if (remain_size > DEFLATE_BLOCK_SIZE) {
		strm.avail_in = DEFLATE_BLOCK_SIZE;
		flush = Z_NO_FLUSH;
	} else {
		strm.avail_in = remain_size;
		flush = Z_FINISH;
	}

	std::string result;
	do {
		strm.avail_out = DEFLATE_BLOCK_SIZE;
		strm.next_out = reinterpret_cast<Bytef*>(&cmpBuf[0]);
		stream = deflate(&strm, flush);    // no bad return value
		int compress_size = DEFLATE_BLOCK_SIZE - strm.avail_out;
		result.append(&cmpBuf[0], compress_size);
	} while (strm.avail_out == 0);

	data_offset += DEFLATE_BLOCK_SIZE;
	return result;
}


/*
 * Constructor decompress with data
 */
DeflateDecompressData::DeflateDecompressData(const char* data_, size_t data_size_, bool gzip_)
	: DeflateData(data_, data_size_),
	  DeflateBlockStreaming(gzip_) { }


DeflateDecompressData::~DeflateDecompressData()
{
	if (state != DeflateState::NONE) {
		inflateEnd(&strm);
	}
}


std::string
DeflateDecompressData::init()
{
	if (state != DeflateState::NONE) {
		inflateEnd(&strm);
	}

	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;

	stream = inflateInit2(&strm, 15 + (gzip ? 16 : 0));
	if (stream < 0) {
		if (gzip) {
			THROW(DeflateException, zerr(stream));
		} else {
			stream = inflateInit2(&strm, -15);
			if (stream < 0) {
				THROW(DeflateException, zerr(stream));
			}
		}
	}
	state = DeflateState::INIT;

	if (!buffer) {
		buffer = std::make_unique<char[]>(buffer_size);
	}
	return next();
}


std::string
DeflateDecompressData::next()
{
	assert(buffer);

	if (data_offset > data_size) {
		state = DeflateState::END;
		return std::string();
	}

	if (data_offset < data_size) {
		strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data + data_offset));
	}

	auto remain_size = data_size - data_offset;
	if (remain_size > DEFLATE_BLOCK_SIZE) {
		strm.avail_in = DEFLATE_BLOCK_SIZE;
	} else {
		strm.avail_in = remain_size;
	}

	std::string result;
	do {
		strm.avail_out = DEFLATE_BLOCK_SIZE;
		strm.next_out = reinterpret_cast<Bytef*>(&buffer[0]);
		stream = inflate(&strm, Z_NO_FLUSH);
		if (stream < 0 && stream != Z_BUF_ERROR) {
			THROW(DeflateException, zerr(stream));
		}
		auto bytes_decompressed = DEFLATE_BLOCK_SIZE - strm.avail_out;
		result.append(&buffer[0], bytes_decompressed);
	} while (strm.avail_out == 0);

	data_offset += DEFLATE_BLOCK_SIZE;
	return result;
}
