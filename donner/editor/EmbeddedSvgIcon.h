#pragma once
/// @file

#include <optional>
#include <span>
#include <vector>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

/**
 * One embedded-SVG rasterization, for the batched atlas path.
 *
 * @see RenderEmbeddedSvgIconBatch
 */
struct EmbeddedSvgIconRequest {
  /// Embedded SVG source bytes. Must point at storage that outlives the call
  /// and, for \ref PrewarmEmbeddedSvgIcons, the cached entry - embedded
  /// resource arrays qualify.
  std::span<const unsigned char> svgBytes;
  /// Square output bitmap size in device pixels.
  int outputSizePx = 0;
  /// True to normalize to a tintable white alpha mask (\ref RenderEmbeddedSvgIcon
  /// semantics), false to preserve authored colors (\ref RenderEmbeddedSvgArtwork).
  bool tintableMask = true;
};

/**
 * Use an editor-owned renderer for subsequent embedded-SVG rasterization.
 *
 * The editor calls this before its first UI frame so icon rendering reuses the
 * already-created backend/device instead of creating a second headless device.
 * The renderer must outlive the matching reset call.
 */
void ConfigureEmbeddedSvgIconRenderer(svg::RendererInterface& renderer);

/**
 * Stop using the configured renderer before its owner is destroyed.
 *
 * The reset is ignored when another editor has since installed a different
 * renderer, which keeps overlapping editor lifetimes safe.
 */
void ResetEmbeddedSvgIconRenderer(const svg::RendererInterface& expectedRenderer);

/**
 * Render an embedded SVG icon resource into a tintable RGBA bitmap.
 *
 * The source SVG is parsed and rasterized through Donner. The returned bitmap
 * is normalized to a white premultiplied-alpha mask so callers can tint it with
 * the current ImGui text color when displaying the resulting texture.
 *
 * @param svgBytes Embedded SVG source bytes.
 * @param outputSizePx Square output bitmap size in device pixels.
 * @return Rendered icon bitmap, or `std::nullopt` if parsing/rendering fails.
 */
[[nodiscard]] std::optional<svg::RendererBitmap> RenderEmbeddedSvgIcon(
    std::span<const unsigned char> svgBytes, int outputSizePx);

/**
 * Render embedded SVG artwork while preserving its authored RGBA colors.
 *
 * Use this for small multi-color assets such as the editor's black-core,
 * white-halo tool icons. Single-color affordances should continue to use
 * \ref RenderEmbeddedSvgIcon so ImGui can tint their alpha mask.
 *
 * @param svgBytes Embedded SVG source bytes.
 * @param outputSizePx Square output bitmap size in device pixels.
 * @return Rendered artwork, or `std::nullopt` if parsing/rendering fails.
 */
[[nodiscard]] std::optional<svg::RendererBitmap> RenderEmbeddedSvgArtwork(
    std::span<const unsigned char> svgBytes, int outputSizePx);

/**
 * Rasterize a set of embedded icons in one render pass and one GPU readback.
 *
 * Icons are packed into a single atlas render target, drawn together, read back
 * once, then sliced apart. Each slice is byte-identical to the bitmap the
 * matching single-icon call would return, so this is purely a cost change: on a
 * GPU backend a readback is an asynchronous buffer mapping whose latency is paid
 * per call, and the per-icon path pays it once per icon.
 *
 * @param requests Icons to rasterize.
 * @return One entry per request, in request order. Entries are `std::nullopt`
 *   for requests that failed to parse or render.
 */
[[nodiscard]] std::vector<std::optional<svg::RendererBitmap>> RenderEmbeddedSvgIconBatch(
    std::span<const EmbeddedSvgIconRequest> requests);

/**
 * Rasterize a set of embedded icons up front so later single-icon calls are free.
 *
 * The editor's first UI frame draws every affordance icon it shows, and each
 * one used to pay its own GPU readback while the frame was blocked on it. This
 * collapses that into one batched readback before the frame starts:
 * \ref RenderEmbeddedSvgIcon and \ref RenderEmbeddedSvgArtwork return the
 * prewarmed bitmap for any request already rasterized here.
 *
 * Requests that fail are simply not cached, so the matching single-icon call
 * still falls back to its own rasterization.
 *
 * @param requests Icons to rasterize and cache.
 */
void PrewarmEmbeddedSvgIcons(std::span<const EmbeddedSvgIconRequest> requests);

}  // namespace donner::editor
