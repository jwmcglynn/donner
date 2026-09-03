/// @file
/// The build-time emitted shader artifacts must equal the committed goldens.
///
/// Two artifacts are produced from one IR program: the golden committed under the shader tests,
/// which is what a reviewer reads in a diff, and the file the genrule emits, which is what the
/// binary actually embeds. They are the same bytes by construction - both come from one EmitWgsl
/// on one module - but "by construction" is an argument, and an argument is not a test. If the
/// generator and the golden ever came from different code paths, or a golden were hand-edited,
/// only this comparison would notice.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <span>
#include <sstream>
#include <string>

#include "donner/base/tests/Runfiles.h"
#include "embed_resources/ColorSpaceConvertWgsl.h"
#include "embed_resources/FilterColorMatrixWgsl.h"
#include "embed_resources/FloodWgsl.h"
#include "embed_resources/OffsetWgsl.h"
#include "embed_resources/SnapshotUnpremultiplyWgsl.h"
#include "embed_resources/SubregionClipWgsl.h"

namespace donner::geode {
namespace {

/// Reads a runfile in full.
/// @param path Runfiles-relative path.
std::string ReadRunfile(const std::string& path) {
  const std::string resolved = donner::Runfiles::instance().Rlocation(path);
  std::ifstream stream(resolved, std::ios::binary);
  EXPECT_TRUE(stream.good()) << "failed to open " << resolved;
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

/// The bytes an embedded artifact carries, as a string.
/// @param resource Embedded resource span produced by the package's genrule.
std::string EmbeddedBytes(std::span<const unsigned char> resource) {
  return std::string(reinterpret_cast<const char*>(resource.data()), resource.size());
}

TEST(GeneratedShaderArtifacts, EmbeddedFilterColorMatrixMatchesTheCommittedGolden) {
  const std::string embedded = EmbeddedBytes(donner::embedded::kFilterColorMatrixWgsl);
  const std::string golden =
      ReadRunfile("donner/gpu/shader/tests/testdata/filter_color_matrix.wgsl");

  ASSERT_FALSE(golden.empty()) << "the committed golden must be readable";
  EXPECT_EQ(embedded, golden)
      << "the embedded shader and its committed golden have diverged; regenerate the golden if "
         "the emitter changed deliberately";
}

TEST(GeneratedShaderArtifacts, EmbeddedFloodMatchesTheCommittedGolden) {
  const std::string embedded = EmbeddedBytes(donner::embedded::kFloodWgsl);
  const std::string golden = ReadRunfile("donner/gpu/shader/tests/testdata/flood.wgsl");

  ASSERT_FALSE(golden.empty()) << "the committed golden must be readable";
  EXPECT_EQ(embedded, golden)
      << "the embedded shader and its committed golden have diverged; regenerate the golden if "
         "the emitter changed deliberately";
}

TEST(GeneratedShaderArtifacts, EmbeddedColorSpaceConvertMatchesTheCommittedGolden) {
  const std::string embedded = EmbeddedBytes(donner::embedded::kColorSpaceConvertWgsl);
  const std::string golden =
      ReadRunfile("donner/gpu/shader/tests/testdata/color_space_convert.wgsl");

  ASSERT_FALSE(golden.empty()) << "the committed golden must be readable";
  EXPECT_EQ(embedded, golden)
      << "the embedded shader and its committed golden have diverged; regenerate the golden if "
         "the emitter changed deliberately";
}

TEST(GeneratedShaderArtifacts, EmbeddedOffsetMatchesTheCommittedGolden) {
  const std::string embedded = EmbeddedBytes(donner::embedded::kOffsetWgsl);
  const std::string golden = ReadRunfile("donner/gpu/shader/tests/testdata/offset.wgsl");

  ASSERT_FALSE(golden.empty()) << "the committed golden must be readable";
  EXPECT_EQ(embedded, golden)
      << "the embedded shader and its committed golden have diverged; regenerate the golden if "
         "the emitter changed deliberately";
}

TEST(GeneratedShaderArtifacts, EmbeddedSnapshotUnpremultiplyMatchesTheCommittedGolden) {
  const std::string embedded = EmbeddedBytes(donner::embedded::kSnapshotUnpremultiplyWgsl);
  const std::string golden =
      ReadRunfile("donner/gpu/shader/tests/testdata/snapshot_unpremultiply.wgsl");

  ASSERT_FALSE(golden.empty()) << "the committed golden must be readable";
  // The bytes the binary embeds, not a re-emission of them: this compares the actual shipped
  // artifact against the reviewed one.
  EXPECT_EQ(embedded, golden)
      << "the embedded shader and its committed golden have diverged; regenerate the golden if "
         "the emitter changed deliberately";
}

TEST(GeneratedShaderArtifacts, EmbeddedSubregionClipMatchesTheCommittedGolden) {
  const std::string embedded = EmbeddedBytes(donner::embedded::kSubregionClipWgsl);
  const std::string golden = ReadRunfile("donner/gpu/shader/tests/testdata/subregion_clip.wgsl");

  ASSERT_FALSE(golden.empty()) << "the committed golden must be readable";
  EXPECT_EQ(embedded, golden)
      << "the embedded shader and its committed golden have diverged; regenerate the golden if "
         "the emitter changed deliberately";
}

}  // namespace
}  // namespace donner::geode
