#pragma once
/// @file
/// gmock matchers and helpers for \c donner::gpu::shader tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>

#include "donner/gpu/shader/ShaderResult.h"

namespace donner::gpu::shader {

/**
 * Matches a \ref ShaderResult that contains a value (no error). On mismatch prints the contained
 * error.
 */
MATCHER(HasShaderResult, "has a result") {
  if (arg.hasError()) {
    *result_listener << "which has error: " << arg.error();
    return false;
  }
  return arg.hasResult();
}

/**
 * Matches a successful \ref ShaderStatus. On mismatch prints the contained error.
 */
MATCHER(IsShaderOk, "is ok") {
  if (arg.hasError()) {
    *result_listener << "which has error: " << arg.error();
    return false;
  }
  return arg.hasResult();
}

/**
 * Matches a \ref ShaderResult whose error message matches \p messageMatcher (string or gmock
 * matcher). On mismatch prints the full error or the absence of one.
 *
 * @param messageMatcher Matcher for the error message.
 */
MATCHER_P(IsShaderError, messageMatcher,
          std::string("is a ShaderError with message ") + testing::PrintToString(messageMatcher)) {
  if (!arg.hasError()) {
    *result_listener << "which has no error";
    return false;
  }
  *result_listener << "which has error: " << arg.error();
  return testing::ExplainMatchResult(messageMatcher, arg.error().message, result_listener);
}

/**
 * Unwraps a \ref ShaderResult, adding a test failure and returning a default-constructed value
 * if it holds an error.
 *
 * @param result Result to unwrap.
 */
template <typename T>
T GetShaderResultOrFail(ShaderResult<T>&& result) {
  if (result.hasError()) {
    ADD_FAILURE() << "Expected a result, got error: " << result.error();
    return T{};
  }
  return std::move(result).result();
}

/**
 * Unwraps a \ref ShaderResult for non-default-constructible types (IrType, IrExpr), adding a
 * test failure and returning \p fallback if it holds an error.
 *
 * @param result Result to unwrap.
 * @param fallback Value returned (with a recorded failure) when \p result holds an error.
 */
template <typename T>
T GetShaderResultOrFail(ShaderResult<T>&& result, T fallback) {
  if (result.hasError()) {
    ADD_FAILURE() << "Expected a result, got error: " << result.error();
    return fallback;
  }
  return std::move(result).result();
}

}  // namespace donner::gpu::shader
