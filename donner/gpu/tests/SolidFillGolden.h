#pragma once
/// @file
/// Compares a backend's render of the solid-fill scene against the renderer's golden for the same
/// scene, so the Metal and Vulkan solid-fill tests and the renderer's Geode golden tests check one
/// golden with one tolerance.

#include <string_view>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::gpu::tests {

/// Runfiles path of the golden for `donner/svg/renderer/testdata/literal_fill_solid.svg`. It is
/// captured from Geode by `//donner/svg/renderer/tests:renderer_geode_golden_tests`, never from
/// these tests.
inline constexpr std::string_view kSolidFillGoldenPath =
    "donner/svg/renderer/testdata/golden/literal_fill_solid.png";

/**
 * Compares \p render against the solid-fill golden within the literal-fill tolerance, adding a
 * gtest failure when they differ by more. Read-only: it never writes the golden. The golden
 * stores straight alpha, so a premultiplied render is converted first.
 *
 * @param render A 256x256 RGBA8 render of the solid-fill scene.
 * @param testLabel Distinguishes this comparison's output images.
 */
void ExpectMatchesSolidFillGolden(const svg::RendererBitmap& render, std::string_view testLabel);

}  // namespace donner::gpu::tests
