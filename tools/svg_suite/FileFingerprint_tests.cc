#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>

#include "tools/svg_suite/Provenance.h"

namespace donner::svg_suite {
namespace {
using testing::HasSubstr;

class FileFingerprintTest : public testing::Test {
protected:
  void SetUp() override {
    static std::atomic<unsigned> serial{0};
    const char* base = std::getenv("TEST_TMPDIR");
    directory_ = (base ? std::filesystem::path(base) : std::filesystem::temp_directory_path()) /
                 ("svg-fingerprint-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                  "-" + std::to_string(serial++));
    std::filesystem::create_directories(directory_ / "corpus/tests");
    write("renderer", "trusted executable fixture");
    write("corpus/tests/a.svg", "<svg/>");
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  void write(const std::filesystem::path& relative, std::string_view text) {
    std::ofstream output(directory_ / relative, std::ios::binary);
    output << text;
  }

  std::filesystem::path directory_;
};

TEST_F(FileFingerprintTest, InvalidPathIsRejectedBeforeFilesystemAccess) {
  const auto path = directory_ / std::string("bad-").append(1, char(0xFF));
  const auto result = FingerprintFile(path, 1024);
  EXPECT_FALSE(result.fingerprint);
  EXPECT_THAT(result.error, HasSubstr("UTF-8"));
}

TEST_F(FileFingerprintTest, StandardSha256KnownVectorsAndStreaming) {
  const std::vector<std::pair<std::string, std::string>> vectors = {
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
      {std::string(1000000, 'a'),
       "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}};
  for (const auto& [input, expected] : vectors) {
    write("vector", input);
    const auto result = FingerprintFile(directory_ / "vector", 1000000);
    ASSERT_TRUE(result.fingerprint) << result.error;
    EXPECT_EQ(result.fingerprint->sha256, expected);
    EXPECT_EQ(result.fingerprint->bytes, input.size());
  }
}
TEST_F(FileFingerprintTest, HashingRejectsNonregularMissingAndOversizedSources) {
  EXPECT_FALSE(FingerprintFile(directory_ / "corpus", 1024).fingerprint);
  EXPECT_FALSE(FingerprintFile(directory_ / "missing", 1024).fingerprint);
  EXPECT_FALSE(FingerprintFile(directory_ / "renderer", 1).fingerprint);
}
TEST_F(FileFingerprintTest, SameLengthSourceMutationIsDetected) {
  const auto snapshot = FingerprintFile(directory_ / "corpus/tests/a.svg", 1024);
  ASSERT_TRUE(snapshot.fingerprint);
  write("corpus/tests/a.svg", "<svg >");
  EXPECT_THAT(VerifyFile(directory_ / "corpus/tests/a.svg", *snapshot.fingerprint),
              HasSubstr("changed"));
}

TEST_F(FileFingerprintTest, SnapshotLookupNormalizesPathsAndRejectsMissingEntries) {
  const auto path = directory_ / "corpus/tests/a.svg";
  const auto snapshot = FingerprintFile(path, 1024);
  ASSERT_TRUE(snapshot.fingerprint);
  RunProvenance run;
  run.files.emplace(path, *snapshot.fingerprint);
  const auto* stored = FindFingerprint(run, directory_ / "corpus/tests/../tests/a.svg");
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(*stored, *snapshot.fingerprint);
  EXPECT_EQ(FindFingerprint(run, directory_ / "missing"), nullptr);
}

}  // namespace
}  // namespace donner::svg_suite
