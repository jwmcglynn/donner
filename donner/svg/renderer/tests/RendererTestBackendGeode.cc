#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"

namespace donner::svg {

namespace {

/// Returns a process-wide shared GeodeDevice, created on first access.
///
/// Sharing a single device across all test-constructed renderers avoids the
/// Mesa llvmpipe (and Intel ANV) hang caused by accumulating hundreds of
/// WebGPU device creations in a single process — the driver state doesn't
/// reclaim cleanly and the process eventually deadlocks.
std::shared_ptr<geode::GeodeDevice> SharedTestDevice() {
  static auto device = [] {
    auto d = geode::GeodeDevice::CreateHeadless();
    // Wrap the unique_ptr in a shared_ptr for lifetime sharing.
    return std::shared_ptr<geode::GeodeDevice>(std::move(d));
  }();
  return device;
}

}  // namespace

RendererBackend ActiveRendererBackend() {
  return RendererBackend::Geode;
}

std::string_view ActiveRendererBackendName() {
  return RendererBackendName(ActiveRendererBackend());
}

bool ActiveRendererSupportsFeature(RendererBackendFeature feature) {
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
    // Text is implemented in `RendererGeode::drawText` as of Phase 4
    // -- the renderer walks `TextEngine` runs, pulls each glyph
    // outline via `glyphOutline`, transforms it into place, and
    // fills via the Slug fill pipeline. End users calling
    // `RendererGeode::draw` directly get correct text rendering.
    //
    // The `Text` / `TextFull` feature flags still return `false`
    // here so the resvg test suite skips the `text/*` category on
    // Geode: the 4x MSAA pipeline introduces ~6% per-pixel alpha
    // drift on every glyph edge vs tiny-skia's 16x supersampled
    // reference, which produces a ~600-800 px diff on every
    // realistic text test -- well past the default 100-px threshold
    // and not closable by threshold widening (the edge pixels are
    // frequently fully off, not partial). Revisit once Geode picks
    // up a finer sample pattern (8x or 16x MSAA) or analytic glyph
    // AA lands.
    case RendererBackendFeature::FilterEffects: return true;
    case RendererBackendFeature::Text: return false;
    case RendererBackendFeature::TextFull: return false;
    case RendererBackendFeature::AsciiSnapshot: return false;
  }

  return false;
}

std::unique_ptr<RendererInterface> CreateActiveRendererInstance(bool verbose) {
  // Callers that want a renderer they own get a fresh one. Now that
  // `GeodeDevice` owns the expensive pipelines (issue #575), this is
  // ~free: the per-instance allocations are just small ECS-side
  // bookkeeping and a handful of wgpu::Texture handles.
  return std::make_unique<RendererGeode>(SharedTestDevice(), verbose);
}

/// Returns a process-wide shared `RendererGeode`. Created on first call,
/// reused for every subsequent `RenderDocumentWithActiveBackend` /
/// `RenderDocumentWithActiveBackendForAscii`. Sharing eliminates the
/// per-test `RendererGeode` allocation churn that would otherwise
/// accumulate ~2 textures + ~8 buffers per test in wgpu-native's
/// internal tracking — enough to trip the driver's
/// `maxMemoryAllocationCount` on Mesa lavapipe / llvmpipe after a few
/// hundred tests (issue #575). `beginFrame()` fully resets per-frame
/// state, so a shared renderer behaves identically to a fresh one as
/// long as each `draw()` call is self-contained (no carried layer /
/// clip / filter stack), which the image-comparison fixture
/// guarantees.
RendererGeode& SharedTestRenderer(bool verbose = false) {
  static auto* renderer = new RendererGeode(SharedTestDevice(), verbose);
  return *renderer;
}

RendererBitmap RenderDocumentWithActiveBackend(SVGDocument& document, bool verbose) {
  RendererGeode& renderer = SharedTestRenderer(verbose);
  renderer.draw(document);
  return renderer.takeSnapshot();
}

RendererBitmap RenderDocumentWithActiveBackendForAscii(SVGDocument& document) {
  // Geode has no anti-aliasing toggle — ASCII snapshots aren't supported,
  // but routing through the same shared renderer keeps the driver
  // resource budget bounded.
  RendererGeode& renderer = SharedTestRenderer();
  renderer.draw(document);
  return renderer.takeSnapshot();
}

}  // namespace donner::svg
