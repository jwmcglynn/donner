#pragma once

#include <cassert>
#include <cstdio>

#ifndef __has_cpp_attribute
#define __has_cpp_attribute(x) 0
#endif

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

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

#if __has_cpp_attribute(nodiscard)
#define UTILS_NO_DISCARD [[nodiscard]]
#elif __has_attribute(warn_unused_result) || (defined(__GNUC__) && !defined(__clang__))
#define UTILS_NO_DISCARD __attribute__((warn_unused_result))
#else
#define UTILS_NO_DISCARD
#endif

#if __has_builtin(__builtin_unreachable)
#define UTILS_UNREACHABLE() __builtin_unreachable()
#else
#define UTILS_UNREACHABLE()
#endif

#ifdef NDEBUG
#define UTILS_RELEASE_ASSERT(x)                                        \
  do {                                                                 \
    const bool _utilsResult = (x);                                     \
    if (!_utilsResult) {                                               \
      fprintf(stderr, "Error: UTILS_RELEASE_ASSERT failed: %s\n", #x); \
      abort();                                                         \
    }                                                                  \
  } while (false)

#define UTILS_RELEASE_ASSERT_MSG(x, msg)                                          \
  do {                                                                            \
    const bool _utilsResult = (x);                                                \
    if (!_utilsResult) {                                                          \
      fprintf(stderr, "Error: UTILS_RELEASE_ASSERT failed: %s, %s\n", #x, (msg)); \
      abort();                                                                    \
    }                                                                             \
  } while (false)
#else
#define UTILS_RELEASE_ASSERT(x) assert(x)
#define UTILS_RELEASE_ASSERT_MSG(x, msg) assert((x) && (msg))
#endif
