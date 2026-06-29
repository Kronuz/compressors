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

#pragma once

// Buffer-core LZ4 compressor. Extracted from Xapiand; the fd-streaming
// LZ4CompressFile / LZ4DecompressFile classes (which pulled in io.hh and thus
// Xapiand's opts.h) stayed in Xapiand. What is left is the CRTP block streaming
// base and the in-memory *Data classes plus the compress/decompress helpers.
//
// The LZ4 buffer format is a sequence of [uint16_t block length][LZ4 block]
// records using the ring-buffer streaming API (LZ4_compress_fast_continue /
// LZ4_decompress_safe_continue), which keeps cross-block back-references. A
// running XXH32 digest is kept over the input and exposed via get_digest();
// it is not part of the stream.

#include <cstdint>          // for uint16_t, uint32_t
#include <cstring>          // for size_t, memcpy
#include <iterator>         // for std::input_iterator_tag
#include <memory>           // for std::unique_ptr
#include <string>           // for string
#include <string_view>     // for std::string_view

#include "lz4.h"            // for LZ4_COMPRESSBOUND, LZ4_resetStream, LZ4_stre...
#include "xxhash.h"         // for XXH32_createState, XXH32_reset, XXH32_digest
#include "exception.h"      // for Error, THROW


constexpr size_t LZ4_BLOCK_SIZE        = 1024 * 2;
constexpr size_t LZ4_MAX_CMP_SIZE      = sizeof(uint16_t) + LZ4_COMPRESSBOUND(LZ4_BLOCK_SIZE);
constexpr size_t LZ4_RING_BUFFER_BYTES = 1024 * 256 + LZ4_BLOCK_SIZE;


class LZ4Exception : public Error {
public:
	template<typename... Args>
	LZ4Exception(Args&&... args) : Error(std::forward<Args>(args)...) { }
};


class LZ4IOError : public LZ4Exception {
public:
	template<typename... Args>
	LZ4IOError(Args&&... args) : LZ4Exception(std::forward<Args>(args)...) { }
};


class LZ4CorruptVolume : public LZ4Exception {
public:
	template<typename... Args>
	LZ4CorruptVolume(Args&&... args) : LZ4Exception(std::forward<Args>(args)...) { }
};


template<typename Impl>
class LZ4BlockStreaming {
protected:
	// These variables must be defined in init function.
	size_t _size;
	size_t _offset;

	static constexpr size_t cmpBuf_size = LZ4_COMPRESSBOUND(LZ4_BLOCK_SIZE);
	static constexpr size_t buffer_size = LZ4_RING_BUFFER_BYTES;

	std::unique_ptr<char[]> cmpBuf;
	std::unique_ptr<char[]> buffer;

	XXH32_state_t xxh_state;

	std::string _init() {
		return static_cast<Impl*>(this)->init();
	}

	std::string _next() {
		return static_cast<Impl*>(this)->next();
	}

	void _reset(int seed) {
		_size = 0;
		_offset = 0;
		XXH32_reset(&xxh_state, seed);
	}

public:
	explicit LZ4BlockStreaming(int seed)
		: _size(0),
		  _offset(0)
	{
		XXH32_reset(&xxh_state, seed);
	}

	class iterator {
		LZ4BlockStreaming* obj;
		std::string current_str;
		size_t offset;

		friend class LZ4BlockStreaming;

	public:
		using iterator_category = std::input_iterator_tag;
		using value_type = LZ4BlockStreaming;
		using difference_type = std::ptrdiff_t;
		using pointer = const std::string*;
		using reference = const std::string&;

		iterator()
			: obj(nullptr),
			  offset(0) { }

		iterator(LZ4BlockStreaming* o, std::string&& str)
			: obj(o),
			  current_str(std::move(str)),
			  offset(0) { }

		iterator& operator++() {
			current_str = obj->_next();
			offset = 0;
			return *this;
		}

		const std::string& operator*() const noexcept {
			return current_str;
		}

		const std::string* operator->() const noexcept {
			return &current_str;
		}

		size_t size() const noexcept {
			return current_str.size();
		}

		bool operator==(const iterator& other) const noexcept {
			return current_str == other.current_str;
		}

		bool operator!=(const iterator& other) const noexcept {
			return !operator==(other);
		}

		explicit operator bool() const noexcept {
			return !current_str.empty();
		}

		size_t read(char* buf, size_t buf_size) {
			size_t res_size = current_str.size() - offset;
			if (!res_size) {
				current_str = obj->_next();
				offset = 0;
				res_size = current_str.size();
			}

			if (res_size < buf_size) {
				buf_size = res_size;
			}
			memcpy(buf, current_str.data() + offset, buf_size);
			offset += buf_size;
			return buf_size;
		}
	};

	iterator begin() {
		return iterator(this, _init());
	}

	iterator end() {
		return iterator(this, std::string());
	}

	size_t size() const noexcept {
		return _size;
	}

	uint32_t get_digest() {
		return XXH32_digest(&xxh_state);
	}
};


class LZ4Data {
protected:
	const char* data;
	size_t data_size;
	size_t data_offset;

	LZ4Data(const char* data_, size_t data_size_)
		: data(data_),
		  data_size(data_size_),
		  data_offset(0) { }

	~LZ4Data() = default;

public:
	int close() {
		data_offset = 0;
		return 0;
	}

	void add_data(const char* data_, size_t data_size_) {
		data = data_;
		data_size = data_size_;
	}
};


/*
 * Compress Data.
 */
class LZ4CompressData : public LZ4Data, public LZ4BlockStreaming<LZ4CompressData> {
	LZ4_stream_t* const lz4Stream;

	std::string init();
	std::string next();

	friend class LZ4BlockStreaming<LZ4CompressData>;

public:
	LZ4CompressData(const char* data_=nullptr, size_t data_size_=0, int seed_=0);

	~LZ4CompressData();

	void reset(const char* data_, size_t data_size_, int seed=0) {
		_reset(seed);
		add_data(data_, data_size_);
		LZ4_resetStream(lz4Stream);
	}
};


/*
 * Decompress Data.
 */
class LZ4DecompressData : public LZ4Data, public LZ4BlockStreaming<LZ4DecompressData> {
	LZ4_streamDecode_t* const lz4StreamDecode;

	std::string init();
	std::string next();

	friend class LZ4BlockStreaming<LZ4DecompressData>;

public:
	LZ4DecompressData(const char* data_=nullptr, size_t data_size_=0, int seed=0);

	~LZ4DecompressData();

	void reset(const char* data_, size_t data_size_, int seed=0) {
		_reset(seed);
		add_data(data_, data_size_);
	}
};


inline std::string
compress_lz4(std::string_view uncompressed)
{
	std::string compressed;
	LZ4CompressData compressor(uncompressed.data(), uncompressed.size());
	for (auto it = compressor.begin(); it; ++it) {
		compressed.append(*it);
	}
	return compressed;
}


inline std::string
decompress_lz4(std::string_view compressed)
{
	std::string decompressed;
	LZ4DecompressData decompressor(compressed.data(), compressed.size());
	for (auto it = decompressor.begin(); it; ++it) {
		decompressed.append(*it);
	}
	return decompressed;
}
