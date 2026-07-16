#pragma once
/// @file
/// gmock matchers and fixtures for \c donner::gpu model tests.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>

#include "donner/gpu/GpuResult.h"

namespace donner::gpu {

/**
 * Matches a \ref Result that contains a value (no error). On mismatch prints the contained
 * error.
 */
MATCHER(HasResult, "has a result") {
  if (arg.hasError()) {
    *result_listener << "which has error: " << arg.error();
    return false;
  }
  return arg.hasResult();
}

/**
 * Matches a successful \ref Status. On mismatch prints the contained error.
 */
MATCHER(IsOk, "is ok") {
  if (arg.hasError()) {
    *result_listener << "which has error: " << arg.error();
    return false;
  }
  return arg.hasResult();
}

/**
 * Matches a \ref Result or \ref GpuError whose error type equals \p expectedType. On mismatch
 * prints the full error (type and message) or the absence of one.
 *
 * @param expectedType Expected \ref GpuErrorType.
 */
MATCHER_P(IsGpuError, expectedType,
          std::string("is a GpuError of type ") + testing::PrintToString(expectedType)) {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  if constexpr (std::is_same_v<ArgType, GpuError>) {
    *result_listener << "which is: " << arg;
    return arg.type == expectedType;
  } else {
    if (!arg.hasError()) {
      *result_listener << "which has no error";
      return false;
    }
    *result_listener << "which has error: " << arg.error();
    return arg.error().type == expectedType;
  }
}

/**
 * Matches a \ref Result or \ref GpuError whose error type equals \p expectedType and whose
 * message matches \p messageMatcher (string or gmock matcher).
 *
 * @param expectedType Expected \ref GpuErrorType.
 * @param messageMatcher Matcher for the error message.
 */
MATCHER_P2(IsGpuErrorWithMessage, expectedType, messageMatcher,
           std::string("is a GpuError of type ") + testing::PrintToString(expectedType) +
               " with message " + testing::PrintToString(messageMatcher)) {
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  const GpuError* error = nullptr;
  if constexpr (std::is_same_v<ArgType, GpuError>) {
    error = &arg;
  } else {
    if (!arg.hasError()) {
      *result_listener << "which has no error";
      return false;
    }
    error = &arg.error();
  }

  *result_listener << "which is: " << *error;
  return error->type == expectedType &&
         testing::ExplainMatchResult(messageMatcher, error->message, result_listener);
}

/**
 * Unwraps a \ref Result, adding a test failure and returning a default-constructed value if it
 * holds an error.
 *
 * @param result Result to unwrap.
 */
template <typename T>
T GetResultOrFail(Result<T>&& result) {
  if (result.hasError()) {
    ADD_FAILURE() << "Expected a result, got error: " << result.error();
    return T{};
  }
  return std::move(result).result();
}

}  // namespace donner::gpu
