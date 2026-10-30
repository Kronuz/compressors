// Shared by the fuzz targets: a scratch file whose descriptor the *File decoders adopt.
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unistd.h>

#define FUZZ_CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: FUZZ_CHECK failed: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

// The scratch file holds `bytes` and its offset is 0 on return; unlinked when the process ends.
inline int fuzz_fd(std::string_view bytes) {
	static const int fd = [] {
		std::string path = "./compressors_fuzz.XXXXXX";
		int f = mkstemp(path.data());
		FUZZ_CHECK(f != -1);
		unlink(path.c_str());
		return f;
	}();
	FUZZ_CHECK(ftruncate(fd, 0) == 0);
	size_t off = 0;
	while (off < bytes.size()) {
		ssize_t w = pwrite(fd, bytes.data() + off, bytes.size() - off, static_cast<off_t>(off));
		FUZZ_CHECK(w > 0);
		off += static_cast<size_t>(w);
	}
	FUZZ_CHECK(lseek(fd, 0, SEEK_SET) == 0);
	return fd;
}
