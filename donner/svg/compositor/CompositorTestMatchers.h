#pragma once
/// @file

#include <gmock/gmock.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/compositor/CompositorHintComponent.h"
#include "donner/svg/compositor/DualPathVerifier.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg::compositor::test {

/// Matches a compositor hint entry by source and weight.
inline auto HintEntryIs(HintSource source, uint16_t weight) {
  return ::testing::AllOf(::testing::Field("source", &HintEntry::source, source),
                          ::testing::Field("weight", &HintEntry::weight, weight));
}

/// Matches an exact dual-path render result and prints all diff counters on failure.
MATCHER(ExactVerifyResult, "an exact dual-path verification result") {
  if (arg.isExact() && arg.mismatchCount == 0u && arg.maxChannelDiff == 0) {
    return true;
  }

  *result_listener << "mismatchCount=" << arg.mismatchCount
                   << ", maxChannelDiff=" << static_cast<int>(arg.maxChannelDiff)
                   << ", totalPixels=" << arg.totalPixels
                   << ", dimensionsMatch=" << arg.dimensionsMatch;
  return false;
}

/// Matches a dual-path render result whose max channel diff is within @p tolerance.
MATCHER_P(VerifyResultWithinTolerance, tolerance,
          "a dual-path verification result within channel tolerance") {
  if (arg.isWithinTolerance(tolerance)) {
    return true;
  }

  *result_listener << "tolerance=" << static_cast<int>(tolerance)
                   << ", mismatchCount=" << arg.mismatchCount
                   << ", maxChannelDiff=" << static_cast<int>(arg.maxChannelDiff)
                   << ", totalPixels=" << arg.totalPixels
                   << ", dimensionsMatch=" << arg.dimensionsMatch;
  return false;
}

/// Matches fallback reasons containing @p reason.
MATCHER_P(FallbackReasonsInclude, reason, "fallback reasons include the expected flag") {
  if ((arg & reason) != FallbackReason::None) {
    return true;
  }

  *result_listener << "fallbackReasons=" << FallbackReasonToString(arg)
                   << ", expectedFlag=" << FallbackReasonToString(reason);
  return false;
}

/// Matches a non-empty renderer bitmap with the expected dimensions.
MATCHER_P(NonEmptyRendererBitmapWithDimensions, expectedDimensions,
          "a non-empty renderer bitmap with expected dimensions") {
  if (!arg.empty() && arg.dimensions == expectedDimensions) {
    return true;
  }

  *result_listener << "dimensions=" << ::testing::PrintToString(arg.dimensions)
                   << ", expectedDimensions=" << ::testing::PrintToString(expectedDimensions)
                   << ", rowBytes=" << arg.rowBytes << ", pixels=" << arg.pixels.size()
                   << ", alphaType=" << static_cast<int>(arg.alphaType);
  return false;
}

/// Matches a renderer bitmap whose pixel array is byte-identical to @p expected.
MATCHER_P(RendererBitmapPixelsEqualTo, expected,
          "a renderer bitmap with byte-identical dimensions and pixels") {
  if (arg.dimensions != expected.dimensions) {
    *result_listener << "dimensions=" << ::testing::PrintToString(arg.dimensions)
                     << ", expectedDimensions=" << ::testing::PrintToString(expected.dimensions);
    return false;
  }

  if (arg.pixels.size() != expected.pixels.size()) {
    *result_listener << "pixels=" << arg.pixels.size()
                     << ", expectedPixels=" << expected.pixels.size();
    return false;
  }

  size_t mismatchCount = 0;
  size_t firstMismatch = 0;
  int maxDiff = 0;
  for (size_t i = 0; i < arg.pixels.size(); ++i) {
    const int actual = static_cast<int>(arg.pixels[i]);
    const int expectedValue = static_cast<int>(expected.pixels[i]);
    const int diff = std::abs(actual - expectedValue);
    if (diff == 0) {
      continue;
    }

    if (mismatchCount == 0) {
      firstMismatch = i;
    }
    ++mismatchCount;
    maxDiff = std::max(maxDiff, diff);
  }

  if (mismatchCount == 0) {
    return true;
  }

  *result_listener << "mismatchCount=" << mismatchCount << ", firstMismatch=" << firstMismatch
                   << ", actual=" << static_cast<int>(arg.pixels[firstMismatch])
                   << ", expected=" << static_cast<int>(expected.pixels[firstMismatch])
                   << ", maxDiff=" << maxDiff
                   << ", dimensions=" << ::testing::PrintToString(arg.dimensions)
                   << ", rowBytes=" << arg.rowBytes << ", expectedRowBytes=" << expected.rowBytes;
  return false;
}

/// Matches an object with `bitmapDims` not equal to the old 1x1 placeholder sentinel.
MATCHER(BitmapDimsAreNotPlaceholder, "bitmap dimensions are not the 1x1 placeholder") {
  if (arg.bitmapDims != Vector2i(1, 1)) {
    return true;
  }

  *result_listener << "bitmapDims=" << ::testing::PrintToString(arg.bitmapDims);
  return false;
}

/// Matches an upload tile whose bitmap dimensions are not the old 1x1 placeholder sentinel.
MATCHER(UploadTileBitmapDimsAreNotPlaceholder,
        "an upload tile whose bitmap dimensions are not the 1x1 placeholder") {
  if (arg.bitmapDims != Vector2i(1, 1)) {
    return true;
  }

  *result_listener << "tileId=" << arg.tileId << ", generation=" << arg.generation
                   << ", layerEntity=" << ::testing::PrintToString(arg.layerEntity)
                   << ", bitmapPixels=" << arg.bitmap.pixels.size()
                   << ", textureSnapshot=" << (arg.textureSnapshot != nullptr);
  return false;
}

/// Matches a tile whose generation is preserved according to a tileId->generation map.
MATCHER_P(CompositorTileGenerationPreservedIn, generationsByTileId,
          "a compositor tile whose generation is preserved in the expected map") {
  const auto it = generationsByTileId.find(arg.tileId);
  if (it == generationsByTileId.end()) {
    *result_listener << "tileId=" << arg.tileId << " is absent; knownTileIds=";
    bool first = true;
    for (const auto& [tileId, generation] : generationsByTileId) {
      if (!first) {
        *result_listener << ",";
      }
      *result_listener << tileId << "->" << generation;
      first = false;
    }
    return false;
  }

  if (arg.generation == it->second) {
    return true;
  }

  *result_listener << "tileId=" << arg.tileId << ", generation=" << arg.generation
                   << ", expectedGeneration=" << it->second << ", isDragTarget=" << arg.isDragTarget
                   << ", layerEntity=" << ::testing::PrintToString(arg.layerEntity);
  return false;
}

/// Matches tile metadata against @p expected while delegating bitmap payload checks.
template <typename BitmapMatcher>
auto CompositorTileMetadataMatches(const CompositorTile& expected, BitmapMatcher bitmapMatcher) {
  return ::testing::AllOf(
      ::testing::Field("tileId", &CompositorTile::tileId, expected.tileId),
      ::testing::Field("generation", &CompositorTile::generation, expected.generation),
      ::testing::Field("bitmapDims", &CompositorTile::bitmapDims, expected.bitmapDims),
      ::testing::Field("canvasOffsetPx", &CompositorTile::canvasOffsetPx, expected.canvasOffsetPx),
      ::testing::Field("isDragTarget", &CompositorTile::isDragTarget, expected.isDragTarget),
      ::testing::Field("bitmap", &CompositorTile::bitmap, bitmapMatcher));
}

}  // namespace donner::svg::compositor::test
