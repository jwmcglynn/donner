#include "donner/base/tests/PackagedResourceTestProvider.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

#include "rules_cc/cc/runfiles/runfiles.h"

namespace donner::tests {
namespace {

ResourceBytes ReadFixture(const std::string& path, size_t expectedSize) {
  if (expectedSize > PackagedResourceSession::kMaximumEncodedBytes) {
    return {};
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  auto bytes = std::make_shared<std::vector<uint8_t>>(expectedSize);
  if (!stream.read(reinterpret_cast<char*>(bytes->data()), bytes->size()) || stream.peek() != EOF) {
    return {};
  }
  return bytes;
}

PackagedResourceProvider FuzzerProvider(const char* executable, std::string* error) {
  if (const char* root = std::getenv("DONNER_TEST_RESOURCE_ROOT")) {
    if (*root == '\0') {
      *error = "DONNER_TEST_RESOURCE_ROOT must name a fixture directory";
      return {};
    }
    return PackagedResourceTestProvider(root);
  }
  using rules_cc::cc::runfiles::Runfiles;
  const std::shared_ptr<Runfiles> runfiles(
      std::getenv("TEST_SRCDIR")
          ? Runfiles::CreateForTest(BAZEL_CURRENT_REPOSITORY, error)
          : Runfiles::Create(executable ? executable : "", BAZEL_CURRENT_REPOSITORY, error));
  if (!runfiles) {
    return {};
  }
  return [runfiles](const PackagedResource& resource) {
    const std::string key = std::string(resource.testRunfilesRoot) + "/" + resource.path;
    return ReadFixture(runfiles->Rlocation(key), resource.encodedSize);
  };
}

}  // namespace

PackagedResourceProvider PackagedResourceTestProvider(std::string root) {
  return [root = std::move(root)](const PackagedResource& resource) {
    const auto path = std::filesystem::path(root) / resource.testRunfilesRoot / resource.path;
    return ReadFixture(path.string(), resource.encodedSize);
  };
}

void InitializeFuzzerPackagedResourceFixtures(const char* executable) {
  std::string error;
  PackagedResourceProvider provider = FuzzerProvider(executable, &error);
  if (provider) {
    if (!SetPackagedResourceSession(
            std::make_shared<PackagedResourceSession>(std::move(provider)))) {
      error = "Cannot replace a fixture session with outstanding borrowed views";
    } else if (PreparePackagedResources(&error)) {
      return;
    }
  }
  std::fprintf(stderr, "Cannot prepare packaged test resources: %s\n", error.c_str());
  std::exit(EXIT_FAILURE);
}

}  // namespace donner::tests
