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
#include <sstream>
#include <string>

#include "donner/base/tests/Runfiles.h"
#include "embed_resources/SnapshotUnpremultiplyWgsl.h"

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

TEST(GeneratedShaderArtifacts, EmbeddedSnapshotUnpremultiplyMatchesTheCommittedGolden) {
  const std::string embedded(
      reinterpret_cast<const char*>(donner::embedded::kSnapshotUnpremultiplyWgsl.data()),
      donner::embedded::kSnapshotUnpremultiplyWgsl.size());
  const std::string golden =
      ReadRunfile("donner/gpu/shader/tests/testdata/snapshot_unpremultiply.wgsl");

  ASSERT_FALSE(golden.empty()) << "the committed golden must be readable";
  // The bytes the binary embeds, not a re-emission of them: this compares the actual shipped
  // artifact against the reviewed one.
  EXPECT_EQ(embedded, golden)
      << "the embedded shader and its committed golden have diverged; regenerate the golden if "
         "the emitter changed deliberately";
}

}  // namespace
}  // namespace donner::geode
