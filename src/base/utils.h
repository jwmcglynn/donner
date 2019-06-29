// Copyright 2018 Jeff McGlynn
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if __has_builtin(__builtin_expect) ||                                         \
    (defined(__GNUC__) && !defined(__clang__))
#define UTILS_PREDICT_TRUE(x) (__builtin_expect((x), true))
#define UTILS_PREDICT_FALSE(x) (__builtin_expect(!!(x), false))
#else
#define UTILS_PREDICT_TRUE(x) (x)
#define UTILS_PREDICT_FALSE(x) (x)
#endif
