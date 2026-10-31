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

#include "compressor_lz4.h"

#include <cassert>               // for assert
#include <cstdio>                // for SEEK_SET
#include <cstdlib>               // for malloc, free
#include <cstring>               // for size_t, memcpy
#include <algorithm>             // for min
#include <memory>                // for unique_ptr
#include <new>                   // for bad_alloc

#include "likely.h"              // for likely, unlikely


static void read_uint16(const void* blockStream, uint16_t* i) {
	memcpy(i, blockStream, sizeof(uint16_t));
}


static void read_partial_uint16(const void* blockStream, uint16_t* i, size_t numBytes, size_t offset=0) {
	memcpy(reinterpret_cast<char*>(i) + offset, blockStream, numBytes);
}


static void read_bin(const void* blockStream, void* array, int arrayBytes) {
	memcpy(array, blockStream, arrayBytes);
}


static void read_partial_bin(const void* blockStream, void* array, size_t arrayBytes, size_t offset=0) {
	memcpy(reinterpret_cast<char*>(array) + offset, blockStream, arrayBytes);
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

	compressors::XXH32_update(&xxh_state, inpPtr, inpBytes);
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

	// A block never compresses to more than cmpBuf_size bytes.
	if (cmpBytes > cmpBuf_size || cmpBytes > data_size - data_offset) {
		THROW(LZ4CorruptVolume, "Data is corrupt");
	}

	read_bin(data + data_offset, &cmpBuf[0], cmpBytes);
	data_offset += cmpBytes;

	char* const decPtr = &buffer[_offset];
	const int decBytes = LZ4_decompress_safe_continue(lz4StreamDecode, &cmpBuf[0], decPtr, cmpBytes, LZ4_BLOCK_SIZE);
	if (decBytes <= 0) {
		THROW(LZ4CorruptVolume, "LZ4_decompress_safe_continue failed!");
	}

	// Add and wraparound the ringbuffer offset
	_offset += decBytes;
	if (_offset >= static_cast<size_t>(LZ4_RING_BUFFER_BYTES - LZ4_BLOCK_SIZE)) {
		_offset = 0;
	}

	compressors::XXH32_update(&xxh_state, decPtr, decBytes);
	_size += decBytes;

	std::string result;
	result.reserve(decBytes);
	result.append(decPtr, decBytes);

	return result;
}


LZ4CompressFile::LZ4CompressFile(std::string_view filename, int seed)
	: LZ4File(LZ4_BLOCK_SIZE, filename),
	  LZ4BlockStreaming(seed),
	  lz4Stream(LZ4_createStream()) { }


LZ4CompressFile::LZ4CompressFile(int fd_, off_t fd_offset_, off_t fd_nbytes_, int seed)
	: LZ4File(LZ4_BLOCK_SIZE, fd_, fd_offset_, fd_nbytes_),
	  LZ4BlockStreaming(seed),
	  lz4Stream(LZ4_createStream()) { }


LZ4CompressFile::~LZ4CompressFile()
{
	LZ4_freeStream(lz4Stream);
}


std::string
LZ4CompressFile::init()
{
	if (fd_offset != -1 && compressors::detail::lseek(fd, fd_offset, SEEK_SET) != static_cast<off_t>(fd_offset)) {
		THROW(LZ4IOError, "IO error: lseek");
	}
	if (!cmpBuf) {
		cmpBuf = std::make_unique<char[]>(cmpBuf_size);
	}
	if (!buffer) {
		buffer = std::make_unique<char[]>(buffer_size);
	}
	return next();
}


std::string
LZ4CompressFile::next()
{
	assert(cmpBuf);
	assert(buffer);

	char* const inpPtr = &buffer[_offset];

	// Read line to the ring buffer.
	auto inpBytes = static_cast<int>(compressors::detail::read(fd, inpPtr, get_read_size()));
	if (inpBytes <= 0) {
		return std::string();
	}

	const int cmpBytes = LZ4_compress_fast_continue(lz4Stream, inpPtr, &cmpBuf[0], inpBytes, cmpBuf_size, 1);
	if (cmpBytes <= 0) {
		THROW(LZ4Exception, "LZ4_compress_fast_continue failed!");
	}

	// Add and wraparound the ringbuffer offset
	_offset += inpBytes;
	if (_offset >= static_cast<size_t>(LZ4_RING_BUFFER_BYTES - LZ4_BLOCK_SIZE)) {
		_offset = 0;
	}

	compressors::XXH32_update(&xxh_state, inpPtr, inpBytes);
	size_t totalBytes = sizeof(uint16_t) + cmpBytes;
	_size += totalBytes;

	auto num_bytes = static_cast<uint16_t>(cmpBytes);
	std::string result;
	result.reserve(totalBytes);
	result.append(reinterpret_cast<const char*>(&num_bytes), sizeof(uint16_t));
	result.append(&cmpBuf[0], cmpBytes);

	return result;
}


LZ4DecompressFile::LZ4DecompressFile(std::string_view filename, int seed)
	: LZ4File(LZ4_MAX_CMP_SIZE, filename),
	  LZ4BlockStreaming(seed),
	  lz4StreamDecode(LZ4_createStreamDecode()),
	  data((char*)malloc(LZ4_MAX_CMP_SIZE)),
	  data_size(-1),
	  data_offset(0) { }


LZ4DecompressFile::LZ4DecompressFile(int fd_, off_t fd_offset_, off_t fd_nbytes_, int seed)
	: LZ4File(LZ4_MAX_CMP_SIZE, fd_, fd_offset_, fd_nbytes_),
	  LZ4BlockStreaming(seed),
	  lz4StreamDecode(LZ4_createStreamDecode()),
	  data((char*)malloc(LZ4_MAX_CMP_SIZE)),
	  data_size(-1),
	  data_offset(0) { }


LZ4DecompressFile::~LZ4DecompressFile()
{
	free(data);
	LZ4_freeStreamDecode(lz4StreamDecode);
}


std::string
LZ4DecompressFile::init()
{
	if (fd_offset != -1 && compressors::detail::lseek(fd, fd_offset, SEEK_SET) != static_cast<off_t>(fd_offset)) {
		THROW(LZ4IOError, "IO error: lseek");
	}

	if unlikely((data_size = compressors::detail::read(fd, data, get_read_size())) < 0) {
		THROW(LZ4IOError, "IO error: read");
	}

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
LZ4DecompressFile::next()
{
	assert(cmpBuf);
	assert(buffer);

	if (data_offset == static_cast<size_t>(data_size)) {
		if unlikely((data_size = compressors::detail::read(fd, data, get_read_size())) < 0) {
			THROW(LZ4IOError, "IO error: read");
		}
		if (data_size == 0) {
			return std::string();
		}
		data_offset = 0;
	}

	uint16_t cmpBytes = 0;
	size_t res_size = data_size - data_offset;
	if (sizeof(uint16_t) > res_size) {
		read_partial_uint16(data + data_offset, &cmpBytes, res_size);
		data_offset = sizeof(uint16_t) - res_size;
		if ((data_size = compressors::detail::read(fd, data, get_read_size())) < static_cast<ssize_t>(data_offset)) {
			THROW(LZ4CorruptVolume, "File is corrupt");
		}
		read_partial_uint16(data, &cmpBytes, data_offset, res_size);
	} else {
		read_uint16(data + data_offset, &cmpBytes);
		data_offset += sizeof(uint16_t);
	}

	// A block never compresses to more than cmpBuf_size bytes.
	if (cmpBytes > cmpBuf_size) {
		THROW(LZ4CorruptVolume, "File is corrupt");
	}

	res_size = data_size - data_offset;
	if (cmpBytes > res_size) {
		read_partial_bin(data + data_offset, &cmpBuf[0], res_size);
		data_offset = cmpBytes - res_size;
		if ((data_size = compressors::detail::read(fd, data, get_read_size())) < static_cast<ssize_t>(data_offset)) {
			THROW(LZ4CorruptVolume, "File is corrupt");
		}
		read_partial_bin(data, &cmpBuf[0], data_offset, res_size);
	} else {
		read_bin(data + data_offset, &cmpBuf[0], cmpBytes);
		data_offset += cmpBytes;
	}

	char* const decPtr = &buffer[_offset];
	const int decBytes = LZ4_decompress_safe_continue(lz4StreamDecode, &cmpBuf[0], decPtr, cmpBytes, LZ4_BLOCK_SIZE);
	if (decBytes <= 0) {
		THROW(LZ4CorruptVolume, "LZ4_decompress_safe_continue failed!");
	}

	// Add and wraparound the ringbuffer offset
	_offset += decBytes;
	if (_offset >= static_cast<size_t>(LZ4_RING_BUFFER_BYTES - LZ4_BLOCK_SIZE)) {
		_offset = 0;
	}

	compressors::XXH32_update(&xxh_state, decPtr, decBytes);
	_size += decBytes;

	std::string result;
	result.reserve(decBytes);
	result.append(decPtr, decBytes);

	return result;
}


std::size_t
lz4_block_bound(std::size_t n) noexcept
{
	return n > LZ4_MAX_INPUT_SIZE ? 0 : static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(n)));
}


std::size_t
compress_lz4_block(std::string_view src, char* dst, std::size_t capacity) noexcept
{
	// LZ4_compress_default's state, kept per thread instead of on the stack each call.
	struct state {
		void* p = std::malloc(static_cast<std::size_t>(LZ4_sizeofState()));
		~state() { std::free(p); }
	};
	thread_local state s;
	if (!s.p || src.size() > LZ4_MAX_INPUT_SIZE) {
		return 0;
	}
	const int cap = capacity > static_cast<std::size_t>(INT32_MAX) ? INT32_MAX : static_cast<int>(capacity);
	const int n = LZ4_compress_fast_extState(s.p, src.data(), dst, static_cast<int>(src.size()), cap, 1);
	return n > 0 ? static_cast<std::size_t>(n) : 0;
}


bool
decompress_lz4_block(std::string_view src, char* dst, std::size_t dst_size) noexcept
{
	if (src.size() > static_cast<std::size_t>(INT32_MAX) || dst_size > static_cast<std::size_t>(INT32_MAX)) {
		return false;
	}
	const int n = LZ4_decompress_safe(src.data(), dst, static_cast<int>(src.size()), static_cast<int>(dst_size));
	return n >= 0 && static_cast<std::size_t>(n) == dst_size;
}


// --- One-shot helpers -------------------------------------------------------------------
//
// compress_lz4 / decompress_lz4 produce and accept the byte stream of LZ4CompressData /
// LZ4DecompressData, which runs the LZ4 streaming API over a ring buffer. LZ4's output
// depends on where each block sits relative to the previous ones (contiguous blocks use the
// prefix mode, a block after the ring wraps uses the external-dictionary mode), so the
// helpers keep that layout:
//   - the first LZ4_RING_BUFFER_BYTES - LZ4_BLOCK_SIZE (256 KB) of data is contiguous in the
//     ring, so it is compressed from (decompressed into) the caller's buffer directly;
//   - past that the ring wraps; the rest goes through a ring buffer exactly as the classes do.
// There is no digest, no per-block string and no zero-filled ring.

namespace {

constexpr size_t wrap_size = LZ4_RING_BUFFER_BYTES - LZ4_BLOCK_SIZE;
constexpr size_t cmp_capacity = LZ4_COMPRESSBOUND(LZ4_BLOCK_SIZE);

// The contiguous phase of a stream (blocks decoded straight into one buffer) ends once the
// output reaches wrap_size, so it never needs more than this.
constexpr size_t contiguous_capacity = wrap_size + LZ4_BLOCK_SIZE;

// The up-front reservation for the part of the output that follows the contiguous phase is
// a multiple of the compressed size, not the header-derived worst case (block count times
// LZ4_BLOCK_SIZE): the headers are attacker-controlled, and 3 bytes of header per block
// would otherwise reserve 2 KB each. Output past the reservation grows by doubling, so
// memory follows what the blocks actually decode to.
constexpr size_t reserve_ratio = 16;

// Scratch memory that is not zero-filled: a small stack buffer, else a per-thread buffer
// kept for reuse (up to 1 MB), else a one-off heap block.
class Scratch {
	static constexpr size_t kept = size_t(1) << 20;
	alignas(16) char small[4096];
	std::unique_ptr<char[]> big;
	struct Cache {
		std::unique_ptr<char[]> p;
		size_t size = 0;
	};
	static Cache& cache() {
		thread_local Cache c;
		return c;
	}
	char* ptr;

public:
	explicit Scratch(size_t n) {
		if (n <= sizeof(small)) {
			ptr = small;
		} else if (n <= kept) {
			auto& c = cache();
			if (c.size < n) {
				c.p.reset(new char[n]);
				c.size = n;
			}
			ptr = c.p.get();
		} else {
			big.reset(new char[n]);
			ptr = big.get();
		}
	}
	char* data() noexcept { return ptr; }
};

struct EncoderState {
	LZ4_stream_t* p = LZ4_createStream();
	~EncoderState() { LZ4_freeStream(p); }
};

}  // namespace


std::string
compress_lz4(std::string_view uncompressed)
{
	const size_t n = uncompressed.size();
	if (n == 0) {
		return std::string();
	}
	const size_t blocks = (n + LZ4_BLOCK_SIZE - 1) / LZ4_BLOCK_SIZE;
	Scratch out(blocks * (sizeof(uint16_t) + cmp_capacity));
	char* o = out.data();

	thread_local EncoderState enc;
	if (!enc.p) {
		throw std::bad_alloc();
	}
	LZ4_resetStream(enc.p);  // a fresh table: the bytes must not depend on earlier calls

	auto block = [&](const char* src, size_t len) {
		const int cmpBytes = LZ4_compress_fast_continue(enc.p, src, o + sizeof(uint16_t), static_cast<int>(len), static_cast<int>(cmp_capacity), 1);
		if (cmpBytes <= 0) {
			THROW(LZ4Exception, "LZ4_compress_fast_continue failed!");
		}
		const auto num_bytes = static_cast<uint16_t>(cmpBytes);
		memcpy(o, &num_bytes, sizeof(uint16_t));
		o += sizeof(uint16_t) + cmpBytes;
	};

	size_t off = 0;
	const char* const in = uncompressed.data();
	while (off < n && off < wrap_size) {
		const size_t len = std::min(LZ4_BLOCK_SIZE, n - off);
		block(in + off, len);
		off += len;
	}
	if (off < n) {
		std::unique_ptr<char[]> ring(new char[LZ4_RING_BUFFER_BYTES]);
		size_t roff = 0;
		while (off < n) {
			const size_t len = std::min(LZ4_BLOCK_SIZE, n - off);
			memcpy(&ring[roff], in + off, len);
			block(&ring[roff], len);
			off += len;
			roff += len;
			if (roff >= wrap_size) {
				roff = 0;
			}
		}
	}
	return std::string(out.data(), static_cast<size_t>(o - out.data()));
}


std::string
decompress_lz4(std::string_view compressed)
{
	const size_t size = compressed.size();
	const char* const in = compressed.data();

	// Walk the block headers: how many blocks, and whether the framing ends badly. The
	// decoded size of a block is at most LZ4_BLOCK_SIZE, which bounds the output.
	size_t pos = 0;
	size_t blocks = 0;
	bool framing_error = false;
	while (pos < size) {
		if (size - pos < sizeof(uint16_t)) {
			framing_error = true;
			break;
		}
		uint16_t cmpBytes;
		memcpy(&cmpBytes, in + pos, sizeof(uint16_t));
		if (cmpBytes > cmp_capacity || cmpBytes > size - pos - sizeof(uint16_t)) {
			framing_error = true;
			break;
		}
		pos += sizeof(uint16_t) + cmpBytes;
		++blocks;
	}
	Scratch scratch(std::min(blocks * LZ4_BLOCK_SIZE, contiguous_capacity));
	char* const out = scratch.data();
	size_t total = 0;

	LZ4_streamDecode_t sd;
	LZ4_setStreamDecode(&sd, nullptr, 0);

	auto decode = [&](const char* src, size_t len, char* dst) {
		const int decBytes = LZ4_decompress_safe_continue(&sd, src, dst, static_cast<int>(len), static_cast<int>(LZ4_BLOCK_SIZE));
		if (decBytes <= 0) {
			THROW(LZ4CorruptVolume, "LZ4_decompress_safe_continue failed!");
		}
		return static_cast<size_t>(decBytes);
	};

	pos = 0;
	size_t b = 0;
	for (; b < blocks && total < wrap_size; ++b) {
		uint16_t cmpBytes;
		memcpy(&cmpBytes, in + pos, sizeof(uint16_t));
		total += decode(in + pos + sizeof(uint16_t), cmpBytes, out + total);
		pos += sizeof(uint16_t) + cmpBytes;
	}
	std::string spill;   // the output once the ring wraps; empty for streams that never do
	if (b < blocks) {
		spill.reserve(std::max(total, std::min(blocks * LZ4_BLOCK_SIZE, size * reserve_ratio)));
		spill.append(out, total);
		std::unique_ptr<char[]> ring(new char[LZ4_RING_BUFFER_BYTES]);
		size_t roff = 0;
		for (; b < blocks; ++b) {
			uint16_t cmpBytes;
			memcpy(&cmpBytes, in + pos, sizeof(uint16_t));
			const size_t dec = decode(in + pos + sizeof(uint16_t), cmpBytes, &ring[roff]);
			spill.append(&ring[roff], dec);
			pos += sizeof(uint16_t) + cmpBytes;
			roff += dec;
			if (roff >= wrap_size) {
				roff = 0;
			}
		}
	}
	if (framing_error) {
		THROW(LZ4CorruptVolume, "Data is corrupt");
	}
	if (!spill.empty()) {
		return spill;
	}
	return std::string(out, total);
}
