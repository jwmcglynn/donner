#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/renderer/PixelFormatUtils.h"  // IWYU: re-export PremultiplyRgba.
#include "tiny_skia/Pixmap.h"

namespace donner::svg {

/**
 * Applies a Donner filter graph to a premultiplied RGBA pixmap in place.
 *
 * @param pixmap Premultiplied RGBA pixmap containing SourceGraphic on entry.
 * @param filterGraph Filter graph to execute.
 * @param deviceFromFilter Transform from filter local coordinates to device coordinates.
 * @param filterRegion Optional filter region in filter local coordinates.
 * @param clipSourceToFilterRegion If true, clip `pixmap`'s contents to `filterRegion` before
 *   running the filter graph.
 * @param fillPaintInput Optional `FillPaint` input pixmap, or nullptr if unused.
 * @param strokePaintInput Optional `StrokePaint` input pixmap, or nullptr if unused.
 * @param executionBudget Optional shared per-frame budget. Direct callers may omit it to apply
 *   only the graph-local limit.
 * @param backgroundInput Optional `BackgroundImage` input pixmap with \p pixmap's dimensions, or
 *   nullptr when the graph reads no backdrop. `BackgroundImage` and `BackgroundAlpha` read
 *   transparent black without it.
 * @return Whether the graph executed successfully.
 */
bool ApplyFilterGraphToPixmap(tiny_skia::Pixmap& pixmap, const components::FilterGraph& filterGraph,
                              const Transform2d& deviceFromFilter,
                              const std::optional<Box2d>& filterRegion,
                              bool clipSourceToFilterRegion = false,
                              const tiny_skia::Pixmap* fillPaintInput = nullptr,
                              const tiny_skia::Pixmap* strokePaintInput = nullptr,
                              components::FilterExecutionBudget* executionBudget = nullptr,
                              const tiny_skia::Pixmap* backgroundInput = nullptr);

struct TransformedShadowGraphs;

/**
 * Execute the shadow branch in its aligned raster, keeping the native source and final crops.
 *
 * @param pixmap Premultiplied input in the padded local raster.
 * @param graphs Split shadow graphs.
 * @param localFromFilter Transform to local raster pixels.
 * @param localFilterRegion Authored filter region in local user units.
 * @param paddedSize Size of the padded raster in local user units.
 * @return Whether the shadow is ready to project.
 */
bool ApplyTransformedShadowToPixmap(tiny_skia::Pixmap& pixmap,
                                    const TransformedShadowGraphs& graphs,
                                    const Transform2d& localFromFilter,
                                    const Box2d& localFilterRegion, const Vector2d& paddedSize);

/**
 * Clears pixels outside the transformed filter region.
 *
 * @param pixmap Premultiplied RGBA pixmap to modify in place.
 * @param filterRegion Optional filter region in filter local coordinates.
 * @param deviceFromFilter Transform from filter local coordinates to device coordinates.
 */
void ClipFilterOutputToRegion(tiny_skia::Pixmap& pixmap, const std::optional<Box2d>& filterRegion,
                              const Transform2d& deviceFromFilter);

}  // namespace donner::svg
