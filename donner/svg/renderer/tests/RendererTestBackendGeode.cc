#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string_view>

#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeNativeRoot.h"
#ifdef DONNER_GEODE_WGPU_REFERENCE
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#endif
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/text/TextTier.h"

namespace donner::svg {

namespace {

struct SharedGeodeBackendState {
  std::shared_ptr<geode::GeodeDevice> device;
  std::unique_ptr<RendererGeode> renderer;
};

SharedGeodeBackendState& SharedTestBackendState() {
  static SharedGeodeBackendState state;
  return state;
}

/// Returns a process-wide shared GeodeDevice, created on first access.
///
/// Sharing a single device across all test-constructed renderers avoids the
/// Mesa llvmpipe (and Intel ANV) hang caused by accumulating hundreds of
/// WebGPU device creations in a single process - the driver state doesn't
/// reclaim cleanly and the process eventually deadlocks.
std::shared_ptr<geode::GeodeDevice> SharedTestDevice() {
  SharedGeodeBackendState& state = SharedTestBackendState();
  if (!state.device) {
#ifdef DONNER_GEODE_WGPU_REFERENCE
    // The Linux comparison renders the production context and renderer through the wgpu
    // reference's runtime devices.
    auto d = geode::CreateWgpuReferenceContext();
#else
    auto d = geode::GeodeDevice::CreateHeadless();
#endif
    // Wrap the unique_ptr in a shared_ptr for lifetime sharing.
    state.device = std::shared_ptr<geode::GeodeDevice>(std::move(d));
  }

  return state.device;
}

bool GeodeSupportsFeature(RendererBackendFeature feature) {
  switch (feature) {
    // FilterEffects reports `true`: Geode renders filter effects through
    // `GeodeFilterEngine`, so the resvg_test_suite `filters/*` categories,
    // gated by `requireFeature(FilterEffects)`, run and compare on Geode.
    //
    // Text follows the build's text tier, as on TinySkia. `RendererGeode::drawText` fills each
    // glyph outline with exact coverage, while the resvg references sample coverage on four
    // sub-scanlines per pixel and place each horizontal edge on a quarter-pixel boundary; rows
    // where that difference alone exceeds the pixel budget opt into
    // `withGeodeQuarterPixelEdgeCoverage()` in resvg_test_suite.cc.
    case RendererBackendFeature::FilterEffects: return true;
    case RendererBackendFeature::Text: return IsTextBuiltIn();
    case RendererBackendFeature::TextFull: return IsFullTextBuiltIn();
    case RendererBackendFeature::AsciiSnapshot: return true;
  }

  return false;
}

std::unique_ptr<RendererInterface> GeodeCreateInstance(bool verbose) {
  // Callers that want a renderer they own get a fresh one. Now that
  // `GeodeDevice` owns the expensive pipelines (issue #575), this is
  // ~free: the per-instance allocations are just small ECS-side
  // bookkeeping and a handful of wgpu::Texture handles.
  return std::make_unique<RendererGeode>(SharedTestDevice(), verbose);
}

/// Returns a process-wide shared `RendererGeode`. Created on first call,
/// reused for every subsequent render. Sharing eliminates the per-test
/// `RendererGeode` allocation churn that would otherwise accumulate
/// ~2 textures + ~8 buffers per test in wgpu-native's internal tracking -
/// enough to trip the driver's `maxMemoryAllocationCount` on Mesa lavapipe /
/// llvmpipe after a few hundred tests (issue #575). `beginFrame()` fully
/// resets per-frame state, so a shared renderer behaves identically to a
/// fresh one as long as each `draw()` call is self-contained (no carried
/// layer / clip / filter stack), which the image-comparison fixture
/// guarantees. The gtest environment below destroys this renderer before
/// LeakSanitizer performs its process-exit leak check.
RendererGeode& SharedTestRenderer(bool verbose = false) {
  SharedGeodeBackendState& state = SharedTestBackendState();
  if (!state.renderer) {
    state.renderer = std::make_unique<RendererGeode>(SharedTestDevice(), verbose);
  }

  return *state.renderer;
}

void ResetSharedTestBackendState() {
  SharedGeodeBackendState& state = SharedTestBackendState();
  state.renderer.reset();
  state.device.reset();
}

class GeodeBackendEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    const char* required = std::getenv("DONNER_REQUIRE_WGPU_REFERENCE");
    if (required == nullptr || std::string_view(required) != "1") {
      return;
    }
    std::shared_ptr<geode::GeodeDevice> device = SharedTestDevice();
    if (device == nullptr) {
      FAIL() << "the resvg wgpu reference could not create its GPU device";
    }
#ifdef DONNER_GEODE_WGPU_REFERENCE
    const geode::GpuBackendKind kind = device->physicalDeviceOwner()->root().capabilities().backend;
    if (kind != geode::GpuBackendKind::External) {
      FAIL() << "the resvg wgpu reference renders through " << geode::GpuBackendKindName(kind)
             << " instead of the wgpu reference's external runtime devices";
    }
#else
    FAIL() << "DONNER_REQUIRE_WGPU_REFERENCE is set, but this binary does not link the wgpu "
              "reference";
#endif

    size_t geodeCases = 0;
    size_t tinyCases = 0;
    const testing::UnitTest* tests = testing::UnitTest::GetInstance();
    for (int suiteIndex = 0; suiteIndex < tests->total_test_suite_count(); ++suiteIndex) {
      const testing::TestSuite* suite = tests->GetTestSuite(suiteIndex);
      for (int caseIndex = 0; caseIndex < suite->total_test_count(); ++caseIndex) {
        const std::string_view name = suite->GetTestInfo(caseIndex)->name();
        geodeCases += name.ends_with("_GeodeGolden") ? 1u : 0u;
        tinyCases += name.ends_with("_TinyGolden") ? 1u : 0u;
      }
    }
    // Registered cases include disabled cases; a corpus change needs explicit review.
    constexpr size_t kReviewedGeodeGoldenCases = 1728;
    if (geodeCases != kReviewedGeodeGoldenCases || tinyCases != geodeCases) {
      FAIL() << "resvg wgpu reference case census changed: GeodeGolden=" << geodeCases
             << ", TinyGolden=" << tinyCases << ", reviewed=" << kReviewedGeodeGoldenCases;
    }
  }

  void TearDown() override {
    ResetSharedTestBackendState();
#ifdef DONNER_GEODE_WGPU_REFERENCE
    // A headless context selects a native backend, so one created anywhere in this binary would
    // compare that backend's pixels instead of the reference's.
    EXPECT_EQ(geode::GeodeDevice::headlessCreationCountForTesting(), 0)
        << "a native headless Geode context was created in the wgpu reference comparison";
#endif
  }
};

[[maybe_unused]] const ::testing::Environment* const geodeBackendEnvironment =
    ::testing::AddGlobalTestEnvironment(new GeodeBackendEnvironment);

/// Reads back the frame `renderer` just drew, once its GPU work has finished.
///
/// The snapshot's deadline is a latency budget that also counts the time spent waiting for the
/// frame's own GPU work. A heavy frame on a slow or busy software rasterizer can take longer than
/// that budget while it is still making progress, and the snapshot would then come back empty even
/// though nothing is wrong. Waiting for the submitted work first, bounded only by lack of progress,
/// leaves the snapshot budget for the readback itself; a device that really stops is still caught
/// by the production stall bound.
RendererBitmap SnapshotAfterSubmittedWork(RendererGeode& renderer) {
  const geode::GpuWaitResult wait = renderer.waitForSubmittedWork();
  if (wait == geode::GpuWaitResult::TimedOut) {
    ADD_FAILURE() << "the frame's GPU work stopped making progress before it finished";
  }
  return renderer.takeSnapshot();
}

RendererBitmap GeodeRender(SVGDocument& document, bool verbose) {
  RendererGeode& renderer = SharedTestRenderer(verbose);
  renderer.setAntialias(true);
  renderer.draw(document);
  return SnapshotAfterSubmittedWork(renderer);
}

RendererBitmap GeodeRenderForAscii(SVGDocument& document) {
  RendererGeode& renderer = SharedTestRenderer();
  renderer.setAntialias(false);
  renderer.draw(document);
  return SnapshotAfterSubmittedWork(renderer);
}

}  // namespace

void RegisterGeodeBackend() {
  RegisterBackendOps(RendererBackend::Geode, BackendOps{
                                                 .render = &GeodeRender,
                                                 .renderForAscii = &GeodeRenderForAscii,
                                                 .supportsFeature = &GeodeSupportsFeature,
                                                 .createInstance = &GeodeCreateInstance,
                                             });
}

}  // namespace donner::svg
