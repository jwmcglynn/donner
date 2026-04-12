/// @file
///
/// Tests for `SvgSource` — the URI/path resolver that feeds the sandbox.
/// Covers the scheme classifier, absolute/relative path resolution, the size
/// cap, and the error surface for missing files, wrong file types, and
/// unsupported schemes.

#include "donner/editor/sandbox/SvgSource.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace donner::editor::sandbox {
namespace {

class SvgSourceTest : public ::testing::Test {
protected:
  void SetUp() override {
    const std::string tmpRoot = ::testing::TempDir();
    // testing::TempDir() may be shared across tests — make a per-test subdir.
    tmpDir_ = std::filesystem::path(tmpRoot) /
              ("svg_source_test_" + std::to_string(::rand()));
    std::filesystem::create_directories(tmpDir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(tmpDir_, ec);
  }

  std::filesystem::path WriteFile(const std::string& name, std::string_view contents) {
    const auto path = tmpDir_ / name;
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path;
  }

  std::filesystem::path tmpDir_;
};

constexpr std::string_view kMiniSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"/>)";

TEST_F(SvgSourceTest, FetchesAbsolutePath) {
  const auto path = WriteFile("mini.svg", kMiniSvg);
  SvgSource source;
  const auto result = source.fetch(path.string());
  ASSERT_EQ(result.status, SvgFetchStatus::kOk) << result.diagnostics;
  EXPECT_EQ(result.bytes.size(), kMiniSvg.size());
}

TEST_F(SvgSourceTest, FetchesFileScheme) {
  const auto path = WriteFile("mini.svg", kMiniSvg);
  SvgSource source;
  const auto result = source.fetch("file://" + path.string());
  ASSERT_EQ(result.status, SvgFetchStatus::kOk) << result.diagnostics;
  EXPECT_EQ(result.bytes.size(), kMiniSvg.size());
}

TEST_F(SvgSourceTest, FetchesRelativePathAgainstBaseDirectory) {
  WriteFile("icon.svg", kMiniSvg);

  SvgSourceOptions opts;
  opts.baseDirectory = tmpDir_;
  SvgSource source(opts);

  const auto result = source.fetch("icon.svg");
  ASSERT_EQ(result.status, SvgFetchStatus::kOk) << result.diagnostics;
  EXPECT_EQ(result.bytes.size(), kMiniSvg.size());
}

// ---------- HTTP(S) scheme routing ----------

TEST_F(SvgSourceTest, HttpsSchemeAttemptsNetworkFetch) {
  // A fetch to a known-bad host should produce kNetworkError, *not*
  // kSchemeNotSupported — proving the scheme classifier routes to the
  // network path.
  SvgSourceOptions opts;
  opts.httpTimeoutSeconds = 2;  // Keep the test fast.
  SvgSource source(opts);
  const auto result = source.fetch("https://localhost:1/does_not_exist.svg");
  EXPECT_EQ(result.status, SvgFetchStatus::kNetworkError);
  EXPECT_TRUE(result.bytes.empty());
  EXPECT_FALSE(result.diagnostics.empty());
}

TEST_F(SvgSourceTest, HttpSchemeAttemptsNetworkFetch) {
  SvgSourceOptions opts;
  opts.httpTimeoutSeconds = 2;
  SvgSource source(opts);
  const auto result = source.fetch("http://localhost:1/does_not_exist.svg");
  EXPECT_EQ(result.status, SvgFetchStatus::kNetworkError);
  EXPECT_TRUE(result.bytes.empty());
  EXPECT_FALSE(result.diagnostics.empty());
}

TEST_F(SvgSourceTest, FtpSchemeStillNotSupported) {
  SvgSource source;
  const auto result = source.fetch("ftp://example.com/icon.svg");
  EXPECT_EQ(result.status, SvgFetchStatus::kSchemeNotSupported);
  EXPECT_TRUE(result.bytes.empty());
  EXPECT_FALSE(result.diagnostics.empty());
}

TEST_F(SvgSourceTest, HttpsResolvedPathIsEmpty) {
  // Network fetches should leave resolvedPath empty — there's no filesystem
  // path to report.
  SvgSourceOptions opts;
  opts.httpTimeoutSeconds = 2;
  SvgSource source(opts);
  const auto result = source.fetch("https://localhost:1/icon.svg");
  EXPECT_TRUE(result.resolvedPath.empty());
}

TEST_F(SvgSourceTest, HttpsRejectsShellMetacharacters) {
  SvgSource source;
  const auto result = source.fetch("https://example.com/icon.svg; rm -rf /");
  // The space and semicolon should be rejected.
  EXPECT_EQ(result.status, SvgFetchStatus::kInvalidUri);
}

TEST_F(SvgSourceTest, MissingFileIsNotFound) {
  SvgSource source;
  const auto result = source.fetch((tmpDir_ / "does_not_exist.svg").string());
  EXPECT_EQ(result.status, SvgFetchStatus::kNotFound);
  EXPECT_TRUE(result.bytes.empty());
}

TEST_F(SvgSourceTest, DirectoryIsNotRegularFile) {
  SvgSource source;
  const auto result = source.fetch(tmpDir_.string());
  EXPECT_EQ(result.status, SvgFetchStatus::kNotRegularFile);
  EXPECT_TRUE(result.bytes.empty());
}

TEST_F(SvgSourceTest, RejectsOversizedFile) {
  const auto path = WriteFile("big.svg", std::string(1024, 'x'));

  SvgSourceOptions opts;
  opts.maxFileBytes = 512;
  SvgSource source(opts);

  const auto result = source.fetch(path.string());
  EXPECT_EQ(result.status, SvgFetchStatus::kTooLarge);
  EXPECT_TRUE(result.bytes.empty());
}

TEST_F(SvgSourceTest, EmptyUriIsInvalid) {
  SvgSource source;
  const auto result = source.fetch("");
  EXPECT_EQ(result.status, SvgFetchStatus::kInvalidUri);
}

TEST_F(SvgSourceTest, ResolvedPathIsCanonicalizedForRelative) {
  WriteFile("indirect.svg", kMiniSvg);
  SvgSourceOptions opts;
  opts.baseDirectory = tmpDir_;
  SvgSource source(opts);

  const auto result = source.fetch("./indirect.svg");
  ASSERT_EQ(result.status, SvgFetchStatus::kOk);
  EXPECT_TRUE(result.resolvedPath.is_absolute());
  EXPECT_EQ(result.resolvedPath.filename(), "indirect.svg");
}

}  // namespace
}  // namespace donner::editor::sandbox
