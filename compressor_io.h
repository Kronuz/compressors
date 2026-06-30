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

// EINTR-safe, dependency-free file I/O helpers used by the fd-streaming *File
// compressor classes. These replace Xapiand's io.hh wrappers (which pulled in
// opts.h / config.h) and keep the standalone library free of any Xapiand
// coupling. They preserve the one behavior of io:: that matters for
// correctness here: retrying on EINTR, and looping over short reads so a full
// block is returned. The random-error injection and fd tracking in io:: are
// test/debug features the library does not need.

#include <cerrno>                // for errno, EINTR
#include <cstddef>               // for size_t
#include <fcntl.h>               // for open, O_RDONLY
#include <sys/types.h>           // for off_t, ssize_t
#include <unistd.h>              // for close, read, lseek


namespace compressors::detail {

inline int open(const char* path, int oflag = O_RDONLY, int mode = 0644) noexcept {
	int fd;
	do { fd = ::open(path, oflag, mode); } while (fd == -1 && errno == EINTR);
	return fd;
}

inline int close(int fd) noexcept {
	int r;
	do { r = ::close(fd); } while (r == -1 && errno == EINTR);
	return r;
}

inline off_t lseek(int fd, off_t offset, int whence) noexcept {
	return ::lseek(fd, offset, whence);
}

// Read up to nbyte bytes, retrying on EINTR and looping over short reads so a
// full block is returned when the file has the data (matching io::read). On
// error before any byte is read, returns -1; otherwise returns bytes read (0
// at EOF).
inline ssize_t read(int fd, void* buf, size_t nbyte) noexcept {
	auto* p = static_cast<char*>(buf);
	while (nbyte != 0u) {
		ssize_t c = ::read(fd, p, nbyte);
		if (c == -1) {
			if (errno == EINTR) {
				continue;
			}
			size_t done = p - static_cast<char*>(buf);
			return done == 0 ? -1 : static_cast<ssize_t>(done);
		}
		if (c == 0) {
			break;
		}
		p += c;
		if (c == static_cast<ssize_t>(nbyte)) {
			break;
		}
		nbyte -= c;
	}
	return p - static_cast<char*>(buf);
}

}  // namespace compressors::detail
