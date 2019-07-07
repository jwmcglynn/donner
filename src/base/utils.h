#pragma once

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if __has_builtin(__builtin_expect) || (defined(__GNUC__) && !defined(__clang__))
#define UTILS_PREDICT_TRUE(x) (__builtin_expect((x), true))
#define UTILS_PREDICT_FALSE(x) (__builtin_expect(!!(x), false))
#else
#define UTILS_PREDICT_TRUE(x) (x)
#define UTILS_PREDICT_FALSE(x) (x)
#endif
