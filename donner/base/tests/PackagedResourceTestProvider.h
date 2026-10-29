#pragma once
/// @file

#include <string>

#include "donner/base/PackagedResource.h"

namespace donner::tests {

/// Explicit fixture provider; the resource session still validates compiled identity and budgets.
/// @param root Test runfiles root containing the generated resource trees.
PackagedResourceProvider PackagedResourceTestProvider(std::string root);

/// Prepare authenticated fixtures once before libFuzzer processes any input; exits on failure.
/// Uses DONNER_TEST_RESOURCE_ROOT when explicitly supplied, otherwise Bazel's runfiles resolver.
/// @param executable The fuzzer's argv[0], used by Bazel to locate direct-run fixtures.
void InitializeFuzzerPackagedResourceFixtures(const char* executable);

}  // namespace donner::tests
