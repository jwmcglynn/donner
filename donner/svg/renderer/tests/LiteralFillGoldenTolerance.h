#pragma once
/// @file
/// The one tolerance every comparison against a literal-fill golden uses: the renderer's Geode
/// golden tests and the Metal and Vulkan solid-fill tests.
///
/// Measured against the goldens, Mesa lavapipe and a discrete Vulkan GPU leave no pixel of any
/// scene over this threshold; a wrong fill rule or color moves more than a thousand.

namespace donner::svg::tests {

/// Per-pixel pixelmatch threshold, the renderer suite's default.
inline constexpr float kLiteralFillGoldenThreshold = 0.02f;

/// Pixels allowed past the threshold.
inline constexpr int kLiteralFillGoldenMaxMismatchedPixels = 10;

}  // namespace donner::svg::tests
