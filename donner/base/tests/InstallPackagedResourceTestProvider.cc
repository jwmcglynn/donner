/// @file
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "donner/base/PackagedResource.h"

namespace donner {
namespace {
const bool kTestResourcesInstalled = [] {
  const char* root = std::getenv("TEST_SRCDIR");
  if (!root) {
    return false;
  }
  SetPackagedResourceSession(std::make_shared<PackagedResourceSession>(
      [root = std::string(root)](const PackagedResource& resource) -> ResourceBytes {
        const auto path = std::filesystem::path(root) / resource.testRunfilesRoot / resource.path;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
          return {};
        }
        auto bytes = std::make_shared<std::vector<uint8_t>>(resource.encodedSize);
        if (!stream.read(reinterpret_cast<char*>(bytes->data()), bytes->size()) ||
            stream.peek() != EOF) {
          return {};
        }
        return bytes;
      }));
  return true;
}();
}  // namespace
}  // namespace donner
