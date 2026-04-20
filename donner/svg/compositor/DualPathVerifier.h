#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <ostream>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg::compositor {

/**
 * Dual-path assertion harness for composited rendering.
 *
 * Runs both the compositor path and a full re-render reference path, then pixel-diffs the results.
 * Use this to verify that composited output matches the ground truth during development and testing.
 *
 * The harness wraps a `CompositorController` and a `RendererInterface`, intercepting `renderFrame`
 * to additionally perform a reference render and diff.
 *
 * Usage in tests:
 * ```cpp
 * DualPathVerifier verifier(compositor, renderer);
 * verifier.renderAndVerify(viewport);
 * EXPECT_EQ(verifier.mismatchCount(), 0u);
 * ```
 */
class DualPathVerifier {
public:
  /// Result of a dual-path verification pass.
  struct VerifyResult {
    /// Number of pixels that differ between compositor and reference output.
    size_t mismatchCount = 0;
    /// Maximum per-channel difference across all pixels (0-255).
    uint8_t maxChannelDiff = 0;
    /// Total number of pixels compared.
    size_t totalPixels = 0;
    /// Whether the two bitmaps have the same dimensions.
    bool dimensionsMatch = true;

    /// Returns true if the outputs are identical.
    [[nodiscard]] bool isExact() const { return mismatchCount == 0 && dimensionsMatch; }

    /// Returns true if the outputs match within the given per-channel tolerance.
    [[nodiscard]] bool isWithinTolerance(uint8_t tolerance) const {
      return dimensionsMatch && maxChannelDiff <= tolerance;
    }
  };

  /**
   * Construct a dual-path verifier.
   *
   * @param compositor The compositor controller to verify.
   * @param renderer The renderer backend used for both the compositor and reference paths.
   */
  DualPathVerifier(CompositorController& compositor, RendererInterface& renderer);

  /**
   * Render a frame through both paths and compare the results.
   *
   * 1. Renders via `compositor.renderFrame(viewport)` and captures a snapshot.
   * 2. Renders via `RendererDriver::draw(document)` and captures a snapshot.
   * 3. Pixel-diffs the two snapshots.
   *
   * @param viewport The viewport for both render passes.
   * @return The verification result.
   */
  VerifyResult renderAndVerify(const RenderViewport& viewport);

  /// Returns the result of the last `renderAndVerify()` call.
  [[nodiscard]] const VerifyResult& lastResult() const { return lastResult_; }

  /// Returns the compositor snapshot from the last verification.
  [[nodiscard]] const RendererBitmap& compositorSnapshot() const { return compositorSnapshot_; }

  /// Returns the reference snapshot from the last verification.
  [[nodiscard]] const RendererBitmap& referenceSnapshot() const { return referenceSnapshot_; }

private:
  /// Compare two bitmaps pixel-by-pixel.
  static VerifyResult compareBitmaps(const RendererBitmap& a, const RendererBitmap& b);

  CompositorController* compositor_;
  RendererInterface* renderer_;
  VerifyResult lastResult_;
  RendererBitmap compositorSnapshot_;
  RendererBitmap referenceSnapshot_;
};

/// Print `VerifyResult` for test diagnostics.
std::ostream& operator<<(std::ostream& os, const DualPathVerifier::VerifyResult& result);

}  // namespace donner::svg::compositor
