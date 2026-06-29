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

#include "compressor_lz4.h"

#include <cassert>               // for assert
#include <cstring>               // for size_t, memcpy

#include "likely.h"              // for likely, unlikely


static void read_uint16(const void* blockStream, uint16_t* i) {
	memcpy(i, blockStream, sizeof(uint16_t));
}


static void read_bin(const void* blockStream, void* array, int arrayBytes) {
	memcpy(array, blockStream, arrayBytes);
}


LZ4CompressData::LZ4CompressData(const char* data_, size_t data_size_, int seed_)
	: LZ4Data(data_, data_size_),
	  LZ4BlockStreaming(seed_),
	  lz4Stream(LZ4_createStream()) { }


LZ4CompressData::~LZ4CompressData()
{
	LZ4_freeStream(lz4Stream);
}


std::string
LZ4CompressData::init()
{
	data_offset = 0;
	if (!cmpBuf) {
		cmpBuf = std::make_unique<char[]>(cmpBuf_size);
	}
	if (!buffer) {
		buffer = std::make_unique<char[]>(buffer_size);
	}
	return next();
}


std::string
LZ4CompressData::next()
{
	assert(cmpBuf);
	assert(buffer);

	if (data_offset >= data_size) {
		return std::string();
	}

	char* const inpPtr = &buffer[_offset];

	// Read line to the ring buffer.
	int inpBytes = 0;
	if ((data_offset + LZ4_BLOCK_SIZE) > data_size) {
		inpBytes = static_cast<int>(data_size - data_offset);
	} else {
		inpBytes = LZ4_BLOCK_SIZE;
	}

	memcpy(inpPtr, data + data_offset, inpBytes);
	data_offset += inpBytes;

	const int cmpBytes = LZ4_compress_fast_continue(lz4Stream, inpPtr, &cmpBuf[0], inpBytes, cmpBuf_size, 1);
	if (cmpBytes <= 0) {
		THROW(LZ4Exception, "LZ4_compress_fast_continue failed!");
	}

	// Add and wraparound the ringbuffer offset
	_offset += inpBytes;
	if (_offset >= static_cast<size_t>(LZ4_RING_BUFFER_BYTES - LZ4_BLOCK_SIZE)) {
		_offset = 0;
	}

	XXH32_update(&xxh_state, inpPtr, inpBytes);
	size_t totalBytes = sizeof(uint16_t) + cmpBytes;
	_size += totalBytes;

	auto num_bytes = static_cast<uint16_t>(cmpBytes);
	std::string result;
	result.reserve(totalBytes);
	result.append(reinterpret_cast<const char*>(&num_bytes), sizeof(uint16_t));
	result.append(&cmpBuf[0], cmpBytes);

	return result;
}


LZ4DecompressData::LZ4DecompressData(const char* data_, size_t data_size_, int seed)
	: LZ4Data(data_, data_size_),
	  LZ4BlockStreaming(seed),
	  lz4StreamDecode(LZ4_createStreamDecode()) { }


LZ4DecompressData::~LZ4DecompressData()
{
	LZ4_freeStreamDecode(lz4StreamDecode);
}


std::string
LZ4DecompressData::init()
{
	data_offset = 0;
	if (!cmpBuf) {
		cmpBuf = std::make_unique<char[]>(cmpBuf_size);
	}
	if (!buffer) {
		buffer = std::make_unique<char[]>(buffer_size);
	}
	return next();
}


std::string
LZ4DecompressData::next()
{
	assert(cmpBuf);
	assert(buffer);

	if (data_offset >= data_size) {
		return std::string();
	}

	if (sizeof(uint16_t) > data_size - data_offset) {
		THROW(LZ4CorruptVolume, "Data is corrupt");
	}

	uint16_t cmpBytes = 0;
	read_uint16(data + data_offset, &cmpBytes);
	data_offset += sizeof(uint16_t);

	if (cmpBytes > data_size - data_offset) {
		THROW(LZ4CorruptVolume, "Data is corrupt");
	}

	read_bin(data + data_offset, &cmpBuf[0], cmpBytes);
	data_offset += cmpBytes;

	char* const decPtr = &buffer[_offset];
	const int decBytes = LZ4_decompress_safe_continue(lz4StreamDecode, &cmpBuf[0], decPtr, cmpBytes, LZ4_BLOCK_SIZE);
	if (decBytes <= 0) {
		THROW(LZ4Exception, "LZ4_decompress_safe_continue failed!");
	}

	// Add and wraparound the ringbuffer offset
	_offset += decBytes;
	if (_offset >= static_cast<size_t>(LZ4_RING_BUFFER_BYTES - LZ4_BLOCK_SIZE)) {
		_offset = 0;
	}

	XXH32_update(&xxh_state, decPtr, decBytes);
	_size += decBytes;

	std::string result;
	result.reserve(decBytes);
	result.append(decPtr, decBytes);

	return result;
}
