#include <fuzzer/FuzzedDataProvider.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>

#include "donner/svg/tool/DonnerSvgToolUtils.h"

namespace donner::svg {

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);  // NOLINT
  const bool forceExtreme = input.starts_with("extreme-aabb-bounds");
  if (input.starts_with("cli-resource-root-boundary")) {
    const std::filesystem::path document =
        std::filesystem::path("/workspace") / "untrusted" / "document.svg";
    const auto root = ResourceSandboxRootForAbsoluteInput(document);
    if (!root || *root != std::filesystem::path("/workspace") / "untrusted" ||
        *root == std::filesystem::path("/workspace")) {
      std::abort();
    }
  }
  FuzzedDataProvider provider(data, size);

  RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(64, 64);
  bitmap.rowBytes = 64 * 4;
  bitmap.pixels.assign(bitmap.rowBytes * 64, 0);

  SampledImageInfo imageInfo{
      provider.ConsumeIntegralInRange<int>(1, 64),
      provider.ConsumeIntegralInRange<int>(1, 64),
      provider.ConsumeFloatingPointInRange<double>(0.001, 1000.0),
      provider.ConsumeFloatingPointInRange<double>(0.001, 1000.0),
  };
  Box2d bounds(
      Vector2d(provider.ConsumeFloatingPoint<double>(), provider.ConsumeFloatingPoint<double>()),
      Vector2d(provider.ConsumeFloatingPoint<double>(), provider.ConsumeFloatingPoint<double>()));
  if (forceExtreme) {
    bounds =
        Box2d(Vector2d(-std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()),
              Vector2d(std::numeric_limits<double>::max(), std::numeric_limits<double>::max()));
  }

  CompositeAABBRect(bitmap, bounds, imageInfo);
  if (bitmap.pixels.size() != bitmap.rowBytes * 64) {
    std::abort();
  }
  return 0;
}

}  // namespace donner::svg
