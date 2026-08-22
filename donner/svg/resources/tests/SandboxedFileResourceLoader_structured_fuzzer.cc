#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "donner/svg/resources/SandboxedFileResourceLoader.h"

namespace donner::svg {
namespace {

class SandboxFixture {
public:
  SandboxFixture() {
    const char* testTmpDir = std::getenv("TEST_TMPDIR");
    const std::filesystem::path temporaryRoot =
        testTmpDir != nullptr ? testTmpDir : std::filesystem::temp_directory_path();
    base_ = temporaryRoot /
            ("donner-resource-fuzzer-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    root_ = base_ / "root";
    sibling_ = base_ / "root-sibling";
    std::filesystem::create_directories(root_ / "subdir");
    std::filesystem::create_directories(sibling_);
    constexpr std::array<uint8_t, 6> kInside = {'i', 'n', 's', 'i', 'd', 'e'};
    WriteFile(root_ / "inside.bin", kInside);
    WriteFile(root_ / "large.bin", std::vector<uint8_t>(65, 0x41));
    WriteFile(sibling_ / "secret.bin", kSecret);

    std::error_code symlinkError;
    std::filesystem::create_directory_symlink(sibling_, root_ / "escape-link", symlinkError);
    hasSymlink_ = !symlinkError;
    loader_ = std::make_unique<SandboxedFileResourceLoader>(root_, root_ / "document.svg", 64);
  }

  ~SandboxFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(base_, ignored);
  }

  ResourceLoaderInterface& loader() { return *loader_; }
  bool hasSymlink() const { return hasSymlink_; }

  static constexpr std::array<uint8_t, 6> kSecret = {0x53, 0x45, 0x43, 0x52, 0x45, 0x54};

private:
  static void WriteFile(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  }

  std::filesystem::path base_;
  std::filesystem::path root_;
  std::filesystem::path sibling_;
  bool hasSymlink_ = false;
  std::unique_ptr<SandboxedFileResourceLoader> loader_;
};

void CheckResult(const std::variant<std::vector<uint8_t>, ResourceLoaderError>& result) {
  if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&result)) {
    if (bytes->size() > 64 || std::ranges::equal(*bytes, SandboxFixture::kSecret)) {
      std::abort();
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static SandboxFixture fixture;
  const std::string_view input(reinterpret_cast<const char*>(data), size);  // NOLINT
  FuzzedDataProvider provider(data, size);

  CheckResult(fixture.loader().fetchExternalResource("inside.bin"));
  CheckResult(fixture.loader().fetchExternalResource("large.bin"));
  CheckResult(fixture.loader().fetchExternalResource("../root-sibling/secret.bin"));
  if (fixture.hasSymlink()) {
    CheckResult(fixture.loader().fetchExternalResource("escape-link/secret.bin"));
  }
  if (input.starts_with("embedded-null-path")) {
    constexpr char kEmbeddedNullPath[] = "..\0ignored/root-sibling/secret.bin";
    const std::string path(kEmbeddedNullPath, sizeof(kEmbeddedNullPath) - 1);
    const auto result = fixture.loader().fetchExternalResource(path);
    CheckResult(result);
    const auto* error = std::get_if<ResourceLoaderError>(&result);
    if (error == nullptr || *error != ResourceLoaderError::SandboxViolation) {
      std::abort();
    }
  }
  if (input.starts_with("path-representation-bounds")) {
    std::string path;
    for (std::size_t i = 0; i <= SandboxedFileResourceLoader::kMaximumPathComponents; ++i) {
      path += "a/";
    }
    const auto result = fixture.loader().fetchExternalResource(path);
    const auto* error = std::get_if<ResourceLoaderError>(&result);
    if (error == nullptr || *error != ResourceLoaderError::SandboxViolation) {
      std::abort();
    }
  }
  if (input.starts_with("invalid-utf8-path")) {
    constexpr char kInvalidUtf8Path[] = "invalid-\xED\xA0\x80.bin";
    const auto result = fixture.loader().fetchExternalResource(
        std::string_view(kInvalidUtf8Path, sizeof(kInvalidUtf8Path) - 1));
    const auto* error = std::get_if<ResourceLoaderError>(&result);
    if (error == nullptr || *error != ResourceLoaderError::SandboxViolation) {
      std::abort();
    }
  }
  CheckResult(fixture.loader().fetchExternalResource(provider.ConsumeRandomLengthString(256)));
  return 0;
}

}  // namespace donner::svg
