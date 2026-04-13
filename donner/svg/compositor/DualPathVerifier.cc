#include "donner/svg/compositor/DualPathVerifier.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "donner/svg/renderer/RendererDriver.h"

namespace donner::svg::compositor {

DualPathVerifier::DualPathVerifier(CompositorController& compositor, RendererInterface& renderer)
    : compositor_(&compositor), renderer_(&renderer) {}

DualPathVerifier::VerifyResult DualPathVerifier::renderAndVerify(const RenderViewport& viewport) {
  // Path 1: Compositor render.
  compositor_->renderFrame(viewport);
  compositorSnapshot_ = renderer_->takeSnapshot();

  // Path 2: Reference full render.
  // The compositor's renderFrame already called prepareDocumentForRendering, so the document is
  // up-to-date. We render again using a fresh RendererDriver.
  RendererDriver driver(*renderer_);
  driver.draw(compositor_->document());
  referenceSnapshot_ = renderer_->takeSnapshot();

  // Compare the two snapshots.
  lastResult_ = compareBitmaps(compositorSnapshot_, referenceSnapshot_);
  return lastResult_;
}

DualPathVerifier::VerifyResult DualPathVerifier::compareBitmaps(const RendererBitmap& a,
                                                                const RendererBitmap& b) {
  VerifyResult result;

  if (a.dimensions != b.dimensions) {
    result.dimensionsMatch = false;
    return result;
  }

  if (a.empty() && b.empty()) {
    return result;  // Both empty — trivially match.
  }

  const int width = a.dimensions.x;
  const int height = a.dimensions.y;
  result.totalPixels = static_cast<size_t>(width) * height;

  // Compare pixel-by-pixel in RGBA format.
  const size_t rowBytes = a.rowBytes > 0 ? a.rowBytes : static_cast<size_t>(width) * 4;

  for (int y = 0; y < height; ++y) {
    const uint8_t* rowA = a.pixels.data() + y * rowBytes;
    const uint8_t* rowB = b.pixels.data() + y * rowBytes;
    for (int x = 0; x < width; ++x) {
      const size_t offset = static_cast<size_t>(x) * 4;
      bool pixelMismatch = false;
      for (int c = 0; c < 4; ++c) {
        const uint8_t valA = rowA[offset + c];
        const uint8_t valB = rowB[offset + c];
        const uint8_t diff = valA > valB ? valA - valB : valB - valA;
        if (diff > 0) {
          pixelMismatch = true;
          result.maxChannelDiff = std::max(result.maxChannelDiff, diff);
        }
      }
      if (pixelMismatch) {
        ++result.mismatchCount;
      }
    }
  }

  return result;
}

std::ostream& operator<<(std::ostream& os, const DualPathVerifier::VerifyResult& result) {
  os << "VerifyResult{";
  if (!result.dimensionsMatch) {
    os << "DIMENSION_MISMATCH";
  } else {
    os << "mismatch=" << result.mismatchCount << "/" << result.totalPixels
       << ", maxDiff=" << static_cast<int>(result.maxChannelDiff);
  }
  os << "}";
  return os;
}

}  // namespace donner::svg::compositor
