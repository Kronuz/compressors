/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
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

// Internal to the library's .cc files: scratch memory for the one-shot functions.

#include <cstddef>   // for size_t
#include <memory>    // for unique_ptr


namespace compressors::detail {

// Memory that is not zero-filled: a small stack buffer, else a per-thread block kept for reuse
// (up to 1 MB, shared by the three codecs), else a one-off heap block. Holds the memory of one
// call: two live at once on one thread would share the per-thread block.
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
	Scratch(const Scratch&) = delete;
	Scratch& operator=(const Scratch&) = delete;
	char* data() noexcept { return ptr; }
};

}  // namespace compressors::detail
