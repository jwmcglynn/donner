#pragma once
/// @file

namespace donner {

/**
 * Compile-time checks for the supported compiler and language baseline.
 */
#if !defined(__clang_major__)
#error "Donner requires Clang; update CompilerConfig if another compiler is needed."
#endif

static_assert(__clang_major__ >= 17, "Donner requires Clang 17 or newer.");
static_assert(
    __cplusplus >= 202302L,
    "Donner requires C++23 support. Configure builds with -std=c++23 or newer.");

}  // namespace donner

