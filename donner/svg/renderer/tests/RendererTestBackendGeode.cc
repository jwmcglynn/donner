#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string_view>

#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#ifdef DONNER_GEODE_WGPU_REFERENCE
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#else
#include "donner/svg/renderer/geode/GeodeNativeRoot.h"
#endif
#include "donner/svg/renderer/tests/RendererTestBackend.h"

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
    auto d = geode::GeodeDevice::CreateHeadless();
    // Wrap the unique_ptr in a shared_ptr for lifetime sharing.
    state.device = std::shared_ptr<geode::GeodeDevice>(std::move(d));
  }

  return state.device;
}

bool GeodeSupportsFeature(RendererBackendFeature feature) {
  switch (feature) {
    // FilterEffects reports `false` here so the resvg_test_suite
    // `filters/*` categories skip cleanly on Geode via the category
    // gate's `requireFeature(FilterEffects)`. The underlying
    // `GeodeFilterEngine` does implement most of the filter primitives
    // (used by the WASM editor path), but the image-comparison suite
    // is blocked on per-backend (Metal / Vulkan / D3D12) pixel-diff
    // tuning that needs its own PR. Once that lands, flip this to
    // `true` and the category gate will let the tests run.
    //
    // Text is implemented in `RendererGeode::drawText`
    // -- the renderer walks `TextEngine` runs, pulls each glyph
    // outline via `glyphOutline`, transforms it into place, and
    // fills via the Slug fill pipeline. End users calling
    // `RendererGeode::draw` directly get correct text rendering.
    //
    // The `Text` / `TextFull` feature flags still return `false`
    // here so the resvg test suite skips the `text/*` category on
    // Geode text currently differs from the tiny-skia reference by
    // ~600-800 pixels on realistic text tests, well past the default
    // 100-pixel threshold. The differing pixels are frequently fully
    // off rather than partial, so threshold widening is not justified.
    // Keep the feature gate until the positional/coverage root cause is
    // identified and fixed.
    case RendererBackendFeature::FilterEffects: return true;
    case RendererBackendFeature::Text: return false;
    case RendererBackendFeature::TextFull: return false;
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
    const geode::GpuBackendKind kind = device->physicalDeviceOwner()->root().capabilities().backend;
    if (kind != geode::GpuBackendKind::TransitionalWgpu) {
      FAIL() << "the resvg wgpu reference selected " << geode::GpuBackendKindName(kind)
             << " instead of the transitional wgpu backend";
    }

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
    constexpr size_t kReviewedGeodeGoldenCases = 1679;
    if (geodeCases != kReviewedGeodeGoldenCases || tinyCases != geodeCases) {
      FAIL() << "resvg wgpu reference case census changed: GeodeGolden=" << geodeCases
             << ", TinyGolden=" << tinyCases << ", reviewed=" << kReviewedGeodeGoldenCases;
    }
  }

  void TearDown() override { ResetSharedTestBackendState(); }
};

[[maybe_unused]] const ::testing::Environment* const geodeBackendEnvironment =
    ::testing::AddGlobalTestEnvironment(new GeodeBackendEnvironment);

RendererBitmap GeodeRender(SVGDocument& document, bool verbose) {
  RendererGeode& renderer = SharedTestRenderer(verbose);
  renderer.setAntialias(true);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

RendererBitmap GeodeRenderForAscii(SVGDocument& document) {
  RendererGeode& renderer = SharedTestRenderer();
  renderer.setAntialias(false);
  renderer.draw(document);
  return renderer.takeSnapshot();
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
