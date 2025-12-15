#pragma once
/// @file

#include <cassert>
#include <cstdio>

#include "donner/base/CompilerConfig.h"

#ifndef DOXYGEN_SHOULD_SKIP_THIS

#ifndef __has_cpp_attribute
#define __has_cpp_attribute(x) 0
#endif

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#endif  // DOXYGEN_SHOULD_SKIP_THIS

/**
 * \def UTILS_PREDICT_TRUE(x)
 *
 * A hint to the compiler that the expression \p x is likely to evaluate to true.
 *
 * @param x The expression to evaluate.
 */

/**
 * \def UTILS_PREDICT_FALSE(x)
 *
 * A hint to the compiler that the expression \p x is likely to evaluate to false.
 *
 * @param x The expression to evaluate.
 */

#if __has_builtin(__builtin_expect) || (defined(__GNUC__) && !defined(__clang__))
#define UTILS_PREDICT_TRUE(x) (__builtin_expect((x), true))
#define UTILS_PREDICT_FALSE(x) (__builtin_expect(!!(x), false))
#else
#define UTILS_PREDICT_TRUE(x) (x)
#define UTILS_PREDICT_FALSE(x) (x)
#endif

/**
 * \def UTILS_UNREACHABLE()
 *
 * A hint to the compiler that the code following this macro is unreachable.
 */

#if __has_builtin(__builtin_unreachable)
#define UTILS_UNREACHABLE() __builtin_unreachable()
#else
#define UTILS_UNREACHABLE()
#endif

/**
 * \def UTILS_RELEASE_ASSERT(x)
 *
 * An assert that evaluates on both release and debug builds.
 *
 * Asserts that \a x is true in release builds, stopping execution with an `abort`. On debug builds,
 * falls back to an `assert`.
 *
 * @param x The expression to evaluate.
 */

/**
 * \def UTILS_RELEASE_ASSERT_MSG(x, msg)
 *
 * An assert that evaluates on both release and debug builds and errors with the provided \p msg.
 *
 * Asserts that \a x is true in release builds, stopping execution with an `abort`. On debug builds,
 * falls back to an `assert`.
 *
 * @param x The expression to evaluate.
 * @param msg A message to print if the assertion fails.
 */

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

/**
 * \def UTILS_EXCEPTIONS_ENABLED()
 *
 * Returns true if exceptions are enabled, false otherwise.
 */
#if defined(__cpp_exceptions) && __cpp_exceptions == 199711
#define UTILS_EXCEPTIONS_ENABLED() true
#else
#define UTILS_EXCEPTIONS_ENABLED() false
#endif
