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

// Minimal, header-only exception surface for the compressors library.
//
// Xapiand throws its compressor failures through THROW(SomeException, "fmt", ...)
// where the exception derives from Error, which in turn derives from a located
// BaseException that records function/file/line. That full machinery now lives
// in the standalone located-exception library
// (github.com/Kronuz/located-exception) and needs its own .cc + CMake target.
//
// The compressors only ever throw on a backend error and read .what(); they
// never inspect the located fields. So rather than pull the whole library in,
// this trims the surface to exactly what the buffer-core uses: an Error base
// (a std::runtime_error that records the call site for diagnostics) and the
// THROW macro that formats a message with std::format. Same spelling as the
// Xapiand call sites, no external dependency.

#include <stdexcept>         // for std::runtime_error
#include <string>            // for std::string
#include <string_view>      // for std::string_view
#include <type_traits>      // for std::forward

#include <format>           // for std::format / std::vformat (C++20)


class Error : public std::runtime_error {
	const char* _function;
	const char* _filename;
	int _line;

	static std::string build(std::string_view function, std::string_view filename, int line, std::string_view message) {
		// "message (in function at filename:line)"
		return std::format("{} (in {} at {}:{})", message, function, filename, line);
	}

public:
	// Plain-message form: THROW(SomeError, "something failed").
	Error(const char* function, const char* filename, int line, const char* /*type*/, std::string_view message = "")
		: std::runtime_error(build(function, filename, line, message)),
		  _function(function),
		  _filename(filename),
		  _line(line) { }

	// Formatted form: THROW(SomeError, "bad value {}", x).
	template <typename Arg, typename... Args>
	Error(const char* function, const char* filename, int line, const char* /*type*/, std::string_view format, Arg&& arg, Args&&... args)
		: std::runtime_error(build(function, filename, line,
			std::vformat(format, std::make_format_args(arg, args...)))),
		  _function(function),
		  _filename(filename),
		  _line(line) { }

	const char* function() const noexcept { return _function; }
	const char* filename() const noexcept { return _filename; }
	int line() const noexcept { return _line; }
};


#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"

// Same spelling as the Xapiand THROW: passes the call site and the stringized
// exception name, then forwards the message / format args.
#define THROW(exception, ...) throw exception(__func__, __FILE__, __LINE__, #exception, ##__VA_ARGS__)

#pragma clang diagnostic pop
