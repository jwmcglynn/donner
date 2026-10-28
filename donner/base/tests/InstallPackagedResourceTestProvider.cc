/// @file
#include <cstdlib>

#include "donner/base/PackagedResource.h"
#include "donner/base/tests/PackagedResourceTestProvider.h"

namespace donner {
namespace {
const bool kTestResourcesInstalled = [] {
  const char* root = std::getenv("TEST_SRCDIR");
  if (!root) {
    return false;
  }
  return SetPackagedResourceSession(
      std::make_shared<PackagedResourceSession>(tests::PackagedResourceTestProvider(root)));
}();
}  // namespace
}  // namespace donner
