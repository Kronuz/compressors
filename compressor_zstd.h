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

// Buffer-core Zstandard compressor. New backend, added when extracting the
// deflate + lz4 buffer-core out of Xapiand into this standalone library. It
// mirrors the deflate/lz4 buffer-core: the same CRTP block-streaming base, the
// same Zstd{Data,CompressData,DecompressData} family, and the same
// compress_zstd / decompress_zstd convenience helpers, so the three backends are
// interchangeable.
//
// zstd's one-shot ZSTD_compress / ZSTD_decompress already operate on a whole
// in-memory buffer, which is exactly the buffer-core contract. The frame written
// by ZSTD_compress records the content size, so decompression recovers the exact
// original length via ZSTD_getFrameContentSize. The block-streaming iterator is
// kept for API symmetry: init() produces the whole compressed (or decompressed)
// buffer as a single block, then the stream ends.

#include <cstdint>          // for uint8_t
#include <cstring>          // for memcpy
#include <iterator>         // for std::input_iterator_tag
#include <memory>           // for std::unique_ptr
#include <string>           // for std::string
#include <string_view>     // for std::string_view

#include <zstd.h>           // for ZSTD_compress, ZSTD_decompress, ZSTD_compressBound

// See compressor_deflate.h: COMPRESSORS_EXCEPTION_HEADER lets a host redirect
// Error/THROW to its own exception header to avoid redefinition when embedding.
#ifndef COMPRESSORS_EXCEPTION_HEADER
#define COMPRESSORS_EXCEPTION_HEADER "exception.h"
#endif
#include COMPRESSORS_EXCEPTION_HEADER  // for Error, THROW


// Default compression level. ZSTD_CLEVEL_DEFAULT is 3 upstream; spelled out so
// the buffer-core does not depend on that macro being visible.
#define ZSTD_COMPRESS_LEVEL 3


class ZstdException : public Error {
public:
	template<typename... Args>
	ZstdException(Args&&... args) : Error(std::forward<Args>(args)...) { }
};


class ZstdCorruptVolume : public ZstdException {
public:
	template<typename... Args>
	ZstdCorruptVolume(Args&&... args) : ZstdException(std::forward<Args>(args)...) { }
};


template <typename Impl>
class ZstdBlockStreaming {
protected:
	enum class ZstdState : uint8_t {
		NONE,
		INIT,
		END,
	};

	int level;

	ZstdState state;

	std::string _init() {
		return static_cast<Impl*>(this)->init();
	}

	std::string _next() {
		return static_cast<Impl*>(this)->next();
	}

public:
	explicit ZstdBlockStreaming(int level_)
		: level(level_),
		  state(ZstdState::NONE) { }

	class iterator {
		ZstdBlockStreaming* obj;
		std::string current_str;
		size_t offset;

		friend class ZstdBlockStreaming;

	public:
		using iterator_category = std::input_iterator_tag;
		using value_type = ZstdBlockStreaming;
		using difference_type = std::ptrdiff_t;
		using pointer = const std::string*;
		using reference = const std::string&;

		iterator()
			: obj(nullptr),
			  offset(0) { }

		iterator(ZstdBlockStreaming* o, std::string&& str)
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
			return obj->state != ZstdState::END;
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
};


class ZstdData {
protected:
	const char* data;
	size_t data_size;
	size_t data_offset;

	ZstdData(const char* data_, size_t data_size_)
		: data(data_),
		  data_size(data_size_),
		  data_offset(0) { }

	~ZstdData() = default;

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
class ZstdCompressData : public ZstdData, public ZstdBlockStreaming<ZstdCompressData> {
	std::string next();

	friend class ZstdBlockStreaming<ZstdCompressData>;

public:
	ZstdCompressData(const char* data_=nullptr, size_t data_size_=0, int level_=ZSTD_COMPRESS_LEVEL);

	~ZstdCompressData();

	std::string init();

	void reset(const char* data_, size_t data_size_, int level_=ZSTD_COMPRESS_LEVEL) {
		level = level_;
		data_offset = 0;
		add_data(data_, data_size_);
	}
};


/*
 * Decompress Data.
 */
class ZstdDecompressData : public ZstdData, public ZstdBlockStreaming<ZstdDecompressData> {
	std::string next();

	friend class ZstdBlockStreaming<ZstdDecompressData>;

public:
	ZstdDecompressData(const char* data_=nullptr, size_t data_size_=0, int level_=ZSTD_COMPRESS_LEVEL);

	~ZstdDecompressData();

	std::string init();

	void reset(const char* data_, size_t data_size_, int level_=ZSTD_COMPRESS_LEVEL) {
		level = level_;
		data_offset = 0;
		add_data(data_, data_size_);
	}
};


inline std::string
compress_zstd(std::string_view uncompressed)
{
	std::string compressed;
	ZstdCompressData compressor(uncompressed.data(), uncompressed.size());
	for (auto it = compressor.begin(); it; ++it) {
		compressed.append(*it);
	}
	return compressed;
}


inline std::string
decompress_zstd(std::string_view compressed)
{
	std::string decompressed;
	ZstdDecompressData decompressor(compressed.data(), compressed.size());
	for (auto it = decompressor.begin(); it; ++it) {
		decompressed.append(*it);
	}
	return decompressed;
}
