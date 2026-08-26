/// @file
/// Check mode for the frozen pixel baselines: re-renders every corpus scene through the current
/// production renderer and requires identity against the committed PNGs.
///
/// The comparison is scoped to the adapter the freeze was captured on. Two GPUs that both
/// implement the same shaders can round a covered edge texel differently, so comparing a frozen
/// capture against a different adapter reports a difference the test cannot attribute to a
/// regression. This suite therefore refuses that comparison and names the environment that is
/// missing a baseline, instead of failing on hardware it was never frozen for.
///
/// Refusing is not the same as passing. On a developer machine the run leaves a capture behind and
/// skips, which is the useful answer on new hardware. On an automated lane it fails, because a
/// suite that skips every case still reports the target as passing, and a gate that compares
/// nothing while looking green is the failure mode this whole freeze exists to prevent.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/baseline/FrozenBaselinePolicy.h"
#include "donner/gpu/baseline/WgpuBaselineCapture.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::gpu::baseline {
namespace {

constexpr const char* kBaselinesRunfileDir = "donner/gpu/baseline/baselines";

std::string ProvenancePathFor(const std::string& slug) {
  return Runfiles::instance().Rlocation(std::string(kBaselinesRunfileDir) + "/" + slug +
                                        "/capture_provenance.txt");
}

std::map<std::string, std::string> ReadProvenance(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  std::map<std::string, std::string> values;
  if (!stream.good()) {
    return values;
  }
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const size_t separator = line.find(':');
    if (separator == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, separator);
    std::string value = line.substr(separator + 1);
    const size_t valueStart = value.find_first_not_of(' ');
    values[key] = valueStart == std::string::npos ? std::string() : value.substr(valueStart);
  }
  return values;
}

bool ProvenanceListsScene(const std::string& capturedScenes, std::string_view sceneName) {
  std::istringstream stream(capturedScenes);
  std::string entry;
  while (std::getline(stream, entry, ',')) {
    if (entry == sceneName) {
      return true;
    }
  }
  return false;
}

/// Where a run without a committed baseline for its adapter leaves the capture it just took, so
/// adding that environment is a matter of collecting the artifact rather than finding a machine.
std::filesystem::path UndeclaredOutputDir() {
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR"); dir != nullptr) {
    return dir;
  }
  return std::filesystem::temp_directory_path();
}

/// One production device serves every scene: creating it compiles all the shared pipelines, which
/// costs far more than any single capture.
WgpuBaselineCapturer* SharedCapturer() {
  static std::unique_ptr<WgpuBaselineCapturer> capturer = WgpuBaselineCapturer::Create();
  return capturer.get();
}

svg::RendererBitmap ToBitmap(std::vector<uint8_t> pixels) {
  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(static_cast<int>(kCorpusSize), static_cast<int>(kCorpusSize));
  bitmap.pixels = std::move(pixels);
  bitmap.rowBytes = static_cast<int>(kCorpusSize) * 4;
  bitmap.alphaType = svg::AlphaType::Premultiplied;
  return bitmap;
}

/// True when the scene has at least one path the encoder admitted, so a fully transparent
/// capture would be a rendering failure rather than the frozen expectation.
bool SceneDrawsSomething(std::string_view sceneName) {
  for (const SceneCounters& scene : ComputeCorpusCounters()) {
    if (scene.name != sceneName) {
      continue;
    }
    for (const PathCounters& path : scene.paths) {
      if (path.outcome == "Ready") {
        return true;
      }
    }
  }
  return false;
}

size_t CountOpaqueEnoughPixels(const std::vector<uint8_t>& pixels) {
  size_t count = 0;
  for (size_t i = 3; i < pixels.size(); i += 4) {
    if (pixels[i] != 0) {
      ++count;
    }
  }
  return count;
}

std::vector<std::string> PixelCorpusSceneNames() {
  std::vector<std::string> names;
  for (const CorpusScene& scene : Corpus()) {
    if (scene.capturesPixels) {
      names.emplace_back(scene.name);
    }
  }
  return names;
}

class FrozenPixelBaselineTest : public testing::TestWithParam<std::string> {
protected:
  void SetUp() override {
    capturer_ = SharedCapturer();
    if (capturer_ == nullptr) {
      handleMissingAdapter();
      return;
    }

    slug_ = EnvironmentSlug(capturer_->environment());
    provenance_ = ReadProvenance(ProvenancePathFor(slug_));
    if (provenance_.empty()) {
      handleUnbaselinedEnvironment();
      return;
    }
  }

  /// Reports a run that could not create a device at all. Locally that is a skip; on an
  /// automated lane it is a failure, because a lane that selected this target and then produced
  /// no device has disabled the comparison rather than passed it.
  void handleMissingAdapter() {
    const MissingComparisonDisposition disposition =
        DispositionForMissingAdapter(RunningUnderContinuousIntegration());
    const std::string message = NoAdapterMessage("the frozen pixel check", disposition);
    if (disposition == MissingComparisonDisposition::FailClosed) {
      FAIL() << message;
    }
    GTEST_SKIP() << message;
  }

  /// Freezes what this adapter renders into the test's undeclared outputs, then either skips with
  /// the directory to commit or fails with it, depending on where this is running. A frozen
  /// capture from another adapter is not a usable expectation here, so comparing anyway is not an
  /// option; the choice is only between handing the capture to a person and refusing to report a
  /// gate that did not run as a pass.
  void handleUnbaselinedEnvironment() {
    const CaptureEnvironment& live = capturer_->environment();
    std::filesystem::path written;
    const std::string captureError =
        WriteFrozenBaselineSet(*capturer_, UndeclaredOutputDir(), "unknown", "unknown", &written);
    const MissingComparisonDisposition disposition =
        DispositionForUnbaselinedAdapter(RunningUnderContinuousIntegration());
    const std::string message = UnbaselinedAdapterMessage(
        live.adapterName, live.adapterBackend, slug_, written.string(), captureError, disposition);

    if (disposition == MissingComparisonDisposition::FailClosed) {
      FAIL() << message;
    }
    GTEST_SKIP() << message;
  }

  WgpuBaselineCapturer* capturer_ = nullptr;
  std::string slug_;
  std::map<std::string, std::string> provenance_;
};

TEST_P(FrozenPixelBaselineTest, MatchesFrozenCapture) {
  const std::string& sceneName = GetParam();
  ASSERT_TRUE(ProvenanceListsScene(provenance_["capturedScenes"], sceneName))
      << "the capture provenance does not list " << sceneName
      << "; re-run the capture so the frozen set covers the whole corpus";

  std::vector<uint8_t> pixels;
  const std::string error = CaptureNamedScene(*capturer_, sceneName, pixels);
  ASSERT_THAT(error, testing::IsEmpty());

  // A scene that lost its geometry would otherwise pass by matching an equally blank golden.
  if (SceneDrawsSomething(sceneName)) {
    EXPECT_GT(CountOpaqueEnoughPixels(pixels), 0u)
        << sceneName << " has admitted geometry but rendered nothing";
  }

  editor::tests::CompareBitmapToGolden(
      ToBitmap(std::move(pixels)),
      std::string(kBaselinesRunfileDir) + "/" + slug_ + "/" + sceneName + ".png",
      "frozen_baseline_" + slug_ + "_" + sceneName, editor::tests::PixelmatchIdentityParams());
}

INSTANTIATE_TEST_SUITE_P(Corpus, FrozenPixelBaselineTest,
                         testing::ValuesIn(PixelCorpusSceneNames()),
                         [](const testing::TestParamInfo<std::string>& info) {
                           return info.param;
                         });

}  // namespace
}  // namespace donner::gpu::baseline
