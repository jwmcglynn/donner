#include "donner/svg/renderer/RendererGeode.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>
#include <webgpu/webgpu.hpp>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Path.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiplyBindings.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/core/Gradient.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/PatternTile.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/geode/GeoEncoder.h"
#include "donner/svg/renderer/geode/GeodeBufferPool.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeGlyphResidency.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePathCacheComponent.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeResidentPathComponent.h"
#include "donner/svg/renderer/geode/GeodeResourceBudget.h"
#include "donner/svg/renderer/geode/GeodeStrokeTolerance.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "donner/svg/resources/ImageResource.h"
#ifdef DONNER_TEXT_ENABLED
#include "donner/base/MathUtils.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/renderer/PlacedTextGeometry.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"
#include "donner/svg/text/TextLayoutParams.h"
#endif

namespace donner::svg {

// Pull the Geode-local label helper into this namespace so that the many
// `.label = wgpuLabel("...")` sites below can stay unqualified. See
// GeodeWgpuUtil.h for the helper rationale.
using ::donner::geode::wgpuLabel;

RendererGeodeTextureSnapshot::RendererGeodeTextureSnapshot(
    std::shared_ptr<geode::GeodeDevice> device, wgpu::Texture texture, Vector2i dimensions,
    wgpu::TextureFormat format, AlphaType alphaType)
    : device_(std::move(device)),
      ownedTexture_(std::move(texture)),
      dimensions_(dimensions),
      format_(format),
      alphaType_(alphaType) {
  texture_ = ownedTexture_.get();
}

RendererGeodeTextureSnapshot RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
    std::shared_ptr<geode::GeodeDevice> device, gpu::Texture texture, Vector2i dimensions,
    wgpu::TextureFormat format, AlphaType alphaType) {
  // The runtime handle is what owns the texture here. The backend alias beside it is only what
  // the readback path still names it by; it borrows, and holding it does not extend the
  // texture's life beyond the handle's.
  wgpu::Texture backendAlias =
      device ? device->adapterDevice().wgpuTextureOf(texture) : wgpu::Texture();
  RendererGeodeTextureSnapshot result(std::move(device), wgpu::Texture(), dimensions, format,
                                      alphaType);
  result.ownedGpuTexture_ = std::move(texture);
  result.texture_ = std::move(backendAlias);
  return result;
}

RendererGeodeTextureSnapshot::~RendererGeodeTextureSnapshot() {
  destroyOwnedBacking();
}

RendererGeodeTextureSnapshot& RendererGeodeTextureSnapshot::operator=(
    RendererGeodeTextureSnapshot&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  destroyOwnedBacking();
  device_ = std::move(other.device_);
  ownedGpuTexture_ = std::move(other.ownedGpuTexture_);
  ownedTexture_ = std::move(other.ownedTexture_);
  texture_ = std::exchange(other.texture_, wgpu::Texture());
  textureView_ = std::move(other.textureView_);
  dimensions_ = std::exchange(other.dimensions_, Vector2i::Zero());
  format_ = std::exchange(other.format_, wgpu::TextureFormat::Undefined);
  alphaType_ = std::exchange(other.alphaType_, AlphaType::Premultiplied);
  return *this;
}

void RendererGeodeTextureSnapshot::destroyOwnedBacking() noexcept {
  textureView_.reset();
  if (ownedTexture_) {
    ownedTexture_.destroyBackingAndReset();
  }
  if (ownedGpuTexture_.isValid() && device_) {
    // Destroy the backend object explicitly rather than only releasing the handle: a succession
    // of presentation snapshots otherwise stays resident until the host runtime collects it.
    (void)device_->adapterDevice().destroyTextureBacking(std::move(ownedGpuTexture_));
  }
  ownedGpuTexture_ = gpu::Texture();
  texture_ = wgpu::Texture();
}

RendererGeodeTextureSnapshot RendererGeodeTextureSnapshot::BorrowCurrentFrame(
    wgpu::Texture texture, Vector2i dimensions, wgpu::TextureFormat format) {
  RendererGeodeTextureSnapshot result(nullptr, wgpu::Texture(), dimensions, format);
  result.texture_ = texture;
  return result;
}

const wgpu::TextureView& RendererGeodeTextureSnapshot::textureView() const {
  if (!textureView_ && texture_) {
    textureView_.reset(texture_.createView());
  }
  return textureView_.get();
}

namespace {

// Render-target texture format stored per-instance in Impl::textureFormat (initialized from
// GeodeDevice). Filter-engine intermediate textures always use RGBA8Unorm for compute-shader
// compatibility regardless of the host format.
constexpr wgpu::TextureFormat kFilterIntermediateFormat = wgpu::TextureFormat::RGBA8Unorm;

/// The unit path bounds used by `objectBoundingBox` gradient coordinates,
/// matching the CPU-renderer helper.
const Box2d kUnitPathBounds(Vector2d::Zero(), Vector2d(1, 1));

/// Source UV rect that samples an entire texture.
const Box2d kWholeTextureUv(Vector2d::Zero(), Vector2d(1, 1));

std::optional<Vector2i> CheckedViewportPixels(const RenderViewport& viewport) {
  const double width = viewport.size.x * viewport.devicePixelRatio;
  const double height = viewport.size.y * viewport.devicePixelRatio;
  constexpr double kMaximum = static_cast<double>(std::numeric_limits<int>::max());
  if (!std::isfinite(viewport.size.x) || !std::isfinite(viewport.size.y) ||
      !std::isfinite(viewport.devicePixelRatio) || viewport.size.x < 0.0 || viewport.size.y < 0.0 ||
      viewport.devicePixelRatio <= 0.0 || !std::isfinite(width) || !std::isfinite(height) ||
      width > kMaximum || height > kMaximum) {
    return std::nullopt;
  }
  return Vector2i(static_cast<int>(width), static_cast<int>(height));
}

bool IsBgraTextureFormat(wgpu::TextureFormat format) {
  return static_cast<WGPUTextureFormat>(format) == WGPUTextureFormat_BGRA8Unorm;
}

// NOTE: `transformPath` now lives in the shared (text-gated) PlacedTextGeometry
// header so both backends share one definition. The
// only geode callers (glyph + decoration placement) are inside the
// DONNER_TEXT_ENABLED region.

#ifdef DONNER_TEXT_ENABLED
TextLayoutParams toTextLayoutParams(const TextParams& params) {
  TextLayoutParams layoutParams;
  layoutParams.fontFamilies = params.fontFamilies;
  layoutParams.fontSize = params.fontSize;
  layoutParams.viewBox = params.viewBox;
  layoutParams.fontMetrics = params.fontMetrics;
  layoutParams.textAnchor = params.textAnchor;
  layoutParams.writingMode = params.writingMode;
  layoutParams.letterSpacingPx = params.letterSpacingPx;
  layoutParams.wordSpacingPx = params.wordSpacingPx;
  layoutParams.textLength = params.textLength;
  layoutParams.lengthAdjust = params.lengthAdjust;
  layoutParams.inlineSizePx = params.inlineSizePx;
  return layoutParams;
}

std::optional<RendererTextMaterializationBudget::Cost> GlyphPredecodeCost(
    const FontManager::GlyphOutlineComplexity& complexity) {
  constexpr std::size_t kCommandCopiesPerVertex = 6;
  constexpr std::size_t kPointCopiesPerVertex = 9;
  if (complexity.maximumVertices >
      std::numeric_limits<std::size_t>::max() / kPointCopiesPerVertex) {
    return std::nullopt;
  }
  const std::size_t commands = complexity.maximumVertices * kCommandCopiesPerVertex;
  const std::size_t points = complexity.maximumVertices * kPointCopiesPerVertex;
  if (commands > std::numeric_limits<std::size_t>::max() / sizeof(Path::Command) ||
      points > std::numeric_limits<std::size_t>::max() / sizeof(Vector2d)) {
    return std::nullopt;
  }
  const std::size_t commandBytes = commands * sizeof(Path::Command);
  const std::size_t pointBytes = points * sizeof(Vector2d);
  if (pointBytes > std::numeric_limits<std::size_t>::max() - commandBytes) {
    return std::nullopt;
  }
  return RendererTextMaterializationBudget::Cost{.uniqueOutlines = 1,
                                                 .commands = commands,
                                                 .points = points,
                                                 .bytes = commandBytes + pointBytes,
                                                 .decodeWork = complexity.work};
}
#endif

/// Hard cap on gradient stops baked into the uniform buffer. Must be
/// <= `GeoEncoder`'s internal `kMaxGradientStops` (which mirrors the WGSL
/// constant in `slug_gradient.wgsl`). Values beyond this cap are truncated
/// with a one-shot warning; the follow-up is a texture-based stop lookup
/// (a `GeodeGradientCacheComponent` holding a stop texture).
constexpr size_t kMaxGradientStopsClient = 16;

int boundedFloorToInt(double value, int minimum, int maximum) {
  if (std::isnan(value)) {
    return minimum;
  }
  if (value <= static_cast<double>(minimum)) {
    return minimum;
  }
  if (value >= static_cast<double>(maximum)) {
    return maximum;
  }
  return static_cast<int>(std::floor(value));
}

std::optional<uint32_t> checkedFilterRasterDimension(double value) {
  if (!std::isfinite(value) || value <= 0.0 ||
      value > static_cast<double>(components::kMaximumFilterSurfaceDimension)) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(std::max(1.0, std::ceil(value)));
}

/// Returns true when the filter graph contains spatial-shift primitives (feOffset) that can bring
/// content from outside the viewport into view, requiring the filter layer buffer to be expanded.
bool graphHasSpatialShift(const components::FilterGraph& filterGraph) {
  using namespace components::filter_primitive;
  for (const components::FilterNode& node : filterGraph.nodes) {
    if (std::holds_alternative<Offset>(node.primitive)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Transformed-blur path detection (ported faithfully from
// RendererTinySkia.cc's `isEligibleForTransformedBlurPath` /
// `graphHasAnisotropicBlur` / `shouldUseTransformedBlurPath` /
// `computeBlurPadding`). Keep in lockstep with tiny-skia - these decide when a
// blur under a rotated/skewed CTM must be rasterized in filter-local space so
// the (anisotropic) blur is oriented in the element's local axes rather than
// device axes. tiny-skia is the parity reference for the result.
// ---------------------------------------------------------------------------

/// Eligible only for a linear chain of blur/offset primitives in user space (no
/// named results, subregions, multi-input, OBB units, or other primitive types).
bool isEligibleForTransformedBlurPath(const components::FilterGraph& filterGraph) {
  if (filterGraph.nodes.empty()) {
    return false;
  }
  if (filterGraph.primitiveUnits == PrimitiveUnits::ObjectBoundingBox) {
    return false;
  }

  bool hasBlur = false;
  for (const components::FilterNode& node : filterGraph.nodes) {
    const bool isBlur =
        std::holds_alternative<components::filter_primitive::GaussianBlur>(node.primitive) ||
        std::holds_alternative<components::filter_primitive::DropShadow>(node.primitive);
    const bool isOffset =
        std::holds_alternative<components::filter_primitive::Offset>(node.primitive);
    if (!isBlur && !isOffset) {
      return false;
    }
    hasBlur |= isBlur;

    if (node.result.has_value()) {
      return false;  // No named result reuse.
    }
    if (node.x.has_value() || node.y.has_value() || node.width.has_value() ||
        node.height.has_value()) {
      return false;  // No primitive subregions.
    }
    if (node.inputs.size() > 1) {
      return false;  // Linear chain only.
    }
    if (node.inputs.size() == 1) {
      const auto& input = node.inputs[0];
      if (std::holds_alternative<components::FilterInput::Previous>(input.value)) {
        // OK
      } else if (const auto* stdInput =
                     std::get_if<components::FilterStandardInput>(&input.value)) {
        if (*stdInput != components::FilterStandardInput::SourceGraphic) {
          return false;
        }
      } else {
        return false;  // Named input - not eligible.
      }
    }
  }

  return hasBlur;
}

/// True if any blur/drop-shadow has stdDeviationX != stdDeviationY.
bool graphHasAnisotropicBlur(const components::FilterGraph& filterGraph) {
  for (const auto& node : filterGraph.nodes) {
    bool anisotropic = false;
    std::visit(
        [&](const auto& prim) {
          using T = std::decay_t<decltype(prim)>;
          if constexpr (std::is_same_v<T, components::filter_primitive::GaussianBlur>) {
            anisotropic = !NearEquals(prim.stdDeviationX, prim.stdDeviationY, 1e-6);
          } else if constexpr (std::is_same_v<T, components::filter_primitive::DropShadow>) {
            anisotropic = !NearEquals(prim.stdDeviationX, prim.stdDeviationY, 1e-6);
          }
        },
        node.primitive);
    if (anisotropic) {
      return true;
    }
  }
  return false;
}

/// True when the CTM is skewed (always needs the local raster), or rotated with
/// an anisotropic blur (whose orientation a device-axis blur can't reproduce).
bool shouldUseTransformedBlurPath(const components::FilterGraph& filterGraph,
                                  const Transform2d& deviceFromFilter) {
  const Vector2d xAxis = deviceFromFilter.transformVector(Vector2d(1.0, 0.0));
  const Vector2d yAxis = deviceFromFilter.transformVector(Vector2d(0.0, 1.0));
  const double dot = xAxis.x * yAxis.x + xAxis.y * yAxis.y;
  if (!NearZero(dot, 1e-6)) {
    return true;  // Skew (non-orthogonal axes).
  }
  const bool hasRotation =
      !NearZero(deviceFromFilter.data[1], 1e-6) || !NearZero(deviceFromFilter.data[2], 1e-6);
  if (!hasRotation) {
    return false;
  }
  return graphHasAnisotropicBlur(filterGraph);
}

/// 3σ + 1 padding (in user-space units) for the maximum blur stdDeviation.
double computeBlurPadding(const components::FilterGraph& filterGraph) {
  double maxSigma = 0.0;
  for (const auto& node : filterGraph.nodes) {
    std::visit(
        [&](const auto& prim) {
          using T = std::decay_t<decltype(prim)>;
          if constexpr (std::is_same_v<T, components::filter_primitive::GaussianBlur>) {
            maxSigma = std::max({maxSigma, prim.stdDeviationX, prim.stdDeviationY});
          } else if constexpr (std::is_same_v<T, components::filter_primitive::DropShadow>) {
            maxSigma = std::max({maxSigma, prim.stdDeviationX, prim.stdDeviationY});
          }
        },
        node.primitive);
  }
  return maxSigma * 3.0 + 1.0;
}

/// WebGPU requires bytesPerRow alignment to 256 when copying textures to
/// buffers. Delegates to the shared helper so the renderer's map-range math
/// can never diverge from the readback-buffer sizing in GeodeDevice: a
/// mapped-range request larger than the buffer returns null rather than
/// raising a validation error.
constexpr uint32_t alignBytesPerRow(uint32_t unpadded) {
  return geode::AlignReadbackBytesPerRow(unpadded);
}

/// Convert an SVG stroke-linecap enum to the donner::LineCap used by
/// Path::strokeToFill.
LineCap toLineCap(StrokeLinecap cap) {
  switch (cap) {
    case StrokeLinecap::Butt: return LineCap::Butt;
    case StrokeLinecap::Round: return LineCap::Round;
    case StrokeLinecap::Square: return LineCap::Square;
  }
  return LineCap::Butt;
}

/// Convert an SVG stroke-linejoin enum to the donner::LineJoin used by
/// Path::strokeToFill. MiterClip and Arcs are specialized SVG2 variants that
/// strokeToFill does not yet distinguish; fall back to Miter (matching the
/// tiny-skia backend's handling of these values).
LineJoin toLineJoin(StrokeLinejoin join) {
  switch (join) {
    case StrokeLinejoin::Miter: return LineJoin::Miter;
    case StrokeLinejoin::MiterClip: return LineJoin::Miter;
    case StrokeLinejoin::Round: return LineJoin::Round;
    case StrokeLinejoin::Bevel: return LineJoin::Bevel;
    case StrokeLinejoin::Arcs: return LineJoin::Miter;
  }
  return LineJoin::Miter;
}

/// Build a donner::StrokeStyle from the SVG StrokeParams.
StrokeStyle toStrokeStyle(const StrokeParams& params) {
  StrokeStyle style;
  style.width = params.strokeWidth;
  style.cap = toLineCap(params.lineCap);
  style.join = toLineJoin(params.lineJoin);
  style.miterLimit = params.miterLimit;
  style.dashArray = params.dashArray;
  style.dashOffset = params.dashOffset;
  style.pathLength = params.pathLength;
  return style;
}

/// Rewrite a geometry so that any *closed* subpath whose anchor points are all
/// collinear (a degenerate zero-thickness line, e.g. `M 30 100 L 170 100 Z`)
/// is left *open* instead.
///
/// `Path::strokeToFill` of a collinear closed subpath emits two same-winding
/// contours that, instead of nesting into an annulus, decompose the stroke
/// rectangle into two overlapping triangles meeting along a diagonal. The
/// analytic dual-ray coverage shader under-covers that thin diagonal-split band
/// (the line's edge rows render at ~half coverage). An *open* subpath strokes
/// into a single clean rectangle that the shader covers correctly. Closing a
/// collinear subpath adds no enclosed area, so de-closing is visually
/// equivalent and only touches this degenerate case.
///
/// Collinearity (not signed area) is the right test: a self-intersecting but
/// genuinely 2D closed polygon - e.g. the symmetric zigzag
/// `40 40 80 160 120 40 160 160` in painting/marker/marker-on-polygon - has
/// zero *signed* area yet is a real shape whose close must be preserved.
Path deCloseZeroAreaSubpaths(const Path& geometry) {
  const std::span<const Path::Command> cmds = geometry.commands();
  const std::span<const Vector2d> pts = geometry.points();

  // Per-subpath: collect the index range of its points and whether it closes.
  struct SubpathInfo {
    size_t firstPoint = 0;
    size_t pointCount = 0;
    bool closed = false;
    bool collinear = false;
  };
  std::vector<SubpathInfo> subpaths;
  {
    size_t pointIdx = 0;
    for (const Path::Command& cmd : cmds) {
      if (cmd.verb == Path::Verb::MoveTo) {
        subpaths.push_back(SubpathInfo{pointIdx, 1, false, false});
      } else if (cmd.verb == Path::Verb::ClosePath) {
        if (!subpaths.empty()) {
          subpaths.back().closed = true;
        }
      } else if (!subpaths.empty()) {
        subpaths.back().pointCount += Path::pointsPerVerb(cmd.verb);
      }
      pointIdx += Path::pointsPerVerb(cmd.verb);
    }
  }

  // A subpath is collinear when every point lies on the line through its first
  // two distinct points (cross product ≈ 0). Single-point subpaths count as
  // collinear (degenerate).
  bool anyDegenerateClosed = false;
  for (SubpathInfo& sp : subpaths) {
    if (!sp.closed) {
      continue;
    }
    const Vector2d& p0 = pts[sp.firstPoint];
    Vector2d dir{0.0, 0.0};
    bool haveDir = false;
    bool collinear = true;
    for (size_t i = 1; i < sp.pointCount; ++i) {
      const Vector2d d = pts[sp.firstPoint + i] - p0;
      if (!haveDir) {
        if (d.lengthSquared() > 1e-12) {
          dir = d;
          haveDir = true;
        }
        continue;
      }
      const double cross = dir.x * d.y - dir.y * d.x;
      if (std::abs(cross) > 1e-6) {
        collinear = false;
        break;
      }
    }
    sp.collinear = collinear;
    if (collinear) {
      anyDegenerateClosed = true;
    }
  }

  if (!anyDegenerateClosed) {
    return geometry;
  }

  // Rebuild, dropping the ClosePath of each collinear closed subpath.
  PathBuilder builder;
  size_t pointIdx = 0;
  size_t subpathIdx = static_cast<size_t>(-1);
  for (const Path::Command& cmd : cmds) {
    switch (cmd.verb) {
      case Path::Verb::MoveTo:
        ++subpathIdx;
        builder.moveTo(pts[pointIdx]);
        break;
      case Path::Verb::LineTo: builder.lineTo(pts[pointIdx]); break;
      case Path::Verb::QuadTo: builder.quadTo(pts[pointIdx], pts[pointIdx + 1]); break;
      case Path::Verb::CurveTo:
        builder.curveTo(pts[pointIdx], pts[pointIdx + 1], pts[pointIdx + 2]);
        break;
      case Path::Verb::ClosePath:
        if (subpathIdx >= subpaths.size() || !subpaths[subpathIdx].collinear) {
          builder.closePath();
        }
        break;
    }
    pointIdx += Path::pointsPerVerb(cmd.verb);
  }

  return builder.build();
}

/// Coerce a `Lengthd` into a percent-bearing length when the gradient is in
/// `objectBoundingBox` mode. Mirrors the helper used by the software renderer
/// for gradient coordinate resolution.
inline Lengthd coerceGradientLength(Lengthd value, bool numbersArePercent) {
  if (!numbersArePercent) {
    return value;
  }
  if (value.unit == Lengthd::Unit::None) {
    value.value *= 100.0;
    value.unit = Lengthd::Unit::Percent;
  }
  return value;
}

/// Resolve a `(x, y)` pair of gradient coordinates against the reference
/// bounds (unit box for objectBoundingBox, viewBox for userSpaceOnUse).
Vector2d resolveGradientCoords(Lengthd x, Lengthd y, const Box2d& bounds, bool numbersArePercent) {
  return Vector2d(coerceGradientLength(x, numbersArePercent)
                      .toPixels(bounds, FontMetrics(), Lengthd::Extent::X),
                  coerceGradientLength(y, numbersArePercent)
                      .toPixels(bounds, FontMetrics(), Lengthd::Extent::Y));
}

/// Resolve a single gradient coordinate (used for the radial `r` and `fr`
/// attributes, which are isotropic and don't carry an X/Y axis hint).
inline double resolveGradientCoord(Lengthd value, const Box2d& bounds, bool numbersArePercent) {
  return coerceGradientLength(value, numbersArePercent).toPixels(bounds, FontMetrics());
}

/// Resolve the `gradientTransform` attribute of a gradient into a concrete
/// Transform2d. If the referenced entity has no transform component, returns
/// identity. Mirrors the logic in the software renderer.
Transform2d resolveGradientTransform(
    const components::ComputedLocalTransformComponent* maybeTransformComponent,
    const Box2d& viewBox) {
  if (maybeTransformComponent == nullptr) {
    return Transform2d();
  }
  const Vector2d origin = maybeTransformComponent->transformOrigin;
  const Transform2d parentFromEntity =
      maybeTransformComponent->rawCssTransform.compute(viewBox, FontMetrics());
  // Pivot order matches shapes (LayoutSystem::getEntityFromParentTransform, #609) and the software
  // renderer: `Translate(-origin)·M·Translate(origin)` (left-first `operator*`). The reversed order
  // pushes a scaled gradient's center off the shape, collapsing it to one stop color (#621).
  return Transform2d::Translate(-origin) * parentFromEntity * Transform2d::Translate(origin);
}

/// Resolved frame for either kind of gradient: the bounds against which
/// gradient coordinates are evaluated (`coordBounds`), the path-from-gradient
/// transform (already inverted to `gradientFromPath`), and a `numbersArePercent`
/// flag for objectBoundingBox-mode coordinate coercion.
struct ResolvedGradientFrame {
  Transform2d gradientFromPath;
  Box2d coordBounds;
  bool numbersArePercent = false;
};

/// Resolve the gradient's coordinate frame and `gradientFromPath` transform.
/// Returns nullopt for malformed or degenerate frames (degenerate
/// objectBoundingBox bounds, singular gradientTransform). Shared by the
/// linear and radial resolvers - they only differ in which start/end /
/// center/radius fields they then read from the typed gradient component.
std::optional<ResolvedGradientFrame> resolveGradientFrame(
    const EntityHandle handle, const components::ComputedGradientComponent& computedGradient,
    const Box2d& pathBounds, const Box2d& viewBox,
    const std::optional<components::PaintContextRemap>& contextRemap) {
  const bool objectBoundingBox = computedGradient.gradientUnits == GradientUnits::ObjectBoundingBox;

  // Paints inherited through `context-fill` / `context-stroke` are evaluated in the context
  // element's space: objectBoundingBox units resolve against the context element's bounding box,
  // and the gradient is remapped from the context element's user space into path-local space.
  const Box2d& referenceBounds = contextRemap ? contextRemap->contextBounds : pathBounds;

  // Degenerate path bounds disable objectBoundingBox gradients; ditto the
  // other backends, which bail out rather than produce garbage coordinates.
  constexpr double kDegenerateBBoxTolerance = 1e-6;
  if (objectBoundingBox && (std::abs(referenceBounds.width()) < kDegenerateBBoxTolerance ||
                            std::abs(referenceBounds.height()) < kDegenerateBBoxTolerance)) {
    return std::nullopt;
  }

  // pathFromGradient maps coordinates expressed in the gradient's own frame
  // (which is either the path's unit bbox or the user-space viewBox) back
  // into the path-space the fragment shader receives. We invert it once on
  // the CPU so the GPU just multiplies row vectors per-pixel.
  //
  // In userSpaceOnUse mode, gradient space == path (user) space modulo the
  // `gradientTransform` attribute, so pathFromGradient == gradientTransform
  // and we can compose directly.
  //
  // In objectBoundingBox mode, coordinates are expressed in the unit box
  // [0..1] relative to the path's bounding box. The mapping back to path
  // space is:
  //   pathFromGradient = bboxFromUnit * gradientTransform
  //                    = Scale(size) · Translate(topLeft) · gradientTransform
  // (post-multiply because the gradientTransform is applied first in the
  // gradient's own local frame.)
  Transform2d pathFromGradient;
  if (objectBoundingBox) {
    const Transform2d gradientTransform = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), kUnitPathBounds);
    const Transform2d bboxFromUnit = Transform2d::Scale(referenceBounds.size()) *
                                     Transform2d::Translate(referenceBounds.topLeft);
    pathFromGradient = gradientTransform * bboxFromUnit;
  } else {
    pathFromGradient = resolveGradientTransform(
        handle.try_get<components::ComputedLocalTransformComponent>(), viewBox);
  }

  if (contextRemap) {
    // Gradient units -> context element user space, then context -> path local space.
    pathFromGradient = pathFromGradient * contextRemap->entityFromContextTransform;
  }

  if (std::abs(pathFromGradient.determinant()) < 1e-12) {
    return std::nullopt;
  }

  ResolvedGradientFrame frame;
  frame.gradientFromPath = pathFromGradient.inverse();
  frame.coordBounds = objectBoundingBox ? kUnitPathBounds : viewBox;
  frame.numbersArePercent = objectBoundingBox;
  return frame;
}

/// Map an SVG `GradientSpreadMethod` to the encoder's `spreadMode` integer.
inline uint32_t toGeoSpreadMode(GradientSpreadMethod method) {
  switch (method) {
    case GradientSpreadMethod::Pad: return 0u;
    case GradientSpreadMethod::Reflect: return 1u;
    case GradientSpreadMethod::Repeat: return 2u;
  }
  return 0u;
}

/// Translate the gradient's stop list into the encoder wire format. Returns
/// the populated count and sets `*outStopsTruncated` if the source had more
/// stops than the encoder's hard cap.
size_t buildGradientStops(const components::ComputedGradientComponent& computedGradient,
                          const css::RGBA currentColor, float opacity,
                          std::vector<geode::LinearGradientParams::Stop>& stopsStorage,
                          bool* outStopsTruncated) {
  const size_t stopCount = std::min(computedGradient.stops.size(), kMaxGradientStopsClient);
  if (outStopsTruncated != nullptr) {
    *outStopsTruncated = computedGradient.stops.size() > kMaxGradientStopsClient;
  }
  stopsStorage.clear();
  stopsStorage.reserve(stopCount);

  // Per SVG 1.1 §13.2.4 / SVG 2 §12.6.2: each stop's `offset` must be
  // monotonically non-decreasing. If a stop specifies an offset less than
  // the largest previous stop's offset, the offset is clamped up to that
  // largest previous value. Missing offsets effectively default to the
  // previous stop's offset via the same rule. The shader's
  // `sample_stops` assumes monotonic offsets - violating the invariant
  // produces wrong colors on the affected range (e-stop-003, e-stop-024).
  float minOffset = 0.0f;
  for (size_t i = 0; i < stopCount; ++i) {
    const GradientStop& stop = computedGradient.stops[i];
    const css::RGBA rgba = stop.color.resolve(currentColor, stop.opacity * opacity);
    geode::LinearGradientParams::Stop out;
    const float clampedOffset = std::clamp<float>(static_cast<float>(stop.offset), 0.0f, 1.0f);
    out.offset = std::max(clampedOffset, minOffset);
    minOffset = out.offset;
    out.rgba[0] = rgba.r / 255.0f;
    out.rgba[1] = rgba.g / 255.0f;
    out.rgba[2] = rgba.b / 255.0f;
    out.rgba[3] = rgba.a / 255.0f;
    stopsStorage.push_back(out);
  }
  return stopCount;
}

#ifdef DONNER_TEXT_ENABLED
/// True when a `ResolvedPaintServer` reference points at a (gradient) paint
/// server entity rather than a pattern. Mirrors tiny-skia, which treats a ref
/// as a gradient when `instantiateGradientShader` succeeds and as a pattern
/// otherwise. Used to let a span's own gradient override an element-level
/// pattern fill/stroke (the driver only stages a pattern slot for the
/// *element*, so the `patternFillPaint`/`patternStrokePaint` flag must not
/// suppress a span's own gradient ref). Only referenced from the text path, so
/// it lives under `DONNER_TEXT_ENABLED` to avoid an unused-function warning in
/// no-text builds.
bool paintReferenceIsGradient(const components::ResolvedPaintServer& server) {
  const auto* ref = std::get_if<components::PaintResolvedReference>(&server);
  if (ref == nullptr) {
    return false;
  }
  const EntityHandle handle = ref->reference.handle;
  if (!handle) {
    return false;
  }
  const auto* computedGradient = handle.try_get<components::ComputedGradientComponent>();
  return computedGradient != nullptr && computedGradient->initialized;
}
#endif  // DONNER_TEXT_ENABLED

/// Attempt to resolve a `PaintResolvedReference` into a concrete linear-gradient
/// draw specification for the Geode encoder. Returns `std::nullopt` for any
/// non-linear / malformed / degenerate gradient; the caller should fall back
/// to the gradient's fallback color or skip the draw.
std::optional<geode::LinearGradientParams> resolveLinearGradientParams(
    const components::PaintResolvedReference& ref, const Box2d& pathBounds, const Box2d& viewBox,
    const css::RGBA currentColor, float opacity,
    std::vector<geode::LinearGradientParams::Stop>& stopsStorage, bool* outStopsTruncated) {
  if (outStopsTruncated != nullptr) {
    *outStopsTruncated = false;
  }

  const EntityHandle handle = ref.reference.handle;
  if (!handle) {
    return std::nullopt;
  }
  const auto* computedGradient = handle.try_get<components::ComputedGradientComponent>();
  if (computedGradient == nullptr || !computedGradient->initialized) {
    return std::nullopt;
  }
  const auto* linear = handle.try_get<components::ComputedLinearGradientComponent>();
  if (linear == nullptr) {
    // Not a linear gradient - caller will try the radial resolver next.
    return std::nullopt;
  }

  auto frame =
      resolveGradientFrame(handle, *computedGradient, pathBounds, viewBox, ref.contextRemap);
  if (!frame.has_value()) {
    return std::nullopt;
  }

  const Vector2d startGrad =
      resolveGradientCoords(linear->x1, linear->y1, frame->coordBounds, frame->numbersArePercent);
  const Vector2d endGrad =
      resolveGradientCoords(linear->x2, linear->y2, frame->coordBounds, frame->numbersArePercent);

  if (computedGradient->stops.empty()) {
    return std::nullopt;
  }

  buildGradientStops(*computedGradient, currentColor, opacity, stopsStorage, outStopsTruncated);

  geode::LinearGradientParams params;
  params.startGrad = startGrad;
  params.endGrad = endGrad;
  params.gradientFromPath = frame->gradientFromPath;
  params.spreadMode = toGeoSpreadMode(computedGradient->spreadMethod);
  params.stops = std::span<const geode::LinearGradientParams::Stop>(stopsStorage);
  return params;
}

/// Result of resolving a radial gradient against a path. Either a fully
/// specified radial gradient ready for the GPU, OR - per SVG2 - a degenerate
/// radial gradient that should be painted as a solid color equal to the
/// last stop. Returning nullopt means "not a radial gradient" (caller should
/// fall through to the next resolver).
struct ResolvedRadialGradient {
  std::optional<geode::RadialGradientParams> gradient;
  std::optional<css::RGBA> solidFallback;
};

/// Same shape as @ref resolveLinearGradientParams, but for radial gradients.
/// Returns `nullopt` if the referenced entity isn't a radial gradient. If it
/// IS a radial gradient but is degenerate (zero outer radius), returns a
/// populated `solidFallback` with the last stop's color, matching the removed
/// full-Skia backend. If the focal circle fully
/// contains the outer circle, returns an empty result (both fields unset)
/// so the caller drops the draw.
std::optional<ResolvedRadialGradient> resolveRadialGradientParams(
    const components::PaintResolvedReference& ref, const Box2d& pathBounds, const Box2d& viewBox,
    const css::RGBA currentColor, float opacity,
    std::vector<geode::RadialGradientParams::Stop>& stopsStorage, bool* outStopsTruncated) {
  if (outStopsTruncated != nullptr) {
    *outStopsTruncated = false;
  }

  const EntityHandle handle = ref.reference.handle;
  if (!handle) {
    return std::nullopt;
  }
  const auto* computedGradient = handle.try_get<components::ComputedGradientComponent>();
  if (computedGradient == nullptr || !computedGradient->initialized) {
    return std::nullopt;
  }
  const auto* radial = handle.try_get<components::ComputedRadialGradientComponent>();
  if (radial == nullptr) {
    return std::nullopt;
  }

  auto frame =
      resolveGradientFrame(handle, *computedGradient, pathBounds, viewBox, ref.contextRemap);
  if (!frame.has_value()) {
    // Recognized as radial but frame is degenerate (singular transform, etc).
    // Drop the draw - no meaningful output possible.
    return ResolvedRadialGradient{};
  }

  if (computedGradient->stops.empty()) {
    return ResolvedRadialGradient{};
  }

  const double radius =
      resolveGradientCoord(radial->r, frame->coordBounds, frame->numbersArePercent);
  // SVG2: a radius of zero collapses the gradient to a single point. Match
  // the removed full-Skia backend's behavior of painting a solid color equal to the LAST
  // stop in the gradient - this keeps elements visible for valid degenerate
  // radial gradients (e.g., `r="0"` with a single visible color).
  if (radius <= 0.0) {
    ResolvedRadialGradient out;
    const auto& lastStop = computedGradient->stops.back();
    const css::RGBA base = lastStop.color.resolve(currentColor, opacity);
    // Multiply in stop-opacity per SVG2 (stop-color * stop-opacity) and then
    // the overall paint opacity factor that `buildGradientStops` also honors.
    const double stopOpacity = std::clamp<double>(lastStop.opacity, 0.0, 1.0);
    out.solidFallback =
        css::RGBA(base.r, base.g, base.b, static_cast<uint8_t>(std::round(base.a * stopOpacity)));
    return out;
  }

  const double focalRadius =
      resolveGradientCoord(radial->fr, frame->coordBounds, frame->numbersArePercent);
  const Vector2d center =
      resolveGradientCoords(radial->cx, radial->cy, frame->coordBounds, frame->numbersArePercent);
  // SVG 2: if `fx` / `fy` aren't specified, they coincide with `cx` / `cy`.
  // Resolved on the spot - keeps the geometry resolution local to the
  // shader's coordinate system.
  const Vector2d focalCenter =
      resolveGradientCoords(radial->fx.value_or(radial->cx), radial->fy.value_or(radial->cy),
                            frame->coordBounds, frame->numbersArePercent);

  // Empty annulus: focal circle entirely contains the outer circle, so the
  // gradient never has a valid `t` anywhere. Match tiny-skia: drop the draw
  // (no shader can produce meaningful colors).
  const double centerDistance = (center - focalCenter).length();
  if (centerDistance + radius <= focalRadius) {
    return ResolvedRadialGradient{};
  }

  buildGradientStops(*computedGradient, currentColor, opacity, stopsStorage, outStopsTruncated);

  geode::RadialGradientParams params;
  params.center = center;
  params.focalCenter = focalCenter;
  params.radius = radius;
  params.focalRadius = focalRadius;
  params.gradientFromPath = frame->gradientFromPath;
  params.spreadMode = toGeoSpreadMode(computedGradient->spreadMethod);
  params.stops = std::span<const geode::RadialGradientParams::Stop>(stopsStorage);

  ResolvedRadialGradient out;
  out.gradient = std::move(params);
  return out;
}

struct RendererGeodeTextureKey {
  uint32_t width = 0;
  uint32_t height = 0;
  gpu::TextureFormat format = gpu::TextureFormat::RGBA8Unorm;
  gpu::TextureUsage usage = gpu::TextureUsage::None;
  // Every current caller uses one sample, but key it anyway: a pooled single-sample texture
  // handed to a future multisample request would validate and silently render wrong. Mip level
  // count and dimension are not keyed because the runtime creates one-mip 2D textures only, so
  // there is no second value for them to take.
  uint32_t sampleCount = 1;

  auto operator<=>(const RendererGeodeTextureKey& other) const = default;

  /// Key identifying the bucket \p desc allocates from.
  /// @param desc Descriptor a texture was, or would be, created with.
  static RendererGeodeTextureKey From(const gpu::TextureDescriptor& desc) {
    return RendererGeodeTextureKey{desc.size.width, desc.size.height, desc.format, desc.usage,
                                   desc.sampleCount};
  }
};

struct RendererGeodeTextureBucket {
  std::vector<gpu::Texture> free;
  uint64_t lastUsedFrame = 0;
};

/**
 * Device-shared pool for transient Geode render textures.
 *
 * Exact-size reuse remains available across RendererGeode instances sharing one GeodeDevice, while
 * one retained-memory budget covers the editor's main, compositor, and thumbnail renderers.
 *
 * The pool's currency is the runtime's own \c gpu::Texture, so a pooled texture is already named
 * to the runtime when it is handed out and needs no per-frame import. It holds the device it
 * allocates from rather than taking one per call, because a texture must be destroyed through
 * that device on eviction and in this pool's destructor, where no caller is present to supply
 * one.
 */
class RendererGeodeTexturePool {
public:
  /// Constructs a pool allocating from \p device.
  /// @param device Device every pooled texture is created on and destroyed through.
  explicit RendererGeodeTexturePool(std::shared_ptr<geode::GeodeDevice> device)
      : device_(std::move(device)) {}

  ~RendererGeodeTexturePool() {
    for (auto& [unusedKey, bucket] : buckets_) {
      (void)unusedKey;
      for (gpu::Texture& texture : bucket.free) {
        destroyPooledTexture(std::move(texture));
      }
    }
  }

  void beginFrame() {
    std::lock_guard lock(mutex_);
    ++currentFrameIndex_;
    evictStalePoolBuckets();
  }

  [[nodiscard]] RendererGeodeTexturePoolStats stats() const {
    std::lock_guard lock(mutex_);
    RendererGeodeTexturePoolStats result;
    result.textureCount = pooledTextureCount_;
    result.bytes = pooledTextureBytes_;
    result.budgetBytes = kTexturePoolBudgetBytes;
    for (const auto& [unusedKey, bucket] : buckets_) {
      (void)unusedKey;
      if (!bucket.free.empty()) {
        ++result.bucketCount;
      }
    }
    return result;
  }

  /// Takes a pooled texture matching \p desc, or creates one on a miss. The runtime counts the
  /// creation itself, so a hit ticks no counter and a miss ticks exactly one.
  /// @param desc Descriptor the texture must match exactly.
  gpu::Texture acquire(const gpu::TextureDescriptor& desc) {
    const RendererGeodeTextureKey key = RendererGeodeTextureKey::From(desc);
    {
      std::lock_guard lock(mutex_);
      RendererGeodeTextureBucket& bucket = buckets_[key];
      bucket.lastUsedFrame = currentFrameIndex_;
      if (!bucket.free.empty()) {
        gpu::Texture texture = std::move(bucket.free.back());
        bucket.free.pop_back();
        pooledTextureBytes_ -= textureByteSize(key);
        --pooledTextureCount_;
        return texture;
      }
    }

    gpu::Result<gpu::Texture> created = device_->adapterDevice().createTexture(desc);
    if (created.hasError()) {
      return gpu::Texture{};
    }
    return std::move(created).result();
  }

  /// Returns \p texture to its bucket, or destroys it when a ceiling rejects it.
  /// @param texture Texture to pool; consumed either way.
  /// @param desc Descriptor \p texture was acquired with.
  void release(gpu::Texture texture, const gpu::TextureDescriptor& desc) {
    if (!texture.isValid()) {
      return;
    }

    std::lock_guard lock(mutex_);
    const RendererGeodeTextureKey key = RendererGeodeTextureKey::From(desc);
    const auto existingBucket = buckets_.find(key);
    if (existingBucket != buckets_.end() &&
        existingBucket->second.free.size() >= kMaxPoolEntriesPerKey) {
      destroyPooledTexture(std::move(texture));
      return;
    }

    const uint64_t textureBytes = textureByteSize(key);
    if (!evictPoolEntriesToFit(textureBytes)) {
      destroyPooledTexture(std::move(texture));
      return;
    }

    RendererGeodeTextureBucket& bucket = buckets_[key];
    bucket.lastUsedFrame = currentFrameIndex_;
    bucket.free.push_back(std::move(texture));
    pooledTextureBytes_ += textureBytes;
    ++pooledTextureCount_;
  }

private:
  // Filter-heavy documents (Splash: three Gaussian blur groups) hold more
  // than 8 live intermediates of one size at once, so the original cap of 8
  // forced ~22 texture re-creations per frame. The cap still keeps one
  // bucket's retained bytes comfortably inside the 64 MiB budget (16 x
  // 1.8 MiB for the 892x512 intermediates), so raising it trades entry
  // churn without inviting budget-eviction churn. Reducing the number of
  // concurrent intermediates (blur ping-pong reuse) is the follow-up that
  // would let the cap come back down.
  static constexpr std::size_t kMaxPoolEntriesPerKey = 16;
  static constexpr uint64_t kTexturePoolBudgetBytes = 64u * 1024u * 1024u;
  static constexpr uint64_t kBucketEvictAfterFrames = 120;

  static uint64_t textureByteSize(const RendererGeodeTextureKey& key) {
    // Every current pool caller uses a single-sampled, one-mip 32-bit RGBA or BGRA texture.
    return static_cast<uint64_t>(key.width) * static_cast<uint64_t>(key.height) * 4u;
  }

  /// Destroys the backend object behind \p texture, not just this pool's name for it.
  ///
  /// Dropping the handle alone releases the runtime slot and leaves the backend texture resident
  /// until the host runtime collects it, which is exactly where a succession of evicted pool
  /// entries shows up as retained memory.
  /// @param texture Texture to destroy; consumed.
  void destroyPooledTexture(gpu::Texture&& texture) {
    if (!texture.isValid()) {
      return;
    }
    const gpu::Status destroyed =
        device_->adapterDevice().destroyTextureBacking(std::move(texture));
    (void)destroyed;  // A texture this pool owns is always live; a stale handle is already gone.
  }

  void destroyPoolBucket(const RendererGeodeTextureKey& key, RendererGeodeTextureBucket& bucket) {
    const uint64_t bucketBytes = textureByteSize(key) * static_cast<uint64_t>(bucket.free.size());
    for (gpu::Texture& texture : bucket.free) {
      destroyPooledTexture(std::move(texture));
    }
    pooledTextureBytes_ -= bucketBytes;
    pooledTextureCount_ -= bucket.free.size();
  }

  bool evictPoolEntriesToFit(uint64_t incomingBytes) {
    if (incomingBytes > kTexturePoolBudgetBytes) {
      return false;
    }

    while (pooledTextureBytes_ > kTexturePoolBudgetBytes - incomingBytes) {
      auto victim = buckets_.end();
      for (auto it = buckets_.begin(); it != buckets_.end(); ++it) {
        if (it->second.free.empty()) {
          continue;
        }
        if (victim == buckets_.end() || it->second.lastUsedFrame < victim->second.lastUsedFrame) {
          victim = it;
        }
      }
      if (victim == buckets_.end()) {
        return false;
      }

      destroyPooledTexture(std::move(victim->second.free.back()));
      victim->second.free.pop_back();
      pooledTextureBytes_ -= textureByteSize(victim->first);
      --pooledTextureCount_;
      if (victim->second.free.empty()) {
        buckets_.erase(victim);
      }
    }
    return true;
  }

  void evictStalePoolBuckets() {
    for (auto it = buckets_.begin(); it != buckets_.end();) {
      if (currentFrameIndex_ - it->second.lastUsedFrame > kBucketEvictAfterFrames) {
        destroyPoolBucket(it->first, it->second);
        it = buckets_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::shared_ptr<geode::GeodeDevice> device_;
  mutable std::mutex mutex_;
  std::map<RendererGeodeTextureKey, RendererGeodeTextureBucket> buckets_;
  uint64_t pooledTextureBytes_ = 0;
  std::size_t pooledTextureCount_ = 0;
  uint64_t currentFrameIndex_ = 0;
};

/// The one pool serving \p device, created on first use. Renderers sharing a device share its
/// pool, so an exact-size texture one renderer released is available to the next.
/// @param device Device to pool textures for; the pool holds it for as long as it lives, because
///   it destroys its textures through that device.
std::shared_ptr<RendererGeodeTexturePool> TexturePoolForDevice(
    const std::shared_ptr<geode::GeodeDevice>& device) {
  static std::mutex registryMutex;
  static std::map<geode::GeodeDevice*, std::weak_ptr<RendererGeodeTexturePool>> pools;

  std::lock_guard lock(registryMutex);
  for (auto it = pools.begin(); it != pools.end();) {
    if (it->second.expired()) {
      it = pools.erase(it);
    } else {
      ++it;
    }
  }

  if (const auto existing = pools.find(device.get()); existing != pools.end()) {
    if (std::shared_ptr<RendererGeodeTexturePool> pool = existing->second.lock()) {
      return pool;
    }
  }

  auto pool = std::make_shared<RendererGeodeTexturePool>(device);
  pools[device.get()] = pool;
  return pool;
}

struct GeodeFilterAdmission {
  Box2d region;
  int bufferWidth = 0;
  int bufferHeight = 0;
  int bufferOffsetX = 0;
  int bufferOffsetY = 0;
  bool transformedCaptureReserved = false;
  bool localRasterRequired = false;
  components::FilterExecutionBudget::Reservation reservation;
};

struct GeodeFilterBuffer {
  Box2d region;
  int width = 0;
  int height = 0;
  int offsetX = 0;
  int offsetY = 0;
};

std::optional<GeodeFilterBuffer> ComputeGeodeFilterBuffer(
    const components::FilterGraph& filterGraph, const std::optional<Box2d>& filterRegion,
    const Transform2d& deviceFromFilter, int viewportWidth, int viewportHeight) {
  GeodeFilterBuffer result{
      filterRegion.value_or(Box2d(Vector2d::Zero(), Vector2d(viewportWidth, viewportHeight))),
      viewportWidth, viewportHeight, 0, 0};
  if (filterRegion.has_value()) {
    constexpr int kMaxExpansion = 4096;
    const Box2d deviceRegion = deviceFromFilter.transformBox(*filterRegion);
    const int regionX0 = boundedFloorToInt(deviceRegion.topLeft.x, -kMaxExpansion, 0);
    const int regionY0 = boundedFloorToInt(deviceRegion.topLeft.y, -kMaxExpansion, 0);
    if ((regionX0 < 0 || regionY0 < 0) && graphHasSpatialShift(filterGraph)) {
      result.offsetX = std::min(-regionX0, std::max(0, kMaxExpansion - viewportWidth));
      result.offsetY = std::min(-regionY0, std::max(0, kMaxExpansion - viewportHeight));
      result.width += result.offsetX;
      result.height += result.offsetY;
    }
  }
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(result.width) * static_cast<std::uint64_t>(result.height);
  if (pixels > components::kMaximumFilterSurfacePixels) {
    result.width = viewportWidth;
    result.height = viewportHeight;
    result.offsetX = 0;
    result.offsetY = 0;
  }
  if (result.width <= 0 || result.height <= 0) {
    return std::nullopt;
  }
  return result;
}

bool ShouldPlanGeodeLocalRaster(const components::FilterGraph& filterGraph,
                                const std::optional<Box2d>& filterRegion,
                                const Transform2d& deviceFromFilter,
                                const GeodeFilterBuffer& buffer, bool fullExecutionFits) {
  if (!filterRegion.has_value() || filterRegion->width() <= 0.0 || filterRegion->height() <= 0.0 ||
      buffer.offsetX != 0 || buffer.offsetY != 0 ||
      NearZero(deviceFromFilter.determinant(), 1e-12)) {
    return false;
  }
  return isEligibleForTransformedBlurPath(filterGraph) &&
         (shouldUseTransformedBlurPath(filterGraph, deviceFromFilter) || !fullExecutionFits);
}

struct GeodeLocalRasterGeometry {
  double scaleX = 1.0;
  double scaleY = 1.0;
  double blurPadding = 0.0;
  Box2d paddedRegion;
  uint32_t width = 0;
  uint32_t height = 0;
};

std::optional<GeodeLocalRasterGeometry> ComputeGeodeLocalRasterGeometry(
    const components::FilterGraph& filterGraph, const std::optional<Box2d>& filterRegion,
    const Transform2d& deviceFromFilter, const GeodeFilterBuffer& buffer, bool fullExecutionFits) {
  if (!ShouldPlanGeodeLocalRaster(filterGraph, filterRegion, deviceFromFilter, buffer,
                                  fullExecutionFits)) {
    return std::nullopt;
  }
  const double scaleX =
      std::max(1.0, deviceFromFilter.transformVector(Vector2d(1.0, 0.0)).length());
  const double scaleY =
      std::max(1.0, deviceFromFilter.transformVector(Vector2d(0.0, 1.0)).length());
  const double blurPadding = computeBlurPadding(filterGraph);
  const Box2d paddedRegion(filterRegion->topLeft - Vector2d(blurPadding, blurPadding),
                           filterRegion->bottomRight + Vector2d(blurPadding, blurPadding));
  const std::optional<uint32_t> width = checkedFilterRasterDimension(paddedRegion.width() * scaleX);
  const std::optional<uint32_t> height =
      checkedFilterRasterDimension(paddedRegion.height() * scaleY);
  if (!width || !height) {
    return std::nullopt;
  }
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(*width) * static_cast<std::uint64_t>(*height);
  if (pixels > components::kMaximumFilterSurfacePixels) {
    return std::nullopt;
  }
  return GeodeLocalRasterGeometry{scaleX, scaleY, blurPadding, paddedRegion, *width, *height};
}

std::optional<std::uint64_t> ComputeGeodeLocalFilterPixels(
    const components::FilterGraph& filterGraph, const std::optional<Box2d>& filterRegion,
    const Transform2d& deviceFromFilter, const GeodeFilterBuffer& buffer, bool fullExecutionFits) {
  const std::optional<GeodeLocalRasterGeometry> geometry = ComputeGeodeLocalRasterGeometry(
      filterGraph, filterRegion, deviceFromFilter, buffer, fullExecutionFits);
  if (!geometry.has_value()) {
    return std::nullopt;
  }
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(geometry->width) * static_cast<std::uint64_t>(geometry->height);
  if (!components::FilterGraphFitsExecutionBudget(filterGraph, pixels,
                                                  components::FilterMemoryModel::GpuAllNodes)) {
    return std::nullopt;
  }
  return pixels;
}

std::optional<GeodeFilterAdmission> AdmitGeodeFilter(const components::FilterGraph& filterGraph,
                                                     const std::optional<Box2d>& filterRegion,
                                                     const Transform2d& deviceFromFilter,
                                                     int viewportWidth, int viewportHeight,
                                                     components::FilterExecutionBudget& budget) {
  const std::optional<GeodeFilterBuffer> buffer = ComputeGeodeFilterBuffer(
      filterGraph, filterRegion, deviceFromFilter, viewportWidth, viewportHeight);
  if (!buffer.has_value()) {
    budget.reject();
    return std::nullopt;
  }
  const std::uint64_t bufferPixels =
      static_cast<std::uint64_t>(buffer->width) * static_cast<std::uint64_t>(buffer->height);
  const bool fullExecutionFits = components::FilterGraphFitsExecutionBudget(
      filterGraph, bufferPixels, components::FilterMemoryModel::GpuAllNodes);
  const std::optional<std::uint64_t> localPixels = ComputeGeodeLocalFilterPixels(
      filterGraph, filterRegion, deviceFromFilter, *buffer, fullExecutionFits);
  const std::uint64_t executionPixels = localPixels.has_value() && fullExecutionFits
                                            ? std::max(bufferPixels, *localPixels)
                                            : localPixels.value_or(bufferPixels);
  const std::uint64_t captureBytes = bufferPixels * 4u + localPixels.value_or(0) * 4u;
  auto reservation = budget.reserve(filterGraph, executionPixels,
                                    components::FilterMemoryModel::GpuAllNodes, captureBytes);
  if (!reservation.has_value()) {
    return std::nullopt;
  }
  return GeodeFilterAdmission{buffer->region,     buffer->width,   buffer->height,
                              buffer->offsetX,    buffer->offsetY, localPixels.has_value(),
                              !fullExecutionFits, *reservation};
}

}  // namespace

/// Gate for ordered cross-entity batching. Enabled: a batch's single draw can
/// no longer observe target state that moved on after its instances were
/// appended. A clip rectangle travels in each instance's record and the
/// fragment stage applies it, and the eligibility predicate still refuses any
/// draw taken under a clip polygon, a clip mask, or an open mask pass, which
/// are the states a deferred draw would otherwise read at flush time. The
/// same-entity instanced path and the solo residence flows are unchanged and
/// still handle everything a scene batch declines.
///
/// Kept as a compile-time constant rather than a runtime option so that
/// turning it off folds the whole eligibility predicate away, leaving the
/// record slab, its buffers and its per-entity slots entirely uncreated.
constexpr bool kEnableSceneBatching = true;

struct RendererGeode::Impl : public geode::GeometryDebugSink,
                             public geode::GeometryAdmission,
                             public geode::FilterTextureAllocator {
  bool verbose = false;
  bool antialias = true;
  std::function<void()> offscreenCreationHookForTesting;

  // Per-frame perf counters. Reset at `beginFrame`, read via
  // `lastFrameTimings()`; `GeodePerf_tests.cc` pins their ceilings.
  geode::GeodeCounters counters;
  std::shared_ptr<components::FilterExecutionBudget> filterExecutionBudget =
      std::make_shared<components::FilterExecutionBudget>();
  bool ownsFilterExecutionBudget = true;
  std::shared_ptr<RendererFilterPreparationBudget> filterPreparationBudget =
      std::make_shared<RendererFilterPreparationBudget>();
  bool ownsFilterPreparationBudget = true;
  std::shared_ptr<geode::GeodeFrameGeometryBudget> geometryBudget =
      std::make_shared<geode::GeodeFrameGeometryBudget>();
  bool ownsGeometryBudget = true;
  std::shared_ptr<RendererSurfaceBudget> surfaceBudget = std::make_shared<RendererSurfaceBudget>();
  bool ownsSurfaceBudget = true;
  std::shared_ptr<RendererTextMaterializationBudget> textMaterializationBudget =
      std::make_shared<RendererTextMaterializationBudget>();
  bool ownsTextMaterializationBudget = true;
  std::size_t frameResourceScopeDepth = 0;
  std::shared_ptr<geode::GeodeDocumentGeometryBudget::Limits> documentGeometryLimits =
      std::make_shared<geode::GeodeDocumentGeometryBudget::Limits>();
  struct DocumentGeometryFrameState {
    std::vector<std::shared_ptr<geode::GeodeDocumentGeometryBudget>> touched;

    void reset() { touched.clear(); }

    void touch(const std::shared_ptr<geode::GeodeDocumentGeometryBudget>& budget) {
      const auto existing = std::find_if(touched.begin(), touched.end(), [&](const auto& value) {
        return value.get() == budget.get();
      });
      if (existing == touched.end()) {
        touched.push_back(budget);
      }
    }
  };
  std::shared_ptr<DocumentGeometryFrameState> documentGeometryFrameState =
      std::make_shared<DocumentGeometryFrameState>();

  // GPU resources. Created in the constructor; if device creation fails,
  // `device` is null and the renderer enters a no-op state.
  //
  // Held via shared_ptr so that test fixtures can share a single GeodeDevice
  // across many short-lived renderer instances (see RendererTestBackendGeode).
  std::shared_ptr<geode::GeodeDevice> device;
  // Non-owning: the pipelines live on `device`. Shared across all
  // RendererGeode instances pointing at the same GeodeDevice (issue
  // #575 - per-renderer pipeline construction leaked ~1.6 MB/renderer
  // through wgpu-native's internal cache; not released even after
  // `wgpuDevicePoll(wait=true)`).
  geode::GeodePipeline* pipeline = nullptr;
  geode::GeodeGradientPipeline* gradientPipeline = nullptr;
  geode::GeodeImagePipeline* imagePipeline = nullptr;

  // --- Host-provided target texture (embedding) ---
  //
  // When non-null, `beginFrame()` renders into this texture instead of
  // creating its own offscreen target. The host retains ownership.
  wgpu::Texture hostTarget;
  bool preserveTargetOnBeginFrame = false;

  // Texture format for all render targets. Matches the GeodeDevice's configured
  // format (RGBA8Unorm for headless, host-specified for embedded mode).
  wgpu::TextureFormat textureFormat = wgpu::TextureFormat::RGBA8Unorm;

  // Per-frame resources, recreated in `beginFrame`.
  RenderViewport viewport;
  int pixelWidth = 0;
  int pixelHeight = 0;
  // Dimensions of `target` as it was created. When the next `beginFrame`
  // comes in at the same size,
  // the textures are reused; otherwise they're reallocated.
  int targetWidth = 0;
  int targetHeight = 0;
  wgpu::Texture target;  // Borrowed active render target.
  /// Primary render target this renderer allocated, held as the runtime handle that owns it.
  /// Null while the embedder supplies the target instead.
  gpu::Texture ownedTarget;
  /// Runtime name for an embedder-supplied target. The embedder owns that texture; this only
  /// names it for the frame's encoders, and is refreshed whenever the supplied target changes.
  gpu::Texture hostTargetHandle;
  /// The runtime handle for the frame's primary target, owned or embedder-supplied. Null when
  /// there is no usable target this frame.
  const gpu::Texture* targetHandle = nullptr;
  /// Backend identity of the texture \ref targetHandle names. Layers, masks, patterns and filter
  /// buffers redirect \ref target to pooled backend textures mid-frame, so the handle is used
  /// only while the two still name the same texture rather than on the assumption that they do.
  WGPUTexture targetHandleTexture = nullptr;
  std::optional<RendererGeodeTextureSnapshot> borrowedTargetSnapshot;

  // Single CommandEncoder owned by RendererGeode for the whole frame.
  // All GeoEncoder instances created during the frame (base + push/pop
  // layer / filter / mask) share this CommandEncoder, so push/pop
  // boundaries no longer force a queue().submit(). Finalised + submitted
  // once in endFrame. The filter engine may chunk this slot mid-frame
  // (finish + submit + replace every 64 filter passes) to bound
  // command-buffer size for pathological filter graphs; the final
  // encoder in the slot is the one endFrame submits.
  geode::ScopedWgpuHandle<wgpu::CommandEncoder> frameCommandEncoder;

  // Runtime command encoder for the frame. It records into `frameCommandEncoder`
  // rather than owning a command buffer of its own: filter passes are still
  // recorded directly on the backend encoder, and pass ordering only holds if
  // every pass appends to the same command buffer. Recreated alongside
  // `frameCommandEncoder` whenever that slot is replaced.
  //
  // A runtime encoder is single-use: what it records reaches the backend encoder only when it is
  // finished and submitted. Because filter passes are recorded on the backend encoder directly,
  // that replay has to happen at each point in the frame where the two interleave, so the frame
  // holds a succession of runtime encoders rather than one. Every encoder stays alive until the
  // frame ends: a finished one records nothing more, but the GeoEncoders that recorded through it
  // still hold a reference to it.
  std::deque<std::unique_ptr<gpu::CommandEncoder>> frameGpuEncoders;
  gpu::CommandEncoder* frameGpuEncoder = nullptr;

  // Runtime handles naming render targets this frame's encoders draw into or
  // sample. The textures themselves belong to the recycling pool or to the
  // embedding host; these handles only name them, so they are dropped at the
  // frame boundary, before a recycled texture could be reached through a stale
  // handle.
  std::deque<gpu::Texture> frameImportedTextures;
  std::deque<gpu::TextureView> frameImportedTextureViews;

  /// Names a backend-owned texture as a runtime texture handle valid for the rest of the frame.
  /// The extent comes from the texture itself, so a caller can never describe it wrongly.
  const gpu::Texture& importTexture(const wgpu::Texture& texture, wgpu::TextureFormat format,
                                    wgpu::TextureUsage usage) {
    gpu::Result<gpu::Texture> imported = device->adapterDevice().importExternalTexture(
        texture, gpu::Extent2d{texture.getWidth(), texture.getHeight()},
        geode::GpuTextureFormatFromWgpu(format), geode::GpuTextureUsageFromWgpu(usage));
    UTILS_RELEASE_ASSERT_MSG(imported.hasResult(), "Failed to name a render target as a texture");
    frameImportedTextures.push_back(std::move(imported).result());
    return frameImportedTextures.back();
  }

  /// Opens a runtime view over an already-named texture, valid for the rest of the frame. Views
  /// cover the whole texture, so this addresses exactly the texels any other full-texture view of
  /// the same texture would.
  const gpu::TextureView& importTextureView(const gpu::Texture& texture) {
    gpu::Result<gpu::TextureView> view = device->adapterDevice().createTextureView(
        texture, gpu::TextureViewDescriptor{"RendererGeodeImportedView"});
    UTILS_RELEASE_ASSERT_MSG(view.hasResult(), "Failed to open a view of a render target");
    frameImportedTextureViews.push_back(std::move(view).result());
    return frameImportedTextureViews.back();
  }

  /// Names a filter-engine result texture. Filter intermediates are storage-writable, a
  /// capability the runtime does not model; what survives the mapping is exactly what the
  /// compositing draw needs, which is the ability to sample it.
  const gpu::Texture& importFilterResult(const wgpu::Texture& texture) {
    return importTexture(texture, kFilterIntermediateFormat,
                         wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc);
  }

  /// Names a pooled texture as a runtime handle, taking its capabilities from the descriptor it
  /// was created from.
  const gpu::Texture& importTexture(const wgpu::Texture& texture,
                                    const wgpu::TextureDescriptor& desc) {
    return importTexture(texture, desc.format, desc.usage);
  }

  /// Runtime name for whatever the renderer is drawing into right now.
  ///
  /// The primary target already has one, because the runtime allocated it or named the
  /// embedder's texture once for the frame. Anything else is a pooled backend texture that gets
  /// a frame-lived name here.
  const gpu::Texture& activeTarget() {
    if (targetHandle != nullptr && static_cast<WGPUTexture>(target) == targetHandleTexture) {
      return *targetHandle;
    }
    return importTarget(target);
  }

  /// Names the active render target, which always carries render-attachment, sampled and
  /// copy-source capability so layers can be composited and read back.
  const gpu::Texture& importTarget(const wgpu::Texture& texture) {
    return importTexture(texture, textureFormat,
                         wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                             wgpu::TextureUsage::CopySrc);
  }

  /// The backend texture behind a runtime handle, for the subsystems that still record through
  /// wgpu directly (the filter engine, and the target alias the snapshot and readback paths
  /// borrow). Borrowed: the runtime handle is what owns it.
  /// @param texture Live runtime texture handle.
  wgpu::Texture backendTextureOf(const gpu::Texture& texture) const {
    return device->adapterDevice().wgpuTextureOf(texture);
  }

  /// The runtime's spelling of the renderer's surface format.
  gpu::TextureFormat gpuTextureFormat() const {
    return geode::GpuTextureFormatFromWgpu(textureFormat);
  }

  /// Extent of the active render target, in texels, read from the target itself.
  gpu::Extent2d targetExtent() const {
    return gpu::Extent2d{target.getWidth(), target.getHeight()};
  }

  /// Extent of a backend texture, in texels, as the runtime spells it.
  static gpu::Extent2d extentOf(const wgpu::Texture& texture) {
    return gpu::Extent2d{texture.getWidth(), texture.getHeight()};
  }

  /// Point the runtime device at the current frame command encoder and open a runtime encoder
  /// that records into it. Returns false when the runtime refuses the encoder.
  [[nodiscard]] bool openFrameGpuEncoder() {
    device->adapterDevice().setHostCommandEncoder(frameCommandEncoder.get());
    gpu::Result<std::unique_ptr<gpu::CommandEncoder>> created =
        device->adapterDevice().createCommandEncoder();
    if (!created.hasResult()) {
      return false;
    }
    frameGpuEncoders.push_back(std::move(created).result());
    frameGpuEncoder = frameGpuEncoders.back().get();
    return frameGpuEncoder != nullptr;
  }

  /// Replay what the runtime encoder has recorded into the frame command encoder and open a fresh
  /// one. Call before recording on the frame command encoder directly and before finishing it, so
  /// the two streams reach the backend in the order they were written.
  [[nodiscard]] bool flushFrameGpuEncoder() {
    if (frameGpuEncoder == nullptr) {
      return true;
    }
    gpu::Result<gpu::CommandBuffer> commandBuffer = frameGpuEncoder->finish();
    if (!commandBuffer.hasError()) {
      gpu::Result<uint64_t> submitted =
          device->adapterDevice().submit(std::move(commandBuffer).result());
      if (submitted.hasError()) {
        std::fprintf(stderr, "[Geode] replaying the frame's recorded draws failed: %s\n",
                     submitted.error().message.c_str());
        return false;
      }
    } else {
      std::fprintf(stderr, "[Geode] closing the frame's recorded draws failed: %s\n",
                   commandBuffer.error().message.c_str());
      return false;
    }
    return openFrameGpuEncoder();
  }

  /// Close the runtime encoder after the frame command encoder it recorded into has been
  /// submitted, so the runtime can retire the work it recorded.
  void closeFrameGpuEncoderAfterSubmit() {
    frameGpuEncoders.clear();
    frameGpuEncoder = nullptr;
    device->adapterDevice().notifyHostSubmitted();
    device->adapterDevice().clearHostCommandEncoder();
  }

  std::unique_ptr<geode::GeoEncoder> encoder;
  std::vector<std::unique_ptr<geode::GeoEncoder>> frameFinishedEncoders;

  void retireFinishedEncoder(std::unique_ptr<geode::GeoEncoder> finishedEncoder) {
    if (finishedEncoder) {
      frameFinishedEncoders.push_back(std::move(finishedEncoder));
    }
  }

  void retireActiveEncoder() {
    if (!encoder) {
      return;
    }
    encoder->finish();
    // Replay before the encoder's recorded draws can outlive what they name: the resources a
    // draw references stay alive only as long as whoever recorded it keeps them, and a
    // subsequent encoder is free to re-upload over them.
    (void)flushFrameGpuEncoder();
    retireFinishedEncoder(std::move(encoder));
  }

  // Reusable scratch storage for gradient stop vectors - keeps the
  // per-fillPath allocation counts down and lets the `std::span` in
  // `LinearGradientParams` remain stable across the call.
  std::vector<geode::LinearGradientParams::Stop> gradientStopScratch;

  // CPU-side state.
  PaintParams paint;
  Transform2d deviceFromLocalTransform;
  std::vector<Transform2d> deviceFromLocalTransformStack;

  // --- Pattern tile state ---
  //
  // When the driver calls `beginPatternTile`, we save the active `encoder`
  // and transform state onto `patternStack`, then allocate an offscreen
  // tile texture + a fresh `GeoEncoder` that redirects subsequent draws into
  // it. `endPatternTile` finishes that encoder, pops the saved state, and
  // stashes the resulting texture as the current fill/stroke pattern paint
  // via `patternFillPaint` / `patternStrokePaint`.
  //
  // The Slug fill shader supports pattern sampling directly (paintMode==1),
  // so the subsequent draw call samples the tile through the existing fill
  // pipeline - no separate textured-quad pass is needed and the path's
  // Slug coverage test naturally handles arbitrary (non-rectangular) fills.

  // Forward declaration so `PatternStackFrame::savedClipStack` can name the
  // clip-stack entry type (defined in full further down alongside `clipStack`).
  struct ClipStackEntry;

  /// A completed pattern tile ready to be sampled as fill or stroke paint.
  struct PatternPaintSlot {
    geode::ScopedWgpuHandle<wgpu::Texture> tile;
    /// Runtime alias for `tile`, owned by the frame's import list. Pattern slots do not outlive
    /// the frame that recorded them, so this never outlives the alias it points at.
    const gpu::Texture* tileHandle = nullptr;
    Vector2d rasterTileSize;
    Transform2d targetFromRaster;
  };

  struct PatternStackFrame {
    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    wgpu::Texture savedTarget;
    Transform2d savedDeviceFromLocalTransform;
    std::vector<Transform2d> savedDeviceFromLocalTransformStack;
    int savedPixelWidth = 0;
    int savedPixelHeight = 0;
    // Outer clip stack (filter-region scissor, clip-path masks, etc.) saved at
    // `beginPatternTile`. The pattern tile rasterises into its OWN texture in a
    // private coordinate space, so the outer scissor/clip MUST NOT apply to it
    // - otherwise the outer filter region's device-pixel scissor (e.g.
    // (10,10)-(490,490)) clips the tile texture's top-left rows/columns,
    // shifting every tiled cell. Restored in `endPatternTile`.
    std::vector<ClipStackEntry> savedClipStack;

    // The pattern tile being recorded.
    Box2d tileRect;                                      // In pattern space (topLeft at origin).
    Transform2d targetFromPattern;                       // Transform used when the tile is sampled.
    geode::ScopedWgpuHandle<wgpu::Texture> tileTexture;  // Sampled after recording.
    int tilePixelWidth = 0;
    int tilePixelHeight = 0;
    // Scale factor applied to all `setTransform` calls while this frame is
    // active, mapping pattern-tile units to tile-texture pixels so the
    // encoder's viewport math works out.
    Vector2d rasterScale = Vector2d(1.0, 1.0);

    // Pattern paints pending at `beginPatternTile` time, saved so tile-content draws don't
    // consume the outer element's pattern slots (e.g. a `context-fill` pattern shared between
    // several consumers re-rendering the same tile subtree). Restored by `endPatternTile`.
    std::optional<PatternPaintSlot> savedPatternFillPaint;
    /// @see savedPatternFillPaint
    std::optional<PatternPaintSlot> savedPatternStrokePaint;
  };
  std::vector<PatternStackFrame> patternStack;

  // --------------------------------------------------------------------
  // Transient-texture pool.
  //
  // Every push/pop of isolated-layer / filter-layer / mask / clip-mask
  // scratch allocates offscreen textures that fired on every frame even when the same
  // document was re-rendered at the same viewport. The pool holds
  // released textures keyed by `(width, height, format, usage)`;
  // same-dim / same-format acquisition on a later frame pops
  // from the bucket instead of calling `createTexture`.
  //
  // Exact-size pooling (no power-of-two bucketing). Works for the
  // repeat-render case this PR targets because layer sizes are
  // derived from `pixelWidth`/`pixelHeight`, which don't change
  // between idle re-renders. A size-bucketing extension is a future
  // follow-up for viewport-resize scenarios.
  // --------------------------------------------------------------------

  std::shared_ptr<RendererGeodeTexturePool> texturePool;

  [[nodiscard]] RendererGeodeTexturePoolStats texturePoolStats() const {
    return texturePool ? texturePool->stats() : RendererGeodeTexturePoolStats{};
  }

  /// Detach a superseded primary target and release it at the next frame boundary.
  ///
  /// Dropping the handle here would free the target while work already recorded against it is
  /// still unsubmitted. Hand it to the deferred-destroy pass instead, which holds it through the
  /// current frame boundary; that also keeps a succession of resized targets from piling up,
  /// which browsers, notably Safari, otherwise leave resident until their own collector runs.
  void retireOwnedTargetAtFrameBoundary() {
    target = wgpu::Texture();
    targetHandle = nullptr;
    targetHandleTexture = nullptr;
    targetWidth = 0;
    targetHeight = 0;
    if (!ownedTarget.isValid()) {
      return;
    }

    if (device) {
      device->deferDestroy(std::move(ownedTarget));
    }
    ownedTarget = gpu::Texture();
  }

  /// Cross-frame arena buffer pool. Every
  /// `GeoEncoder` this renderer constructs gets a pointer via
  /// `setBufferPool`; encoders recycle their fully-grown arena buffers
  /// through it instead of re-creating them each frame. Companion to
  /// `texturePool` on the buffer side.
  geode::GeodeBufferPool arenaBufferPool;

  /// Apply the renderer-wide encoder configuration shared by every encoder
  /// this renderer constructs: arena buffer recycling and the antialias mode.
  void configureEncoder(geode::GeoEncoder& encoderToConfigure) {
    encoderToConfigure.setBufferPool(&arenaBufferPool);
    encoderToConfigure.setGeometryAdmission(this);
    encoderToConfigure.setAntialias(antialias);
  }

  /// Frame index used to age resident cache entries and to tell "this frame
  /// already claimed that buffer" from "an earlier, submitted frame did".
  ///
  /// DEVICE-scoped, not per-renderer: an offscreen renderer built from this
  /// device renders `feImage` fragments and layer thumbnails out of the same
  /// document while this renderer's frame is still open, and a per-renderer
  /// counter would let those two frames alias. See
  /// `GeodeDevice::beginFrameGeneration`.
  uint64_t currentFrameIndex = 0;
  /// True while `currentFrameIndex` names a generation this renderer opened and
  /// has not closed. Frames are closed at `endFrame`, at the next `beginFrame`
  /// (a caller that abandons a frame), and at teardown.
  bool frameGenerationOpen = false;

  /// Close this renderer's open frame generation, if any.
  void closeFrameGeneration() {
    if (frameGenerationOpen && device) {
      device->endFrameGeneration(currentFrameIndex);
    }
    frameGenerationOpen = false;
  }

  /// Charges one surface of \p size against the frame's surface budget.
  /// @param size Extent of the surface about to be allocated, in texels.
  [[nodiscard]] bool reserveTextureSurface(const gpu::Extent2d& size) {
    if (!device || size.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        size.height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        !surfaceBudget->reserve(static_cast<int>(size.width), static_cast<int>(size.height))) {
      return false;
    }
    return true;
  }

  /// Acquire a pooled texture matching `desc`, or create a fresh one
  /// on miss. Always increments the `textureCreates` counter on miss;
  /// never on hit. Returns an invalid texture on device failure.
  gpu::Texture acquireTexture(const gpu::TextureDescriptor& desc) {
    if (!texturePool || !reserveTextureSurface(desc.size)) {
      return gpu::Texture{};
    }
    return texturePool->acquire(desc);
  }

  gpu::Texture acquireFilterTexture(const gpu::TextureDescriptor& desc) override {
    return acquireTexture(desc);
  }

  /// Return a texture to its pool bucket IMMEDIATELY. Caller must
  /// pass the same descriptor used to acquire, otherwise the next
  /// acquire with the original descriptor will miss the bucket.
  ///
  /// Prefer `releaseTextureAtFrameEnd` for textures whose GPU work was
  /// recorded into the shared `frameCommandEncoder`: releasing those
  /// mid-frame would let a subsequent `acquireTexture` on the same
  /// bucket hand the texture back out before the GPU has finished
  /// writing it.
  void releaseTexture(gpu::Texture texture, const gpu::TextureDescriptor& desc) {
    if (!texture.isValid()) {
      return;
    }
    const bool releasedSurface =
        desc.size.width <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
        desc.size.height <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
        surfaceBudget->release(static_cast<int>(desc.size.width),
                               static_cast<int>(desc.size.height));
    UTILS_RELEASE_ASSERT(releasedSurface);
    if (texturePool) {
      texturePool->release(std::move(texture), desc);
    } else {
      // No pool to return it to, so the backend object is destroyed here rather than left
      // resident until the host runtime collects it.
      const gpu::Status destroyed =
          device->adapterDevice().destroyTextureBacking(std::move(texture));
      (void)destroyed;
    }
  }

  /// Defer a release until after the frame's command buffer has been
  /// submitted. Used by `popIsolatedLayer` / `popFilterLayer` / etc.,
  /// where the layer texture is still referenced by commands recorded
  /// into the frame encoder and must not be recycled mid-frame.
  struct PendingRelease {
    gpu::Texture texture;
    gpu::TextureDescriptor desc;
  };
  std::vector<PendingRelease> framePendingReleases;

  void releaseTextureAtFrameEnd(gpu::Texture texture, const gpu::TextureDescriptor& desc) {
    if (!texture.isValid()) {
      return;
    }
    framePendingReleases.push_back({std::move(texture), desc});
  }

  void releaseFilterTextureAtFrameEnd(gpu::Texture texture,
                                      const gpu::TextureDescriptor& desc) override {
    releaseTextureAtFrameEnd(std::move(texture), desc);
  }

  void drainPendingReleases() {
    for (auto& pending : framePendingReleases) {
      releaseTexture(std::move(pending.texture), pending.desc);
    }
    framePendingReleases.clear();
  }

  bool submitFilterBudgetChunk() {
    if (!device || !frameCommandEncoder || !target || !filterStack.empty() ||
        filterExecutionBudget->rejectionReason() !=
            components::FilterExecutionBudget::RejectionReason::MemoryLimit ||
        filterExecutionBudget->activeGpuReservations() != 0 ||
        filterExecutionBudget->retainedGpuBytes() == 0) {
      return false;
    }

    retireActiveEncoder();
    if (!flushFrameGpuEncoder()) {
      return false;
    }
    geode::ScopedWgpuHandle<wgpu::CommandBuffer> commandBuffer(frameCommandEncoder.get().finish());
    if (!commandBuffer) {
      return false;
    }
    device->queue().submit(1, &commandBuffer.get());
    device->countSubmit();
    closeFrameGpuEncoderAfterSubmit();

    frameFinishedEncoders.clear();
    drainPendingReleases();

    wgpu::CommandEncoderDescriptor descriptor = {};
    descriptor.label = wgpuLabel("RendererGeodeFilterBudgetChunk");
    frameCommandEncoder.reset(device->device().createCommandEncoder(descriptor));
    if (!frameCommandEncoder) {
      return false;
    }
    if (!openFrameGpuEncoder()) {
      return false;
    }
    encoder =
        std::make_unique<geode::GeoEncoder>(*device, *pipeline, *gradientPipeline, *imagePipeline,
                                            activeTarget(), targetExtent(), *frameGpuEncoder);
    configurePathEncoder(*encoder);
    encoder->setLoadPreserve();
    updateEncoderScissor();
    return filterExecutionBudget->beginChunkAfterSubmit();
  }

  /// A saved encoder + target state for an in-progress isolated layer
  /// (`pushIsolatedLayer` / `popIsolatedLayer`). When the driver begins a
  /// group with non-identity opacity or a non-Normal blend mode, we
  /// redirect subsequent draws into an offscreen texture of the same size
  /// as the current target. On `pop`, the offscreen is composited back
  /// onto the saved target with the stored opacity.
  struct LayerStackFrame {
    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    wgpu::Texture savedTarget;
    gpu::Texture layerTexture;
    /// Descriptors captured at push time so `popIsolatedLayer` can
    /// release the textures back to the correct pool bucket via
    /// `releaseTexture`.
    gpu::TextureDescriptor layerDesc = {};
    double opacity = 1.0;
    /// SVG `mix-blend-mode`. `Normal` (default) keeps the
    /// plain premultiplied source-over compositing path;
    /// anything else drives `popIsolatedLayer` through the blend-blit
    /// variant that snapshots the parent and uses the W3C formulas.
    MixBlendMode blendMode = MixBlendMode::Normal;
  };
  std::vector<LayerStackFrame> layerStack;

  /// GPU filter-graph executor. Non-owning pointer into the
  /// shared GeodeDevice - see pipeline field comment above for why.
  geode::GeodeFilterEngine* filterEngine = nullptr;

  /// State for an in-progress `<mask>` element. Two offscreen
  /// textures, one capturing the mask element's content and one
  /// capturing the masked subtree. `popMask` composites them via
  /// `GeoEncoder::blitFullTargetMasked` back onto the saved parent
  /// target.
  ///
  /// Phase sequencing matches `RendererTinySkia`:
  ///   * `pushMask` → allocate mask capture, redirect encoder.
  ///   * `transitionMaskToContent` → switch to content texture.
  ///   * `popMask` → blit (content * luminance(mask)) onto parent.
  struct MaskStackFrame {
    enum class Phase { Capturing, Content };
    Phase phase = Phase::Capturing;
    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    wgpu::Texture savedTarget;
    gpu::Texture maskTexture;     // Mask element's content (RGBA).
    gpu::Texture contentTexture;  // Masked element's content (RGBA).
    /// Descriptors captured at push for texture-pool release.
    gpu::TextureDescriptor maskDesc = {};
    gpu::TextureDescriptor contentDesc = {};
    /// Raw mask-bounds rectangle from the driver, in the coordinate
    /// space of `maskBoundsTransform` (userSpaceOnUse or the
    /// objectBoundingBox-mapped user space - either way, NOT yet in
    /// device pixels).
    std::optional<Box2d> maskBounds;
    /// `deviceFromLocalTransform` snapshotted at `pushMask` time so that
    /// `popMask` can lift `maskBounds` into device-pixel space. This
    /// mirrors `RendererTinySkia::SurfaceFrame::maskBoundsTransform`.
    Transform2d maskBoundsTransform;
    MaskType maskType = MaskType::Luminance;
  };
  std::vector<MaskStackFrame> maskStack;

  std::optional<PatternPaintSlot> patternFillPaint;
  std::optional<PatternPaintSlot> patternStrokePaint;

  /// Axis-aligned clip rectangles in target-pixel coords. Each entry
  /// corresponds to a `pushClip` with a non-empty `clipRect`. The active
  /// scissor is the intersection of every entry's `pixelRect` on this
  /// stack.
  /// Entries with `valid == false` represent pushClip calls that had no
  /// `clipRect` component (path- or mask-only clips) - they're tracked
  /// so `popClip` stays balanced with `pushClip`.
  ///
  /// For non-axis-aligned ancestor transforms (e.g., a rotated `<svg>`
  /// or `<use>`), the scissor rect is the AABB of the transformed clip
  /// rect - which over-reports coverage. In that case the entry also
  /// carries the 4 polygon corners of the clip in device-pixel space,
  /// and the fragment shader tests each sample against the polygon's
  /// half-planes on top of the scissor rect. We only honour the TOPMOST
  /// polygon-bearing entry (`setClipPolygon` has no in-shader
  /// intersection with a previous polygon) - nested rotated clips are
  /// rare enough that we accept the over-coverage fallback.
  struct ClipStackEntry {
    ClipStackEntry() = default;
    ~ClipStackEntry() = default;

    // Move-only, explicitly. The mask textures this entry owns are move-only runtime handles, so
    // a copy could never have worked; saying so here matters because a standard library that
    // instantiates a container's copy constructor eagerly (libstdc++ does, libc++ does not)
    // fails to compile on the implicit copy rather than on a copy anyone wrote.
    ClipStackEntry(const ClipStackEntry&) = delete;
    ClipStackEntry& operator=(const ClipStackEntry&) = delete;
    ClipStackEntry(ClipStackEntry&&) = default;
    ClipStackEntry& operator=(ClipStackEntry&&) = default;

    Box2d pixelRect;
    bool valid = false;
    bool hasPolygon = false;
    bool allocationRejected = false;
    Vector2d polygonCorners[4];
    /// Path-clip mask. When non-null these name a 1-sample texture sampled
    /// by the fill / gradient pipelines through their clip-mask bindings.
    /// The texture is allocated per `pushClip` call and parked in
    /// `maskLayerTextures`, which keeps it alive until `popClip`.
    ///
    /// For nested `<clipPath>` references, the pushClip code builds
    /// one mask per clip-path layer (deepest first); each outer
    /// layer's mask is rendered with the previous layer's mask as an
    /// input clip mask so every outer shape is intersected with the
    /// deeper union. The outermost layer's resolve is the one named here.
    ///
    /// The texture handle names the entry in \ref maskLayerTextures that owns the mask; the
    /// view is an alias owned by the frame's import list. Both are valid for the rest of the
    /// frame and are null together.
    const gpu::Texture* maskResolveTextureHandle = nullptr;
    const gpu::TextureView* maskResolveViewHandle = nullptr;
    /// Paired (texture, descriptor) entries. Every clip-mask texture
    /// allocated by `pushClip` (across all nested layers) lives here
    /// until `popClip` hands them back to the texture pool.
    /// A deque, not a vector: \ref maskResolveTextureHandle points at one of these entries and
    /// stays valid while the owning clip entry is moved between the live stack and a filter
    /// frame's saved stack, which a vector's reallocation would not survive.
    std::deque<PendingRelease> maskLayerTextures;
  };

  /// State for an in-progress filter layer. Captures all draws
  /// between `pushFilterLayer` / `popFilterLayer` into an offscreen texture,
  /// then runs the stored `FilterGraph` through `GeodeFilterEngine` and
  /// composites the result back onto the outer target.
  struct FilterStackFrame {
    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    wgpu::Texture savedTarget;
    gpu::Texture layerTexture;
    /// Descriptors captured at push for texture-pool release.
    gpu::TextureDescriptor layerDesc = {};
    components::FilterGraph filterGraph;
    components::FilterExecutionBudget::Reservation filterReservation;
    Box2d filterRegion;
    Transform2d deviceFromFilter;  // Full CTM at push time.
    bool transformedCaptureReserved = false;
    bool localRasterRequiredForBudget = false;
    int filterBufferOffsetX = 0;  // Expansion into negative device X.
    int filterBufferOffsetY = 0;  // Expansion into negative device Y.
    std::vector<ClipStackEntry> savedClipStack;
    bool allocationRejected = false;
  };
  std::vector<ClipStackEntry> clipStack;
  std::vector<FilterStackFrame> filterStack;
  std::size_t rejectedFilterDepth = 0;

  bool initializeClipEntry(const ResolvedClip& clip, ClipStackEntry& entry) {
    if (rejectedFilterDepth != 0) {
      clipStack.push_back(std::move(entry));
      return false;
    }
    if (!clip.clipRect.has_value()) {
      return true;
    }
    const Transform2d& deviceFromClip = deviceFromLocalTransform;
    entry.pixelRect = deviceFromClip.transformBox(*clip.clipRect);
    entry.valid = true;
    const double a = deviceFromClip.data[0];
    const double b = deviceFromClip.data[1];
    const double c = deviceFromClip.data[2];
    const double d = deviceFromClip.data[3];
    constexpr double kAxisAlignedEps = 1e-9;
    const bool axisAligned = (std::abs(b) < kAxisAlignedEps && std::abs(c) < kAxisAlignedEps) ||
                             (std::abs(a) < kAxisAlignedEps && std::abs(d) < kAxisAlignedEps);
    if (axisAligned) {
      return true;
    }
    const Box2d& local = *clip.clipRect;
    entry.polygonCorners[0] =
        deviceFromClip.transformPosition(Vector2d(local.topLeft.x, local.topLeft.y));
    entry.polygonCorners[1] =
        deviceFromClip.transformPosition(Vector2d(local.bottomRight.x, local.topLeft.y));
    entry.polygonCorners[2] =
        deviceFromClip.transformPosition(Vector2d(local.bottomRight.x, local.bottomRight.y));
    entry.polygonCorners[3] =
        deviceFromClip.transformPosition(Vector2d(local.topLeft.x, local.bottomRight.y));
    entry.hasPolygon = true;
    return true;
  }

  bool readyForFilterCapture() const {
    return device && pipeline && gradientPipeline && imagePipeline && encoder && filterEngine;
  }

  void pushRejectedFilterFrame() {
    FilterStackFrame frame;
    frame.savedEncoder = std::move(encoder);
    frame.savedTarget = target;
    frame.allocationRejected = true;
    filterStack.push_back(std::move(frame));
    ++rejectedFilterDepth;
  }

  bool beginFilterCapture(const components::FilterGraph& filterGraph,
                          const GeodeFilterAdmission& admission,
                          const Transform2d& deviceFromFilter) {
    const gpu::TextureDescriptor textureDesc{
        "RendererGeodeFilterLayer",
        gpu::Extent2d{static_cast<uint32_t>(admission.bufferWidth),
                      static_cast<uint32_t>(admission.bufferHeight)},
        gpuTextureFormat(),
        gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::Sampled |
            gpu::TextureUsage::CopySrc};
    gpu::Texture layerTexture = acquireTexture(textureDesc);
    if (!layerTexture.isValid()) {
      return false;
    }

    encoder->finish();
    FilterStackFrame frame;
    frame.savedEncoder = std::move(encoder);
    frame.savedTarget = target;
    frame.layerDesc = textureDesc;
    frame.filterGraph = filterGraph;
    frame.filterReservation = admission.reservation;
    frame.filterRegion = admission.region;
    frame.deviceFromFilter = deviceFromFilter;
    frame.transformedCaptureReserved = admission.transformedCaptureReserved;
    frame.localRasterRequiredForBudget = admission.localRasterRequired;
    frame.filterBufferOffsetX = admission.bufferOffsetX;
    frame.filterBufferOffsetY = admission.bufferOffsetY;
    frame.savedClipStack = std::move(clipStack);
    clipStack.clear();

    frame.layerTexture = std::move(layerTexture);
    target = backendTextureOf(frame.layerTexture);
    auto newEncoder = std::make_unique<geode::GeoEncoder>(*device, *pipeline, *gradientPipeline,
                                                          *imagePipeline, importTarget(target),
                                                          textureDesc.size, *frameGpuEncoder);
    configurePathEncoder(*newEncoder, /*collectGeometry=*/true, admission.bufferOffsetX,
                         admission.bufferOffsetY);
    newEncoder->clear(css::RGBA(0, 0, 0, 0));
    encoder = std::move(newEncoder);
    if (admission.bufferOffsetX != 0 || admission.bufferOffsetY != 0) {
      deviceFromLocalTransform =
          deviceFromLocalTransform *
          Transform2d::Translate(admission.bufferOffsetX, admission.bufferOffsetY);
    }
    ClipStackEntry filterClipEntry;
    filterClipEntry.pixelRect = deviceFromLocalTransform.transformBox(admission.region);
    filterClipEntry.valid = true;
    clipStack.push_back(std::move(filterClipEntry));
    filterStack.push_back(std::move(frame));
    updateEncoderScissor();
    return true;
  }

  bool tryCompositeTransformedFilter(FilterStackFrame& frame) {
    if (!frame.transformedCaptureReserved || !filterEngine || frame.filterGraph.empty()) {
      return false;
    }
    const GeodeFilterBuffer buffer{frame.filterRegion, static_cast<int>(frame.layerDesc.size.width),
                                   static_cast<int>(frame.layerDesc.size.height),
                                   frame.filterBufferOffsetX, frame.filterBufferOffsetY};
    const std::optional<GeodeLocalRasterGeometry> geometry = ComputeGeodeLocalRasterGeometry(
        frame.filterGraph, frame.filterRegion, frame.deviceFromFilter, buffer,
        !frame.localRasterRequiredForBudget);
    if (!geometry.has_value()) {
      return false;
    }

    const gpu::TextureDescriptor localDesc{
        "RendererGeodeBlurLocal", gpu::Extent2d{geometry->width, geometry->height},
        gpuTextureFormat(),
        gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::Sampled |
            gpu::TextureUsage::CopySrc};
    gpu::Texture localTexture = acquireTexture(localDesc);
    if (!localTexture.isValid()) {
      return false;
    }
    const wgpu::Texture localBackendTexture = backendTextureOf(localTexture);

    const Transform2d filterFromDevice = frame.deviceFromFilter.inverse();
    const Transform2d localFromDevice = filterFromDevice *
                                        Transform2d::Translate(-geometry->paddedRegion.topLeft.x,
                                                               -geometry->paddedRegion.topLeft.y) *
                                        Transform2d::Scale(geometry->scaleX, geometry->scaleY);
    geode::GeoEncoder resampleEncoder(*device, *pipeline, *gradientPipeline, *imagePipeline,
                                      importTarget(localBackendTexture), localDesc.size,
                                      *frameGpuEncoder);
    configureEncoder(resampleEncoder);
    resampleEncoder.setTransform(localFromDevice);
    resampleEncoder.drawTexture(
        importTarget(backendTextureOf(frame.layerTexture)),
        Box2d::FromXYWH(0.0, 0.0, static_cast<double>(frame.layerDesc.size.width),
                        static_cast<double>(frame.layerDesc.size.height)),
        kWholeTextureUv, 1.0, /*pixelated=*/false, /*sourceIsPremultiplied=*/true);
    resampleEncoder.finish();

    const Transform2d localDeviceFromFilter =
        Transform2d::Scale(geometry->scaleX, geometry->scaleY);
    const Box2d localFilterRegion(Vector2d(geometry->blurPadding, geometry->blurPadding),
                                  Vector2d(geometry->blurPadding + frame.filterRegion.width(),
                                           geometry->blurPadding + frame.filterRegion.height()));
    if (!flushFrameGpuEncoder()) {
      return false;
    }
    wgpu::Texture localFiltered =
        filterEngine->execute(frame.filterGraph, localBackendTexture, localFilterRegion,
                              localDeviceFromFilter, *this, frameCommandEncoder);

    const Transform2d deviceFromLocal =
        Transform2d::Scale(1.0 / geometry->scaleX, 1.0 / geometry->scaleY) *
        Transform2d::Translate(geometry->paddedRegion.topLeft.x, geometry->paddedRegion.topLeft.y) *
        frame.deviceFromFilter;
    target = frame.savedTarget;
    auto compositeEncoder = std::make_unique<geode::GeoEncoder>(
        *device, *pipeline, *gradientPipeline, *imagePipeline, importTarget(frame.savedTarget),
        targetExtent(), *frameGpuEncoder);
    configurePathEncoder(*compositeEncoder);
    compositeEncoder->setLoadPreserve();
    encoder = std::move(compositeEncoder);
    updateEncoderScissor();
    encoder->setTransform(deviceFromLocal);
    encoder->drawTexture(importFilterResult(localFiltered),
                         Box2d::FromXYWH(0.0, 0.0, static_cast<double>(geometry->width),
                                         static_cast<double>(geometry->height)),
                         kWholeTextureUv, 1.0, /*pixelated=*/false, /*sourceIsPremultiplied=*/true);
    encoder->setTransform(Transform2d());
    releaseTextureAtFrameEnd(std::move(localTexture), localDesc);
    frame.localRasterRequiredForBudget = false;
    return true;
  }

  /// Recompute the intersection of every rectangular clip entry on
  /// `clipStack` and apply it to the active encoder as a scissor,
  /// plus forward the topmost polygon clip (if any) through
  /// `setClipPolygon`. Called whenever the clip stack changes.
  void updateEncoderScissor() {
    if (!encoder) {
      return;
    }
    std::optional<Box2d> active;
    const ClipStackEntry* polygonEntry = nullptr;
    const ClipStackEntry* maskEntry = nullptr;
    for (const ClipStackEntry& entry : clipStack) {
      if (entry.allocationRejected) {
        encoder->clearClipPolygon();
        encoder->clearClipMask();
        encoder->setScissorRect(0, 0, 0, 0);
        return;
      }
      if (entry.valid) {
        if (!active.has_value()) {
          active = entry.pixelRect;
        } else {
          // Intersect: take the overlap of the two rectangles.
          const double x0 = std::max(active->topLeft.x, entry.pixelRect.topLeft.x);
          const double y0 = std::max(active->topLeft.y, entry.pixelRect.topLeft.y);
          const double x1 = std::min(active->bottomRight.x, entry.pixelRect.bottomRight.x);
          const double y1 = std::min(active->bottomRight.y, entry.pixelRect.bottomRight.y);
          active = Box2d(Vector2d(x0, y0), Vector2d(std::max(x0, x1), std::max(y0, y1)));
        }
      }
      if (entry.hasPolygon) {
        // Overwrite with the topmost polygon entry; no multi-polygon
        // intersection in the shader (see ClipStackEntry docs).
        polygonEntry = &entry;
      }
      if (entry.maskResolveViewHandle != nullptr) {
        // Same deal for the path-clip mask - we always bind the
        // topmost one, and nested path-clip intersections are a
        // TODO (would need multiple clip-mask bindings in the
        // fragment shader or a per-clip compositing pass).
        maskEntry = &entry;
      }
    }

    if (polygonEntry != nullptr) {
      encoder->setClipPolygon(polygonEntry->polygonCorners);
    } else {
      encoder->clearClipPolygon();
    }

    if (maskEntry != nullptr) {
      // Pass the parent texture alongside the view so the encoder keeps
      // the Vulkan resource alive even after this clip-stack entry is
      // destroyed. The 1-arg setClipMask overload accidentally left the
      // view dangling across `popClip`→`pop_back`→destructor →
      // `updateEncoderScissor` sequences; see issue #551.
      encoder->setClipMask(*maskEntry->maskResolveTextureHandle, *maskEntry->maskResolveViewHandle);
    } else {
      encoder->clearClipMask();
    }

    if (!active.has_value()) {
      encoder->clearScissorRect();
      return;
    }
    // Convert the Box2d corners to integer pixel coords. Floor on topLeft
    // and ceil on bottomRight so we don't accidentally clip fractional
    // edge pixels that the rasterizer still wants to cover.
    const int32_t x = static_cast<int32_t>(std::floor(active->topLeft.x));
    const int32_t y = static_cast<int32_t>(std::floor(active->topLeft.y));
    const int32_t w = std::max(0, static_cast<int32_t>(std::ceil(active->bottomRight.x)) - x);
    const int32_t h = std::max(0, static_cast<int32_t>(std::ceil(active->bottomRight.y)) - y);
    encoder->setScissorRect(x, y, w, h);
  }

  // Stub-state latches - set on first warning in verbose mode so each
  // unimplemented feature logs exactly once per renderer.
  bool warnedLayer = false;
  bool warnedGradient = false;
  bool warnedText = false;

  /// Resolve the current fill/stroke paint's fallback to a solid RGBA color
  /// when the referenced paint server can't instantiate. Returns nullopt for
  /// None and for paint-server references without a fallback.
  std::optional<css::RGBA> resolveSolidFill() {
    return resolveSolidPaint(paint.fill, paint.fillOpacity);
  }

  std::optional<css::RGBA> resolveSolidStroke() {
    return resolveSolidPaint(paint.stroke, paint.strokeOpacity);
  }

  std::optional<css::RGBA> resolveSolidPaint(const components::ResolvedPaintServer& server,
                                             double effectiveOpacity) {
    if (std::holds_alternative<PaintServer::None>(server)) {
      return std::nullopt;
    }
    const css::RGBA currentColor = paint.currentColor.rgba();
    const float opacity = static_cast<float>(effectiveOpacity);
    if (const auto* solid = std::get_if<PaintServer::Solid>(&server)) {
      return solid->color.resolve(currentColor, opacity);
    }
    if (const auto* ref = std::get_if<components::PaintResolvedReference>(&server)) {
      if (ref->fallback.has_value()) {
        return ref->fallback->resolve(currentColor, opacity);
      }
    }
    return std::nullopt;
  }

  /// Build a pattern paint whose shader coordinates and repeat period are both in raster-tile
  /// pixels. The fill shader's sample position is path-local, which is also the pattern target
  /// space supplied by the driver, so no device-space reconstruction is needed.
  geode::GeoEncoder::PatternPaint buildPatternPaint(const PatternPaintSlot& slot,
                                                    double opacity) const {
    geode::GeoEncoder::PatternPaint p;
    p.tile = slot.tileHandle;
    p.tileSize = slot.rasterTileSize;
    p.patternFromPath = slot.targetFromRaster.inverse();
    p.opacity = opacity;
    return p;
  }

  /// Push the renderer's deviceFromLocalTransform onto the encoder before drawing.
  void syncTransform() {
    if (encoder) {
      encoder->setTransform(deviceFromLocalTransform);
    }
  }

  /// Path-local curve-flattening tolerance for stroke outlines generated at the
  /// current transform.
  ///
  /// Stroke geometry is submitted in path-local (document) space and scaled by
  /// `deviceFromLocalTransform` on the GPU, so a fixed path-local tolerance is
  /// magnified by the view scale - a circle flattened once at document scale
  /// shows as a visible segment chain at any meaningful zoom. Deriving the
  /// tolerance from the draw-time transform keeps the chord error under
  /// `geode::kStrokeFlattenDevicePixels` device pixels at every scale. The
  /// scale is quantized to power-of-two buckets so a continuous zoom re-flattens
  /// only on bucket crossings.
  [[nodiscard]] double strokeFlattenTolerance() const {
    return geode::StrokeFlattenToleranceFor(deviceFromLocalTransform);
  }

  /// Record a freshly flattened stroke outline for the perf/regression
  /// counters. Cache hits do not call this, so the counter measures the
  /// flattening work a frame actually performed - and, because the point count
  /// scales with the flattening density, it is the observable signal that the
  /// tolerance tracked the device scale.
  void countStrokeOutline(const Path& strokedOutline) {
    ++counters.strokeOutlineFlattens;
    counters.strokeOutlinePoints += strokedOutline.points().size();
  }

  // --------------------------------------------------------------------
  // Path-encode cache.
  //
  // Our entt `on_update<ComputedPathComponent>` / `on_destroy<ComputedPathComponent>`
  // listener is connected lazily at `draw()` entry. Presence is tracked
  // via a sentinel context component on the registry itself
  // (`ListenerInstalled`) - pointer-identity on `&registry` would be
  // unsafe across document lifetimes (a destroyed document's registry
  // memory can be reused, giving us the same pointer value for an
  // entirely different `entt::basic_registry` with no listener).
  // --------------------------------------------------------------------

  /// Sentinel context component, emplaced on a registry the first
  /// time it's seen by `ensureCacheInvalidationWired`. Lifetime ties
  /// to the registry - dies with it, so a re-allocated registry at
  /// the same address doesn't carry the tag.
  struct ListenerInstalled {};

  /// `<use>`-batch detection: track the source
  /// entity of the most recent `drawPath` call so `drawPath` can bump
  /// `sameSourceDrawPairs` whenever it sees two consecutive
  /// entity-matched calls. Reset to `entt::null` at `beginFrame`.
  /// Value is the `PathShape::sourceEntity`'s entity (not its
  /// registry-qualified handle - two drawPath calls from different
  /// registries are never considered "consecutive same-source").
  Entity lastDrawSourceEntity = entt::null;

  /// Debug geometry overlay (see `RendererGeode::setDebugGeometryOverlay`).
  /// Default off; the normal path pays only the null-sink check at an
  /// actual Slug submission and allocates no capture storage.
  bool debugGeometryOverlay = false;

  struct GeometryDebugEdge {
    Vector2d a;
    Vector2d b;
  };
  std::vector<GeometryDebugEdge> geometryDebugEdges;

  /// Map pixels in the current encoder target back into final root-target
  /// pixels. Filter captures may be expanded at their top/left; all other
  /// layer, mask, and clip targets share the root pixel coordinate system.
  [[nodiscard]] Transform2d geometryDebugRootFromCurrentTarget(
      int additionalFilterOffsetX = 0, int additionalFilterOffsetY = 0) const {
    int offsetX = additionalFilterOffsetX;
    int offsetY = additionalFilterOffsetY;
    for (const FilterStackFrame& frame : filterStack) {
      offsetX += frame.filterBufferOffsetX;
      offsetY += frame.filterBufferOffsetY;
    }
    return Transform2d::Translate(-offsetX, -offsetY);
  }

  /// Configure a path-capable encoder. Pattern-tile content deliberately
  /// opts out: the consuming path is the root-visible Slug submission, while
  /// resource-internal geometry must not leak into the document overlay.
  void configurePathEncoder(geode::GeoEncoder& pathEncoder, bool collectGeometry = true,
                            int additionalFilterOffsetX = 0, int additionalFilterOffsetY = 0) {
    configureEncoder(pathEncoder);
    if (debugGeometryOverlay && collectGeometry) {
      pathEncoder.setGeometryDebugSink(this, geometryDebugRootFromCurrentTarget(
                                                 additionalFilterOffsetX, additionalFilterOffsetY));
    }
  }

  /// Capture the exact post-vertex Slug triangles at the GPU submission
  /// boundary. This reproduces `slug_fill.wgsl`'s half-pixel miter dilation
  /// and its ill-conditioned-transform AABB fallbacks before mapping the
  /// vertices into root-target device pixels.
  void recordSlugDraw(const geode::EncodedPath& encoded, const Transform2d& targetFromPath,
                      const Transform2d& rootFromTarget,
                      std::span<const float> instanceTransforms) override {
    if (!debugGeometryOverlay || !patternStack.empty() || encoded.boundingVertexCount < 3u) {
      return;
    }

    const size_t instanceCount = instanceTransforms.empty() ? 1u : instanceTransforms.size() / 8u;
    if (!instanceTransforms.empty() && instanceTransforms.size() != instanceCount * 8u) {
      return;
    }

    const auto instanceAt = [&](size_t index) {
      Transform2d instance;
      if (instanceTransforms.empty()) {
        return instance;
      }
      const float* packed = instanceTransforms.data() + index * 8u;
      instance.data[0] = packed[0];
      instance.data[2] = packed[1];
      instance.data[4] = packed[2];
      instance.data[1] = packed[4];
      instance.data[3] = packed[5];
      instance.data[5] = packed[6];
      return instance;
    };
    for (size_t instanceIndex = 0; instanceIndex < instanceCount; ++instanceIndex) {
      const Transform2d instance = instanceAt(instanceIndex);
      const auto targetVector = [&](const Vector2d& vector) {
        return targetFromPath.transformVector(instance.transformVector(vector));
      };
      const Vector2d xAxis = targetVector(Vector2d(1.0, 0.0));
      const Vector2d yAxis = targetVector(Vector2d(0.0, 1.0));
      const double determinant = xAxis.x * yAxis.y - xAxis.y * yAxis.x;
      const double axisScale = xAxis.length() * yAxis.length();
      const bool axesWellConditioned =
          axisScale > 0.0 && axisScale < 1e30 && std::abs(determinant) > axisScale * 1e-6;

      std::vector<Vector2d> pathVertices;
      std::vector<Vector2d> targetVertices;
      pathVertices.reserve(encoded.boundingVertexCount);
      targetVertices.reserve(encoded.boundingVertexCount);
      for (uint32_t index = 0; index < encoded.boundingVertexCount; ++index) {
        const auto& vertex = encoded.boundingVertices[index];
        pathVertices.emplace_back(vertex.x, vertex.y);
        targetVertices.push_back(
            targetFromPath.transformPosition(instance.transformPosition(pathVertices.back())));
      }

      double pathAabbExpansion = 0.0;
      const double maxAxisComponent =
          std::max({std::abs(xAxis.x), std::abs(xAxis.y), std::abs(yAxis.x), std::abs(yAxis.y)});
      if (maxAxisComponent > 0.0 && maxAxisComponent < 1e30) {
        const Vector2d scaledX = xAxis / maxAxisComponent;
        const Vector2d scaledY = yAxis / maxAxisComponent;
        const double scaledDeterminant = std::abs(scaledX.x * scaledY.y - scaledX.y * scaledY.x);
        if (scaledDeterminant > 0.0) {
          const double scaledFrobenius = std::sqrt(scaledX.dot(scaledX) + scaledY.dot(scaledY));
          const double expansion =
              0.7071068 * scaledFrobenius / (maxAxisComponent * scaledDeterminant);
          if (expansion > 0.0 && expansion < 1e30) {
            pathAabbExpansion = expansion;
          }
        }
      }

      bool useDeviceAabb = false;
      if (axesWellConditioned) {
        const double orientation = determinant > 0.0 ? 1.0 : -1.0;
        for (uint32_t index = 0; index < encoded.boundingVertexCount; ++index) {
          const uint32_t previousIndex =
              (index + encoded.boundingVertexCount - 1u) % encoded.boundingVertexCount;
          const uint32_t nextIndex = (index + 1u) % encoded.boundingVertexCount;
          const Vector2d incoming = targetVertices[index] - targetVertices[previousIndex];
          const Vector2d outgoing = targetVertices[nextIndex] - targetVertices[index];
          const double incomingLength = incoming.length();
          const double outgoingLength = outgoing.length();
          if (!(incomingLength > 1e-6 && incomingLength < 1e30 && outgoingLength > 1e-6 &&
                outgoingLength < 1e30)) {
            useDeviceAabb = true;
            break;
          }

          const Vector2d incomingEdge = incoming / incomingLength;
          const Vector2d outgoingEdge = outgoing / outgoingLength;
          const Vector2d incomingNormal(orientation * incomingEdge.y,
                                        -orientation * incomingEdge.x);
          const Vector2d outgoingNormal(orientation * outgoingEdge.y,
                                        -orientation * outgoingEdge.x);
          const double denominator = 1.0 + incomingNormal.dot(outgoingNormal);
          if (!(denominator > 1e-6)) {
            useDeviceAabb = true;
            break;
          }
          const Vector2d miter = (incomingNormal + outgoingNormal) * (0.5 / denominator);
          if (!(miter.length() <= 2.0)) {
            useDeviceAabb = true;
            break;
          }
        }
      }

      const bool usePathAabb = !axesWellConditioned && pathAabbExpansion > 0.0;
      std::vector<Vector2d> effectiveVertices;
      if (usePathAabb) {
        Vector2d pathMin(1e30, 1e30);
        Vector2d pathMax(-1e30, -1e30);
        for (const Vector2d& vertex : pathVertices) {
          pathMin.x = std::min(pathMin.x, vertex.x);
          pathMin.y = std::min(pathMin.y, vertex.y);
          pathMax.x = std::max(pathMax.x, vertex.x);
          pathMax.y = std::max(pathMax.y, vertex.y);
        }
        effectiveVertices = {
            {pathMin.x - pathAabbExpansion, pathMin.y - pathAabbExpansion},
            {pathMax.x + pathAabbExpansion, pathMin.y - pathAabbExpansion},
            {pathMax.x + pathAabbExpansion, pathMax.y + pathAabbExpansion},
            {pathMin.x - pathAabbExpansion, pathMax.y + pathAabbExpansion},
        };
        for (Vector2d& vertex : effectiveVertices) {
          vertex = targetFromPath.transformPosition(instance.transformPosition(vertex));
        }
      } else if (useDeviceAabb) {
        Vector2d targetMin(1e30, 1e30);
        Vector2d targetMax(-1e30, -1e30);
        for (const Vector2d& vertex : targetVertices) {
          targetMin.x = std::min(targetMin.x, vertex.x);
          targetMin.y = std::min(targetMin.y, vertex.y);
          targetMax.x = std::max(targetMax.x, vertex.x);
          targetMax.y = std::max(targetMax.y, vertex.y);
        }
        effectiveVertices = {
            {targetMin.x - 0.5, targetMin.y - 0.5},
            {targetMax.x + 0.5, targetMin.y - 0.5},
            {targetMax.x + 0.5, targetMax.y + 0.5},
            {targetMin.x - 0.5, targetMax.y + 0.5},
        };
      } else if (!axesWellConditioned) {
        effectiveVertices = targetVertices;
      } else {
        effectiveVertices.reserve(encoded.boundingVertexCount);
        const double orientation = determinant > 0.0 ? 1.0 : -1.0;
        for (uint32_t index = 0; index < encoded.boundingVertexCount; ++index) {
          const uint32_t previousIndex =
              (index + encoded.boundingVertexCount - 1u) % encoded.boundingVertexCount;
          const uint32_t nextIndex = (index + 1u) % encoded.boundingVertexCount;
          const Vector2d previousDelta = targetVertices[index] - targetVertices[previousIndex];
          const Vector2d nextDelta = targetVertices[nextIndex] - targetVertices[index];
          const Vector2d previousEdge = previousDelta / previousDelta.length();
          const Vector2d nextEdge = nextDelta / nextDelta.length();
          const Vector2d previousNormal(orientation * previousEdge.y,
                                        -orientation * previousEdge.x);
          const Vector2d nextNormal(orientation * nextEdge.y, -orientation * nextEdge.x);
          const double denominator = 1.0 + previousNormal.dot(nextNormal);
          effectiveVertices.push_back(targetVertices[index] +
                                      (previousNormal + nextNormal) * (0.5 / denominator));
        }
      }

      for (Vector2d& vertex : effectiveVertices) {
        vertex = rootFromTarget.transformPosition(vertex);
      }

      std::vector<GeometryDebugEdge> submissionEdges;
      submissionEdges.reserve(encoded.boundingDrawVertexCount());
      const auto addUniqueEdge = [&](const Vector2d& a, const Vector2d& b) {
        const bool duplicate = std::ranges::any_of(submissionEdges, [&](const auto& edge) {
          return (edge.a == a && edge.b == b) || (edge.a == b && edge.b == a);
        });
        if (!duplicate) {
          submissionEdges.push_back({a, b});
        }
      };

      const auto polygonIndex = [&](uint32_t vertexIndex) {
        const uint32_t triangle = vertexIndex / 3u;
        if (triangle >= effectiveVertices.size() - 2u) {
          return 0u;
        }
        const uint32_t corner = vertexIndex % 3u;
        return corner == 0u ? 0u : triangle + corner;
      };
      for (uint32_t triangleStart = 0; triangleStart < encoded.boundingDrawVertexCount();
           triangleStart += 3u) {
        const Vector2d& p0 = effectiveVertices[polygonIndex(triangleStart)];
        const Vector2d& p1 = effectiveVertices[polygonIndex(triangleStart + 1u)];
        const Vector2d& p2 = effectiveVertices[polygonIndex(triangleStart + 2u)];
        addUniqueEdge(p0, p1);
        addUniqueEdge(p1, p2);
        addUniqueEdge(p2, p0);
      }
      geometryDebugEdges.insert(geometryDebugEdges.end(), submissionEdges.begin(),
                                submissionEdges.end());
    }
  }

  /// Draw every captured edge once, after normal SVG painting and compositing.
  /// The replay encoder intentionally has no geometry sink, preventing the
  /// overlay path from recursively observing itself.
  void emitGeometryDebugOverlay() {
    if (!debugGeometryOverlay || geometryDebugEdges.empty() || !target || !frameCommandEncoder) {
      return;
    }

    PathBuilder builder;
    for (const GeometryDebugEdge& edge : geometryDebugEdges) {
      const Vector2d direction = edge.b - edge.a;
      const double length = direction.length();
      if (length <= 1e-6) {
        continue;
      }
      const Vector2d halfPixelNormal(-direction.y / length * 0.5, direction.x / length * 0.5);
      builder.moveTo(edge.a + halfPixelNormal)
          .lineTo(edge.b + halfPixelNormal)
          .lineTo(edge.b - halfPixelNormal)
          .lineTo(edge.a - halfPixelNormal)
          .closePath();
    }

    encoder =
        std::make_unique<geode::GeoEncoder>(*device, *pipeline, *gradientPipeline, *imagePipeline,
                                            activeTarget(), targetExtent(), *frameGpuEncoder);
    configureEncoder(*encoder);
    encoder->setLoadPreserve();
    encoder->setTransform(Transform2d());
    encoder->fillPath(builder.build(), css::RGBA(255, 0, 255, 255), FillRule::NonZero);
  }

  /// Deferred `<use>` batch for
  /// consecutive `drawPath` calls that share a source entity + resolved
  /// solid paint + no stroke + no subtree complication. Each matching
  /// call appends its `deviceFromLocalTransform` into `transforms`; the batch
  /// is flushed by `flushPendingBatch()` whenever state changes in a
  /// way that invalidates the batch (paint-key mismatch on the next
  /// drawPath, any push/pop, end of frame, ...). A flush of size >= 2
  /// routes through `GeoEncoder::fillPathInstanced` - one GPU draw
  /// with `instanceCount == N`. Size-1 flushes degrade to the regular
  /// single-draw path so we don't pay the per-instance-buffer cost for
  /// unbatched draws.
  struct PendingBatch {
    enum class Mode : uint8_t {
      None,
      /// Consecutive draws of the SAME entity with the same solid
      /// paint, collapsed into one instanced draw via the arena path
      /// (per-instance transforms, one record per instance).
      SameEntity,
      /// Ordered batching: consecutive DISTINCT entities with
      /// solid paints and resident slots, collapsed into one draw over
      /// their record-slab records (one record per entity).
      Scene,
    };
    Mode mode = Mode::None;

    /// --- SameEntity mode fields ---
    /// Registry the batch's source entity belongs to. Entity ids are only
    /// unique within one registry, so a frame that draws more than one
    /// document (the icon-atlas pass) has to compare this too - otherwise a
    /// second document's identically-numbered entity extends the first
    /// document's batch and is drawn with the first document's geometry.
    Registry* sourceRegistry = nullptr;
    Entity sourceEntity = entt::null;
    css::RGBA color;
    FillRule rule = FillRule::NonZero;
    const geode::EncodedPath* encoded = nullptr;
    /// Borrowed source geometry, read only by the size-1 flush when it falls back to the
    /// arena `fillPath`. This is the one place a `PathShape`'s pointer outlives the
    /// `drawPath` call that delivered it, so it carries the strictest precondition in this
    /// file.
    ///
    /// A batch is only started for a draw with a non-null `sourceEntity`, and for those the
    /// driver points `PathShape::path` at the entity's `ComputedPathComponent::spline`. That
    /// address is stable while the component exists (component storage is paged, so emplacing
    /// other components never relocates it), but erasing that component would swap the
    /// storage's last element into the slot and silently repoint us at another entity's
    /// geometry - wrong pixels, no crash, invisible to ASAN. What makes the retention safe is
    /// that nothing erases a `ComputedPathComponent` between the borrow and the flush: every
    /// removal site runs outside draw traversal, and a flush always happens within the frame
    /// that started the batch. Draws whose geometry lives on the caller's stack (`drawRect`,
    /// `drawEllipse`, overlay chrome) have no source entity and never reach here.
    const Path* path = nullptr;
    /// `deviceFromLocalTransform` captured at each `drawPath`. On flush, the
    /// outer encoder transform is set to identity and these are
    /// uploaded as per-instance transforms (see
    /// `flushPendingBatch` for the math).
    std::vector<Transform2d> deviceFromLocalTransforms;
    /// Encoder clip-state version when the SameEntity batch started. The
    /// scene conversion only converts a pending singleton when the current
    /// clip state matches, so a scene batch never spans a clip change.
    uint64_t sameEntityClipVersion = 0;

    /// --- Scene mode fields ---
    struct SceneInstance {
      geode::GeodeResidentSlot* slot = nullptr;
      const geode::EncodedPath* encoded = nullptr;
      const Path* path = nullptr;
      css::RGBA color;
      FillRule rule = FillRule::NonZero;
      Transform2d deviceFromLocal;
      uint32_t vertexCount = 0;
      /// Record slot resolved by `resolveSceneRecordSlot` for THIS
      /// instance: null means the slot's primary record; non-null points
      /// into `sceneTempRecordSlots` (a deque, so the address is stable
      /// for the rest of the frame) when the primary record was already
      /// referenced this frame. The flush-time re-ensure MUST pass this
      /// same slot - re-ensuring a temp-diverted instance against the
      /// primary record would queue a buffer write that retroactively
      /// retransforms the earlier draw that references the primary
      /// (buffer writes execute before every draw at submit).
      const geode::GeodeRecordSlab::Slot* recordSlotOverride = nullptr;
      /// Last-written bytes of `recordSlotOverride`, when that slot persists
      /// across frames (per-occurrence text records). Non-null turns the
      /// override write into a skip-when-unchanged write. Null for per-frame
      /// temporary slots, which are always fresh and always written.
      std::vector<uint8_t>* recordCacheOverride = nullptr;
      /// The record fields that come from encoder-side state - the paint
      /// scalars published into the slot, and the clip rectangle - captured
      /// when this instance was appended. The flush-time re-ensure replays
      /// these rather than re-reading the slot and the encoder, both of which
      /// can have moved on to a later draw by then.
      geode::GeoEncoder::SceneRecordState recordState;
    };
    /// Consecutive instances. Each instance's record-slab slot index must
    /// be `firstInstanceIndex + i`, its geometry must live in the same
    /// slab chunk, and its record must live in the same record-slab
    /// buffer as the batch.
    std::vector<SceneInstance> sceneInstances;
    gpu::BufferRef sceneChunkBuffer;
    uint64_t sceneChunkBytes = 0;
    gpu::BufferRef sceneRecordBuffer;
    /// Stable identities of the two buffers above (see
    /// `GeodeDevice::AllocateBufferId`). The encoder's bind-group cache
    /// outlives the document, so it keys on these rather than on the handle
    /// addresses, which are recycled once the document is destroyed.
    uint64_t sceneChunkBufferId = 0;
    uint64_t sceneRecordBufferId = 0;
    uint32_t sceneFirstInstance = 0;
    /// Byte offset of the first instance's record inside sceneRecordBuffer
    /// (indices are global across slab chunks; offsets are per-buffer).
    uint64_t sceneFirstRecordOffset = 0;
    uint32_t sceneVertexCount = 0;
    /// Encoder clip-state version at the batch's first append. A batch
    /// never spans a clip change (one draw carries one clip state).
    uint64_t sceneClipVersion = 0;
    /// GPU-residence slot for the encode this batch started on - the source
    /// entity's fill slot for a fill, its stroke slot for a stroked outline.
    /// Used only by the size-1 flush path; a size >= 2 flush routes through
    /// the instanced arena path, which has no per-entity residence.
    geode::GeodeResidentSlot* residentSlot = nullptr;
  };
  std::optional<PendingBatch> pendingBatch;

  /// Record-slab slots allocated this frame for same-entity repeat draws
  /// (markers, repeated `<use>`): one record per draw, so earlier recorded
  /// batches keep their own content at submit time. Freed (deferred) at
  /// the next frame's draw(); the slab merges the frees at beginFrame.
  /// Temporary per-frame record slots for same-frame repeat draws. A deque
  /// keeps element addresses stable (callers hold a pointer to the newest
  /// entry while later draws append), and each entry carries the slab it
  /// was allocated from so a multi-document frame frees every slot into
  /// its own slab rather than whichever document draw() saw last.
  struct SceneTempRecordSlot {
    geode::GeodeRecordSlab::Slot slot;
    std::shared_ptr<geode::GeodeRecordSlab> slab;
  };
  std::deque<SceneTempRecordSlot> sceneTempRecordSlots;

  /// Choose the record slot for a scene-batch instance of `slot` WITHOUT
  /// marking it used: the primary slot when no unsubmitted frame reads it yet
  /// (nullptr: the ensure uses the cached write path), or a fresh temporary
  /// slot otherwise. The primary is off-limits exactly when a recorded BATCH
  /// draw of a still-open frame references it - a repeat earlier in THIS
  /// frame, or an outer renderer whose frame is still open while an offscreen
  /// pass renders the same document - because buffer writes are queue-ordered
  /// ahead of every draw in a frame's submit and rewriting the record (or the
  /// paint block the ensure republishes with it) would retroactively repaint
  /// that draw. Generations are device-scoped, so the claim test compares
  /// against `GeodeDevice::oldestOpenFrameGeneration` rather than our own
  /// frame index.
  ///
  /// A solo resident draw of the same slot is NOT such a reference: it binds
  /// the device's shared identity record and takes every parameter from the
  /// slot's uniform, so it never reads the slot's own record. Diverting for it
  /// cost a fresh temporary record, and its always-write contract, on every
  /// frame of any document that draws one entity both solo and batched.
  /// Callers mark `slot.lastSceneFrame = currentFrameIndex` only after a
  /// successful ensure.
  bool resolveSceneRecordSlot(geode::GeodeResidentSlot& slot,
                              const geode::GeodeRecordSlab::Slot*& outSlot) {
    outSlot = nullptr;
    if (device->frameStampClaimed(slot.lastSceneFrame)) {
      outSlot = allocateTempRecordSlot(slot.recordSlab);
      return outSlot != nullptr;
    }
    return true;
  }

  /// Allocate a record slot that lives only for this frame, retained in
  /// `sceneTempRecordSlots` so its address stays valid until the next
  /// `draw()` returns it to `slab`. Returns null when `slab` is absent or the
  /// device cannot back the allocation.
  const geode::GeodeRecordSlab::Slot* allocateTempRecordSlot(
      const std::shared_ptr<geode::GeodeRecordSlab>& slab) {
    if (!slab) {
      return nullptr;
    }
    geode::GeodeRecordSlab::Slot tempSlot;
    if (!slab->allocateSlot(*device, tempSlot)) {
      return nullptr;
    }
    sceneTempRecordSlots.push_back(SceneTempRecordSlot{tempSlot, slab});
    return &sceneTempRecordSlots.back().slot;
  }

  /// Connect (or rewire) our `on_update<ComputedPathComponent>` /
  /// `on_destroy<ComputedPathComponent>` listener onto `registry`.
  /// Called at the start of each `draw()`. Idempotent for the same
  /// registry. When switching registries (test fixtures reusing one
  /// renderer), disconnects from the old first.
  void ensureCacheInvalidationWired(Registry& registry);

  /// Wipe the cache component from an entity when its source
  /// `ComputedPathComponent` is rewritten by `ShapeSystem` or destroyed.
  /// Connected to entt's `on_update` / `on_destroy` signals.
  /// File-scope free function with this signature is the only shape
  /// entt's `.connect<&fn>()` accepts that doesn't couple lifetime to
  /// `this`.
  static void onComputedPathChanged(Registry& registry, Entity entity);

  /// Scratch buffer for the no-source-entity stroke path. `getStrokeDerived`
  /// uses this as stable storage when there's no `GeodePathCacheComponent`
  /// to live on (e.g. `drawRect` / `drawEllipse` convenience draws). Only
  /// one active draw at a time, so a single slot is safe.
  Path strokeScratchPath;
  std::optional<geode::EncodedPath> fillScratchEncode;
  std::optional<geode::EncodedPath> strokeScratchEncode;
  geode::EncodedPath emptyGeometry;
  geode::EncodedPath rejectedGeometry = [] {
    geode::EncodedPath encoded;
    encoded.outcome = geode::EncodedPath::Outcome::Rejected;
    return encoded;
  }();
  std::deque<geode::EncodedPath> transientTextEncodes;

  struct AdmittedEncode {
    const geode::EncodedPath* encoded = nullptr;
    bool persistent = false;
  };

  /// Value returned by `getFillEncode` / `getStrokeDerived` describing
  /// which encode the caller should pass down to `GeoEncoder`.
  struct StrokeDerived {
    /// Path to draw. Null means "no stroke geometry - skip the draw".
    /// Points into the entity's cache slot on hit, or into
    /// `strokeScratchPath` on the no-entity fallback.
    const Path* strokedPath = nullptr;
    /// Precomputed encode pointer for `GeoEncoder`. Non-null only when
    /// the stroke came from a cache slot - the no-entity fallback
    /// leaves this null and lets `GeoEncoder` encode inline.
    const geode::EncodedPath* encoded = nullptr;
    /// True when both `strokedPath` and `encoded` live in the entity cache.
    bool persistent = false;
    /// Fill rule to use. For open-path strokes, `strokeToFill` emits one
    /// subpath → NonZero; for closed-path strokes, two → EvenOdd.
    FillRule fillRule = FillRule::NonZero;
  };

  bool admitGeometry(const geode::EncodedPath& encoded, std::size_t logicalDraws) override {
    if (encoded.rejected()) {
      geometryBudget->reject();
      return false;
    }
    const std::size_t retainedBytes = encoded.retainedBytes();
    const std::size_t items = encoded.geometryItemCount();
    if (encoded.empty()) {
      return true;
    }
    if (logicalDraws == 0u || retainedBytes == std::numeric_limits<std::size_t>::max() ||
        items > std::numeric_limits<std::size_t>::max() / logicalDraws ||
        retainedBytes > std::numeric_limits<std::uint64_t>::max() / logicalDraws) {
      geometryBudget->reject();
      return false;
    }
    return geometryBudget->reserve(logicalDraws, items * logicalDraws,
                                   static_cast<std::uint64_t>(retainedBytes) * logicalDraws);
  }

  bool canEncodeGeometry() const override { return !geometryBudget->rejected(); }

  void releaseGeometry(const geode::EncodedPath& encoded, std::size_t logicalDraws) override {
    if (encoded.empty() || encoded.rejected() || logicalDraws == 0u) {
      return;
    }
    const std::size_t retainedBytes = encoded.retainedBytes();
    const std::size_t items = encoded.geometryItemCount();
    if (retainedBytes == std::numeric_limits<std::size_t>::max() ||
        items > std::numeric_limits<std::size_t>::max() / logicalDraws ||
        retainedBytes > std::numeric_limits<std::uint64_t>::max() / logicalDraws) {
      return;
    }
    geometryBudget->release(logicalDraws, items * logicalDraws,
                            static_cast<std::uint64_t>(retainedBytes) * logicalDraws);
  }

  std::optional<geode::EncodedPath> encodeGeometry(const Path& path, FillRule rule) {
    if (geometryBudget->rejected()) {
      return std::nullopt;
    }
    device->countPathEncode();
    geode::EncodedPath encoded = geode::GeodePathEncoder::encode(path, rule);
    if (encoded.rejected()) {
      geometryBudget->reject();
      return std::nullopt;
    }
    return encoded;
  }

  const geode::EncodedPath* encodeTransientGeometry(const Path& path, FillRule rule) {
    std::optional<geode::EncodedPath> encoded = encodeGeometry(path, rule);
    if (!encoded.has_value()) {
      return &rejectedGeometry;
    }
    transientTextEncodes.push_back(std::move(*encoded));
    return &transientTextEncodes.back();
  }

  /// Encode-side of the cache. Every path is admitted before insertion or transient upload.
  AdmittedEncode getFillEncode(EntityHandle source, const Path& path, FillRule rule) {
    if (!source) {
      fillScratchEncode = encodeGeometry(path, rule);
      return {fillScratchEncode ? &*fillScratchEncode : &rejectedGeometry, false};
    }
    ensureCacheInvalidationWired(*source.registry());
    std::shared_ptr<geode::GeodeDocumentGeometryBudget> documentBudget =
        documentGeometryBudget(*source.registry());
    auto& cache = source.get_or_emplace<geode::GeodePathCacheComponent>();
    if (cache.fillEncode) {
      return {&*cache.fillEncode, true};
    }

    std::optional<geode::EncodedPath> encoded = encodeGeometry(path, rule);
    if (!encoded.has_value()) {
      return {&rejectedGeometry, false};
    }
    const std::size_t retainedBytes = encoded->retainedBytes();
    if (retainedBytes != std::numeric_limits<std::size_t>::max() &&
        cache.fillReservation.replace(documentBudget, retainedBytes)) {
      cache.fillEncode = std::move(*encoded);
      return {&*cache.fillEncode, true};
    }
    fillScratchEncode = std::move(*encoded);
    return {&*fillScratchEncode, false};
  }

  AdmittedEncode getFillEncodeForPaint(EntityHandle source, const Path& path, FillRule rule) {
    if (!paint.drawFillComponent || std::holds_alternative<PaintServer::None>(paint.fill)) {
      return {&emptyGeometry, false};
    }
    return getFillEncode(source, path, rule);
  }

  /// GPU-residence slot for `source`'s fill encode. Returns null for a null
  /// source (editor/overlay draws stay on the arena path). The slot lives on
  /// a `GeodeResidentPathComponent` beside the CPU encode cache and is
  /// invalidated by the same listener.
  /// Document-scoped resident slab: one growable chunk-set per registry,
  /// bound to the current device. The slab is swapped when the document is
  /// rendered by a different device, and freed with the registry.
  /// Returns the registry's resident slab, shared so slots can keep the
  /// old slab alive across a device change.
  /// Document-scoped painter-ordered record slab (ordered
  /// batching). Mirrors `residentSlab`'s registry-context wiring: one slab
  /// per document, swapped when a different device renders the document.
  std::shared_ptr<geode::GeodeRecordSlab> recordSlab(Registry& registry) {
    std::shared_ptr<geode::GeodeDocumentGeometryBudget> documentBudget =
        documentGeometryBudget(registry);
    auto* slabPtr = registry.ctx().find<std::shared_ptr<geode::GeodeRecordSlab>>();
    if (slabPtr == nullptr) {
      registry.ctx().emplace<std::shared_ptr<geode::GeodeRecordSlab>>(nullptr);
      slabPtr = registry.ctx().find<std::shared_ptr<geode::GeodeRecordSlab>>();
    }
    std::shared_ptr<geode::GeodeRecordSlab>& slab = *slabPtr;
    if (!slab || slab->owningDeviceId() != device->deviceId()) {
      slab =
          std::make_shared<geode::GeodeRecordSlab>(device->deviceId(), std::move(documentBudget));
    }
    return slab;
  }

  /// Ensure `slot` has a record-slab slot on the current device's slab, and
  /// report whether it now has one.
  ///
  /// Only a draw that can join a cross-entity batch needs a record: every
  /// other draw takes its paint from the uniform and binds the device's
  /// shared identity record. Allocating from here rather than from the
  /// residence getters keeps the slab (and its buffer creations, its
  /// per-entity slots and its deferred frees) entirely out of a document
  /// that never batches.
  bool ensureRecordSlot(geode::GeodeResidentSlot& slot, Registry& registry) {
    std::shared_ptr<geode::GeodeRecordSlab> slab = recordSlab(registry);
    if (slot.recordSlab.get() != slab.get()) {
      // Device change: the old slab is retired with the registry-context
      // swap; drop the stale slot handle and allocate fresh on the new
      // slab.
      slot.recordSlab = std::move(slab);
      slot.recordSlot = geode::GeodeRecordSlab::Slot{};
    }
    if (!slot.recordSlot.buffer.isValid()) {
      (void)slot.recordSlab->allocateSlot(*device, slot.recordSlot);
    }
    return slot.recordSlot.buffer.isValid();
  }

  /// Glyph-residency budget, in distinct cached outlines and summed retained
  /// outline plus encode bytes. Defaults come from `GeodeGlyphCache`; a test shrinks them to reach
  /// eviction without building a font-sized working set.
  size_t glyphCacheMaxEntries = geode::GeodeGlyphCache::kDefaultMaxEntries;
  uint64_t glyphCacheMaxRetainedBytes = geode::GeodeGlyphCache::kDefaultMaxRetainedBytes;

  /// Non-cached glyphs needed after the document cache reaches its admission cap. The deque keeps
  /// entry addresses stable for the frame's pending scene batches; beginFrame clears it only after
  /// the previous frame has submitted and its pending batch has been discarded.
  std::deque<geode::GeodeGlyphResidentEntry> transientGlyphEntries;

  /// Document-scoped glyph-outline residency. Mirrors `residentSlab`'s
  /// registry-context wiring: one cache per document, replaced when a
  /// different device renders the document, because every cached entry holds
  /// that device's buffer and bind group.
  std::shared_ptr<geode::GeodeGlyphCache> glyphCache(Registry& registry) {
    auto* cachePtr = registry.ctx().find<std::shared_ptr<geode::GeodeGlyphCache>>();
    if (cachePtr == nullptr) {
      registry.ctx().emplace<std::shared_ptr<geode::GeodeGlyphCache>>(nullptr);
      cachePtr = registry.ctx().find<std::shared_ptr<geode::GeodeGlyphCache>>();
    }
    std::shared_ptr<geode::GeodeGlyphCache>& cache = *cachePtr;
    if (!cache || cache->owningDeviceId() != device->deviceId()) {
      cache = std::make_shared<geode::GeodeGlyphCache>(device->deviceId(),
                                                       documentGeometryBudget(registry));
    } else {
      (void)documentGeometryBudget(registry);
    }
    // Trim to budget at the first touch of each frame, the same shape (and for
    // the same reason) as the slabs' pending-free merge: dropping an entry
    // returns its slab range through the deferred free list, so the range only
    // becomes reusable at the NEXT merge, never inside a frame whose recorded
    // draws still read it. Gating here rather than at one draw entry point
    // covers the multi-document tile paths that never reach `draw()`.
    device->countGlyphResidencyEvictions(
        cache->beginFrame(currentFrameIndex, device->oldestOpenFrameGeneration(),
                          glyphCacheMaxEntries, glyphCacheMaxRetainedBytes));
    return cache;
  }

  /// Per-occurrence record storage for one text element's glyph fills.
  ///
  /// `component` non-null means the element's persistent slots are in play and
  /// `next` is the ordinal of the occurrence about to be drawn. Null means
  /// this pass takes per-frame temporaries instead (no source entity, batching
  /// off, or the element already drew once this frame).
  struct TextRecordCursor {
    geode::GeodeTextInstanceRecordComponent* component = nullptr;
    size_t next = 0;
  };

  /// Open per-occurrence record allocation for the text element rooted at
  /// `textRoot`.
  TextRecordCursor beginTextRecords(Registry& registry, Entity textRoot) {
    TextRecordCursor cursor;
    // Records only earn their keep when a batch can read them; with ordered
    // batching off every glyph draws solo from the uniform and a record would
    // be pure cost.
    if (!kEnableSceneBatching || textRoot == entt::null) {
      return cursor;
    }
    EntityHandle handle(registry, textRoot);
    if (!handle) {
      return cursor;
    }
    auto& component = handle.get_or_emplace<geode::GeodeTextInstanceRecordComponent>();
    std::shared_ptr<geode::GeodeRecordSlab> slab = recordSlab(registry);
    if (component.recordSlab.get() != slab.get()) {
      // Device change retired the old slab with the registry-context swap;
      // the held slots belong to it, so drop them and start fresh.
      component.freeRecordSlots();
      component.recordSlab = std::move(slab);
    }
    if (component.lastFrame >= device->oldestOpenFrameGeneration()) {
      // Some frame that wrote these slots has not submitted: this element drew
      // earlier in this frame (a `<use>` of the text), or an outer frame
      // recorded its glyph batch and an offscreen pass is now rendering the
      // same document. Every buffer write in a frame lands before every draw in
      // the same submit, so rewriting the slots would retroactively retransform
      // that recorded draw. Take per-frame temporaries instead.
      return cursor;
    }
    component.lastFrame = currentFrameIndex;
    cursor.component = &component;
    return cursor;
  }

  /// Take the next occurrence's record slot, growing the element's persistent
  /// list on demand. Falls back to a per-frame temporary when there is no
  /// persistent list. Leaves `outSlot` null when no record can be had at all,
  /// which sends the occurrence down the solo resident draw path.
  void nextTextRecord(TextRecordCursor& cursor, Registry& registry,
                      const geode::GeodeRecordSlab::Slot*& outSlot,
                      std::vector<uint8_t>*& outCache) {
    outSlot = nullptr;
    outCache = nullptr;
    if (cursor.component != nullptr) {
      auto& occurrences = cursor.component->occurrences;
      if (cursor.next == occurrences.size() && cursor.component->recordSlab) {
        std::shared_ptr<geode::GeodeDocumentGeometryBudget> documentBudget =
            documentGeometryBudget(registry);
        if (cursor.component->reserveOccurrence(documentBudget)) {
          geode::GeodeRecordSlab::Slot slot;
          if (cursor.component->recordSlab->allocateSlot(*device, slot)) {
            if (!cursor.component->appendReservedOccurrence(slot)) {
              cursor.component->recordSlab->freeSlot(slot);
            }
          } else {
            cursor.component->rollbackOccurrence();
          }
        }
      }
      if (cursor.next < occurrences.size()) {
        outSlot = &occurrences[cursor.next]->slot;
        outCache = &occurrences[cursor.next]->lastRecord;
        ++cursor.next;
        return;
      }
    }
    if (!kEnableSceneBatching) {
      return;
    }
    outSlot = allocateTempRecordSlot(recordSlab(registry));
  }

  /// Resident CPU encode + GPU geometry for one glyph identity, creating it on
  /// first use.
  ///
  /// `buildOutline` is a callable returning the glyph's unplaced `Path`; it
  /// runs only on a miss, which is the whole point - fetching an outline from
  /// the font backend is the cost this cache exists to pay once. An entry
  /// whose outline comes back empty (a glyph the font has no vector outline
  /// for) is still cached, so that miss also costs one backend call per
  /// document rather than one per occurrence per frame.
#ifdef DONNER_TEXT_ENABLED
  void admitTextRuns(std::vector<TextRun>& runs) {
    std::size_t glyphOccurrences = 0;
    for (const TextRun& run : runs) {
      if (run.glyphs.size() > std::numeric_limits<std::size_t>::max() - glyphOccurrences) {
        textMaterializationBudget->reject();
        runs.clear();
        return;
      }
      glyphOccurrences += run.glyphs.size();
    }
    if (!textMaterializationBudget->reserveGlyphOccurrences(glyphOccurrences)) {
      runs.clear();
    }
  }

  Path materializePlacedGlyph(const Path& outline, const Transform2d& glyphFromLocal) {
    if (!textMaterializationBudget->reservePathCopy(outline)) {
      return Path();
    }
    return TransformPath(outline, glyphFromLocal);
  }

  template <typename BuildOutlineFn>
  geode::GeodeGlyphResidentEntry* residentGlyphEntry(Registry& registry, FontManager& fontManager,
                                                     FontHandle font, int glyphIndex,
                                                     const geode::GlyphGeometryKey& key,
                                                     BuildOutlineFn&& buildOutline) {
    if (textMaterializationBudget->rejected()) {
      return nullptr;
    }
    std::shared_ptr<geode::GeodeGlyphCache> cache = glyphCache(registry);
    geode::GeodeGlyphResidentEntry* entry = cache->find(key);
    if (entry != nullptr) {
      device->countGlyphResidencyHit();
    } else {
      const std::optional<FontManager::GlyphOutlineComplexity> complexity =
          fontManager.glyphOutlineComplexity(font, glyphIndex);
      if (complexity.has_value()) {
        const std::optional<RendererTextMaterializationBudget::Cost> cost =
            GlyphPredecodeCost(*complexity);
        if (!cost.has_value() || !textMaterializationBudget->reserve(*cost)) {
          return nullptr;
        }
      } else if (!fontManager.isTrustedFont(font)) {
        textMaterializationBudget->reject();
        return nullptr;
      }

      // Admission has to happen before outline decoding and before insertion. The frame-start
      // trim cannot bound a cache that begins below its limit and creates a large working set in
      // one frame; entries touched by that open frame are intentionally ineligible for eviction.
      // Reclaim only entries no open frame can still reference, then fail closed if the new entry
      // still would exceed the configured count cap.
      bool cacheAdmissionAvailable = glyphCacheMaxEntries != 0u;
      if (cacheAdmissionAvailable && cache->size() >= glyphCacheMaxEntries) {
        const size_t evicted =
            cache->evictToBudget(device->oldestOpenFrameGeneration(), glyphCacheMaxEntries - 1u,
                                 glyphCacheMaxRetainedBytes);
        device->countGlyphResidencyEvictions(evicted);
        if (cache->size() >= glyphCacheMaxEntries) {
          cacheAdmissionAvailable = false;
        }
      }
      Path outline = buildOutline();
      if (!complexity.has_value() && !outline.empty()) {
        const std::optional<std::size_t> retainedBytes = outline.retainedBytes();
        if (!retainedBytes.has_value() ||
            !textMaterializationBudget->reserve({.uniqueOutlines = 1,
                                                 .commands = outline.commands().size(),
                                                 .points = outline.points().size(),
                                                 .bytes = *retainedBytes})) {
          return nullptr;
        }
      }
      geode::EncodedPath encoded;
      if (!outline.empty()) {
        std::optional<geode::EncodedPath> admitted = encodeGeometry(outline, FillRule::NonZero);
        if (!admitted.has_value()) {
          return nullptr;
        }
        encoded = std::move(*admitted);
      }
      if (cacheAdmissionAvailable) {
        size_t evicted = 0;
        entry = cache->insertWithinBudget(key, std::move(outline), std::move(encoded),
                                          device->oldestOpenFrameGeneration(), glyphCacheMaxEntries,
                                          glyphCacheMaxRetainedBytes, &evicted);
        device->countGlyphResidencyEvictions(evicted);
        if (entry != nullptr) {
          device->countGlyphResidencyUpload();
        }
      }
      if (entry == nullptr) {
        transientGlyphEntries.emplace_back();
        entry = &transientGlyphEntries.back();
        entry->outline = std::move(outline);
        entry->encoded = std::move(encoded);
      }
    }
    entry->lastUsedFrame = currentFrameIndex;

    // Wire the slot to the document's current slabs, exactly like the
    // per-entity residence getters do.
    std::shared_ptr<geode::GeodeResidentSlab> slab = residentSlab(registry);
    if (entry->slot.slab.get() != slab.get()) {
      entry->slot.reset();
    }
    entry->slot.slab = std::move(slab);
    std::shared_ptr<geode::GeodeRecordSlab> records = recordSlab(registry);
    if (entry->slot.recordSlab.get() != records.get()) {
      entry->slot.recordSlot = geode::GeodeRecordSlab::Slot{};
      entry->slot.recordSlab = std::move(records);
    }
    return entry;
  }
#endif

  /// Draw one glyph occurrence over shared resident geometry.
  ///
  /// `recordSlot` / `recordCache` are this occurrence's own record storage.
  /// When a record is available the occurrence joins (or opens) a batch of
  /// consecutive records over the same slab chunk; otherwise it falls back to
  /// a solo resident draw whose placement rides in the encoder transform. The
  /// glyph's geometry is uploaded at most once either way.
  void emitGlyphFill(geode::GeodeGlyphResidentEntry& entry, const css::RGBA& color,
                     const Transform2d& deviceFromGlyph,
                     const geode::GeodeRecordSlab::Slot* recordSlot,
                     std::vector<uint8_t>* recordCache) {
    if (entry.encoded.empty()) {
      return;
    }
    if (tryAppendGlyphBatch(entry, color, deviceFromGlyph, recordSlot, recordCache)) {
      return;
    }
    flushPendingBatch();
    encoder->setTransform(deviceFromGlyph);
    encoder->fillPathResident(entry.slot, entry.encoded, color, FillRule::NonZero,
                              currentFrameIndex);
  }

  /// Append one glyph occurrence to the pending ordered batch, opening a new
  /// one when the current batch cannot cover it. Returns false when the
  /// occurrence cannot batch at all and the caller must draw it solo.
  ///
  /// Unlike the per-entity scene path this never needs the same-frame repeat
  /// divert: the geometry is shared by construction and every occurrence
  /// already owns a distinct record, so no record a recorded draw depends on
  /// is ever rewritten.
  bool tryAppendGlyphBatch(geode::GeodeGlyphResidentEntry& entry, const css::RGBA& color,
                           const Transform2d& deviceFromGlyph,
                           const geode::GeodeRecordSlab::Slot* recordSlot,
                           std::vector<uint8_t>* recordCache) {
    if (!kEnableSceneBatching || recordSlot == nullptr || encoder->hasActiveClipState() ||
        encoder->hasOpenMaskPass()) {
      return false;
    }
    // The ensure both establishes residence on first sight of this glyph and
    // publishes this occurrence's record; a repeat frame writes neither.
    geode::GeoEncoder::SceneRecordState recordState;
    if (!encoder->ensureResidentSceneRecord(
            entry.slot, entry.encoded, geode::GeoEncoder::ScenePaint{color}, FillRule::NonZero,
            deviceFromGlyph, recordSlot, recordCache, &recordState)) {
      return false;
    }

    const gpu::BufferRef chunk = entry.slot.buffer;
    const uint64_t chunkId = entry.slot.bufferId;
    const uint64_t chunkBytes =
        entry.slot.slab != nullptr ? entry.slot.slab->chunkBytesForId(chunkId) : 0;
    const uint32_t vertexCount = entry.encoded.boundingDrawVertexCount();
    const uint64_t clipVersion = encoder->clipStateVersion();
    const PendingBatch::SceneInstance instance{
        &entry.slot,     &entry.encoded, &entry.outline, color,       FillRule::NonZero,
        deviceFromGlyph, vertexCount,    recordSlot,     recordCache, recordState};

    if (pendingBatch.has_value() && pendingBatch->mode == PendingBatch::Mode::Scene &&
        pendingBatch->sceneChunkBufferId == chunkId &&
        pendingBatch->sceneRecordBufferId == recordSlot->bufferId &&
        pendingBatch->sceneClipVersion == clipVersion &&
        pendingBatch->sceneFirstInstance + pendingBatch->sceneInstances.size() ==
            recordSlot->index) {
      pendingBatch->sceneInstances.push_back(instance);
      pendingBatch->sceneVertexCount = std::max(pendingBatch->sceneVertexCount, vertexCount);
      entry.slot.lastSceneFrame = currentFrameIndex;
      return true;
    }

    flushPendingBatch();
    pendingBatch = PendingBatch{};
    pendingBatch->mode = PendingBatch::Mode::Scene;
    pendingBatch->sceneChunkBuffer = chunk;
    pendingBatch->sceneChunkBytes = chunkBytes;
    pendingBatch->sceneRecordBuffer = recordSlot->buffer;
    pendingBatch->sceneChunkBufferId = chunkId;
    pendingBatch->sceneRecordBufferId = recordSlot->bufferId;
    pendingBatch->sceneFirstInstance = recordSlot->index;
    pendingBatch->sceneFirstRecordOffset = recordSlot->offset;
    pendingBatch->sceneClipVersion = clipVersion;
    pendingBatch->sceneVertexCount = vertexCount;
    pendingBatch->sceneInstances.push_back(instance);
    // Claim the shared glyph slot now rather than at flush: while this batch is
    // pending, a solo resident draw of the same glyph must already see the slot
    // as taken, or it would publish its paint through the slot's uniform and
    // through `lastResidentFrame` while the batch still depends on it.
    entry.slot.lastSceneFrame = currentFrameIndex;
    return true;
  }

  std::shared_ptr<geode::GeodeDocumentGeometryBudget> documentGeometryBudget(Registry& registry) {
    auto* budgetPtr = registry.ctx().find<std::shared_ptr<geode::GeodeDocumentGeometryBudget>>();
    if (budgetPtr == nullptr) {
      std::shared_ptr<components::DocumentResourceFamilyBudget> family;
      if (const auto* context = registry.ctx().find<components::DocumentResourceFamilyContext>()) {
        family = context->budget;
      }
      registry.ctx().emplace<std::shared_ptr<geode::GeodeDocumentGeometryBudget>>(
          std::make_shared<geode::GeodeDocumentGeometryBudget>(std::move(family)));
      budgetPtr = registry.ctx().find<std::shared_ptr<geode::GeodeDocumentGeometryBudget>>();
    }
    std::shared_ptr<geode::GeodeDocumentGeometryBudget> budget = *budgetPtr;
    budget->setLimitsForTesting(*documentGeometryLimits);
    documentGeometryFrameState->touch(budget);
    return budget;
  }

  std::shared_ptr<geode::GeodeResidentSlab> residentSlab(Registry& registry) {
    std::shared_ptr<geode::GeodeDocumentGeometryBudget> documentBudget =
        documentGeometryBudget(registry);
    auto* slabPtr = registry.ctx().find<std::shared_ptr<geode::GeodeResidentSlab>>();
    if (slabPtr == nullptr) {
      registry.ctx().emplace<std::shared_ptr<geode::GeodeResidentSlab>>(nullptr);
      slabPtr = registry.ctx().find<std::shared_ptr<geode::GeodeResidentSlab>>();
    }
    std::shared_ptr<geode::GeodeResidentSlab>& slab = *slabPtr;
    if (!slab || slab->owningDeviceId() != device->deviceId()) {
      slab =
          std::make_shared<geode::GeodeResidentSlab>(device->deviceId(), std::move(documentBudget));
    }
    // Merge the previous frame's freed ranges, at most once per frame (the
    // slab gates on the index). Gating here rather than at one draw entry
    // point covers every path that can record resident draws, including
    // multi-document tile frames that bypass draw(), and keeps a repeated
    // draw() of the same document within one frame from prematurely
    // recycling ranges its own recorded draws still reference.
    slab->beginFrame(currentFrameIndex);
    return slab;
  }

  geode::GeodeResidentSlot* residentFillSlot(EntityHandle source) {
    if (!source) {
      return nullptr;
    }
    ensureCacheInvalidationWired(*source.registry());
    geode::GeodeResidentSlot& slot =
        source.get_or_emplace<geode::GeodeResidentPathComponent>().fillSlot;
    std::shared_ptr<geode::GeodeResidentSlab> slab = residentSlab(*source.registry());
    if (slot.slab.get() != slab.get()) {
      // The registry's slab changed (device change): the slot's borrowed
      // buffer may reference a released chunk, so its residence is stale
      // and must re-upload from the new slab. reset() keeps the old slab
      // reference alive via this slot until the swap below.
      slot.reset();
    }
    slot.slab = std::move(slab);
    return &slot;
  }

  /// GPU-residence slot for `source`'s stroke encode. See `residentFillSlot`.
  geode::GeodeResidentSlot* residentStrokeSlot(EntityHandle source) {
    if (!source) {
      return nullptr;
    }
    ensureCacheInvalidationWired(*source.registry());
    geode::GeodeResidentSlot& slot =
        source.get_or_emplace<geode::GeodeResidentPathComponent>().strokeSlot;
    std::shared_ptr<geode::GeodeResidentSlab> slab = residentSlab(*source.registry());
    if (slot.slab.get() != slab.get()) {
      slot.reset();
    }
    slot.slab = std::move(slab);
    return &slot;
  }

  /// GPU-residence slot for `source`'s gradient-painted stroke.
  /// See `residentGradientFillSlot`; the slot holds the cached
  /// stroke-outline encode plus the resolved gradient uniform.
  geode::GeodeResidentGradientSlot* residentGradientStrokeSlot(EntityHandle source) {
    if (!source) {
      return nullptr;
    }
    ensureCacheInvalidationWired(*source.registry());
    geode::GeodeResidentGradientSlot& slot =
        source.get_or_emplace<geode::GeodeResidentPathComponent>().gradientStrokeSlot;
    std::shared_ptr<geode::GeodeResidentSlab> slab = residentSlab(*source.registry());
    if (slot.slab.get() != slab.get()) {
      slot.reset();
    }
    slot.slab = std::move(slab);
    return &slot;
  }

  /// GPU-residence slot for `source`'s gradient-painted fill.
  /// See `residentFillSlot`; the slot lives on the
  /// same `GeodeResidentPathComponent` and is invalidated by the same
  /// listener.
  geode::GeodeResidentGradientSlot* residentGradientFillSlot(EntityHandle source) {
    if (!source) {
      return nullptr;
    }
    ensureCacheInvalidationWired(*source.registry());
    geode::GeodeResidentGradientSlot& slot =
        source.get_or_emplace<geode::GeodeResidentPathComponent>().gradientFillSlot;
    std::shared_ptr<geode::GeodeResidentSlab> slab = residentSlab(*source.registry());
    if (slot.slab.get() != slab.get()) {
      slot.reset();
    }
    slot.slab = std::move(slab);
    return &slot;
  }

  /// Emit a solid fill, preferring the persistent-residence path when a
  /// resident slot and a cached encode are both available. Falls back to
  /// the per-frame arena `fillPath` otherwise (null source, no cached encode,
  /// or gradient/pattern fallbacks). `GeoEncoder::fillPathResident` itself
  /// falls back to the arena path when a clip / mask is active, so this is
  /// always correct.
  void emitSolidFill(const Path& drawPath, const css::RGBA& color, FillRule rule,
                     const geode::EncodedPath* precomputedEncoded,
                     geode::GeodeResidentSlot* residentSlot) {
    if (residentSlot != nullptr && precomputedEncoded != nullptr) {
      encoder->fillPathResident(*residentSlot, *precomputedEncoded, color, rule, currentFrameIndex);
    } else {
      encoder->fillPath(drawPath, color, rule, precomputedEncoded);
    }
  }

  /// Pack a 2D affine into the 8-float wire format the shader expects
  /// (two `vec4f` rows, `(a, c, e, 0)` / `(b, d, f, 0)` - see
  /// `struct InstanceTransform` in `shaders/slug_fill.wgsl`).
  /// `Transform2d::data` is column-major `[a, b, c, d, e, f]`.
  static void packTransform(const Transform2d& xf, float out[8]) {
    out[0] = static_cast<float>(xf.data[0]);  // a
    out[1] = static_cast<float>(xf.data[2]);  // c
    out[2] = static_cast<float>(xf.data[4]);  // e
    out[3] = 0.0f;
    out[4] = static_cast<float>(xf.data[1]);  // b
    out[5] = static_cast<float>(xf.data[3]);  // d
    out[6] = static_cast<float>(xf.data[5]);  // f
    out[7] = 0.0f;
  }

  /// True when the pending batch still points at `encoded`, either as the
  /// same-entity singleton's encode or as one of an ordered batch's instances.
  /// Both retain the pointer until the batch flushes, so anything that
  /// replaces an `EncodedPath` in place has to ask.
  bool pendingBatchReferences(const geode::EncodedPath* encoded) const {
    if (!pendingBatch.has_value() || encoded == nullptr) {
      return false;
    }
    if (pendingBatch->encoded == encoded) {
      return true;
    }
    for (const PendingBatch::SceneInstance& instance : pendingBatch->sceneInstances) {
      if (instance.encoded == encoded) {
        return true;
      }
    }
    return false;
  }

  /// Emit any pending `<use>` batch. No-op if there's nothing pending.
  /// On size == 1 the batch degrades to a single `fillPath` call so we
  /// don't pay the per-instance-buffer cost when we accumulated
  /// exactly one draw. Size >= 2 is one instanced GPU draw.
  ///
  /// Flushing mutates `deviceFromLocalTransform` to push the batch's
  /// transform(s) down to the encoder, then RESTORES it to the
  /// caller's current value. Without the restore, a flush in the
  /// middle of `drawPath` (between fill and stroke, for example)
  /// would leave the transform stuck at the flushed batch's value
  /// for the subsequent stroke emit - breaks any fixture that
  /// mixes batchable fills with stroked siblings.
  void flushPendingBatch() {
    const bool empty =
        !pendingBatch.has_value() || (pendingBatch->mode == PendingBatch::Mode::None) ||
        (pendingBatch->mode == PendingBatch::Mode::SameEntity &&
         pendingBatch->deviceFromLocalTransforms.empty()) ||
        (pendingBatch->mode == PendingBatch::Mode::Scene && pendingBatch->sceneInstances.empty());
    if (empty) {
      pendingBatch.reset();
      return;
    }
    if (!encoder) {
      pendingBatch.reset();
      return;
    }
    const Transform2d savedDeviceFromLocalTransform = deviceFromLocalTransform;
    PendingBatch batch = std::move(*pendingBatch);
    pendingBatch.reset();

    if (batch.mode == PendingBatch::Mode::Scene) {
      // Ordered batch: one draw over the consecutive record-slab
      // records. Every instance's geometry and record were ensured via
      // `ensureResidentSceneRecord`; the encoder transform is identity so
      // the batch uniform is orthographic-only and each record carries its
      // own full transform.
      deviceFromLocalTransform = Transform2d();
      syncTransform();

      if (debugGeometryOverlay) {
        // Ordered batches issue one GPU draw, so the overlay sink never
        // sees the per-instance geometry through the draw itself. Report
        // each instance in paint order with the packed-transform wire
        // format the instanced path uses (encoder transform is identity
        // here, matching the batch draw's uniform).
        for (const PendingBatch::SceneInstance& inst : batch.sceneInstances) {
          if (inst.encoded != nullptr) {
            float packed[8];
            packTransform(inst.deviceFromLocal, packed);
            encoder->recordGeometryDebugInstance(*inst.encoded, std::span<const float>(packed, 8u));
          }
        }
      }

      geode::GeoEncoder::SceneBatchBinding binding = {};
      binding.chunkBuffer = batch.sceneChunkBuffer;
      binding.chunkBytes = batch.sceneChunkBytes;
      binding.recordBuffer = batch.sceneRecordBuffer;
      binding.chunkBufferId = batch.sceneChunkBufferId;
      binding.recordBufferId = batch.sceneRecordBufferId;
      binding.firstRecordOffset = batch.sceneFirstRecordOffset;
      binding.instanceCount = static_cast<uint32_t>(batch.sceneInstances.size());
      binding.vertexCount = batch.sceneVertexCount;
      // Every instance's record lives in this batch's record buffer, so the
      // first instance's slab owns the batch uniform too.
      binding.recordSlab = batch.sceneInstances.front().slot != nullptr
                               ? batch.sceneInstances.front().slot->recordSlab.get()
                               : nullptr;

      // The batched entry points read colour and fill rule from each instance's
      // record, so these two only populate the draw-level uniform the batched
      // shader does not consult. They come from the first instance because the
      // uniform still has to hold something well-formed.
      const css::RGBA batchColor = batch.sceneInstances.front().color;
      const FillRule batchRule = batch.sceneInstances.front().rule;
      // Ensure each instance's geometry + batch-form record immediately
      // before the draw that consumes them, so solo flushes never observe
      // the batch form's bytes. Pass each instance's resolved record slot:
      // a temp-diverted instance must re-ensure its TEMP record, never the
      // primary (a primary write here would retroactively retransform the
      // earlier draw that references the primary record, because all
      // buffer writes execute before all draws at submit). For
      // primary-slot instances the cached-record compare makes this a
      // no-op when the record is unchanged.
      for (PendingBatch::SceneInstance& inst : batch.sceneInstances) {
        if (inst.slot != nullptr && inst.encoded != nullptr) {
          (void)encoder->ensureResidentSceneRecord(
              *inst.slot, *inst.encoded, geode::GeoEncoder::ScenePaint{inst.color}, inst.rule,
              inst.deviceFromLocal, inst.recordSlotOverride, inst.recordCacheOverride,
              // Replay what the append captured. Re-reading the slot's paint
              // or the encoder's clip here would take a later draw's state.
              &inst.recordState, /*publishPaint=*/false);
        }
      }
      encoder->fillPathSceneBatch(batchColor, batchRule, binding);
      // The batch now references each instance's record: mark them so a
      // same-frame repeat of any of them gets a fresh temporary record
      // instead of overwriting this batch's content.
      for (const PendingBatch::SceneInstance& inst : batch.sceneInstances) {
        if (inst.slot != nullptr) {
          inst.slot->lastSceneFrame = currentFrameIndex;
        }
      }
      deviceFromLocalTransform = savedDeviceFromLocalTransform;
      return;
    }

    if (batch.deviceFromLocalTransforms.size() == 1) {
      // Single draw - restore the captured transform + use the
      // non-instanced path. Prefer the persistent-residence path so an
      // unbatched solid fill re-uploads zero geometry on an unchanged frame.
      // A fill lands here whenever it never gained a second batch member:
      // it was scene-ineligible (a clip polygon or mask, an open mask pass, no
      // cached encode, no residence slot, or a same-frame repeat), or it was
      // eligible but its record slot did not extend the pending run. A
      // rectangular clip is NOT among those reasons any more - it travels in
      // the instance record. Differing paint does NOT
      // land a draw here - a scene batch carries color and fill rule per
      // instance record; only the same-entity instanced mode ends its run on
      // a paint or fill-rule change.
      deviceFromLocalTransform = batch.deviceFromLocalTransforms.front();
      syncTransform();
      emitSolidFill(*batch.path, batch.color, batch.rule, batch.encoded, batch.residentSlot);
    } else {
      // Instanced: set encoder transform to identity so the shader's
      // `uniforms.mvp` carries only the orthographic screen-pixel mapping.
      // Each instance transform already encodes the full
      // `worldFromEntity * surfaceFromCanvas` composition that a
      // non-batched draw would fold into deviceFromLocalTransform; compose with
      // identity `uniforms.mvp` is equivalent to composing with the
      // original deviceFromLocalTransform per-draw.
      deviceFromLocalTransform = Transform2d();
      syncTransform();

      // Pack transforms into the wire format the shader expects.
      std::vector<float> packed(batch.deviceFromLocalTransforms.size() * 8u);
      for (size_t i = 0; i < batch.deviceFromLocalTransforms.size(); ++i) {
        packTransform(batch.deviceFromLocalTransforms[i], packed.data() + i * 8u);
      }
      encoder->fillPathInstanced(*batch.encoded, batch.color, batch.rule, packed);
    }

    // Restore so subsequent draw/state ops see the driver-set
    // transform intact. Draw-emitting helpers (`syncTransform` +
    // `encoder->fillPath*`) read `deviceFromLocalTransform` when re-entered,
    // so we don't need to re-sync the encoder right now.
    deviceFromLocalTransform = savedDeviceFromLocalTransform;
  }

  /// Start a fresh size-1 same-entity batch holding the current draw.
  /// The caller has already flushed any previous pending batch.
  void startSameEntitySingleton(Registry* sourceRegistry, Entity sourceEntity, const Path& path,
                                const css::RGBA& color, FillRule rule,
                                const geode::EncodedPath* encoded,
                                geode::GeodeResidentSlot* residentSlot) {
    pendingBatch = PendingBatch{};
    pendingBatch->mode = PendingBatch::Mode::SameEntity;
    pendingBatch->sameEntityClipVersion = encoder->clipStateVersion();
    pendingBatch->sourceRegistry = sourceRegistry;
    pendingBatch->sourceEntity = sourceEntity;
    pendingBatch->color = color;
    pendingBatch->rule = rule;
    pendingBatch->encoded = encoded;
    pendingBatch->path = &path;
    pendingBatch->residentSlot = residentSlot;
    pendingBatch->deviceFromLocalTransforms.push_back(deviceFromLocalTransform);
  }

  /// Predicate: would a batchable draw with this key extend the
  /// currently pending batch, or does it start a new one? Returns
  /// true when the current `drawPath` call should NOT emit (because
  /// it's been absorbed into a batch). Always returns true on a
  /// non-empty batch state - either appends or flushes + starts new.
  /// The caller is expected to have already verified the draw is
  /// "batch-compatible" (solid paint, has source entity, has a cached
  /// encode, no in-flight pattern). Both an element's fill and its stroked
  /// outline can be offered here, in that order: they are two solid fills of
  /// two different cached encodes on the same entity, and appending them
  /// consecutively is exactly the painter order a fill-then-stroke element
  /// requires.
  ///
  /// \p path is retained until the batch flushes, so it must be storage that outlives this
  /// call - see `PendingBatch::path`.
  /// @param allowInstancedAppend True when this draw may extend a pending
  ///   same-entity instanced batch. That batch draws from the per-frame arena
  ///   and has no residence, so folding an otherwise-resident draw into it
  ///   trades a draw call for a full geometry re-upload every frame. It is
  ///   worth it for a run of repeated `<use>` draws, which is what it exists
  ///   for; it is not worth it for a draw that also has a stroked outline,
  ///   whose element is going to open an ordered batch anyway.
  bool tryAppendOrStartBatch(Registry* sourceRegistry, Entity sourceEntity, const Path& path,
                             const geode::GeoEncoder::ScenePaint& paint, FillRule rule,
                             const geode::EncodedPath* encoded,
                             geode::GeodeResidentSlot* residentSlot,
                             bool allowInstancedAppend = true) {
    // The record-sourced paint fields are the only place a gradient can live
    // in a batch, so a gradient draw is offered to the ordered path and
    // nothing else: the instanced same-entity path and the size-1 singleton
    // flush both take their paint from the draw-level uniform, which carries
    // one colour and no gradient.
    const css::RGBA& color = paint.color;
    const bool gradientPaint = paint.isGradient();

    // Same-entity mode: consecutive draws of the SAME entity with the
    // same paint AND the same encode extend the same-entity instanced batch.
    // The encode comparison separates a repeated `<use>` - one shape at many
    // transforms, which is exactly what an instanced draw expresses - from an
    // entity's fill followed by its own stroke outline. Those two share an
    // entity and can share a colour, but they are different shapes, and
    // treating the second as another instance of the first would paint the
    // fill geometry twice instead of painting the outline.
    if (!gradientPaint && allowInstancedAppend && pendingBatch.has_value() &&
        pendingBatch->mode == PendingBatch::Mode::SameEntity &&
        pendingBatch->sourceRegistry == sourceRegistry &&
        pendingBatch->sourceEntity == sourceEntity && pendingBatch->color == color &&
        pendingBatch->rule == rule && pendingBatch->encoded == encoded &&
        pendingBatch->residentSlot == residentSlot) {
      pendingBatch->deviceFromLocalTransforms.push_back(deviceFromLocalTransform);
      return true;
    }

    // Scene mode: distinct entities whose records are painter-ordered
    // and whose geometry shares a slab chunk. Scene batches always form
    // by converting a pending size-1 same-entity entry (the first entity
    // starts as SameEntity(1); the second, different entity converts it),
    // so a same-entity `<use>` repeat still prefers the instanced path.
    // Scene batches cover entities drawn ONCE per frame. A same-frame
    // repeat (markers, repeated <use>) belongs to the same-entity
    // path and the solo flush's arena fallback, whose semantics predate
    // ordered batching and are already exact for repeated draws.
    const geode::GeodeRecordSlab::Slot* recordSlotPtr = nullptr;
    const uint64_t clipVersion = encoder->clipStateVersion();

    // Whether this draw has anything to batch WITH. A solid draw with nothing
    // to join becomes a size-1 same-entity entry, which flushes as a solo
    // resident draw reading its paint from the slot's uniform and needing no
    // record at all. Publishing a record for it anyway is not just wasted
    // work: two such draws from one slot both write the slot's primary record
    // and neither reads it, so a steady frame rewrites those bytes forever.
    // The slot is still ALLOCATED below, in draw order, because record indices
    // are the batch's ordering and a slot handed out late would take a lower
    // index than a draw that already has one and split the pair.
    const bool havePendingSceneBatch =
        pendingBatch.has_value() && pendingBatch->mode == PendingBatch::Mode::Scene;
    const bool haveConvertibleSingleton = pendingBatch.has_value() &&
                                          pendingBatch->mode == PendingBatch::Mode::SameEntity &&
                                          pendingBatch->deviceFromLocalTransforms.size() == 1 &&
                                          pendingBatch->sameEntityClipVersion == clipVersion &&
                                          pendingBatch->residentSlot != residentSlot;
    const bool wantsRecord = gradientPaint || havePendingSceneBatch || haveConvertibleSingleton;

    // `kEnableSceneBatching` is the compile-time gate for ordered
    // cross-entity batching; with it off the whole predicate folds away, so
    // no draw allocates a record slot, writes a record, or touches the record
    // slab. Every gate below still has to hold before a draw can join a
    // batch.
    //
    // The record-slab slot is allocated HERE, on the eligibility path,
    // rather than when the residence slot is created: a draw that can never
    // batch takes its paint from the uniform and binds the device's shared
    // identity record, so giving it a slot would be pure cost.
    //
    // The remaining target-state guards - a clip polygon or mask, and an open
    // mask pass - are what make deferring a draw to flush time safe: each is
    // state the batch would observe at flush rather than at draw. Solid fills
    // do not currently reach here during a mask pass, but the residency
    // helpers all carry the same guard, and an ordered batch is the one caller
    // for which getting it wrong silently paints into the wrong target.
    //
    // A rectangular clip is NOT among them. It travels in the instance record
    // and the fragment stage applies it, so a deferred draw does not read
    // raster-stage scissor state that may have moved on by flush time.
    const bool sceneEligible =
        kEnableSceneBatching && residentSlot != nullptr && encoded != nullptr &&
        !encoder->hasActiveClipState() && !encoder->hasOpenMaskPass() &&
        !device->frameStampClaimed(residentSlot->lastSceneFrame) &&
        !device->frameStampClaimed(residentSlot->lastResidentFrame) && sourceRegistry != nullptr &&
        ensureRecordSlot(*residentSlot, *sourceRegistry) &&
        resolveSceneRecordSlot(*residentSlot, recordSlotPtr);
    geode::GeoEncoder::SceneRecordState recordState;
    if (sceneEligible && wantsRecord &&
        encoder->ensureResidentSceneRecord(*residentSlot, *encoded, paint, rule,
                                           deviceFromLocalTransform, recordSlotPtr,
                                           /*overrideRecordCache=*/nullptr, &recordState)) {
      const geode::GeodeRecordSlab::Slot& effectiveRecordSlot =
          recordSlotPtr != nullptr ? *recordSlotPtr : residentSlot->recordSlot;
      const gpu::BufferRef chunk = residentSlot->buffer;
      const gpu::BufferRef recordBuf = effectiveRecordSlot.buffer;
      const uint64_t chunkId = residentSlot->bufferId;
      const uint64_t chunkBytes =
          residentSlot->slab != nullptr ? residentSlot->slab->chunkBytesForId(chunkId) : 0;
      const uint64_t recordBufId = effectiveRecordSlot.bufferId;
      const uint32_t recordIndex = effectiveRecordSlot.index;
      const uint32_t vertexCount = encoded->boundingDrawVertexCount();
      // One description of THIS draw, used by every branch below. It carries
      // the record state the ensure just captured, so wherever the instance
      // ends up, its flush replays what was appended.
      const PendingBatch::SceneInstance current{
          residentSlot, encoded,       &path,   color,      rule, deviceFromLocalTransform,
          vertexCount,  recordSlotPtr, nullptr, recordState};

      auto appendSceneInstance = [&](PendingBatch& b, const PendingBatch::SceneInstance& inst) {
        b.sceneInstances.push_back(inst);
        b.sceneVertexCount = std::max(b.sceneVertexCount, inst.vertexCount);
        // The record is now claimed by a pending batch: a same-frame
        // repeat must not overwrite it.
        if (inst.slot != nullptr) {
          inst.slot->lastSceneFrame = currentFrameIndex;
        }
      };

      if (pendingBatch.has_value() && pendingBatch->mode == PendingBatch::Mode::Scene) {
        // Buffer identities, not handles: the two are interchangeable inside
        // one frame, but comparing ids keeps every "is this the same buffer?"
        // question in this file answered the same way.
        if (pendingBatch->sceneChunkBufferId == chunkId &&
            pendingBatch->sceneRecordBufferId == recordBufId &&
            pendingBatch->sceneClipVersion == clipVersion &&
            pendingBatch->sceneFirstInstance + pendingBatch->sceneInstances.size() == recordIndex) {
          appendSceneInstance(*pendingBatch, current);
        } else {
          flushPendingBatch();
          pendingBatch = PendingBatch{};
          pendingBatch->mode = PendingBatch::Mode::Scene;
          pendingBatch->sceneChunkBuffer = chunk;
          pendingBatch->sceneChunkBytes = chunkBytes;
          pendingBatch->sceneRecordBuffer = recordBuf;
          pendingBatch->sceneChunkBufferId = chunkId;
          pendingBatch->sceneRecordBufferId = recordBufId;
          pendingBatch->sceneFirstInstance = recordIndex;
          pendingBatch->sceneFirstRecordOffset = effectiveRecordSlot.offset;
          pendingBatch->sceneClipVersion = clipVersion;
          appendSceneInstance(*pendingBatch, current);
        }
        return true;
      }

      // Converting a pending singleton that draws from the SAME residence
      // slot as this draw would hand both instances the slot's one primary
      // record: the two ensures resolve to it independently (neither has
      // marked the slot yet), the later write wins because every buffer write
      // in a submit executes before every draw, and the earlier instance is
      // silently repainted with this draw's colour and transform. Two draws of
      // one entity that differ only in paint therefore stay unconverted; the
      // singleton flushes solo and this draw opens a fresh one. Same-encode
      // same-paint repeats are the instanced same-entity path above and never
      // reach here.
      if (pendingBatch.has_value() && pendingBatch->mode == PendingBatch::Mode::SameEntity &&
          pendingBatch->deviceFromLocalTransforms.size() == 1 &&
          pendingBatch->sameEntityClipVersion == clipVersion &&
          pendingBatch->residentSlot != residentSlot) {
        // Convert the pending size-1 same-entity entry into the scene
        // batch's first instance.
        PendingBatch::SceneInstance first{pendingBatch->residentSlot,
                                          pendingBatch->encoded,
                                          pendingBatch->path,
                                          pendingBatch->color,
                                          pendingBatch->rule,
                                          pendingBatch->deviceFromLocalTransforms.front(),
                                          pendingBatch->encoded->boundingDrawVertexCount()};
        // Resolve the first entity's record slot: its primary slot on its
        // first scene use this frame, or a fresh temporary slot when it
        // was already batched earlier this frame (its primary record is
        // referenced by that earlier batch and must not be overwritten).
        // The singleton reached the batcher without being scene eligible
        // itself, so it may still hold a slot from a slab this device does
        // not own (another device rendered the document in between): ensure
        // it against the current slab before resolving.
        const geode::GeodeRecordSlab::Slot* firstRecordSlotPtr = nullptr;
        if (first.slot != nullptr && first.encoded != nullptr &&
            pendingBatch->sourceRegistry != nullptr &&
            ensureRecordSlot(*first.slot, *pendingBatch->sourceRegistry) &&
            resolveSceneRecordSlot(*first.slot, firstRecordSlotPtr) &&
            encoder->ensureResidentSceneRecord(
                *first.slot, *first.encoded, geode::GeoEncoder::ScenePaint{first.color}, first.rule,
                first.deviceFromLocal, firstRecordSlotPtr, /*overrideRecordCache=*/nullptr,
                &first.recordState)) {
          first.slot->lastSceneFrame = currentFrameIndex;
          // Carry the resolved slot on the instance so the flush-time
          // re-ensure targets the same record this batch draws from.
          first.recordSlotOverride = firstRecordSlotPtr;
          const geode::GeodeRecordSlab::Slot& effectiveFirstRecordSlot =
              firstRecordSlotPtr != nullptr ? *firstRecordSlotPtr : first.slot->recordSlot;
          const gpu::BufferRef firstChunk = first.slot->buffer;
          const gpu::BufferRef firstRecordBuf = effectiveFirstRecordSlot.buffer;
          const uint64_t firstChunkId = first.slot->bufferId;
          const uint64_t firstChunkBytes =
              first.slot->slab != nullptr ? first.slot->slab->chunkBytesForId(firstChunkId) : 0;
          const uint64_t firstRecordBufId = effectiveFirstRecordSlot.bufferId;
          const uint32_t firstIndex = effectiveFirstRecordSlot.index;
          pendingBatch = PendingBatch{};
          pendingBatch->mode = PendingBatch::Mode::Scene;
          pendingBatch->sceneChunkBuffer = firstChunk;
          pendingBatch->sceneChunkBytes = firstChunkBytes;
          pendingBatch->sceneRecordBuffer = firstRecordBuf;
          pendingBatch->sceneChunkBufferId = firstChunkId;
          pendingBatch->sceneRecordBufferId = firstRecordBufId;
          pendingBatch->sceneFirstInstance = firstIndex;
          pendingBatch->sceneFirstRecordOffset = effectiveFirstRecordSlot.offset;
          pendingBatch->sceneClipVersion = clipVersion;
          pendingBatch->sceneVertexCount = first.vertexCount;
          pendingBatch->sceneInstances.push_back(first);
          if (firstChunkId == chunkId && firstRecordBufId == recordBufId &&
              firstIndex + 1 == recordIndex) {
            appendSceneInstance(*pendingBatch, current);
          } else {
            flushPendingBatch();
            pendingBatch = PendingBatch{};
            pendingBatch->mode = PendingBatch::Mode::Scene;
            pendingBatch->sceneChunkBuffer = chunk;
            pendingBatch->sceneChunkBytes = chunkBytes;
            pendingBatch->sceneRecordBuffer = recordBuf;
            pendingBatch->sceneChunkBufferId = chunkId;
            pendingBatch->sceneRecordBufferId = recordBufId;
            pendingBatch->sceneFirstInstance = recordIndex;
            pendingBatch->sceneFirstRecordOffset = effectiveRecordSlot.offset;
            pendingBatch->sceneClipVersion = clipVersion;
            appendSceneInstance(*pendingBatch, current);
          }
        } else {
          // The pending singleton could not take scene form: emit it solo
          // and keep the CURRENT draw pending. Returning without recording the
          // current draw would silently drop it (its ensured record is not
          // referenced by any batch).
          claimSceneSlot(*residentSlot, paint);
          flushPendingBatch();
          if (!paint.isGradient()) {
            encoder->releasePreparedSceneAdmission(*encoded);
          }
          startPending(sourceRegistry, sourceEntity, path, paint, rule, encoded, residentSlot,
                       chunk, chunkBytes, recordBuf, chunkId, recordBufId, recordIndex,
                       effectiveRecordSlot.offset, clipVersion, current);
        }
        return true;
      }

      if (!pendingBatch.has_value() || pendingBatch->mode != PendingBatch::Mode::Scene) {
        // First draw of an entity (or after a flushed pair): a solid fill
        // starts a same-entity batch of size 1, so a different entity can
        // convert it into a scene batch and a repeat can extend it as
        // instanced. A gradient has neither of those futures and opens the
        // scene batch directly.
        claimSceneSlot(*residentSlot, paint);
        if (pendingBatch.has_value()) {
          flushPendingBatch();
        }
        startPending(sourceRegistry, sourceEntity, path, paint, rule, encoded, residentSlot, chunk,
                     chunkBytes, recordBuf, chunkId, recordBufId, recordIndex,
                     effectiveRecordSlot.offset, clipVersion, current);
        return true;
      }

      flushPendingBatch();
      pendingBatch = PendingBatch{};
      pendingBatch->mode = PendingBatch::Mode::Scene;
      pendingBatch->sceneChunkBuffer = chunk;
      pendingBatch->sceneChunkBytes = chunkBytes;
      pendingBatch->sceneRecordBuffer = recordBuf;
      pendingBatch->sceneChunkBufferId = chunkId;
      pendingBatch->sceneRecordBufferId = recordBufId;
      pendingBatch->sceneFirstInstance = recordIndex;
      pendingBatch->sceneFirstRecordOffset = effectiveRecordSlot.offset;
      pendingBatch->sceneClipVersion = clipVersion;
      appendSceneInstance(*pendingBatch, current);
      return true;
    }

    // Not scene-eligible. A gradient has no non-ordered batch form, so the
    // caller emits it through the ordinary gradient path; a solid fill starts
    // a fresh same-entity batch.
    flushPendingBatch();
    if (gradientPaint) {
      return false;
    }
    startSameEntitySingleton(sourceRegistry, sourceEntity, path, color, rule, encoded,
                             residentSlot);
    return true;
  }

  /// Claim `slot` for a scene batch that is about to be opened, BEFORE any
  /// flush that could emit a solo draw of the same slot.
  ///
  /// Draining the pending batch can flush a same-entity singleton that draws
  /// from this very slot, and a solo resident draw republishes the slot's paint
  /// as solid on its way out. Claiming first sends that draw down the arena
  /// fallback instead, which leaves the paint this draw just published intact.
  /// Only a gradient needs it: solid paint publishes nothing a later solid
  /// draw could disturb, and a solid draw that becomes a singleton still wants
  /// its own residence, which the claim would take away.
  void claimSceneSlot(geode::GeodeResidentSlot& slot, const geode::GeoEncoder::ScenePaint& paint) {
    if (paint.isGradient()) {
      slot.lastSceneFrame = currentFrameIndex;
    }
  }

  /// Open a new pending batch holding exactly this draw: a same-entity
  /// singleton for solid paint, or a scene batch of one instance for gradient
  /// paint, which the uniform-sourced singleton flush cannot express. The
  /// caller has already flushed whatever was pending and has already ensured
  /// this draw's record.
  void startPending(Registry* sourceRegistry, Entity sourceEntity, const Path& path,
                    const geode::GeoEncoder::ScenePaint& paint, FillRule rule,
                    const geode::EncodedPath* encoded, geode::GeodeResidentSlot* residentSlot,
                    const gpu::BufferRef& chunk, uint64_t chunkBytes,
                    const gpu::BufferRef& recordBuffer, uint64_t chunkId, uint64_t recordBufferId,
                    uint32_t recordIndex, uint64_t recordOffset, uint64_t clipVersion,
                    const PendingBatch::SceneInstance& instance) {
    if (!paint.isGradient()) {
      startSameEntitySingleton(sourceRegistry, sourceEntity, path, paint.color, rule, encoded,
                               residentSlot);
      return;
    }
    pendingBatch = PendingBatch{};
    pendingBatch->mode = PendingBatch::Mode::Scene;
    pendingBatch->sceneChunkBuffer = chunk;
    pendingBatch->sceneChunkBytes = chunkBytes;
    pendingBatch->sceneRecordBuffer = recordBuffer;
    pendingBatch->sceneChunkBufferId = chunkId;
    pendingBatch->sceneRecordBufferId = recordBufferId;
    pendingBatch->sceneFirstInstance = recordIndex;
    pendingBatch->sceneFirstRecordOffset = recordOffset;
    pendingBatch->sceneClipVersion = clipVersion;
    pendingBatch->sceneVertexCount = instance.vertexCount;
    pendingBatch->sceneInstances.push_back(instance);
    residentSlot->lastSceneFrame = currentFrameIndex;
  }

  /// Determine the fill rule for a stroked outline. Each painted interval of
  /// a valid dash pattern becomes a separate closed ribbon. Those ribbons use
  /// NonZero so adjacent or wrapped dashes union instead of canceling their
  /// overlaps. Invalid, oversized, and all-zero patterns fall back to solid stroking, where
  /// open paths produce one subpath (NonZero) and closed paths produce two
  /// same-winding subpaths (EvenOdd hollow-ring semantics).
  static FillRule strokeFillRuleFor(const Path& strokedOutline, const StrokeStyle& strokeStyle) {
    if (!strokeStyle.dashArray.empty() &&
        strokeStyle.dashArray.size() <= StrokeStyle::kMaxDashEntries) {
      double dashSum = 0.0;
      for (double dash : strokeStyle.dashArray) {
        if (!std::isfinite(dash) || dash < 0.0) {
          dashSum = 0.0;
          break;
        }
        dashSum += dash;
      }
      if (std::isfinite(dashSum) && dashSum > 0.0) {
        return FillRule::NonZero;
      }
    }

    size_t subpathCount = 0;
    for (const auto& cmd : strokedOutline.commands()) {
      if (cmd.verb == Path::Verb::MoveTo) {
        ++subpathCount;
      }
    }
    return (subpathCount <= 1) ? FillRule::NonZero : FillRule::EvenOdd;
  }

  /// Stroke-side of the cache. Builds (or reuses) the `strokeToFill`
  /// output and its encode on `source`'s `GeodePathCacheComponent`.
  /// Returns a `StrokeDerived` pointing into the cache (entity path) or
  /// into `strokeScratchPath` (no-entity fallback). The caller must
  /// check `strokedPath == nullptr` for the "zero-stroke" case.
  StrokeDerived getStrokeDerived(EntityHandle source, const Path& geometry,
                                 const StrokeStyle& strokeStyle) {
    StrokeDerived result;
    // Device-aware flattening tolerance for this draw. Part of the cache key
    // below: a zoom change that crosses a scale bucket must re-flatten instead
    // of serving the outline tessellated for the old scale.
    const double flattenTolerance = strokeFlattenTolerance();
    if (source) {
      ensureCacheInvalidationWired(*source.registry());
      std::shared_ptr<geode::GeodeDocumentGeometryBudget> documentBudget =
          documentGeometryBudget(*source.registry());
      auto& cache = source.get_or_emplace<geode::GeodePathCacheComponent>();
      if (cache.strokeSlot && cache.strokeSlot->strokeKey == strokeStyle &&
          cache.strokeSlot->flattenTolerance == flattenTolerance) {
        result.strokedPath = &cache.strokeSlot->strokedPath;
        result.encoded = &cache.strokeSlot->strokedEncode;
        result.persistent = true;
        result.fillRule = cache.strokeSlot->strokeFillRule;
        return result;
      }

      {
        // Miss (or stroke-params / device-scale changed) - rebuild. De-close
        // zero-area closed subpaths first (see `deCloseZeroAreaSubpaths`) so a
        // degenerate `M L Z` line strokes into a clean rectangle the analytic
        // shader covers correctly, instead of overlapping triangles.
        Path stroked =
            deCloseZeroAreaSubpaths(geometry).strokeToFill(strokeStyle, flattenTolerance);
        if (stroked.empty()) {
          if (cache.strokeSlot.has_value() &&
              pendingBatchReferences(&cache.strokeSlot->strokedEncode)) {
            flushPendingBatch();
          }
          if (auto* resident = source.try_get<geode::GeodeResidentPathComponent>()) {
            resident->strokeSlot.reset();
            resident->gradientStrokeSlot.reset();
          }
          cache.strokeReservation.reset();
          cache.strokeSlot.reset();
          return result;  // strokedPath stays null.
        }
        countStrokeOutline(stroked);
        const FillRule fillRule = strokeFillRuleFor(stroked, strokeStyle);
        std::optional<geode::EncodedPath> encoded = encodeGeometry(stroked, fillRule);
        if (!encoded.has_value()) {
          return result;
        }
        geode::GeodePathCacheComponent::StrokeSlot candidate{
            .strokeKey = strokeStyle,
            .flattenTolerance = flattenTolerance,
            .strokedPath = std::move(stroked),
            .strokedEncode = std::move(*encoded),
            .strokeFillRule = fillRule,
        };
        const std::optional<std::size_t> retainedBytes = candidate.retainedBytes();
        if (!retainedBytes.has_value() ||
            !cache.strokeReservation.replace(documentBudget, *retainedBytes)) {
          strokeScratchPath = std::move(candidate.strokedPath);
          strokeScratchEncode = std::move(candidate.strokedEncode);
          result.strokedPath = &strokeScratchPath;
          result.encoded = &*strokeScratchEncode;
          result.fillRule = fillRule;
          return result;
        }
        if (cache.strokeSlot.has_value() &&
            pendingBatchReferences(&cache.strokeSlot->strokedEncode)) {
          flushPendingBatch();
        }
        if (auto* resident = source.try_get<geode::GeodeResidentPathComponent>()) {
          resident->strokeSlot.reset();
          resident->gradientStrokeSlot.reset();
        }
        cache.strokeSlot = std::move(candidate);
      }
      result.strokedPath = &cache.strokeSlot->strokedPath;
      result.encoded = &cache.strokeSlot->strokedEncode;
      result.persistent = true;
      result.fillRule = cache.strokeSlot->strokeFillRule;
      return result;
    }
    // No-entity fallback: compute and admit into Impl-local scratch buffers.
    strokeScratchPath =
        deCloseZeroAreaSubpaths(geometry).strokeToFill(strokeStyle, flattenTolerance);
    if (strokeScratchPath.empty()) {
      return result;
    }
    countStrokeOutline(strokeScratchPath);
    result.strokedPath = &strokeScratchPath;
    result.fillRule = strokeFillRuleFor(strokeScratchPath, strokeStyle);
    strokeScratchEncode = encodeGeometry(strokeScratchPath, result.fillRule);
    if (!strokeScratchEncode.has_value()) {
      return {};
    }
    result.encoded = &*strokeScratchEncode;
    return result;
  }

  /// Resolve a paint-server reference into gradient parameters a batched
  /// instance can carry. Returns false when the reference is not a usable
  /// gradient - a pattern, a malformed or empty gradient, a degenerate frame,
  /// or a radial that collapses to a single stop colour. Those keep the
  /// ordinary emit path, which owns the fallback-colour handling.
  ///
  /// The resolved stops live in `gradientStopScratch`, so the outputs are
  /// valid only until the next resolve. That is enough: the encoder copies
  /// everything it needs into the entity's persistent paint block during the
  /// append, and a batch's flush-time re-ensure reads the paint back off the
  /// residence slot rather than from the caller.
  bool resolveBatchGradient(const Path& geometryPath, const components::ResolvedPaintServer& server,
                            double effectiveOpacity,
                            std::optional<geode::LinearGradientParams>& outLinear,
                            std::optional<geode::RadialGradientParams>& outRadial) {
    const auto* ref = std::get_if<components::PaintResolvedReference>(&server);
    if (ref == nullptr) {
      return false;
    }
    const css::RGBA currentColor = paint.currentColor.rgba();
    const float opacity = static_cast<float>(effectiveOpacity);
    const Box2d geometryBounds = geometryPath.bounds();
    bool stopsTruncated = false;
    outLinear = resolveLinearGradientParams(*ref, geometryBounds, paint.viewBox, currentColor,
                                            opacity, gradientStopScratch, &stopsTruncated);
    if (!outLinear.has_value()) {
      auto radial = resolveRadialGradientParams(*ref, geometryBounds, paint.viewBox, currentColor,
                                                opacity, gradientStopScratch, &stopsTruncated);
      if (radial.has_value() && radial->gradient.has_value()) {
        outRadial = std::move(radial->gradient);
      }
    }
    if (stopsTruncated && verbose && !warnedGradient) {
      std::cerr << "RendererGeode: gradient has more than " << kMaxGradientStopsClient
                << " stops; truncating (follow-up: texture-based stop lookup)\n";
      warnedGradient = true;
    }
    return outLinear.has_value() || outRadial.has_value();
  }

  /// Issue a fill of the given path using the current `paint.fill`. Handles
  /// solid colors, linear and radial gradients, and pattern tiles. None and
  /// unsupported types are ignored or fall back to their fallback color.
  ///
  /// `precomputedEncoded` is the path-encode cache-hit payload (see
  /// `getFillEncode`). When non-null, the encoder skips the
  /// `GeodePathEncoder::encode` + `countPathEncode()` pair; otherwise
  /// `GeoEncoder` runs the inline encode path.
  void fillResolved(const Path& path, FillRule rule,
                    const geode::EncodedPath* precomputedEncoded = nullptr,
                    geode::GeodeResidentSlot* residentSlot = nullptr,
                    geode::GeodeResidentGradientSlot* gradientResidentSlot = nullptr) {
    if (!encoder) {
      return;
    }
    // Pattern dispatch comes first: a pattern slot is populated by the
    // driver via `endPatternTile`, and is consumed by the very next fill or
    // stroke draw (matching the CPU-renderer semantics).
    if (patternFillPaint.has_value()) {
      syncTransform();
      const double opacity = paint.fillOpacity;
      encoder->fillPathPattern(path, rule, buildPatternPaint(*patternFillPaint, opacity),
                               precomputedEncoded);
      if (patternFillPaint->tile) {
        device->deferDestroy(patternFillPaint->tile.take());
      }
      patternFillPaint.reset();
      return;
    }
    const double effectiveOpacity = paint.fillOpacity;
    drawPaintedPath(path, paint.fill, effectiveOpacity, rule, precomputedEncoded, residentSlot,
                    gradientResidentSlot);
  }

  /// Core dispatch: given a path and a resolved paint server, emit the
  /// appropriate fill call (solid color or gradient).
  void drawPaintedPath(const Path& path, const components::ResolvedPaintServer& server,
                       double effectiveOpacity, FillRule rule,
                       const geode::EncodedPath* precomputedEncoded = nullptr,
                       geode::GeodeResidentSlot* residentSlot = nullptr,
                       geode::GeodeResidentGradientSlot* gradientResidentSlot = nullptr) {
    drawPaintedPathAgainst(path, path, server, effectiveOpacity, rule, precomputedEncoded,
                           residentSlot, gradientResidentSlot);
  }

  /// Same as `drawPaintedPath`, but the gradient's objectBoundingBox is
  /// computed from `geometryPath` while the GPU draw uses `drawPath`. This
  /// is required for stroked outlines: SVG specifies that the
  /// `objectBoundingBox` of a stroke gradient is derived from the *original*
  /// geometry, not the expanded stroke outline, otherwise a thick stroke
  /// would warp the gradient direction relative to the underlying shape.
  void drawPaintedPathAgainst(const Path& geometryPath, const Path& drawPath,
                              const components::ResolvedPaintServer& server,
                              double effectiveOpacity, FillRule rule,
                              const geode::EncodedPath* precomputedEncoded = nullptr,
                              geode::GeodeResidentSlot* residentSlot = nullptr,
                              geode::GeodeResidentGradientSlot* gradientResidentSlot = nullptr) {
    if (!encoder || drawPath.empty()) {
      return;
    }
    if (std::holds_alternative<PaintServer::None>(server)) {
      return;
    }

    const css::RGBA currentColor = paint.currentColor.rgba();
    const float opacity = static_cast<float>(effectiveOpacity);

    // Solid color: straight through the flat fill pipeline.
    if (const auto* solid = std::get_if<PaintServer::Solid>(&server)) {
      syncTransform();
      emitSolidFill(drawPath, solid->color.resolve(currentColor, opacity), rule, precomputedEncoded,
                    residentSlot);
      return;
    }

    // Paint-server reference: try linear gradient first, then radial; if
    // neither matches, fall back to the reference's solid fallback color.
    if (const auto* ref = std::get_if<components::PaintResolvedReference>(&server)) {
      const Box2d geometryBounds = geometryPath.bounds();
      bool stopsTruncated = false;

      auto linear = resolveLinearGradientParams(*ref, geometryBounds, paint.viewBox, currentColor,
                                                opacity, gradientStopScratch, &stopsTruncated);
      if (linear.has_value()) {
        if (stopsTruncated && verbose && !warnedGradient) {
          std::cerr << "RendererGeode: gradient has more than " << kMaxGradientStopsClient
                    << " stops; truncating (follow-up: texture-based stop lookup)\n";
          warnedGradient = true;
        }
        syncTransform();
        if (gradientResidentSlot != nullptr && precomputedEncoded != nullptr) {
          encoder->fillPathLinearGradientResident(*gradientResidentSlot, *precomputedEncoded,
                                                  *linear, rule, currentFrameIndex);
        } else {
          encoder->fillPathLinearGradient(drawPath, *linear, rule, precomputedEncoded);
        }
        return;
      }

      auto radial = resolveRadialGradientParams(*ref, geometryBounds, paint.viewBox, currentColor,
                                                opacity, gradientStopScratch, &stopsTruncated);
      if (radial.has_value()) {
        if (stopsTruncated && verbose && !warnedGradient) {
          std::cerr << "RendererGeode: gradient has more than " << kMaxGradientStopsClient
                    << " stops; truncating (follow-up: texture-based stop lookup)\n";
          warnedGradient = true;
        }
        if (radial->gradient.has_value()) {
          syncTransform();
          if (gradientResidentSlot != nullptr && precomputedEncoded != nullptr) {
            encoder->fillPathRadialGradientResident(*gradientResidentSlot, *precomputedEncoded,
                                                    *radial->gradient, rule, currentFrameIndex);
          } else {
            encoder->fillPathRadialGradient(drawPath, *radial->gradient, rule, precomputedEncoded);
          }
          return;
        }
        if (radial->solidFallback.has_value()) {
          // SVG2 degenerate radial (r=0): paint the last stop color as a
          // solid fill so the element remains visible.
          syncTransform();
          emitSolidFill(drawPath, *radial->solidFallback, rule, precomputedEncoded, residentSlot);
          return;
        }
        // Recognized as radial but otherwise unusable (empty stops, focal
        // circle containing outer, singular transform, degenerate
        // objectBoundingBox frame). Fall through to the paint-server
        // fallback below - per SVG2, a gradient paint server that can't
        // be instantiated on a given element should use the reference's
        // fallback color (e.g., `stroke="url(#lg) green"` paints green on
        // a zero-height horizontal line where the objectBoundingBox
        // gradient can't be applied).
      }

      // Neither linear nor radial - could be a pattern, a sweep gradient
      // (not yet supported by the donner SVG parser), a malformed gradient
      // with no stops, or a degenerate frame. Fall back to the reference's
      // solid fallback color if one was declared, otherwise drop the draw.
      if (ref->fallback.has_value()) {
        syncTransform();
        emitSolidFill(drawPath, ref->fallback->resolve(currentColor, opacity), rule,
                      precomputedEncoded, residentSlot);
        return;
      }

      // No fallback, no gradient support - issue a one-shot warning so
      // verbose callers can see it.
      if (verbose && !warnedGradient) {
        std::cerr << "RendererGeode: paint server is neither linear nor radial gradient and "
                     "has no fallback (patterns and sweep gradients are Phase 2H+)\n";
        warnedGradient = true;
      }
    }
  }

  // The cache-invalidation listener is a free function with no dependency
  // on `this`, so Impl teardown intentionally leaves it attached. Connections
  // die with the `Registry` they live on, and calling `.disconnect<&fn>()` from
  // a renderer dtor would UB when the registry was destroyed first.

  static void releaseTextureBacking(geode::ScopedWgpuHandle<wgpu::Texture>& texture) {
    texture.reset();
  }

  static void releaseTexture(wgpu::Texture& texture) { geode::ReleaseWgpuHandle(texture); }

  /// Drops a runtime texture handle without pooling it, for teardown paths where the pool and
  /// the surface budget are on their way out too.
  /// @param texture Handle to drop; left invalid.
  static void releaseRuntimeTexture(gpu::Texture& texture) { texture = gpu::Texture(); }

  static void releasePendingTexture(PendingRelease& release) {
    releaseRuntimeTexture(release.texture);
  }

  static void releaseClipStackTextures(std::vector<ClipStackEntry>& entries) {
    for (ClipStackEntry& entry : entries) {
      for (PendingRelease& release : entry.maskLayerTextures) {
        releasePendingTexture(release);
      }
      entry.maskLayerTextures.clear();
      entry.maskResolveTextureHandle = nullptr;
      entry.maskResolveViewHandle = nullptr;
    }
    entries.clear();
  }

  void resetForBeginFrame(const RenderViewport& nextViewport) {
    borrowedTargetSnapshot.reset();
    if (device) {
      device->drainDeferredDestroys();
    }
    viewport = nextViewport;
    const Vector2i pixelSize = CheckedViewportPixels(nextViewport).value_or(Vector2i::Zero());
    pixelWidth = pixelSize.x;
    pixelHeight = pixelSize.y;
    deviceFromLocalTransform = Transform2d();
    deviceFromLocalTransformStack.clear();
    paint = PaintParams();
    encoder.reset();
    frameFinishedEncoders.clear();
    geometryDebugEdges.clear();
    rejectedFilterDepth = 0;
    if (frameResourceScopeDepth == 0) {
      resetOwnedFrameBudgets();
    }
    if (device) {
      device->filterEngine().beginFrame();
    }
    if (texturePool) {
      texturePool->beginFrame();
    }
    closeFrameGeneration();
    if (device) {
      currentFrameIndex = device->beginFrameGeneration();
      frameGenerationOpen = true;
    } else {
      ++currentFrameIndex;
    }
    lastDrawSourceEntity = entt::null;
    pendingBatch.reset();
    transientGlyphEntries.clear();
    transientTextEncodes.clear();
    counters.reset();
  }

  void resetOwnedFrameBudgets() {
    if (ownsFilterExecutionBudget) {
      filterExecutionBudget->reset();
    }
    if (ownsFilterPreparationBudget) {
      filterPreparationBudget->reset();
    }
    if (ownsGeometryBudget) {
      geometryBudget->reset();
      documentGeometryFrameState->reset();
    }
    if (ownsSurfaceBudget) {
      surfaceBudget->reset();
    }
    if (ownsTextMaterializationBudget) {
      textMaterializationBudget->reset();
    }
  }

  bool prepareFrameTarget() {
    if (!device || !pipeline || !gradientPipeline || !imagePipeline || pixelWidth <= 0 ||
        pixelHeight <= 0) {
      retireOwnedTargetAtFrameBoundary();
      return false;
    }
    device->setCounters(&counters);
    if (hostTarget) {
      retireOwnedTargetAtFrameBoundary();
      if (hostTarget.getWidth() > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
          hostTarget.getHeight() > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        (void)surfaceBudget->reserve(-1, -1);
        target = wgpu::Texture();
        return false;
      }
      pixelWidth = static_cast<int>(hostTarget.getWidth());
      pixelHeight = static_cast<int>(hostTarget.getHeight());
      if (!surfaceBudget->reserve(pixelWidth, pixelHeight)) {
        target = wgpu::Texture();
        return false;
      }
      // The embedder owns this texture, so the runtime only names it. That name is refreshed
      // every frame because the embedder is free to hand over a different texture at any time.
      hostTargetHandle = gpu::Texture();
      gpu::Result<gpu::Texture> named = device->adapterDevice().importExternalTexture(
          hostTarget, gpu::Extent2d{hostTarget.getWidth(), hostTarget.getHeight()},
          geode::GpuTextureFormatFromWgpu(textureFormat),
          geode::GpuTextureUsageFromWgpu(wgpu::TextureUsage::RenderAttachment |
                                         wgpu::TextureUsage::TextureBinding |
                                         wgpu::TextureUsage::CopySrc));
      if (!named.hasResult()) {
        target = wgpu::Texture();
        return false;
      }
      hostTargetHandle = std::move(named).result();
      targetHandle = &hostTargetHandle;
      target = hostTarget;
      targetHandleTexture = static_cast<WGPUTexture>(target);
      return true;
    }

    if (!surfaceBudget->reserve(pixelWidth, pixelHeight)) {
      target = wgpu::Texture();
      return false;
    }

    const bool canReuseTargets =
        ownedTarget.isValid() && targetWidth == pixelWidth && targetHeight == pixelHeight;
    if (!canReuseTargets) {
      retireOwnedTargetAtFrameBoundary();
      gpu::Result<gpu::Texture> created =
          device->adapterDevice().createTexture(gpu::TextureDescriptor{
              "RendererGeodeTarget",
              gpu::Extent2d{static_cast<uint32_t>(pixelWidth), static_cast<uint32_t>(pixelHeight)},
              geode::GpuTextureFormatFromWgpu(textureFormat),
              gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc |
                  gpu::TextureUsage::Sampled});
      if (!created.hasResult()) {
        target = wgpu::Texture();
        return false;
      }
      // The runtime counts the creation itself, so there is no explicit tick here.
      ownedTarget = std::move(created).result();
      targetWidth = pixelWidth;
      targetHeight = pixelHeight;
    }
    targetHandle = &ownedTarget;
    // Readback and the layer-snapshot copy still name the target through the backend; both
    // borrow, and the runtime handle above is what owns it.
    target = device->adapterDevice().wgpuTextureOf(ownedTarget);
    targetHandleTexture = static_cast<WGPUTexture>(target);
    return true;
  }

  void createFrameEncoder() {
    wgpu::CommandEncoderDescriptor commandEncoderDesc = {};
    commandEncoderDesc.label = wgpuLabel("RendererGeodeFrameCE");
    frameCommandEncoder.reset(device->device().createCommandEncoder(commandEncoderDesc));
    UTILS_RELEASE_ASSERT_MSG(openFrameGpuEncoder(), "Failed to open the frame command encoder");
    encoder =
        std::make_unique<geode::GeoEncoder>(*device, *pipeline, *gradientPipeline, *imagePipeline,
                                            activeTarget(), targetExtent(), *frameGpuEncoder);
    configurePathEncoder(*encoder);
    if (preserveTargetOnBeginFrame) {
      encoder->setLoadPreserve();
    } else {
      encoder->clear(css::RGBA(0, 0, 0, 0));
    }
  }

  ~Impl() {
    encoder.reset();
    frameCommandEncoder.reset();
    frameFinishedEncoders.clear();

    if (device && device->device()) {
      // Bounded drain before releasing frame resources; skips (and stays
      // skipped) once the device is lost so renderer teardown never blocks
      // on a hung driver.
      device->waitForQueueIdle();
    }

    for (PendingRelease& release : framePendingReleases) {
      releasePendingTexture(release);
    }
    framePendingReleases.clear();

    for (LayerStackFrame& frame : layerStack) {
      releaseRuntimeTexture(frame.layerTexture);
    }
    layerStack.clear();

    for (MaskStackFrame& frame : maskStack) {
      releaseRuntimeTexture(frame.maskTexture);
      releaseRuntimeTexture(frame.contentTexture);
    }
    maskStack.clear();

    releaseClipStackTextures(clipStack);
    for (FilterStackFrame& frame : filterStack) {
      releaseRuntimeTexture(frame.layerTexture);
      releaseClipStackTextures(frame.savedClipStack);
    }
    filterStack.clear();

    for (PatternStackFrame& frame : patternStack) {
      releaseTextureBacking(frame.tileTexture);
    }
    patternStack.clear();
    if (patternFillPaint.has_value()) {
      releaseTextureBacking(patternFillPaint->tile);
    }
    if (patternStrokePaint.has_value()) {
      releaseTextureBacking(patternStrokePaint->tile);
    }
    patternFillPaint.reset();
    patternStrokePaint.reset();
    frameImportedTextureViews.clear();
    frameImportedTextures.clear();

    // Releasing the handle frees the target: the runtime slot it names is what holds the
    // backend object.
    ownedTarget = gpu::Texture();
    hostTargetHandle = gpu::Texture();
    targetHandle = nullptr;
    targetHandleTexture = nullptr;
    texturePool.reset();

    if (device) {
      device->drainDeferredDestroys();
      if (device->device()) {
        // Bounded; a no-op when the device is already lost.
        device->waitForQueueIdle();
      }
    }
  }

  /// Wire up per-renderer state against the shared GeodeDevice. Before
  /// issue #575's fix each renderer constructed its own pipelines
  /// here - that leaked ~1.6 MB/renderer through wgpu-native's internal
  /// pipeline cache (see `GeodeDevice::pipeline()` header comment).
  /// Now we just point at the device's shared copies. The `verboseFlag`
  /// parameter is kept on the method signature for API continuity but
  /// no longer toggles filter-engine logging - the shared filter engine
  /// is always constructed with `verbose=false`.
  void initPipelines(bool /*verboseFlag*/) {
    texturePool = TexturePoolForDevice(device);
    textureFormat = device->textureFormat();
    device->setCounters(&counters);
    pipeline = &device->pipeline();
    gradientPipeline = &device->gradientPipeline();
    imagePipeline = &device->imagePipeline();
    filterEngine = &device->filterEngine();
  }
};

void RendererGeode::Impl::ensureCacheInvalidationWired(Registry& registry) {
  // Sentinel lives on the registry's context store, so its presence
  // implies "this registry has already had our listener connected".
  // When the registry is destroyed the sentinel goes with it - a
  // later registry allocated at the same address will (correctly)
  // miss the sentinel and get its own listener. Pointer-identity on
  // `&registry` alone can't distinguish those cases.
  if (registry.ctx().contains<ListenerInstalled>()) {
    return;
  }
  registry.ctx().emplace<ListenerInstalled>();
  registry.on_update<components::ComputedPathComponent>().connect<&Impl::onComputedPathChanged>();
  registry.on_destroy<components::ComputedPathComponent>().connect<&Impl::onComputedPathChanged>();
  // Leaving the connection attached across renderer destruction is
  // intentional: our free-function listener has no `this`-capture,
  // so it's safe to outlive the renderer. Connections die with the
  // registry.
}

void RendererGeode::Impl::onComputedPathChanged(Registry& registry, Entity entity) {
  // entt allows `remove` on a component the entity doesn't hold - it's a
  // cheap no-op in that case. We don't need an `all_of` guard.
  registry.remove<geode::GeodePathCacheComponent>(entity);
  // Drop the GPU residence in lock-step with the CPU encode cache.
  // Removing the component runs `GeodeResidentSlot::reset()`, which frees
  // the persistent buffer and settles the live-bytes gauge. This fires on
  // geometry change (on_update) and entity/registry teardown (on_destroy),
  // both after the previous frame's submit, so destroying the buffer is
  // safe (wgpu-native keeps submitted resources alive until GPU-complete).
  registry.remove<geode::GeodeResidentPathComponent>(entity);
}

namespace {

class HeadlessGeodeDevicePool {
public:
  std::shared_ptr<geode::GeodeDevice> acquire() {
    std::shared_ptr<geode::GeodeDevice> device;
    {
      const std::lock_guard lock(mutex_);
      if (!idle_.empty()) {
        device = std::move(idle_.back());
        idle_.pop_back();
      }
    }
    if (!device) {
      device = std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    }
    if (!device) {
      return nullptr;
    }

    auto lease = std::make_shared<Lease>(this, std::move(device));
    return std::shared_ptr<geode::GeodeDevice>(lease, lease->device.get());
  }

private:
  struct Lease {
    Lease(HeadlessGeodeDevicePool* pool, std::shared_ptr<geode::GeodeDevice> device)
        : pool(pool), device(std::move(device)) {}
    ~Lease() { pool->release(std::move(device)); }

    HeadlessGeodeDevicePool* pool;
    std::shared_ptr<geode::GeodeDevice> device;
  };

  void release(std::shared_ptr<geode::GeodeDevice> device) {
    const std::lock_guard lock(mutex_);
    if (idle_.size() < kMaxIdleDevices) {
      idle_.push_back(std::move(device));
    }
  }

  static constexpr std::size_t kMaxIdleDevices = 4;
  std::mutex mutex_;
  std::vector<std::shared_ptr<geode::GeodeDevice>> idle_;
};

HeadlessGeodeDevicePool& SharedHeadlessGeodeDevicePool() {
  // Active leases retain a raw pointer to the pool. Keep the small bounded
  // cache alive through process teardown so global renderers cannot outlive it.
  static auto* pool = new HeadlessGeodeDevicePool();
  return *pool;
}

}  // namespace

RendererGeode::RendererGeode(bool verbose) : impl_(std::make_unique<Impl>()) {
  impl_->verbose = verbose;
  impl_->device = SharedHeadlessGeodeDevicePool().acquire();
  if (!impl_->device) {
    if (verbose) {
      std::cerr << "RendererGeode: GeodeDevice::CreateHeadless() failed - entering no-op mode\n";
    }
    return;
  }
  impl_->initPipelines(verbose);
}

RendererGeode::RendererGeode(std::shared_ptr<geode::GeodeDevice> device, bool verbose)
    : RendererGeode(std::move(device), verbose, {}) {}

RendererGeode::RendererGeode(std::shared_ptr<geode::GeodeDevice> device, bool verbose,
                             std::function<void()> constructionHook)
    : impl_(std::make_unique<Impl>()) {
  impl_->verbose = verbose;
  impl_->device = std::move(device);
  if (!impl_->device) {
    if (verbose) {
      std::cerr << "RendererGeode: null GeodeDevice passed - entering no-op mode\n";
    }
    return;
  }
  if (constructionHook) {
    constructionHook();
  }
  impl_->initPipelines(verbose);
}

void RendererGeode::enableTimestamps(bool /*enabled*/) {
  // Reserved for future work. Counters
  // are the durable regression signal and are always enabled.
}

void RendererGeode::setDebugGeometryOverlay(bool enabled) {
  if (impl_->debugGeometryOverlay != enabled) {
    if (enabled) {
      impl_->geometryDebugEdges.clear();
    } else {
      // Debug captures can be large for glyph-heavy documents. Return their
      // heap when the mode is disabled so the normal renderer retains no
      // debug-frame allocation high-water mark.
      std::vector<Impl::GeometryDebugEdge>().swap(impl_->geometryDebugEdges);
    }
  }
  impl_->debugGeometryOverlay = enabled;
}

bool RendererGeode::debugGeometryOverlay() const {
  return impl_->debugGeometryOverlay;
}

FrameTimings RendererGeode::lastFrameTimings() const {
  FrameTimings timings;
  timings.counters = impl_->counters;
  // `renderPassNs` / `totalGpuNs` stay zero until timestamp support lands.
  return timings;
}

RendererGeodeTexturePoolStats RendererGeode::texturePoolStats() const {
  return impl_->texturePoolStats();
}

void RendererGeode::setTargetTexture(wgpu::Texture texture) {
  impl_->borrowedTargetSnapshot.reset();
  impl_->hostTarget = std::move(texture);
}

void RendererGeode::clearTargetTexture() {
  impl_->borrowedTargetSnapshot.reset();
  impl_->hostTarget = wgpu::Texture();
  impl_->preserveTargetOnBeginFrame = false;
}

void RendererGeode::setPreserveTargetOnBeginFrame(bool preserve) {
  impl_->preserveTargetOnBeginFrame = preserve;
}

void RendererGeode::setAntialias(bool antialias) {
  impl_->antialias = antialias;
}

RendererGeode::~RendererGeode() {
  // GeodeDevice holds a raw `counters_` pointer into our Impl (see `Impl::initPipelines` →
  // `device->setCounters(&counters)`). If this renderer's counters are still the ones the
  // (shared) device refers to, clear the pointer before our Impl (and its `counters` member) is
  // freed. Otherwise the next `countBuffer`/`countTexture` call by any peer renderer sharing this
  // device will dereference freed memory - which is exactly how chained feImage rendering
  // crashes: multiple offscreen renderers share one device, each one overrides `counters_` in
  // `initPipelines`, and the first one destroyed leaves `counters_` dangling for the others.
  if (impl_ && impl_->device && impl_->device->counters() == &impl_->counters) {
    impl_->device->setCounters(nullptr);
  }
  if (impl_) {
    impl_->closeFrameGeneration();
  }
}
RendererGeode::RendererGeode(RendererGeode&&) noexcept = default;
RendererGeode& RendererGeode::operator=(RendererGeode&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  // `unique_ptr` move-assignment destroys our current Impl without invoking
  // `RendererGeode::~RendererGeode()`. Detach its counter sink first so a
  // shared device cannot retain a pointer into the displaced Impl. The moved
  // Impl itself stays at the same address, so a binding to `other` remains
  // valid after ownership transfers to `this`.
  if (impl_ && impl_->device && impl_->device->counters() == &impl_->counters) {
    impl_->device->setCounters(nullptr);
  }

  impl_ = std::move(other.impl_);
  return *this;
}

void RendererGeode::draw(SVGDocument& document) {
  // No-op mode: if adapter/device acquisition failed there is no GPU to
  // render with. Return before running the driver so none of the
  // per-element callbacks (drawPath, pushClip, pushFilterLayer, ...)
  // dereference a null device. Matches beginFrame's existing no-op guard.
  if (!impl_->device) {
    return;
  }

  // Wire the path-encode cache-invalidation listener onto this document's
  // registry BEFORE the driver runs `RenderingContext::instantiateRenderTree`.
  // The listener must be connected when `ShapeSystem` fires its
  // `on_update<ComputedPathComponent>` signals; otherwise a geometry
  // change between draws would silently leave a stale encode in
  // `GeodePathCacheComponent`.
  impl_->ensureCacheInvalidationWired(document.registry());

  // Merge the previous frame's freed slab ranges before any draw records
  // against this frame's command buffer (a range freed this frame is never
  // reused this frame; see GeodeResidentSlab::beginFrame).
  impl_->residentSlab(document.registry())->beginFrame(impl_->currentFrameIndex);
  impl_->recordSlab(document.registry())->beginFrame(impl_->currentFrameIndex);

  // Release the previous frame's temporary record slots (same-frame
  // repeat draws). freeSlot defers to the NEXT beginFrame, so freeing
  // here makes them reusable starting the frame after this one - the
  // batches recorded against them last frame have long submitted.
  for (const auto& tempSlot : impl_->sceneTempRecordSlots) {
    tempSlot.slab->freeSlot(tempSlot.slot);
  }
  impl_->sceneTempRecordSlots.clear();

  RendererDriver driver(*this, impl_->verbose);
  driver.draw(document);
}

bool RendererGeode::sceneBatchingEnabledForTesting() {
  return kEnableSceneBatching;
}

void RendererGeode::setGlyphResidencyBudgetForTesting(size_t maxEntries,
                                                      uint64_t maxRetainedBytes) {
  impl_->glyphCacheMaxEntries = maxEntries;
  impl_->glyphCacheMaxRetainedBytes = maxRetainedBytes;
}

void RendererGeode::setGeometryBudgetForTesting(std::size_t maximumDraws, std::size_t maximumItems,
                                                std::uint64_t maximumFrameBytes,
                                                std::uint64_t maximumCacheBytes,
                                                std::uint64_t maximumResidentBytes) {
  impl_->geometryBudget->setLimitsForTesting(
      {.draws = maximumDraws, .items = maximumItems, .retainedBytes = maximumFrameBytes});
  impl_->documentGeometryLimits->cacheBytes =
      std::min(impl_->documentGeometryLimits->cacheBytes, maximumCacheBytes);
  impl_->documentGeometryLimits->residentBytes =
      std::min(impl_->documentGeometryLimits->residentBytes, maximumResidentBytes);
  for (const std::shared_ptr<geode::GeodeDocumentGeometryBudget>& document :
       impl_->documentGeometryFrameState->touched) {
    document->setLimitsForTesting(*impl_->documentGeometryLimits);
  }
}

void RendererGeode::setSurfaceBudgetForTesting(std::size_t maximumSurfaces,
                                               std::uint64_t maximumBytes) {
  impl_->surfaceBudget->setLimitsForTesting({.bytes = maximumBytes, .surfaces = maximumSurfaces});
}

void RendererGeode::setTextMaterializationBudgetForTesting(
    RendererTextMaterializationBudget::Cost limits, std::size_t maximumGlyphOccurrences) {
  impl_->textMaterializationBudget->setLimitsForTesting(limits);
  impl_->textMaterializationBudget->setGlyphOccurrenceLimitForTesting(maximumGlyphOccurrences);
}

void RendererGeode::injectScenePreparationFailureAfterForTesting(
    std::size_t successfulPreparations) {
  if (impl_->encoder) {
    impl_->encoder->injectScenePreparationFailureAfterForTesting(successfulPreparations);
  }
}

RendererResourceStats RendererGeode::resourceStats() const {
  std::uint64_t documentBytes = 0;
  bool documentRejected = false;
  for (const std::shared_ptr<geode::GeodeDocumentGeometryBudget>& document :
       impl_->documentGeometryFrameState->touched) {
    const std::uint64_t retained = document->cacheBytes() + document->residentBytes();
    documentBytes = retained > std::numeric_limits<std::uint64_t>::max() - documentBytes
                        ? std::numeric_limits<std::uint64_t>::max()
                        : documentBytes + retained;
    documentRejected = documentRejected || document->rejected();
  }
  const std::uint64_t geometryBytes = impl_->geometryBudget->retainedBytes();
  const std::uint64_t totalGeometryBytes =
      documentBytes > std::numeric_limits<std::uint64_t>::max() - geometryBytes
          ? std::numeric_limits<std::uint64_t>::max()
          : documentBytes + geometryBytes;
  const std::size_t reportedGeometryBytes =
      totalGeometryBytes > std::numeric_limits<std::size_t>::max()
          ? std::numeric_limits<std::size_t>::max()
          : static_cast<std::size_t>(totalGeometryBytes);
  return {
      .filterBudgetSupported = true,
      .filterExecutions = impl_->filterExecutionBudget->executions(),
      .filterWorkUnits = impl_->filterExecutionBudget->workUnits(),
      .filterRetainedBytes = impl_->filterExecutionBudget->retainedBytes(),
      .filterCaptureBytesReserved = impl_->filterExecutionBudget->captureBytesReserved(),
      .filterBudgetRejected = impl_->filterExecutionBudget->rejected(),
      .geometryBudgetSupported = true,
      .geometryDraws = impl_->geometryBudget->draws(),
      .geometryItems = impl_->geometryBudget->items(),
      .geometryRetainedBytes = reportedGeometryBytes,
      .geometryBudgetRejected = impl_->geometryBudget->rejected() || documentRejected,
      .surfaceBudgetSupported = true,
      .surfaceCount = impl_->surfaceBudget->surfaces(),
      .surfaceBytes = impl_->surfaceBudget->bytes(),
      .surfaceBudgetRejected = impl_->surfaceBudget->rejected(),
      .textMaterializationBudgetSupported = true,
      .textUniqueOutlines = impl_->textMaterializationBudget->uniqueOutlines(),
      .textMaterializationCommands = impl_->textMaterializationBudget->commands(),
      .textMaterializationPoints = impl_->textMaterializationBudget->points(),
      .textMaterializationBytes = impl_->textMaterializationBudget->bytes(),
      .textGlyphDecodeWork = impl_->textMaterializationBudget->decodeWork(),
      .textGlyphOccurrences = impl_->textMaterializationBudget->glyphOccurrences(),
      .textMaterializationBudgetRejected = impl_->textMaterializationBudget->rejected(),
  };
}

size_t RendererGeode::residentGlyphCountForTesting(SVGDocument& document) {
  if (!impl_->device) {
    return 0;
  }
  auto* cachePtr = document.registry().ctx().find<std::shared_ptr<geode::GeodeGlyphCache>>();
  if (cachePtr == nullptr || !*cachePtr ||
      (*cachePtr)->owningDeviceId() != impl_->device->deviceId()) {
    return 0;
  }
  return (*cachePtr)->size();
}

void RendererGeode::beginFrameResourceScope() {
  if (impl_->frameResourceScopeDepth == 0) {
    impl_->resetOwnedFrameBudgets();
  }
  ++impl_->frameResourceScopeDepth;
}

void RendererGeode::endFrameResourceScope() {
  UTILS_RELEASE_ASSERT(impl_->frameResourceScopeDepth > 0);
  --impl_->frameResourceScopeDepth;
}

int RendererGeode::width() const {
  return impl_->pixelWidth;
}
int RendererGeode::height() const {
  return impl_->pixelHeight;
}

void RendererGeode::beginFrame(const RenderViewport& viewport) {
  impl_->resetForBeginFrame(viewport);
  if (impl_->prepareFrameTarget()) {
    impl_->createFrameEncoder();
  }
}

void RendererGeode::endFrame() {
  // Flush any pending `<use>`-batch before closing out
  // the frame. Without this, the last run of batchable draws in the
  // frame would never emit.
  impl_->flushPendingBatch();

  if (impl_->encoder) {
    // Ends the open render pass without submitting - shared-mode.
    impl_->retireActiveEncoder();
  }

  // Replay the captured post-vertex Slug triangle edges only after every
  // normal paint, layer, filter, and mask composite. This makes the debug
  // wireframe globally topmost within the renderer's final target.
  impl_->emitGeometryDebugOverlay();
  if (impl_->encoder) {
    impl_->retireActiveEncoder();
  }

  // Finalise and submit the single frame-wide CommandEncoder. After
  // this one submit, all recorded render passes (base + every pushed
  // layer / filter / mask) execute on the GPU in program order.
  if (impl_->frameCommandEncoder) {
    UTILS_RELEASE_ASSERT_MSG(impl_->flushFrameGpuEncoder(),
                             "Failed to replay the frame's recorded draws");
    {
      geode::ScopedWgpuHandle<wgpu::CommandBuffer> cmdBuf(
          impl_->frameCommandEncoder.get().finish());
      impl_->device->queue().submit(1, &cmdBuf.get());
      impl_->device->countSubmit();
    }
    impl_->closeFrameGpuEncoderAfterSubmit();
    impl_->frameCommandEncoder.reset();
    impl_->frameFinishedEncoders.clear();
    // Every encoder that could still reach these aliases is gone, and the textures they name are
    // about to be recycled, so drop them before any of them can be handed out again.
    impl_->frameImportedTextureViews.clear();
    impl_->frameImportedTextures.clear();
  }

  // The frame's work is submitted, so nothing it recorded can still be waiting
  // to read a buffer: its generation stops holding back the caches that defer
  // recycling until every frame that touched them has submitted.
  impl_->closeFrameGeneration();

  // Now that the command buffer is submitted, it's safe to return the
  // frame's transient layer / filter / mask / snapshot textures to
  // the pool. WebGPU's driver tracks texture dependencies across
  // submits, so acquiring these on the next frame will schedule the
  // new writes after the previous submit's GPU work completes.
  impl_->drainPendingReleases();

  impl_->deviceFromLocalTransform = Transform2d();
  impl_->deviceFromLocalTransformStack.clear();
}

void RendererGeode::setTransform(const Transform2d& transform) {
  // Inside a pattern tile we're rasterising into a texture whose pixel
  // dimensions may not match the logical tile-space units. Pre-compose the
  // raster scale onto the transform so draws submitted to the tile encoder
  // map 1:1 onto the tile texture's pixel grid. (Matches
  // `RendererTinySkia::setTransform`'s `scaleTransformOutput` path.)
  if (!impl_->patternStack.empty()) {
    const Vector2d& scale = impl_->patternStack.back().rasterScale;
    Transform2d scaled = transform;
    scaled.data[0] *= scale.x;
    scaled.data[2] *= scale.x;
    scaled.data[4] *= scale.x;
    scaled.data[1] *= scale.y;
    scaled.data[3] *= scale.y;
    scaled.data[5] *= scale.y;
    impl_->deviceFromLocalTransform = scaled;
    return;
  }
  if (!impl_->filterStack.empty()) {
    const auto& filterFrame = impl_->filterStack.back();
    if (filterFrame.filterBufferOffsetX != 0 || filterFrame.filterBufferOffsetY != 0) {
      impl_->deviceFromLocalTransform =
          transform *
          Transform2d::Translate(filterFrame.filterBufferOffsetX, filterFrame.filterBufferOffsetY);
      return;
    }
  }
  impl_->deviceFromLocalTransform = transform;
}

void RendererGeode::pushTransform(const Transform2d& transform) {
  impl_->deviceFromLocalTransformStack.push_back(impl_->deviceFromLocalTransform);
  impl_->deviceFromLocalTransform = transform * impl_->deviceFromLocalTransform;
}

void RendererGeode::popTransform() {
  if (impl_->deviceFromLocalTransformStack.empty()) {
    return;
  }
  impl_->deviceFromLocalTransform = impl_->deviceFromLocalTransformStack.back();
  impl_->deviceFromLocalTransformStack.pop_back();
}

void RendererGeode::pushClip(const ResolvedClip& clip) {
  // Flush the batch before a state change - a subsequent
  // drawPath inside the new clip is no longer "batch-compatible"
  // with the pending run from the outer clip region.
  impl_->flushPendingBatch();

  // Rectangular clip (the nested-`<svg>` viewport, `overflow: hidden`, and
  // `<image>` dest-rect cases) is implemented via the WebGPU scissor rect
  // (plus the convex polygon clip for non-axis-aligned ancestors).
  // Path-based clip-paths are implemented via the mask pipeline below.
  // `<mask>` alpha masks run through the mask
  // blit pipeline via `pushMaskLayer` - by the time `pushClip` runs the
  // mask is already composed upstream, so there's nothing to do for
  // `clip.mask` here.

  // Compose the incoming clip rect (in user-space) with the current
  // transform to get pixel-space coordinates, then push onto the stack.
  // The active scissor is the INTERSECTION of everything on the stack.
  Impl::ClipStackEntry entry;
  if (!impl_->initializeClipEntry(clip, entry)) {
    return;
  }

  // Path-clip mask. When the clip has any `clipPaths`,
  // render them into R8Unorm mask textures via the Slug mask
  // pipeline and hand the view of the outermost layer to
  // the encoder so subsequent fill / gradient draws multiply clip
  // coverage into their output.
  //
  // `clip.clipPaths` is a flat list in traversal order with a
  // per-shape `layer` index. Paths at the same layer are UNIONED;
  // when the layer decreases (we cross back from a nested clipPath
  // reference to its parent), that whole nested layer is
  // INTERSECTED with each path at the outer layer. This matches the
  // recursive `buildLayerMask` in `RendererTinySkia::pushClip`.
  //
  // The union step lives in the hardware blend (`BlendOperation::Max`
  // on the R channel). The intersection step lives in the slug_mask
  // fragment shader: when a deeper layer's mask is bound as the
  // `clipMaskTexture`, each outer-layer shape samples it at its
  // pixel center and multiplies the coverage output, so
  // `max(shape_i ∩ nested)` = `(union of shape_i) ∩ nested`.
  //
  // Bottom-up traversal order: we scan the clipPaths list and
  // partition it into contiguous runs of equal layer. Deeper layers
  // appear AFTER outer layers in the list (the driver emits them in
  // the order they're encountered during DFS, and references push a
  // higher layer for the nested clip's children). So the deepest
  // layer is at the tail; we render each layer's mask in reverse,
  // binding the previously-rendered deeper mask as the input clip.
  if (!clip.clipPaths.empty() && impl_->device && impl_->encoder && impl_->pixelWidth > 0 &&
      impl_->pixelHeight > 0) {
    const auto makeMaskTexture = [&](const char* label, gpu::TextureDescriptor& outDesc) {
      outDesc =
          gpu::TextureDescriptor{RcString(label),
                                 gpu::Extent2d{static_cast<uint32_t>(impl_->pixelWidth),
                                               static_cast<uint32_t>(impl_->pixelHeight)},
                                 gpu::TextureFormat::RGBA8Unorm,
                                 gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::Sampled};
      return impl_->acquireTexture(outDesc);
    };
    // Partition `clip.clipPaths` into contiguous [begin, end) ranges,
    // one per layer, in the order they appear. Layers appear in
    // traversal order - within a run the layer is constant, and
    // boundaries correspond to clipPath-reference crossings. For
    // each run we'll render one mask texture.
    struct LayerRun {
      size_t begin;
      size_t end;
      int layer;
    };
    std::vector<LayerRun> runs;
    {
      size_t i = 0;
      while (i < clip.clipPaths.size()) {
        const int layer = clip.clipPaths[i].layer;
        size_t j = i + 1;
        while (j < clip.clipPaths.size() && clip.clipPaths[j].layer == layer) {
          ++j;
        }
        runs.push_back({i, j, layer});
        i = j;
      }
    }

    // Render runs bottom-up (deepest layer first). Each run's mask
    // is rendered with the previously-rendered deeper mask bound as
    // the input clip so shapes get intersected with the deeper
    // union. Runs at the same layer don't intersect with each other
    // - the Max blend on the R channel handles union within a run.
    const Transform2d savedDeviceFromLocalTransform = impl_->deviceFromLocalTransform;

    // If any clip stack entry already carries a path mask (e.g., an
    // ancestor `<g>` with its own `clip-path`), use the topmost one
    // as the initial nested mask so this new clip gets intersected
    // with it as it's being rendered. Without this seed the outer
    // ancestor clip would be lost the moment the inner clip lands
    // because `updateEncoderScissor` only binds the topmost entry.
    // Aliases owned by the frame's import lists, so they stay valid for the rest of the frame.
    const gpu::Texture* nestedMaskTextureHandle = nullptr;
    const gpu::TextureView* nestedMaskViewHandle = nullptr;
    for (auto rit = impl_->clipStack.rbegin(); rit != impl_->clipStack.rend(); ++rit) {
      if (rit->maskResolveViewHandle != nullptr) {
        nestedMaskTextureHandle = rit->maskResolveTextureHandle;
        nestedMaskViewHandle = rit->maskResolveViewHandle;
        break;
      }
    }

    for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
      gpu::TextureDescriptor maskDesc = {};
      gpu::Texture maskTexture = makeMaskTexture("RendererGeodeClipMask", maskDesc);
      if (!maskTexture.isValid()) {
        entry.allocationRejected = true;
        break;
      }

      // Bind the previously-rendered nested mask (if any) so this
      // layer's fragment shader samples it and intersects.
      if (nestedMaskViewHandle != nullptr) {
        impl_->encoder->setClipMask(*nestedMaskTextureHandle, *nestedMaskViewHandle);
      } else {
        impl_->encoder->clearClipMask();
      }

      // The mask is parked in the clip entry that owns it before anything names it, so the
      // handle recorded below points at the owner rather than at a copy that outlives its scope.
      entry.maskLayerTextures.push_back({std::move(maskTexture), maskDesc});
      const gpu::Texture& maskTextureHandle = entry.maskLayerTextures.back().texture;
      impl_->encoder->beginMaskPass(maskTextureHandle);
      for (size_t s = it->begin; s < it->end; ++s) {
        const ClipPathShape& shape = clip.clipPaths[s];
        const Transform2d composed =
            clip.clipPathUnitsTransform * shape.parentFromEntity * savedDeviceFromLocalTransform;
        impl_->encoder->setTransform(composed);
        impl_->encoder->fillPathIntoMask(shape.path, shape.fillRule);
      }
      impl_->encoder->endMaskPass();

      nestedMaskTextureHandle = &maskTextureHandle;
      nestedMaskViewHandle = &impl_->importTextureView(maskTextureHandle);

      // The outermost layer (the LAST one processed by this loop,
      // i.e. the FIRST run in `runs`) provides the mask view the
      // main draws sample as their clip.
      entry.maskResolveTextureHandle = nestedMaskTextureHandle;
      entry.maskResolveViewHandle = nestedMaskViewHandle;
    }

    // Clear the encoder's internal clip-mask state - the next main
    // pass will rebind via `updateEncoderScissor`.
    impl_->encoder->clearClipMask();
    impl_->encoder->setTransform(savedDeviceFromLocalTransform);
  }

  impl_->clipStack.push_back(std::move(entry));
  impl_->updateEncoderScissor();
}

void RendererGeode::popClip() {
  // Flush before popping so the batched draws stay
  // inside the clip region they were accumulated under.
  impl_->flushPendingBatch();

  if (!impl_->clipStack.empty()) {
    // Defer release of the mask textures to endFrame - the main
    // encoder that was just drawing under this clip may have recorded
    // samples from the resolved mask texture into the frame encoder, and
    // recycling mid-frame could hand the texture to a later acquire
    // before the submit.
    Impl::ClipStackEntry& entry = impl_->clipStack.back();
    for (auto& release : entry.maskLayerTextures) {
      impl_->releaseTextureAtFrameEnd(std::move(release.texture), release.desc);
    }
    entry.maskLayerTextures.clear();
    impl_->clipStack.pop_back();
  }
  impl_->updateEncoderScissor();
}

void RendererGeode::pushIsolatedLayer(double opacity, MixBlendMode blendMode) {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.

  // All 16 `mix-blend-mode` values are implemented: the pushed
  // layer renders normally, and `popIsolatedLayer` switches to a
  // blend-blit compositor that reads a frozen snapshot of the parent
  // target and runs the matching W3C Compositing 1 formula per
  // pixel. `MixBlendMode::Normal` keeps the existing plain
  // source-over composite path.
  if (!impl_->device || !impl_->pipeline || !impl_->gradientPipeline || !impl_->imagePipeline ||
      !impl_->encoder) {
    // Headless or degenerate state - drop silently but still push a
    // placeholder frame so popIsolatedLayer stays balanced.
    impl_->layerStack.push_back({});
    return;
  }

  // Allocate an offscreen layer. All draws issued between push/pop land
  // here; pop composites it back onto the outer target with the stored opacity.
  const gpu::TextureDescriptor td{"RendererGeodeIsolatedLayer",
                                  gpu::Extent2d{static_cast<uint32_t>(impl_->pixelWidth),
                                                static_cast<uint32_t>(impl_->pixelHeight)},
                                  impl_->gpuTextureFormat(),
                                  gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::Sampled |
                                      gpu::TextureUsage::CopySrc};
  gpu::Texture layerTexture = impl_->acquireTexture(td);
  if (!layerTexture.isValid()) {
    impl_->layerStack.push_back({});
    return;
  }

  // Finish the outer encoder so its queued draws land on the saved target
  // before we redirect subsequent work into the offscreen layer. Same
  // shape as `beginPatternTile` - two render-pass submissions ordered
  // serially on the queue.
  impl_->encoder->finish();

  Impl::LayerStackFrame frame;
  frame.savedEncoder = std::move(impl_->encoder);
  frame.savedTarget = impl_->target;
  frame.layerTexture = std::move(layerTexture);
  frame.layerDesc = td;
  frame.opacity = opacity;
  frame.blendMode = blendMode;

  impl_->target = impl_->backendTextureOf(frame.layerTexture);
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->importTarget(impl_->target), td.size, *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*newEncoder);
  newEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->encoder = std::move(newEncoder);
  impl_->layerStack.push_back(std::move(frame));
  // The layer inherits the outer clip stack - reapply it to the new
  // encoder so scissors carry through.
  impl_->updateEncoderScissor();
}

void RendererGeode::popIsolatedLayer() {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  if (impl_->layerStack.empty()) {
    return;
  }
  Impl::LayerStackFrame frame = std::move(impl_->layerStack.back());
  impl_->layerStack.pop_back();

  if (!frame.layerTexture.isValid()) {
    return;  // Placeholder frame from the headless/error path.
  }

  // Finish the layer's render pass so the texture contents are ready.
  if (impl_->encoder) {
    impl_->retireActiveEncoder();
  }
  impl_->retireFinishedEncoder(std::move(frame.savedEncoder));

  // Restore outer target references.
  impl_->target = frame.savedTarget;

  if (frame.blendMode != MixBlendMode::Normal) {
    // SVG `mix-blend-mode`. The fragment shader needs the parent's current pixels as a
    // backdrop, but a render pass may not read from its own color attachment. Snapshot the
    // parent's 1-sample resolve target into a separate texture with a texture-to-texture copy,
    // then open a fresh parent encoder over the parent target; the load-op note further down
    // covers why that encoder preserves the target's contents.
    const gpu::TextureDescriptor snapDesc{"RendererGeodeBlendDstSnapshot",
                                          gpu::Extent2d{static_cast<uint32_t>(impl_->pixelWidth),
                                                        static_cast<uint32_t>(impl_->pixelHeight)},
                                          impl_->gpuTextureFormat(),
                                          gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst};
    gpu::Texture snapshot = impl_->acquireTexture(snapDesc);

    if (snapshot.isValid()) {
      const gpu::Texture& savedTargetHandle = impl_->importTarget(frame.savedTarget);
      const gpu::Texture& snapshotHandle = snapshot;

      // The layer's render pass ended when its encoder was retired above, so the copy can be
      // recorded outside a pass. It joins the frame's recorded stream after those draws and
      // before the composite recorded below, which is the order the backdrop has to be frozen in.
      const gpu::Status copied = impl_->frameGpuEncoder->copyTextureToTexture(
          savedTargetHandle, snapshotHandle,
          gpu::Extent2d{static_cast<uint32_t>(impl_->pixelWidth),
                        static_cast<uint32_t>(impl_->pixelHeight)});
      UTILS_RELEASE_ASSERT_MSG(!copied.hasError(),
                               "Failed to record the mix-blend-mode backdrop snapshot copy");

      // Open a fresh parent encoder that PRESERVES the target's existing
      // contents (the backdrop pre-push - identical to `snapshot` at
      // this point, since we just copied from `savedTarget` above).
      //
      // We must not CLEAR here: if an outer clip/scissor is active,
      // `updateEncoderScissor` will restrict the blend blit to the clip
      // rect, and any pixels outside that rect would remain at the
      // clear color (transparent) - losing the backdrop outside the
      // clip. With Load, out-of-scissor pixels are preserved from the
      // attachment, which already holds the backdrop.
      //
      // No feedback loop: `snapshot` is a copy of `savedTarget`, not an
      // alias, so sampling `snapshot` while writing `savedTarget` is
      // safe.
      auto newEncoder = std::make_unique<geode::GeoEncoder>(
          *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
          savedTargetHandle, impl_->targetExtent(), *impl_->frameGpuEncoder);
      impl_->configurePathEncoder(*newEncoder);
      newEncoder->setLoadPreserve();
      impl_->encoder = std::move(newEncoder);
      impl_->updateEncoderScissor();
      impl_->encoder->blitFullTargetBlended(frame.layerTexture, snapshotHandle,
                                            static_cast<uint32_t>(frame.blendMode), frame.opacity);
      // Defer release: `blitFullTargetBlended` recorded samples from
      // both `frame.layerTexture` and `snapshot` into the shared
      // frameCommandEncoder; they must stay alive until that buffer
      // is submitted at `endFrame`.
      impl_->releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
      impl_->releaseTextureAtFrameEnd(std::move(snapshot), snapDesc);
      return;
    }
    // If snapshot allocation failed fall through to the Normal path -
    // at least the layer content shows up even if unblended.
  }

  // Plain premultiplied source-over (the `Normal` case). Create a
  // fresh encoder that preserves its existing contents. Draw the layer
  // texture across the target with the stored opacity as compositing alpha.
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->importTarget(frame.savedTarget), impl_->targetExtent(), *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*newEncoder);
  newEncoder->setLoadPreserve();
  impl_->encoder = std::move(newEncoder);
  impl_->updateEncoderScissor();
  impl_->encoder->blitFullTarget(frame.layerTexture, frame.opacity);
  // Same deferred-release rationale as the blend-mode branch above.
  impl_->releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
}

void RendererGeode::pushFilterLayer(const components::FilterGraph& filterGraph,
                                    const std::optional<Box2d>& filterRegion) {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  if (impl_->rejectedFilterDepth != 0) {
    impl_->pushRejectedFilterFrame();
    return;
  }
  if (!impl_->readyForFilterCapture()) {
    impl_->filterStack.push_back({});
    return;
  }
  std::optional<GeodeFilterAdmission> admission =
      AdmitGeodeFilter(filterGraph, filterRegion, impl_->deviceFromLocalTransform,
                       impl_->pixelWidth, impl_->pixelHeight, *impl_->filterExecutionBudget);
  if (!admission.has_value() && impl_->filterExecutionBudget->executions() != 0 &&
      impl_->submitFilterBudgetChunk()) {
    admission =
        AdmitGeodeFilter(filterGraph, filterRegion, impl_->deviceFromLocalTransform,
                         impl_->pixelWidth, impl_->pixelHeight, *impl_->filterExecutionBudget);
  }
  if (!admission.has_value()) {
    impl_->pushRejectedFilterFrame();
    return;
  }

  if (!impl_->beginFilterCapture(filterGraph, *admission, impl_->deviceFromLocalTransform)) {
    auto reservation = admission->reservation;
    impl_->filterExecutionBudget->release(reservation);
    impl_->filterExecutionBudget->reject();
    impl_->pushRejectedFilterFrame();
  }
}

void RendererGeode::popFilterLayer() {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  if (impl_->filterStack.empty()) {
    return;
  }
  Impl::FilterStackFrame frame = std::move(impl_->filterStack.back());
  impl_->filterStack.pop_back();
  impl_->clipStack = std::move(frame.savedClipStack);

  if (frame.allocationRejected) {
    if (impl_->rejectedFilterDepth != 0) {
      --impl_->rejectedFilterDepth;
    }
    if (frame.savedEncoder) {
      impl_->target = frame.savedTarget;
      impl_->encoder = std::move(frame.savedEncoder);
      impl_->updateEncoderScissor();
    }
    return;
  }

  if (!frame.layerTexture.isValid()) {
    return;  // Placeholder frame from the headless/error path.
  }

  // Finish the filter layer's render pass so the texture is ready.
  if (impl_->encoder) {
    impl_->retireActiveEncoder();
  }
  impl_->retireFinishedEncoder(std::move(frame.savedEncoder));

  // When the filter buffer was expanded to capture negative-coordinate content, adjust
  // deviceFromFilter to include the offset so the filter engine interprets coordinates correctly.
  const Transform2d bufferDeviceFromFilter =
      (frame.filterBufferOffsetX != 0 || frame.filterBufferOffsetY != 0)
          ? frame.deviceFromFilter *
                Transform2d::Translate(frame.filterBufferOffsetX, frame.filterBufferOffsetY)
          : frame.deviceFromFilter;

  if (impl_->tryCompositeTransformedFilter(frame)) {
    impl_->filterExecutionBudget->release(frame.filterReservation);
    impl_->releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
    return;
  }

  // Run the filter graph on the captured layer texture. The filter engine still records
  // through wgpu directly, so it takes the backend alias of the pooled capture and hands back
  // another backend texture its own arena owns.
  const wgpu::Texture layerBackendTexture = impl_->backendTextureOf(frame.layerTexture);
  wgpu::Texture filteredTexture = layerBackendTexture;
  const bool discardCapturedLayer = frame.localRasterRequiredForBudget;
  if (frame.localRasterRequiredForBudget) {
    impl_->filterExecutionBudget->reject();
  } else if (impl_->filterEngine && !frame.filterGraph.empty() &&
             static_cast<std::uint64_t>(frame.layerDesc.size.width) * frame.layerDesc.size.height <=
                 components::kMaximumFilterSurfacePixels) {
    UTILS_RELEASE_ASSERT_MSG(impl_->flushFrameGpuEncoder(),
                             "Failed to replay recorded draws before the filter passes");
    filteredTexture =
        impl_->filterEngine->execute(frame.filterGraph, layerBackendTexture, frame.filterRegion,
                                     bufferDeviceFromFilter, *impl_, impl_->frameCommandEncoder);
  }

  // Restore outer target and create a fresh encoder that preserves its
  // existing contents. Composite the filtered texture back with full
  // opacity (filter results are already premultiplied).
  impl_->target = frame.savedTarget;
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->importTarget(frame.savedTarget), impl_->targetExtent(), *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*newEncoder);
  newEncoder->setLoadPreserve();
  impl_->encoder = std::move(newEncoder);
  impl_->updateEncoderScissor();
  if (discardCapturedLayer) {
    impl_->filterExecutionBudget->release(frame.filterReservation);
    impl_->releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
    return;
  }
  if (frame.filterBufferOffsetX != 0 || frame.filterBufferOffsetY != 0) {
    // The filter result is in an expanded texture. Extract the viewport-sized region at the
    // buffer offset using a GPU texture copy, then blit the viewport-sized result.
    const uint32_t vpW = static_cast<uint32_t>(impl_->pixelWidth);
    const uint32_t vpH = static_cast<uint32_t>(impl_->pixelHeight);

    const gpu::TextureDescriptor vpDesc{"RendererGeodeFilterViewport", gpu::Extent2d{vpW, vpH},
                                        geode::GpuTextureFormatFromWgpu(kFilterIntermediateFormat),
                                        gpu::TextureUsage::CopyDst | gpu::TextureUsage::Sampled};
    gpu::Texture viewportTexture = impl_->acquireTexture(vpDesc);

    if (viewportTexture.isValid()) {
      const gpu::Texture& filteredHandle = impl_->importFilterResult(filteredTexture);
      const gpu::Texture& viewportHandle = viewportTexture;

      // Everything recorded before this point has already been replayed into the frame command
      // encoder by the encoder retire above, so the copy lands after those draws and before the
      // composite recorded below.
      const gpu::Status copied = impl_->frameGpuEncoder->copyTextureToTexture(
          filteredHandle, viewportHandle, gpu::Extent2d{vpW, vpH},
          gpu::Origin2d{static_cast<uint32_t>(frame.filterBufferOffsetX),
                        static_cast<uint32_t>(frame.filterBufferOffsetY)});
      UTILS_RELEASE_ASSERT_MSG(!copied.hasError(),
                               "Failed to record the filter viewport extraction copy");

      impl_->encoder->blitFullTarget(viewportHandle, 1.0);
      impl_->releaseTextureAtFrameEnd(std::move(viewportTexture), vpDesc);
    }
  } else {
    // TODO(geode): Clip the composite to the filter region per SVG 2 §15.5.
    // `frame.filterRegion` is passed in from the driver in user-space
    // coordinates, not pixel-space; we'd need to transform by the current
    // CTM snapshot before using it as a scissor. Skipping for this PR -
    // all current feGaussianBlur resvg tests pass without the clip.
    impl_->encoder->blitFullTarget(impl_->importFilterResult(filteredTexture), 1.0);
  }
  // Defer release to endFrame: `blitFullTarget` recorded a sample from
  // `filteredTexture` (which is `frame.layerTexture` when the filter
  // graph is empty) into the frame encoder. Filter-engine-owned
  // intermediates have already been queued for frame-end pool release
  // through `FilterTextureAllocator`; recycle the layer capture here.
  impl_->releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
  impl_->filterExecutionBudget->release(frame.filterReservation);
}

void RendererGeode::pushMask(const std::optional<Box2d>& maskBounds, MaskType maskType) {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  if (!impl_->device || !impl_->pipeline || !impl_->gradientPipeline || !impl_->imagePipeline ||
      !impl_->encoder || impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    // Headless / degenerate - push a placeholder so popMask stays balanced.
    impl_->maskStack.push_back({});
    return;
  }

  const auto allocTexture = [&](const char* label, gpu::Texture& outTexture,
                                gpu::TextureDescriptor& outDesc) {
    outDesc = gpu::TextureDescriptor{RcString(label),
                                     gpu::Extent2d{static_cast<uint32_t>(impl_->pixelWidth),
                                                   static_cast<uint32_t>(impl_->pixelHeight)},
                                     impl_->gpuTextureFormat(),
                                     gpu::TextureUsage::RenderAttachment |
                                         gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc};
    outTexture = impl_->acquireTexture(outDesc);
  };

  Impl::MaskStackFrame frame;
  allocTexture("RendererGeodeMaskCapture", frame.maskTexture, frame.maskDesc);
  allocTexture("RendererGeodeMaskContent", frame.contentTexture, frame.contentDesc);
  if (!frame.maskTexture.isValid() || !frame.contentTexture.isValid()) {
    impl_->maskStack.push_back({});
    return;
  }
  frame.maskBounds = maskBounds;
  frame.maskBoundsTransform = impl_->deviceFromLocalTransform;
  frame.maskType = maskType;

  // Flush the outer encoder's pending draws so they land before we
  // redirect subsequent commands into the mask capture.
  impl_->encoder->finish();

  frame.savedEncoder = std::move(impl_->encoder);
  frame.savedTarget = impl_->target;
  frame.phase = Impl::MaskStackFrame::Phase::Capturing;

  impl_->target = impl_->backendTextureOf(frame.maskTexture);
  auto captureEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->importTarget(impl_->target), frame.maskDesc.size, *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*captureEncoder);
  captureEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->encoder = std::move(captureEncoder);
  impl_->maskStack.push_back(std::move(frame));
  impl_->updateEncoderScissor();
}

void RendererGeode::transitionMaskToContent() {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  if (impl_->maskStack.empty()) {
    return;
  }
  Impl::MaskStackFrame& frame = impl_->maskStack.back();
  if (frame.phase != Impl::MaskStackFrame::Phase::Capturing) {
    return;
  }
  if (!frame.contentTexture.isValid() || !impl_->encoder) {
    frame.phase = Impl::MaskStackFrame::Phase::Content;
    return;
  }

  // Flush the mask-capture encoder so the mask texture is ready to
  // sample in popMask.
  impl_->retireActiveEncoder();

  impl_->target = impl_->backendTextureOf(frame.contentTexture);
  auto contentEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->importTarget(impl_->target), frame.contentDesc.size, *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*contentEncoder);
  contentEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->encoder = std::move(contentEncoder);
  frame.phase = Impl::MaskStackFrame::Phase::Content;
  impl_->updateEncoderScissor();
}

void RendererGeode::popMask() {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  if (impl_->maskStack.empty()) {
    return;
  }
  Impl::MaskStackFrame frame = std::move(impl_->maskStack.back());
  impl_->maskStack.pop_back();

  if (!frame.savedEncoder) {
    // Placeholder frame from the headless path - nothing to do.
    return;
  }

  // Finish the content encoder so its target is ready to sample.
  if (impl_->encoder) {
    impl_->retireActiveEncoder();
  }
  impl_->retireFinishedEncoder(std::move(frame.savedEncoder));

  // Restore the outer target and reopen a new encoder with load-preserve.
  impl_->target = frame.savedTarget;
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->importTarget(frame.savedTarget), impl_->targetExtent(), *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*newEncoder);
  newEncoder->setLoadPreserve();
  impl_->encoder = std::move(newEncoder);
  impl_->updateEncoderScissor();

  // Lift the raw mask-bounds rect into device-pixel space using the
  // transform captured at pushMask time. This handles `maskUnits ==
  // userSpaceOnUse` (where bounds are in user space and need the
  // element's world transform applied) and `objectBoundingBox`
  // (where the driver has already folded the bbox mapping into the
  // bounds, but the outer world transform still applies).
  std::optional<Box2d> pixelMaskBounds;
  if (frame.maskBounds.has_value()) {
    pixelMaskBounds = frame.maskBoundsTransform.transformBox(*frame.maskBounds);
  }

  // Composite content through the selected luminance or alpha mask onto the outer target.
  impl_->encoder->blitFullTargetMasked(frame.contentTexture, frame.maskTexture, frame.maskType,
                                       pixelMaskBounds);

  // Defer release to endFrame - `blitFullTargetMasked` recorded
  // samples from both `contentTexture` and `maskTexture` into the
  // frame encoder.
  impl_->releaseTextureAtFrameEnd(std::move(frame.maskTexture), frame.maskDesc);
  impl_->releaseTextureAtFrameEnd(std::move(frame.contentTexture), frame.contentDesc);
}

bool RendererGeode::beginPatternTile(const Box2d& tileRect, const Transform2d& targetFromPattern) {
  if (impl_->rejectedFilterDepth != 0 || !impl_->device || !impl_->pipeline) {
    return false;
  }

  const Transform2d deviceFromPattern = targetFromPattern * impl_->deviceFromLocalTransform;
  const std::optional<PatternTileRasterMetrics> rasterMetrics =
      ComputePatternTileRasterMetrics(tileRect, deviceFromPattern);
  if (!rasterMetrics.has_value()) {
    return false;
  }

  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  const int tilePixelWidth = rasterMetrics->pixelWidth;
  const int tilePixelHeight = rasterMetrics->pixelHeight;

  // Pattern tile target sampled by the Slug fill shader when used as paint.
  wgpu::TextureDescriptor td = {};
  td.label = wgpuLabel("RendererGeodePatternTile");
  td.size = {static_cast<uint32_t>(tilePixelWidth), static_cast<uint32_t>(tilePixelHeight), 1u};
  td.format = impl_->textureFormat;
  td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
             wgpu::TextureUsage::CopySrc;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  td.dimension = wgpu::TextureDimension::_2D;
  if (!impl_->reserveTextureSurface(gpu::Extent2d{static_cast<uint32_t>(tilePixelWidth),
                                                  static_cast<uint32_t>(tilePixelHeight)})) {
    return false;
  }
  geode::ScopedWgpuHandle<wgpu::Texture> tileTexture(impl_->device->device().createTexture(td));
  impl_->device->countTexture();
  if (!tileTexture) {
    return false;
  }

  const wgpu::Texture tileTextureHandle = tileTexture.get();

  // Stash the currently-active encoder/target/transform state. A nested
  // encoder can't share a render pass with the outer one, so we finish any
  // pending outer work below. `GeoEncoder` submits its command buffer on
  // `finish()`, so finishing the outer encoder here commits its queued
  // draws; we then create a fresh outer encoder on pop (see
  // `endPatternTile`).
  //
  // An alternate design would share a single command encoder and use two
  // render passes, but `GeoEncoder` owns its command encoder and render
  // pass together. Splitting into two submissions keeps the `GeoEncoder`
  // API unchanged and still orders correctly because each submission is
  // serialised on the same queue.
  Impl::PatternStackFrame frame;
  if (impl_->encoder) {
    impl_->encoder->finish();
  }
  frame.savedEncoder = std::move(impl_->encoder);
  frame.savedTarget = impl_->target;
  frame.savedDeviceFromLocalTransform = impl_->deviceFromLocalTransform;
  frame.savedDeviceFromLocalTransformStack = std::move(impl_->deviceFromLocalTransformStack);
  frame.savedPixelWidth = impl_->pixelWidth;
  frame.savedPixelHeight = impl_->pixelHeight;
  // The tile rasterises in its own coordinate space; the outer clip/scissor
  // (e.g. the active filter-region scissor) must not bleed into it. Save and
  // clear the clip stack so the tile encoder starts unclipped; endPatternTile
  // restores it before the outer fill that samples the tile.
  frame.savedClipStack = std::move(impl_->clipStack);
  impl_->clipStack.clear();
  // Tile-content draws must not consume the outer element's pending pattern slots (a shape
  // inside the tile would otherwise pick them up as its own fill/stroke and release them).
  frame.savedPatternFillPaint = std::move(impl_->patternFillPaint);
  frame.savedPatternStrokePaint = std::move(impl_->patternStrokePaint);
  impl_->patternFillPaint.reset();
  impl_->patternStrokePaint.reset();
  frame.tileRect = tileRect;
  frame.targetFromPattern = targetFromPattern;
  frame.tileTexture = std::move(tileTexture);
  frame.tilePixelWidth = tilePixelWidth;
  frame.tilePixelHeight = tilePixelHeight;
  // Map pattern-tile units onto tile-texture pixels: this factor is applied
  // to every `setTransform` call while this frame is on the stack so the
  // encoder's pixelWidth/pixelHeight-based MVP renders the tile at its
  // native resolution.
  frame.rasterScale = rasterMetrics->rasterFromPatternScale;
  impl_->patternStack.push_back(std::move(frame));

  // Redirect all subsequent draw calls into the new tile texture. The new
  // encoder uses a coordinate system where path-space (0,0)..(tileRect.w,
  // tileRect.h) maps to (0,0)..(tilePixelWidth, tilePixelHeight) - i.e.,
  // the tile is rasterised at its native pixel resolution.
  //
  // The driver calls `setTransform` on the renderer before issuing draws
  // inside the tile subtree, so we don't need to preserve the outer
  // transform here. But we do need `pixelWidth/pixelHeight` to match the
  // tile texture so the new encoder's MVP maps correctly.
  impl_->pixelWidth = tilePixelWidth;
  impl_->pixelHeight = tilePixelHeight;
  impl_->target = tileTextureHandle;
  impl_->deviceFromLocalTransformStack.clear();
  // Initialise the current transform to the raster scale so direct draws
  // issued before the driver's next `setTransform` still land in the
  // correct place on the tile texture.
  const Vector2d& rasterScale = impl_->patternStack.back().rasterScale;
  impl_->deviceFromLocalTransform = Transform2d::Scale(rasterScale.x, rasterScale.y);

  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->importTexture(tileTextureHandle, td), Impl::extentOf(tileTextureHandle),
      *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*newEncoder, /*collectGeometry=*/false);
  // Transparent clear so unpainted tile pixels contribute nothing.
  newEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->encoder = std::move(newEncoder);
  // Apply the (now-empty) clip stack so the fresh tile encoder has no scissor.
  impl_->updateEncoderScissor();

  // endPatternTile composes the logical pattern transform with this raster scale.
  return true;
}

void RendererGeode::endPatternTile(bool forStroke) {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  if (impl_->patternStack.empty()) {
    return;
  }

  Impl::PatternStackFrame frame = std::move(impl_->patternStack.back());
  impl_->patternStack.pop_back();

  // Finish the tile's render pass so the texture contents are available
  // for sampling by subsequent draws.
  if (impl_->encoder) {
    impl_->retireActiveEncoder();
  }
  impl_->retireFinishedEncoder(std::move(frame.savedEncoder));
  // Restore outer state.
  impl_->target = frame.savedTarget;
  impl_->pixelWidth = frame.savedPixelWidth;
  impl_->pixelHeight = frame.savedPixelHeight;
  impl_->deviceFromLocalTransform = frame.savedDeviceFromLocalTransform;
  impl_->deviceFromLocalTransformStack = std::move(frame.savedDeviceFromLocalTransformStack);
  // Restore the outer clip stack (saved/cleared in beginPatternTile) so the
  // upcoming fill that samples this tile is scissored to the outer clip /
  // filter region again.
  impl_->clipStack = std::move(frame.savedClipStack);

  // Create a fresh encoder for the outer target. The old outer encoder was
  // finished in `beginPatternTile`; the new one loads the current contents
  // of the target (no clear), since we don't want to wipe previously-drawn
  // content. `GeoEncoder` defaults to clearing unless `hasExplicitClear` is
  // false and no draws are issued - but its render pass *always* uses
  // LoadOp::Clear with a transparent clearColor by default. That would
  // wipe the outer target on the next draw. Work around this by using the
  // saved encoder path: we can't directly reuse the saved encoder because
  // it was finished; instead, manually load the previous contents via a
  // separate copy pass is overkill for the MVP.
  //
  // Instead we issue a `CopyTextureToTexture` before the new encoder's
  // first draw to preserve the outer target's contents... actually an
  // even simpler fix is to have the new encoder skip its default clear by
  // explicitly calling `clear()` - but clear() sets the clearColor which
  // still wipes the target.
  //
  // Simpler: run the pre-existing content through. We allocate a *scratch*
  // intermediate target, copy old contents in, create the new encoder on
  // the scratch, then merge... that's a lot of work.
  //
  // Practical fix: change the render pass load op to Load instead of Clear
  // when there's been at least one prior submission. This requires
  // extending GeoEncoder; see the `reopen` helper added below.
  if (impl_->device && impl_->pipeline && impl_->gradientPipeline && impl_->imagePipeline &&
      frame.savedTarget) {
    auto newEncoder = std::make_unique<geode::GeoEncoder>(
        *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
        impl_->importTarget(frame.savedTarget), impl_->targetExtent(), *impl_->frameGpuEncoder);
    impl_->configurePathEncoder(*newEncoder);
    // Preserve existing target contents: the pattern subtree may have
    // submitted work on the outer target *before* the pattern tile opened
    // (via the finish() in beginPatternTile), so we must not clear it
    // again. GeoEncoder's constructor defaults to LoadOp::Clear with a
    // transparent clearValue; call setLoadPreserve() to switch it to
    // LoadOp::Load for the next render pass.
    newEncoder->setLoadPreserve();
    impl_->encoder = std::move(newEncoder);
    // Re-apply the active clip stack to the freshly-created encoder.
    // The scissor state lived on the OLD encoder (finished inside
    // beginPatternTile) and doesn't carry over automatically - without
    // this call, a pattern capture triggered from inside a viewport
    // or `<use>` clip would leak the subsequent pattern fill outside
    // that clip rect. Mirrors the scissor restore in push/
    // popIsolatedLayer.
    impl_->updateEncoderScissor();
  } else {
    impl_->encoder.reset();
  }

  // Stash the completed tile in raster-pixel space, matching the texture sampled by the shader.
  Impl::PatternPaintSlot slot;
  slot.tile = std::move(frame.tileTexture);
  slot.tileHandle =
      &impl_->importTexture(slot.tile.get(), impl_->textureFormat,
                            wgpu::TextureUsage::RenderAttachment |
                                wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc);
  slot.rasterTileSize = Vector2d(frame.tilePixelWidth, frame.tilePixelHeight);
  slot.targetFromRaster = TargetFromPatternRaster(frame.targetFromPattern, frame.rasterScale);

  // Restore the pattern slots pending at `beginPatternTile`, then overwrite the slot this tile
  // was recorded for (releasing any stale tile texture it held).
  impl_->patternFillPaint = std::move(frame.savedPatternFillPaint);
  impl_->patternStrokePaint = std::move(frame.savedPatternStrokePaint);
  std::optional<Impl::PatternPaintSlot>& targetSlot =
      forStroke ? impl_->patternStrokePaint : impl_->patternFillPaint;
  if (targetSlot.has_value() && targetSlot->tile) {
    impl_->device->deferDestroy(targetSlot->tile.take());
  }
  targetSlot = std::move(slot);
}

void RendererGeode::setPaint(const PaintParams& paint) {
  impl_->paint = paint;
}

void RendererGeode::drawPath(const PathShape& path, const StrokeParams& stroke) {
  // No-op mode guard (matches drawImage / drawText): with no device or no
  // active frame encoder, skip cleanly instead of dereferencing a null
  // device via getFillEncode() -> GeodeDevice::countPathEncode().
  if (!impl_->device || !impl_->encoder) {
    return;
  }

  // `<use>`-batch detection: when a `<use>`
  // draws a path that was also just drawn by the previous call -
  // same source entity, same paint - this is exactly the case an
  // instancing pass would collapse into one GPU draw. Count it here
  // so the benefit of a future batcher is measurable before the
  // batcher ships. Null source (non-driver callers) never matches,
  // so editor overlay / convenience `drawRect` calls don't skew the
  // counter.
  if (path.sourceEntity.entity() != entt::null &&
      path.sourceEntity.entity() == impl_->lastDrawSourceEntity) {
    ++impl_->counters.sameSourceDrawPairs;
  }
  impl_->lastDrawSourceEntity = path.sourceEntity.entity();

  // `PathShape` borrows its geometry; resolve it once here. `pathOrEmpty` substitutes a
  // shared empty path for a null pointer, and that substitute has static storage, so the
  // reference stays valid for as long as a driver-supplied path would (this matters for the
  // pending batch, which retains the address past the end of this call).
  const Path& drawPathGeometry = path.pathOrEmpty();

  // Path-encode cache lookup for the fill. Null `sourceEntity` (editor
  // overlay, test-harness direct draws) returns nullptr and `GeoEncoder`
  // falls back to the inline encode path.
  const Impl::AdmittedEncode fillAdmission =
      impl_->getFillEncodeForPaint(path.sourceEntity, drawPathGeometry, path.fillRule);
  const geode::EncodedPath* fillEncoded = fillAdmission.encoded;

  // Try to append to a pending batch. Preconditions:
  //  - Source entity valid (non-null handle).
  //  - Fill encode cache hit (shared across all instances).
  //  - Solid paint (gradient / pattern need per-draw uniforms today).
  //  - No active pattern-fill handoff from the driver.
  //
  // When all hold: append this draw's `deviceFromLocalTransform` into the pending
  // batch (flushing + restarting if the paint/source key differs). A later
  // state change (pushClip, popLayer, endFrame, setPaint-different-key,
  // non-batchable draw) flushes as one instanced draw.
  //
  // Under ordered cross-entity batching, a stroke on the same element is NOT
  // a blocker. A stroked outline is itself a solid fill of a separately
  // cached encode, so it is offered to the batcher immediately after the fill
  // and lands as the next instance - the same painter order a fill-then-stroke
  // element requires. That matters a lot on stroke-heavy documents: excluding
  // stroked elements ends the run at every one of them, so a document that
  // alternates fill and stroke cannot batch anything at all.
  //
  // Without ordered batching the only batch a stroked element's fill could
  // join is the same-entity instanced one, which draws from the per-frame
  // arena and has no residence. Merging two otherwise-resident solo fills
  // into it trades a draw call for a full geometry re-upload every frame, so
  // that configuration keeps the original exclusion.
  const bool hasStroke = !(stroke.strokeWidth <= 0.0 ||
                           std::holds_alternative<PaintServer::None>(impl_->paint.stroke));
  const bool solidFill = std::holds_alternative<PaintServer::Solid>(impl_->paint.fill);
  const bool referenceFill =
      std::holds_alternative<components::PaintResolvedReference>(impl_->paint.fill);
  const bool fillShapeBatchable = fillAdmission.persistent &&
                                  path.sourceEntity.entity() != entt::null &&
                                  !impl_->patternFillPaint.has_value() &&
                                  impl_->encoder != nullptr && impl_->paint.drawFillComponent;

  // A gradient fill can batch too: the record carries its stop-ramp reference,
  // gradient transform and spread, so a run that alternates solid and gradient
  // paint collapses into one draw instead of one draw per shape plus a
  // pipeline switch at every boundary. The parameters are resolved here rather
  // than at emit time because the batcher needs them before it can decide.
  std::optional<geode::LinearGradientParams> batchLinear;
  std::optional<geode::RadialGradientParams> batchRadial;
  //
  // The state-driven reasons a gradient would be declined are cheap to test up
  // front, and testing them here keeps the ordinary emit path from re-resolving
  // a gradient the batcher just discarded. The remaining decline is a
  // same-frame repeat of one entity, which needs the residence slot to detect
  // and is rare enough not to be worth fetching one for.
  if (kEnableSceneBatching && referenceFill && fillShapeBatchable &&
      !impl_->encoder->hasActiveClipState() && !impl_->encoder->hasOpenMaskPass()) {
    impl_->resolveBatchGradient(drawPathGeometry, impl_->paint.fill, impl_->paint.fillOpacity,
                                batchLinear, batchRadial);
  }
  const bool gradientFillBatchable = batchLinear.has_value() || batchRadial.has_value();

  const bool fillBatchable = (kEnableSceneBatching || !hasStroke) && fillShapeBatchable &&
                             (solidFill || gradientFillBatchable);

  // GPU residence: a persistent per-entity fill slot, eligible for
  // any solid fill with a cached encode. Shared by the batch size-1 flush,
  // the cross-entity scene batch and the direct `fillResolved` path, so a
  // solid fill re-uploads zero geometry on an unchanged frame however it
  // ends up being drawn.
  geode::GeodeResidentSlot* residentFill =
      (fillAdmission.persistent && (solidFill || gradientFillBatchable))
          ? impl_->residentFillSlot(path.sourceEntity)
          : nullptr;

  // Gradient residence: a persistent per-entity gradient slot for
  // gradient-painted fills with a cached encode. Pattern references also
  // pass through here and simply never use the slot (the resident
  // gradient methods only run for resolved gradients).
  geode::GeodeResidentGradientSlot* residentGradientFill =
      (fillAdmission.persistent && referenceFill)
          ? impl_->residentGradientFillSlot(path.sourceEntity)
          : nullptr;

  bool fillAbsorbed = false;
  if (fillBatchable) {
    geode::GeoEncoder::ScenePaint scenePaint;
    if (solidFill) {
      const auto& solid = std::get<PaintServer::Solid>(impl_->paint.fill);
      scenePaint.color = solid.color.resolve(impl_->paint.currentColor.rgba(),
                                             static_cast<float>(impl_->paint.fillOpacity));
    } else {
      scenePaint.linearGradient = batchLinear.has_value() ? &*batchLinear : nullptr;
      scenePaint.radialGradient = batchRadial.has_value() ? &*batchRadial : nullptr;
    }
    fillAbsorbed = impl_->tryAppendOrStartBatch(
        path.sourceEntity.registry(), path.sourceEntity.entity(), drawPathGeometry, scenePaint,
        path.fillRule, fillEncoded, residentFill, /*allowInstancedAppend=*/!hasStroke);
  }
  if (fillAbsorbed) {
    if (!hasStroke) {
      return;
    }
    // Fall through so the stroke can join the same batch. Every path below
    // that emits directly flushes first, which drains this fill ahead of the
    // stroke and preserves painter order.
  } else {
    // The fill did not join a batch, either because it could never batch or
    // because a gradient failed the ordered-path gates (it declines rather
    // than falling back to a paint form the singleton flush cannot express).
    // Flush whatever is pending, then emit normally.
    impl_->flushPendingBatch();
    // Honor the driver's paint-order component switch: when paint-order issues a
    // per-component pass (RendererInterface PaintParams::drawFillComponent), skip
    // the fill on stroke-only passes so Geode reorders rather than double-paints.
    // Both default to true, so this is a no-op for ordinary fill-then-stroke draws.
    if (impl_->paint.drawFillComponent) {
      impl_->fillResolved(drawPathGeometry, path.fillRule, fillEncoded, residentFill,
                          residentGradientFill);
    }
  }

  // Mirror fillResolved's no-op safety: if there's no encoder (headless
  // device init failed, zero-pixel viewport, or draw-before-beginFrame),
  // the stroke branch must bail too. Otherwise the encoder dereference
  // below crashes.
  if (!impl_->encoder) {
    return;
  }

  if (!impl_->paint.drawStrokeComponent || stroke.strokeWidth <= 0.0 ||
      std::holds_alternative<PaintServer::None>(impl_->paint.stroke)) {
    return;
  }

  // Dash support is wired through `Path::strokeToFill`.
  // `toStrokeStyle` plumbs `dashArray`/`dashOffset`/`pathLength` from the
  // SVG stroke params into the stroke style; `strokeToFill` walks each
  // subpath, splits it at the dash on/off transitions, and emits one
  // capped ribbon per on-segment.
  //
  // Expand the stroked outline into a filled path and reuse the Slug fill /
  // gradient-fill / pattern pipeline. `strokeToFill` handles flattening,
  // cap/join generation, and miter-limit fallback to bevel internally.
  //
  // Fill rule: for closed subpaths, strokeToFill emits the outer and
  // inner contours as two *same-winding* closed subpaths (not opposite),
  // so NonZero would over-fill the interior and EvenOdd is required to
  // get a hollow ring. For open subpaths, `strokeToFill` emits one
  // closed polygon - but with overlapping start/end caps (e.g. the
  // resvg `stroke-linecap/open-path-with-*` tests where the 4-point path
  // `M 150 50 l 0 80 -100 -40 100 -40` ends at its start), the inside-
  // miter shortcut in `emitJoin` creates a self-intersecting polygon
  // whose interior has the wrong winding under EvenOdd (the first-
  // segment rectangle drops out). NonZero handles that case correctly
  // because the overlapping winding still sums to non-zero.
  //
  // The cache (`GeodePathCacheComponent::strokeSlot`) memoizes the
  // `strokeToFill` output + its encode + the derived fill rule, keyed
  // by `StrokeStyle` equality. A cache hit skips all three computations.
  const StrokeStyle strokeStyle = toStrokeStyle(stroke);
  const Impl::StrokeDerived strokeDerived =
      impl_->getStrokeDerived(path.sourceEntity, drawPathGeometry, strokeStyle);
  if (!strokeDerived.strokedPath) {
    return;
  }
  const Path& strokedOutline = *strokeDerived.strokedPath;

  // Pattern dispatch comes first: a stroke pattern slot was populated by the
  // driver via `endPatternTile(forStroke=true)` and consumed here.
  if (impl_->patternStrokePaint.has_value()) {
    // This element's fill may still be sitting in the pending batch; drain it
    // so the pattern stroke lands on top of it rather than under it.
    impl_->flushPendingBatch();
    impl_->syncTransform();
    const double opacity = impl_->paint.strokeOpacity;
    impl_->encoder->fillPathPattern(strokedOutline, strokeDerived.fillRule,
                                    impl_->buildPatternPaint(*impl_->patternStrokePaint, opacity),
                                    strokeDerived.encoded);
    if (impl_->patternStrokePaint->tile) {
      impl_->device->deferDestroy(impl_->patternStrokePaint->tile.take());
    }
    impl_->patternStrokePaint.reset();
    return;
  }

  // Otherwise dispatch through the shared painted-path routine so stroke
  // gradients get the same handling as fill gradients (including the
  // gradient-unit objectBoundingBox transform, which is relative to the
  // *original* path bounds - we deliberately do NOT use
  // `strokedOutline.bounds()` here).
  const double effectiveOpacity = impl_->paint.strokeOpacity;
  auto strokeServer = impl_->paint.stroke;
  // GPU residence for solid strokes (the stroked outline is a solid
  // fill of the cached stroke encode).
  geode::GeodeResidentSlot* residentStroke =
      (strokeDerived.persistent && std::holds_alternative<PaintServer::Solid>(strokeServer))
          ? impl_->residentStrokeSlot(path.sourceEntity)
          : nullptr;
  // Gradient residence for gradient-painted strokes: the cached
  // stroke-outline encode lives in a gradient slot so an unchanged outline
  // with an unchanged resolved gradient re-uploads zero geometry.
  geode::GeodeResidentGradientSlot* residentGradientStroke =
      (strokeDerived.persistent &&
       std::holds_alternative<components::PaintResolvedReference>(strokeServer))
          ? impl_->residentGradientStrokeSlot(path.sourceEntity)
          : nullptr;

  // A solid stroke is a solid fill of the cached stroke encode, so it is
  // offered to the batcher on the same terms as the fill above. Landing here
  // right after this element's own fill is what lets a run of stroked
  // elements collapse: the outline becomes the next instance instead of
  // ending the run.
  //
  // The stroked outline is retained by the batch until it flushes. That is
  // safe for the same reason the fill geometry is: the outline lives in this
  // entity's `GeodePathCacheComponent::strokeSlot`, which is only replaced by
  // a `getStrokeDerived` miss on this same entity, and the pending batch is
  // always flushed within the frame that started it.
  if (kEnableSceneBatching && residentStroke != nullptr && strokeDerived.persistent &&
      path.sourceEntity.entity() != entt::null && !impl_->patternStrokePaint.has_value()) {
    const auto& solidStroke = std::get<PaintServer::Solid>(strokeServer);
    geode::GeoEncoder::ScenePaint strokePaint;
    strokePaint.color = solidStroke.color.resolve(impl_->paint.currentColor.rgba(),
                                                  static_cast<float>(effectiveOpacity));
    if (impl_->tryAppendOrStartBatch(path.sourceEntity.registry(), path.sourceEntity.entity(),
                                     strokedOutline, strokePaint, strokeDerived.fillRule,
                                     strokeDerived.encoded, residentStroke,
                                     /*allowInstancedAppend=*/false)) {
      return;
    }
  }

  // Non-batchable stroke: drain this element's pending fill first so the
  // outline paints over it.
  impl_->flushPendingBatch();
  impl_->drawPaintedPathAgainst(drawPathGeometry, strokedOutline, strokeServer, effectiveOpacity,
                                strokeDerived.fillRule, strokeDerived.encoded, residentStroke,
                                residentGradientStroke);
}

void RendererGeode::drawRect(const Box2d& rect, const StrokeParams& stroke) {
  const Path path = PathBuilder().addRect(rect).build();
  drawPath(PathShape{&path, FillRule::NonZero}, stroke);
}

void RendererGeode::drawEllipse(const Box2d& bounds, const StrokeParams& stroke) {
  const Path path = PathBuilder().addEllipse(bounds).build();
  drawPath(PathShape{&path, FillRule::NonZero}, stroke);
}

void RendererGeode::drawImage(const ImageResource& image, const ImageParams& params) {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  if (!impl_->encoder) {
    return;
  }
  // The element's own `opacity` is handled by `pushIsolatedLayer` in the
  // driver before this call lands, so we do NOT multiply it back in here
  // (doing so would double-apply the group opacity, producing opacity²).
  // Match RendererTinySkia's behavior in `drawImage`: use `params.opacity`
  // (which is the image-specific opacity component) without the
  // paint.opacity factor.
  const double combinedOpacity = params.opacity;
  if (combinedOpacity <= 0.0) {
    return;
  }
  impl_->syncTransform();
  ImageRendering imageRendering = params.imageRendering;
  if (imageRendering == ImageRendering::Auto && params.imageRenderingPixelated) {
    imageRendering = ImageRendering::Pixelated;
  }
  impl_->encoder->drawImage(image, params.targetRect, combinedOpacity, imageRendering);
}

bool RendererGeode::drawTextureSnapshot(const RendererTextureSnapshot& texture,
                                        const Box2d& targetRect, double opacity, bool pixelated) {
  impl_->flushPendingBatch();
  if (!impl_->encoder || opacity <= 0.0) {
    return false;
  }

  if (texture.backend() != RendererTextureSnapshotBackend::Geode) {
    return false;
  }
  const auto* geodeTexture = static_cast<const RendererGeodeTextureSnapshot*>(&texture);
  if (geodeTexture == nullptr || !geodeTexture->texture()) {
    return false;
  }

  // A snapshot may report a content extent smaller than its backing texture (host uploads
  // keep an oversized allocation so a resized payload can reuse the texture). Sample only
  // that region, otherwise the unused remainder is squeezed into `targetRect` and every
  // tile edge lands in the wrong place.
  const Vector2i contentDimensions = geodeTexture->dimensions();
  const uint32_t textureWidth = geodeTexture->texture().getWidth();
  const uint32_t textureHeight = geodeTexture->texture().getHeight();
  if (contentDimensions.x <= 0 || contentDimensions.y <= 0 || textureWidth == 0 ||
      textureHeight == 0) {
    return false;
  }
  const Box2d sourceUv(Vector2d::Zero(),
                       Vector2d(std::min(1.0, static_cast<double>(contentDimensions.x) /
                                                  static_cast<double>(textureWidth)),
                                std::min(1.0, static_cast<double>(contentDimensions.y) /
                                                  static_cast<double>(textureHeight))));

  impl_->syncTransform();
  impl_->encoder->drawTexture(
      impl_->importTexture(geodeTexture->texture(), geodeTexture->format(),
                           wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst),
      targetRect, sourceUv, opacity, pixelated,
      geodeTexture->alphaType() == AlphaType::Premultiplied);
  return true;
}

void RendererGeode::drawText(Registry& registry, const components::ComputedTextComponent& text,
                             const TextParams& params) {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
#ifdef DONNER_TEXT_ENABLED
  if (!impl_->device || !impl_->encoder || impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    return;
  }
  if (!registry.ctx().contains<TextEngine>()) {
    if (impl_->verbose && !impl_->warnedText) {
      std::cerr << "RendererGeode: TextEngine not available in registry context\n";
      impl_->warnedText = true;
    }
    return;
  }

  auto& textEngine = registry.ctx().get<TextEngine>();
  auto& fontManager = registry.ctx().get<FontManager>();

  // Use cached layout runs from `ComputedTextGeometryComponent` when
  // available; otherwise lay out fresh via the engine. This matches
  // the pattern in `RendererTinySkia::drawText`.
  std::vector<TextRun> runs;
  if (params.textRootEntity != entt::null) {
    if (const auto* cache =
            registry.try_get<components::ComputedTextGeometryComponent>(params.textRootEntity)) {
      runs = cache->runs;
    }
  }
  if (runs.empty()) {
    const TextLayoutParams layoutParams = toTextLayoutParams(params);
    runs = textEngine.layout(text, layoutParams);
  }

  impl_->admitTextRuns(runs);

  const float textFontSizePx = static_cast<float>(
      params.fontSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));

  // Text bounding box for `objectBoundingBox` gradient/pattern paint. A tspan
  // has no bbox, so span gradient/pattern paint maps through this element-level
  // box - same computation as `RendererTinySkia::drawText` (shared helper). The
  // bbox is passed to `drawPaintedPathAgainst` as the gradient *geometry* path
  // while the glyph outline is the *draw* path.
  const Box2d textBounds = ComputeTextBounds(textEngine, runs, text.spans, params.viewBox,
                                             params.fontMetrics, textFontSizePx);
  const Path textBoundsPath =
      textBounds.isEmpty() ? Path() : PathBuilder().addRect(textBounds).build();

  // Element-level pattern fill: the driver stages a `patternFillPaint` slot
  // (via renderPattern / endPatternTile) for a `<text fill="url(#pattern)">`,
  // to be consumed by the next fill. Mirror `RendererTinySkia::drawText`, which
  // fills the glyph outlines with the pattern (clipped to the glyphs). We must
  // also reset the slot once below so it does not leak onto the next shape's
  // `fillResolved` -- the bug this fixes was an unfilled glyph + the staged
  // pattern bleeding onto the following element. Per-span pattern fill is a
  // separate gap (matching the element-level handling here).
  const bool hasPatternFill = impl_->patternFillPaint.has_value();

  // Resolve a default fill colour from the text-element-level paint
  // state. Per-span fills override below when present.
  std::optional<css::RGBA> defaultFill = impl_->resolveSolidFill();
  if (!defaultFill.has_value()) {
    // No solid fill and no fallback -- text is effectively invisible.
    // Still walk through in case a per-span fill kicks in.
    defaultFill = css::RGBA(0, 0, 0, 0);
  }

  const css::RGBA currentColor = impl_->paint.currentColor.rgba();

  // The element-level fill is already scaled by the top-level
  // `setPaint` call, but per-span fill overrides need their own
  // `span.fillOpacity * span.opacity` applied.
  const auto resolveSpanFill = [&](size_t runIndex) -> css::RGBA {
    if (runIndex >= text.spans.size()) {
      return *defaultFill;
    }
    const auto& span = text.spans[runIndex];
    const float opacityScale = static_cast<float>(span.fillOpacity * span.opacity);
    if (const auto* solid = std::get_if<PaintServer::Solid>(&span.resolvedFill)) {
      return solid->color.resolve(currentColor, opacityScale);
    }
    if (const auto* ref = std::get_if<components::PaintResolvedReference>(&span.resolvedFill)) {
      if (ref->fallback.has_value()) {
        return ref->fallback->resolve(currentColor, opacityScale);
      }
    }
    return *defaultFill;
  };

  // Snapshot the encoder's current transform so we can restore it if
  // per-glyph rotations mess with it. `fillPath` honours
  // `impl_->deviceFromLocalTransform` via `setTransform`, and the glyph
  // outline coordinates are already mapped into the text element's
  // local space by the transformPath call below -- so we want the
  // encoder to use the element's deviceFromLocalTransform unchanged.
  impl_->encoder->setTransform(impl_->deviceFromLocalTransform);

  // Per-occurrence record storage for this element's solid-fill glyphs. The
  // ordinal advances across runs, so the element's persistent slots stay in
  // one consecutive range and a batch can cover the whole element.
  Impl::TextRecordCursor textRecords = impl_->beginTextRecords(registry, params.textRootEntity);

  for (size_t runIndex = 0; runIndex < runs.size(); ++runIndex) {
    const auto& run = runs[runIndex];
    if (run.font == FontHandle()) {
      continue;
    }

    float spanFontSizePx = textFontSizePx;
    if (runIndex < text.spans.size() && text.spans[runIndex].fontSize.value != 0.0) {
      spanFontSizePx = static_cast<float>(text.spans[runIndex].fontSize.toPixels(
          params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed));
    }

    const float scale = textEngine.scaleForPixelHeight(run.font, spanFontSizePx);
    if (scale <= 0.0f) {
      continue;
    }
    if (textEngine.isBitmapOnly(run.font)) {
      // Bitmap-only (color emoji) fonts need the `GeodeTextureEncoder`
      // path, which drawText doesn't wire up yet. Skip the run so the
      // rest of the text still renders.
      continue;
    }

    const css::RGBA spanFill = resolveSpanFill(runIndex);
    const bool hasFill = spanFill.a != 0;

    // Effective per-run fill paint *server*: a span's own non-None fill overrides
    // the element-level fill. This carries gradient refs that the solid
    // `resolveSpanFill` collapses (it returns the inherited default for a ref) -
    // `drawPaintedPathAgainst` resolves them against the text bbox below (mirrors
    // `RendererTinySkia::drawText`). Patterns are NOT routed here: the driver
    // stages a `patternFillPaint` slot for a pattern fill, so `hasPatternFill`
    // distinguishes pattern from gradient (both are `PaintResolvedReference`).
    // A span's own non-None fill overrides the element-level fill. Track whether
    // the chosen server came from the span (an override) vs. was inherited from
    // the element: a span-level gradient override must beat the element pattern,
    // which the driver only staged for the element. Mirrors
    // `RendererTinySkia::drawText`, where a span ref that instantiates as a
    // gradient wins unconditionally and only falls to `patternFillPaint_` when
    // it is not a gradient.
    const bool spanOverridesFill =
        runIndex < text.spans.size() &&
        !std::holds_alternative<PaintServer::None>(text.spans[runIndex].resolvedFill);
    const components::ResolvedPaintServer& fillServer =
        spanOverridesFill ? text.spans[runIndex].resolvedFill : impl_->paint.fill;
    // The fill is a gradient when (a) the span overrode with its own gradient
    // ref - wins even if the element has a pattern, or (b) the element-level fill
    // is a gradient ref and no element pattern was staged.
    const bool fillIsGradient =
        !textBoundsPath.empty() &&
        (spanOverridesFill
             ? paintReferenceIsGradient(fillServer)
             : (std::holds_alternative<components::PaintResolvedReference>(fillServer) &&
                !hasPatternFill));
    const double fillOpacity = runIndex < text.spans.size()
                                   ? text.spans[runIndex].fillOpacity * text.spans[runIndex].opacity
                                   : impl_->paint.fillOpacity;

    // A run inherits the element-level pattern fill when it has neither a solid
    // fill nor a gradient of its own. `resolveSpanFill` returns alpha=0 for a
    // ref, so `!hasFill` marks "no solid"; `!fillIsGradient` ensures a span's own
    // gradient is drawn as a gradient (above) rather than the inherited pattern.
    const bool usePatternFill = hasPatternFill && !hasFill && !fillIsGradient;

    // Resolve per-run stroke. A per-span stroke overrides the element-level
    // stroke. Solid is taken directly; gradient/pattern strokes route through
    // the server below. Mirrors `RendererTinySkia::drawText`.
    std::optional<css::RGBA> strokeColor;
    std::optional<StrokeStyle> strokeStyle;
    const components::ResolvedPaintServer* strokeServer = &impl_->paint.stroke;
    double strokeOpacity = impl_->paint.strokeOpacity;
    bool spanOverridesStroke = false;
    {
      StrokeParams runStrokeParams = params.strokeParams;
      std::optional<css::RGBA> color = impl_->resolveSolidStroke();
      if (runIndex < text.spans.size()) {
        const auto& span = text.spans[runIndex];
        if (span.strokeWidth > 0.0) {
          runStrokeParams.strokeWidth = span.strokeWidth;
        }
        if (!std::holds_alternative<PaintServer::None>(span.resolvedStroke)) {
          spanOverridesStroke = true;
          strokeServer = &span.resolvedStroke;
          strokeOpacity = span.strokeOpacity * span.opacity;
          color = impl_->resolveSolidPaint(span.resolvedStroke, span.strokeOpacity * span.opacity);
        }
      }
      if (runStrokeParams.strokeWidth > 0.0) {
        // Solid stroke colour available → draw flat. Otherwise, a gradient/
        // pattern stroke server drives the paint below.
        if (color.has_value() && color->a != 0) {
          strokeColor = color;
        }
        const bool strokeIsServer =
            std::holds_alternative<components::PaintResolvedReference>(*strokeServer);
        if (strokeColor.has_value() || strokeIsServer || impl_->patternStrokePaint.has_value()) {
          strokeStyle = toStrokeStyle(runStrokeParams);
        }
      }
    }
    // A stroke is a gradient when (a) the span overrode with its own gradient ref
    // - wins even if the element has a pattern stroke, or (b) the element-level
    // stroke is a gradient ref and no element pattern stroke was staged. Pattern
    // stroke stages a `patternStrokePaint` slot (driver), distinguishing it from
    // a gradient stroke (both `PaintResolvedReference`). Mirrors the fill
    // precedence above and `RendererTinySkia::drawText`.
    const bool strokeIsGradient =
        strokeStyle.has_value() && !strokeColor.has_value() && !textBoundsPath.empty() &&
        (spanOverridesStroke
             ? paintReferenceIsGradient(*strokeServer)
             : (std::holds_alternative<components::PaintResolvedReference>(*strokeServer) &&
                !impl_->patternStrokePaint.has_value()));
    // Inherit the element-level pattern stroke only when the span did not provide
    // its own gradient stroke (a span's own gradient is drawn above).
    const bool usePatternStroke = strokeStyle.has_value() && !strokeColor.has_value() &&
                                  !strokeIsGradient && impl_->patternStrokePaint.has_value();
    const bool hasStrokePaint = strokeColor.has_value() || strokeIsGradient || usePatternStroke;

    const TextDecoration spanDecoration =
        runIndex < text.spans.size() ? text.spans[runIndex].textDecoration : params.textDecoration;
    if (!hasFill && !usePatternFill && !fillIsGradient && !hasStrokePaint &&
        spanDecoration == TextDecoration::None) {
      continue;  // Nothing to fill, stroke, or decorate.
    }

    // A solid fill draws each glyph from shared resident geometry: the outline
    // is cached once per glyph identity and the occurrence contributes only a
    // placement transform. Gradient, pattern, and stroke paint still consume a
    // glyph outline placed in the element's local space, so those keep
    // building placed paths.
    const bool residentGlyphFill = hasFill && !fillIsGradient && !usePatternFill;
    const bool needPlacedGlyphPaths = fillIsGradient || usePatternFill || hasStrokePaint;

    // Glyph occurrences of this run, in paint order. Parallel to
    // `runGlyphPaths` when both are populated.
    struct RunGlyphOccurrence {
      geode::GeodeGlyphResidentEntry* entry = nullptr;
      Transform2d glyphFromLocal;
    };
    std::vector<RunGlyphOccurrence> runGlyphOccurrences;
    std::vector<Path> runGlyphPaths;
    if (residentGlyphFill || needPlacedGlyphPaths) {
      runGlyphOccurrences.reserve(run.glyphs.size());
    }
    if (needPlacedGlyphPaths) {
      runGlyphPaths.reserve(run.glyphs.size());
    }
    // A run can reach here with no glyph paint at all - decoration lines only -
    // and must not then fetch, encode, and permanently cache an outline per
    // glyph for geometry nothing draws.
    const bool runNeedsGlyphGeometry = residentGlyphFill || needPlacedGlyphPaths;
    for (const auto& glyph : runNeedsGlyphGeometry ? std::span<const TextGlyph>(run.glyphs)
                                                   : std::span<const TextGlyph>()) {
      if (glyph.glyphIndex == 0) {
        continue;  // `.notdef` -- skip to match tiny-skia.
      }

      // Outline + placement are split by PlacedTextGeometry, which both
      // backends share so they cannot drift on placement. The split encodes
      // tiny-skia's order: stretch the raw outline, then `Rotate * Translate`.
      // Geode once composed `Scale * Rotate * Translate`, which stretched in
      // the post-rotation frame and diverged from tiny-skia.
      geode::GlyphGeometryKey key;
      // The font handle's entity id, versioned by entt, so a font that is
      // unloaded and a different one loaded into the same slot cannot be
      // mistaken for the original.
      key.fontId = static_cast<uint64_t>(static_cast<uint32_t>(run.font.entity()));
      key.glyphIndex = static_cast<uint32_t>(glyph.glyphIndex);
      key.outlineScale = scale * glyph.fontSizeScale;
      key.stretchScaleX = glyph.stretchScaleX;
      key.stretchScaleY = glyph.stretchScaleY;
      key.rotateDegrees = glyph.rotateDegrees;

      geode::GeodeGlyphResidentEntry* entry = impl_->residentGlyphEntry(
          registry, fontManager, run.font, glyph.glyphIndex, key, [&]() -> Path {
            return UnplacedGlyphOutline(textEngine, run.font, glyph, scale).outline;
          });
      if (entry == nullptr || entry->outline.empty()) {
        continue;  // No vector outline; bitmap-only fonts were skipped above.
      }

      const Transform2d glyphFromLocal = GlyphPlacementTransform(glyph);
      runGlyphOccurrences.push_back(RunGlyphOccurrence{entry, glyphFromLocal});
      if (needPlacedGlyphPaths) {
        runGlyphPaths.push_back(impl_->materializePlacedGlyph(entry->outline, glyphFromLocal));
      }
    }

    const auto drawRunFill = [&]() {
      if (residentGlyphFill) {
        for (const RunGlyphOccurrence& occurrence : runGlyphOccurrences) {
          // The ordinal advances even for an occurrence whose encode turns out
          // empty (a whitespace-only outline), so an element's occurrence ->
          // slot mapping stays stable across frames. That occurrence's slot is
          // allocated and then left alone: the emit returns before any record
          // is written, so the slot holds nothing and no draw reads it.
          const geode::GeodeRecordSlab::Slot* recordSlot = nullptr;
          std::vector<uint8_t>* recordCache = nullptr;
          impl_->nextTextRecord(textRecords, registry, recordSlot, recordCache);
          // `Transform2d`'s product applies its left operand first, so the
          // glyph's placement into element-local space comes first and the
          // element's device transform second. This is the same composition
          // `pushTransform` performs for a nested element.
          impl_->emitGlyphFill(*occurrence.entry, spanFill,
                               occurrence.glyphFromLocal * impl_->deviceFromLocalTransform,
                               recordSlot, recordCache);
        }
        // Stroke, decoration, and the next run all emit straight through the
        // encoder without flushing, so close the batch here or they would land
        // ahead of these glyphs. The flush leaves the encoder transform at
        // identity; restore the element's.
        impl_->flushPendingBatch();
        impl_->encoder->setTransform(impl_->deviceFromLocalTransform);
        return;
      }
      for (const Path& placed : runGlyphPaths) {
        if (fillIsGradient) {
          const geode::EncodedPath* encoded =
              impl_->encodeTransientGeometry(placed, FillRule::NonZero);
          // Gradient fill on text: resolve the gradient against the text bbox
          // (the gradient *geometry*), fill the glyph outline (the *draw* path).
          impl_->drawPaintedPathAgainst(textBoundsPath, placed, fillServer, fillOpacity,
                                        FillRule::NonZero, encoded);
        } else if (usePatternFill) {
          const geode::EncodedPath* encoded =
              impl_->encodeTransientGeometry(placed, FillRule::NonZero);
          // Fill the glyph outline with the staged pattern tile, same as a
          // pattern-filled `<path>` (see Impl::fillResolved). The glyph path is
          // already in the element's local space and `deviceFromLocalTransform`
          // is set above, so `buildPatternPaint` composes the right sample space.
          impl_->syncTransform();
          impl_->encoder->fillPathPattern(
              placed, FillRule::NonZero,
              impl_->buildPatternPaint(*impl_->patternFillPaint, impl_->paint.fillOpacity),
              encoded);
        }
      }
    };
    const auto drawRunStroke = [&]() {
      for (const Path& placed : runGlyphPaths) {
        if (hasStrokePaint) {
          // Closed glyph contours expand to same-winding outer+inner subpaths, so
          // the stroked outline needs `strokeFillRuleFor` (EvenOdd for the ring),
          // not a hardcoded NonZero -- see RendererGeode::drawPath's stroke notes.
          // Same device-aware tolerance as `drawPath`'s stroke: glyph outlines
          // are submitted in text-local space and scaled on the GPU, so the
          // flattening must track the draw transform or stroked glyphs facet
          // at high zoom.
          const Path stroked = placed.strokeToFill(*strokeStyle, impl_->strokeFlattenTolerance());
          if (!stroked.empty()) {
            impl_->countStrokeOutline(stroked);
            const FillRule strokeRule = Impl::strokeFillRuleFor(stroked, *strokeStyle);
            const geode::EncodedPath* encoded = impl_->encodeTransientGeometry(stroked, strokeRule);
            if (strokeIsGradient) {
              // Gradient stroke: resolve against the text bbox (the *original*
              // geometry, per SVG), fill the stroked outline.
              impl_->drawPaintedPathAgainst(textBoundsPath, stroked, *strokeServer, strokeOpacity,
                                            strokeRule, encoded);
            } else if (usePatternStroke) {
              impl_->syncTransform();
              impl_->encoder->fillPathPattern(
                  stroked, strokeRule,
                  impl_->buildPatternPaint(*impl_->patternStrokePaint, strokeOpacity), encoded);
            } else {
              impl_->encoder->fillPath(stroked, *strokeColor, strokeRule, encoded);
            }
          }
        }
      }
    };

    PaintOrder spanPaintOrder;
    if (runIndex < text.spans.size()) {
      spanPaintOrder = text.spans[runIndex].paintOrder;
    }
    bool fillDrawn = false;
    bool strokeDrawn = false;
    for (const PaintComponent component : spanPaintOrder.order) {
      if (component == PaintComponent::Fill && !fillDrawn) {
        drawRunFill();
        fillDrawn = true;
      } else if (component == PaintComponent::Stroke && !strokeDrawn) {
        drawRunStroke();
        strokeDrawn = true;
      }
    }

    // Text-decoration lines (underline / overline / line-through). Mirrors
    // `RendererTinySkia::drawText`; solid paint only (gradient decoration paint
    // is a separate gap). Per CSS Text Decoration §3 the decoration uses the
    // declaring element's paint + font metrics.
    if (spanDecoration != TextDecoration::None && !run.glyphs.empty() &&
        runIndex < text.spans.size()) {
      const auto& span = text.spans[runIndex];

      const float decoFontSizePx =
          span.decorationFontSizePx > 0.0f ? span.decorationFontSizePx : spanFontSizePx;
      const float decoScale = textEngine.scaleForPixelHeight(run.font, decoFontSizePx);
      const float decoEmScale = textEngine.scaleForEmToPixels(run.font, decoFontSizePx);

      const FontVMetrics vmetrics = textEngine.fontVMetrics(run.font);
      const int ascent = vmetrics.ascent;
      const int descent = vmetrics.descent;

      double fontUnderlinePos = 0.0;
      double fontUnderlineThick = 0.0;
      if (auto ul = textEngine.underlineMetrics(run.font)) {
        fontUnderlinePos = ul->position;
        fontUnderlineThick = ul->thickness;
      }
      double fontStrikePos = 0.0;
      double fontStrikeThick = 0.0;
      if (auto strike = textEngine.strikeoutMetrics(run.font)) {
        fontStrikePos = strike->position;
        fontStrikeThick = strike->thickness;
      }

      const double thickness = fontUnderlineThick > 0.0
                                   ? fontUnderlineThick * decoEmScale
                                   : static_cast<double>(ascent - descent) * decoScale / 18.0;

      // Decoration fill colour (solid): declaring element's decoration fill,
      // falling back to the span fill. When the element uses a pattern fill and
      // the decoration has no explicit solid fill of its own, the decoration
      // inherits the pattern too (per CSS Text Decoration §3 + tiny-skia).
      css::RGBA decoFill = spanFill;
      bool decoUsesPattern = usePatternFill;
      if (!std::holds_alternative<PaintServer::None>(span.resolvedDecorationFill)) {
        if (auto c = impl_->resolveSolidPaint(span.resolvedDecorationFill,
                                              span.decorationFillOpacity * span.opacity)) {
          decoFill = *c;
          decoUsesPattern = false;
        }
      }
      // Decoration stroke (solid).
      std::optional<css::RGBA> decoStrokeColor;
      std::optional<StrokeStyle> decoStrokeStyle;
      if (span.decorationStrokeWidth > 0.0 &&
          !std::holds_alternative<PaintServer::None>(span.resolvedDecorationStroke)) {
        if (auto c = impl_->resolveSolidPaint(span.resolvedDecorationStroke,
                                              span.decorationStrokeOpacity * span.opacity)) {
          if (c->a != 0) {
            decoStrokeColor = c;
            StrokeParams decoSp;
            decoSp.strokeWidth = span.decorationStrokeWidth;
            decoStrokeStyle = toStrokeStyle(decoSp);
          }
        }
      }

      const auto drawDecoPath = [&](const Path& path) {
        if (path.empty()) {
          return;
        }
        const geode::EncodedPath* encoded = impl_->encodeTransientGeometry(path, FillRule::NonZero);
        if (decoUsesPattern) {
          impl_->syncTransform();
          impl_->encoder->fillPathPattern(
              path, FillRule::NonZero,
              impl_->buildPatternPaint(*impl_->patternFillPaint, impl_->paint.fillOpacity),
              encoded);
        } else if (decoFill.a != 0) {
          impl_->encoder->fillPath(path, decoFill, FillRule::NonZero, encoded);
        }
        if (decoStrokeColor.has_value()) {
          // Device-aware flattening, as in every other renderer-side stroke.
          const Path stroked = path.strokeToFill(*decoStrokeStyle, impl_->strokeFlattenTolerance());
          if (!stroked.empty()) {
            impl_->countStrokeOutline(stroked);
            const FillRule strokeRule = Impl::strokeFillRuleFor(stroked, *decoStrokeStyle);
            const geode::EncodedPath* strokeEncoded =
                impl_->encodeTransientGeometry(stroked, strokeRule);
            impl_->encoder->fillPath(stroked, *decoStrokeColor, strokeRule, strokeEncoded);
          }
        }
      };

      const auto isRenderedGlyph = [](const auto& g) {
        return g.glyphIndex != 0 && g.xAdvance > 0.0;
      };
      const bool hasRotation = std::any_of(run.glyphs.begin(), run.glyphs.end(),
                                           [](const auto& g) { return g.rotateDegrees != 0.0; });

      for (const TextDecoration decoType :
           {TextDecoration::Underline, TextDecoration::Overline, TextDecoration::LineThrough}) {
        if (!hasFlag(spanDecoration, decoType)) {
          continue;
        }

        double decoThickness = thickness;
        if (decoType == TextDecoration::LineThrough && fontStrikeThick > 0.0) {
          decoThickness = fontStrikeThick * decoEmScale;
        }

        double decoOffsetY = 0.0;
        if (decoType == TextDecoration::Underline) {
          decoOffsetY = fontUnderlinePos != 0.0 ? -fontUnderlinePos * decoEmScale
                                                : -static_cast<double>(descent) * decoScale * 0.4;
        } else if (decoType == TextDecoration::Overline) {
          decoOffsetY = -static_cast<double>(ascent) * decoScale;
        } else {  // LineThrough
          decoOffsetY = fontStrikePos != 0.0 ? -fontStrikePos * decoEmScale
                                             : -static_cast<double>(ascent) * decoScale * 0.35;
        }

        double decoTopY = decoOffsetY - decoThickness / 2.0;

        const bool hasMultipleDecorationLines =
            (hasFlag(spanDecoration, TextDecoration::Underline) ? 1 : 0) +
                (hasFlag(spanDecoration, TextDecoration::Overline) ? 1 : 0) +
                (hasFlag(spanDecoration, TextDecoration::LineThrough) ? 1 : 0) >
            1;
        if (span.decorationDeclarationCount == 1 && hasMultipleDecorationLines) {
          if (decoType == TextDecoration::Overline) {
            decoTopY += decoThickness * 1.5;
          } else if (decoType == TextDecoration::LineThrough) {
            decoTopY -= decoThickness;
          }
        }

        if (hasRotation) {
          for (size_t gi = 0; gi < run.glyphs.size(); ++gi) {
            const auto& glyph = run.glyphs[gi];
            if (!isRenderedGlyph(glyph)) {
              continue;
            }
            double segmentWidth = glyph.xAdvance;
            for (size_t nj = gi + 1; nj < run.glyphs.size(); ++nj) {
              if (!isRenderedGlyph(run.glyphs[nj])) {
                continue;
              }
              segmentWidth = std::min(segmentWidth, run.glyphs[nj].xPosition - glyph.xPosition);
              break;
            }
            if (segmentWidth <= 0.0) {
              continue;
            }
            const Path segPath = PathBuilder()
                                     .moveTo(Vector2d(0.0, decoTopY))
                                     .lineTo(Vector2d(segmentWidth, decoTopY))
                                     .lineTo(Vector2d(segmentWidth, decoTopY + decoThickness))
                                     .lineTo(Vector2d(0.0, decoTopY + decoThickness))
                                     .closePath()
                                     .build();
            Transform2d segFromLocal = Transform2d::Translate(glyph.xPosition, glyph.yPosition);
            if (glyph.rotateDegrees != 0.0) {
              segFromLocal =
                  Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
                  segFromLocal;
            }
            drawDecoPath(TransformPath(segPath, segFromLocal));
          }
        } else {
          const auto firstGlyph =
              std::find_if(run.glyphs.begin(), run.glyphs.end(), isRenderedGlyph);
          const auto lastGlyph =
              std::find_if(run.glyphs.rbegin(), run.glyphs.rend(), isRenderedGlyph);
          if (firstGlyph == run.glyphs.end() || lastGlyph == run.glyphs.rend()) {
            continue;
          }
          const double baselineY = firstGlyph->yPosition;
          const bool sameBaseline =
              std::all_of(run.glyphs.begin(), run.glyphs.end(), [&](const auto& g) {
                return !isRenderedGlyph(g) || std::abs(g.yPosition - baselineY) < 1e-6;
              });
          if (sameBaseline) {
            const double x0 = firstGlyph->xPosition;
            const double x1 = lastGlyph->xPosition + lastGlyph->xAdvance;
            const double y = baselineY + decoTopY;
            drawDecoPath(PathBuilder()
                             .moveTo(Vector2d(x0, y))
                             .lineTo(Vector2d(x1, y))
                             .lineTo(Vector2d(x1, y + decoThickness))
                             .lineTo(Vector2d(x0, y + decoThickness))
                             .closePath()
                             .build());
          } else {
            PathBuilder decoBuilder;
            for (size_t gi = 0; gi < run.glyphs.size(); ++gi) {
              const auto& glyph = run.glyphs[gi];
              if (!isRenderedGlyph(glyph)) {
                continue;
              }
              const double x0 = glyph.xPosition;
              double x1 = glyph.xPosition + glyph.xAdvance;
              for (size_t nj = gi + 1; nj < run.glyphs.size(); ++nj) {
                if (!isRenderedGlyph(run.glyphs[nj])) {
                  continue;
                }
                x1 = std::min(x1, run.glyphs[nj].xPosition);
                break;
              }
              if (x1 <= x0) {
                continue;
              }
              const double y = glyph.yPosition + decoTopY;
              decoBuilder.moveTo(Vector2d(x0, y));
              decoBuilder.lineTo(Vector2d(x1, y));
              decoBuilder.lineTo(Vector2d(x1, y + decoThickness));
              decoBuilder.lineTo(Vector2d(x0, y + decoThickness));
              decoBuilder.closePath();
            }
            if (!decoBuilder.empty()) {
              drawDecoPath(decoBuilder.build());
            }
          }
        }
      }
    }
  }

  // Consume the element-level pattern fill/stroke slots exactly once. Even if no
  // run actually used them (e.g. every glyph was .notdef), they must be reset
  // here so the staged pattern does not leak onto the next shape's draw.
  if (hasPatternFill) {
    impl_->patternFillPaint.reset();
  }
  impl_->patternStrokePaint.reset();
#else
  (void)registry;
  (void)text;
  (void)params;
  if (impl_->verbose && !impl_->warnedText) {
    std::cerr << "RendererGeode: text rendering requires DONNER_TEXT_ENABLED\n";
    impl_->warnedText = true;
  }
#endif
}

std::unique_ptr<RendererInterface> RendererGeode::createOffscreenInstance() const {
  if (!impl_->device) {
    return nullptr;
  }
  auto renderer = std::unique_ptr<RendererGeode>(
      new RendererGeode(impl_->device, impl_->verbose, impl_->offscreenCreationHookForTesting));
  renderer->impl_->filterExecutionBudget = impl_->filterExecutionBudget;
  renderer->impl_->ownsFilterExecutionBudget = false;
  renderer->impl_->filterPreparationBudget = impl_->filterPreparationBudget;
  renderer->impl_->ownsFilterPreparationBudget = false;
  renderer->impl_->geometryBudget = impl_->geometryBudget;
  renderer->impl_->ownsGeometryBudget = false;
  renderer->impl_->surfaceBudget = impl_->surfaceBudget;
  renderer->impl_->ownsSurfaceBudget = false;
  renderer->impl_->textMaterializationBudget = impl_->textMaterializationBudget;
  renderer->impl_->ownsTextMaterializationBudget = false;
  renderer->impl_->documentGeometryLimits = impl_->documentGeometryLimits;
  renderer->impl_->documentGeometryFrameState = impl_->documentGeometryFrameState;
  return renderer;
}

RendererFilterPreparationBudget* RendererGeode::filterPreparationBudget() {
  return impl_->filterPreparationBudget.get();
}

void RendererGeode::setOffscreenCreationHookForTesting(std::function<void()> hook) {
  impl_->offscreenCreationHookForTesting = std::move(hook);
}

std::uint64_t RendererGeode::filterBudgetChunksForTesting() const {
  return impl_->filterExecutionBudget->chunks();
}

std::shared_ptr<const RendererTextureSnapshot> RendererGeode::takeTextureSnapshot() {
  impl_->borrowedTargetSnapshot.reset();
  if (!impl_->device || !impl_->ownedTarget.isValid() || impl_->hostTarget ||
      impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    return nullptr;
  }

  gpu::Texture texture = std::move(impl_->ownedTarget);
  impl_->ownedTarget = gpu::Texture();
  const Vector2i dimensions(impl_->pixelWidth, impl_->pixelHeight);
  impl_->target = wgpu::Texture();
  impl_->targetHandle = nullptr;
  impl_->targetHandleTexture = nullptr;
  impl_->targetWidth = 0;
  impl_->targetHeight = 0;

  return std::make_shared<RendererGeodeTextureSnapshot>(
      RendererGeodeTextureSnapshot::AdoptRuntimeTexture(impl_->device, std::move(texture),
                                                        dimensions, impl_->textureFormat,
                                                        AlphaType::Premultiplied));
}

const RendererTextureSnapshot* RendererGeode::borrowTextureSnapshot() {
  if (!impl_->device || !impl_->ownedTarget.isValid() || impl_->hostTarget ||
      impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    impl_->borrowedTargetSnapshot.reset();
    return nullptr;
  }

  impl_->borrowedTargetSnapshot.emplace(RendererGeodeTextureSnapshot::BorrowCurrentFrame(
      impl_->device->adapterDevice().wgpuTextureOf(impl_->ownedTarget),
      Vector2i(impl_->pixelWidth, impl_->pixelHeight), impl_->textureFormat));
  return &*impl_->borrowedTargetSnapshot;
}

RendererBitmap RendererGeode::takeSnapshot() const {
  return takeSnapshotInterruptibly({});
}

// Read a Geode-rendered texture back into a tightly packed straight-alpha RGBA
// bitmap. Shared by the live renderer target and detached texture snapshots.
// `sourceAlphaType` describes the texels: render targets store premultiplied alpha and are
// divided back out, while a snapshot wrapping a straight-alpha host upload is copied as-is.
//
// Premultiplied RGBA8Unorm targets take the GPU path: a compute pass
// unpremultiplies the target into a straight-alpha RGBA8 staging texture,
// which is then copied into a map-readable buffer and packed out. The GPU
// path output is byte-identical to the CPU reference loop below. Everything
// else (BGRA targets, straight-alpha host uploads, unbindable textures) uses
// the CPU copy path.
namespace {

/// Outcome of a completed readback map wait.
enum class ReadbackMapStatus {
  /// The map completed successfully; the buffer is mapped and owned by the caller until unmap().
  Success,
  /// A cancellation request fired; the buffer was unmapped and destroyed.
  Cancelled,
  /// The readback deadline expired with no map completion; the buffer was
  /// unmapped and destroyed and the device was declared lost. Distinct from
  /// Cancelled so a caller can avoid starting a second full-deadline wait
  /// against a device that just proved unresponsive.
  TimedOut,
  /// The device was already declared lost before the map was requested; no
  /// GPU wait was performed.
  DeviceLost,
  /// The map completed with a non-success status.
  Failed,
};

/// One slice of the readback map wait.
///
/// The runtime's wait takes a slice and a budget; this path drives its own budget so the poll
/// count, the cancellation cadence, and the timeout attribution stay exactly what they were
/// before the wait moved onto the runtime, so each call is given a budget of one slice and the
/// loop below owns the deadline.
constexpr double kReadbackWaitSliceSeconds =
    std::chrono::duration<double>(geode::kGpuWaitPollInterval).count();

/// Result of \ref MapAndWaitReadback: the outcome, plus the live mapping on success.
struct ReadbackMapResult {
  ReadbackMapStatus status = ReadbackMapStatus::Failed;  //!< How the wait ended.
  gpu::BufferMapping mapping;                            //!< Live mapping; valid only on success.
};

/// Map `buffer` for read and wait for the GPU to deliver it. On cancellation,
/// timeout, or map failure the buffer is unmapped and destroyed and a
/// non-success status is returned. Readback poll statistics are recorded on
/// the device regardless of the outcome.
///
/// The wait is a loop of single-slice runtime waits rather than one runtime wait with the full
/// budget, because four things this path reports are properties of the loop and not of the
/// runtime's wait: the number of slices actually run, whether the backend waited on the map's
/// completion event or polled for it, the wait-site attribution a timeout declares on the
/// device, and destroying the buffer whose map was abandoned.
///
/// @param device Device the buffer belongs to.
/// @param buffer Buffer to map; destroyed and left invalid on cancellation or timeout.
/// @param mapSize Bytes to map, from offset zero.
/// @param shouldCancel Optional cancellation predicate, polled once per slice.
ReadbackMapResult MapAndWaitReadback(const std::shared_ptr<geode::GeodeDevice>& device,
                                     gpu::Buffer& buffer, uint64_t mapSize,
                                     const std::function<bool()>& shouldCancel) {
  if (device->isDeviceLost()) {
    // A lost device will never deliver the map. Fail fast so a caller does
    // not spend another full readback deadline against a hung driver; the
    // caller drops the buffer without pooling it.
    return ReadbackMapResult{ReadbackMapStatus::DeviceLost, {}};
  }

  geode::GeodeWgpuAdapterDevice& runtime = device->adapterDevice();
  gpu::Result<gpu::BufferMapping> mapping =
      runtime.mapBufferAsync(buffer, gpu::MapMode::Read, 0, mapSize);
  if (mapping.hasError()) {
    // The request never started, so no slice ran and there are no poll statistics to record. A
    // device lost between the check above and here is reported as such rather than as a plain
    // failure, because the caller retries a failure and must not retry a lost device.
    return ReadbackMapResult{
        device->isDeviceLost() ? ReadbackMapStatus::DeviceLost : ReadbackMapStatus::Failed, {}};
  }
  gpu::BufferMapping liveMapping = std::move(mapping).result();

  int pollIter = 0;
  bool cancelled = false;
  bool timedOut = false;
  gpu::MapWaitOutcome outcome = gpu::MapWaitOutcome::TimedOut;
  const auto readbackWaitStart = std::chrono::steady_clock::now();
  const auto readbackDeadline = readbackWaitStart + geode::kReadbackMapTimeout;
  while (true) {
    if (shouldCancel && shouldCancel()) {
      cancelled = true;
      break;
    }
    if (std::chrono::steady_clock::now() >= readbackDeadline) {
      timedOut = true;
      break;
    }
    // Counted after the two checks above, so a wait that was cancelled or already past its
    // deadline reports zero slices - the signal a caller uses to tell "gave up immediately"
    // from "waited and then gave up".
    ++pollIter;

    const gpu::Result<gpu::MapWaitOutcome> slice = runtime.waitForMapping(
        liveMapping, gpu::MapWaitParams{kReadbackWaitSliceSeconds, kReadbackWaitSliceSeconds},
        /*shouldCancel=*/{});
    if (slice.hasError()) {
      outcome = gpu::MapWaitOutcome::Failed;
      break;
    }
    outcome = slice.result();
    // A slice budget equal to one slice reports TimedOut for "not ready yet"; every other
    // outcome is terminal.
    if (outcome != gpu::MapWaitOutcome::TimedOut) {
      break;
    }
  }

  device->recordReadback(runtime.mappingUsedTimedWaitAny(liveMapping), pollIter);

  if (cancelled || timedOut) {
    if (timedOut) {
      device->markDeviceLostAfterWaitTimeout(
          geode::GpuWaitSite::ReadbackMap,
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                readbackWaitStart),
          "snapshot readback map did not complete within the readback deadline");
    }
    // Abandoning a pending map leaves the buffer unusable for anything else, so it is destroyed
    // here rather than returned to a pool that would hand it out again.
    (void)runtime.unmapBuffer(std::move(liveMapping));
    (void)runtime.destroyBufferBacking(std::move(buffer));
    return ReadbackMapResult{cancelled ? ReadbackMapStatus::Cancelled : ReadbackMapStatus::TimedOut,
                             {}};
  }

  if (outcome == gpu::MapWaitOutcome::Ready) {
    return ReadbackMapResult{ReadbackMapStatus::Success, std::move(liveMapping)};
  }

  // Device loss and map failure leave the buffer alone: the caller drops the whole set rather
  // than pooling it, and a lost device's buffers are going away with it.
  (void)runtime.unmapBuffer(std::move(liveMapping));
  return ReadbackMapResult{outcome == gpu::MapWaitOutcome::DeviceLost
                               ? ReadbackMapStatus::DeviceLost
                               : ReadbackMapStatus::Failed,
                           {}};
}

/// GPU-side snapshot readback for premultiplied RGBA8Unorm render targets: a
/// compute pass unpremultiplies the target into a straight-alpha RGBA8
/// staging texture, which is then copied into a map-readable buffer. The
/// output bytes are identical to the CPU reference loop in
/// ReadGeodeTextureSnapshot. Returns an empty bitmap on any failure so the
/// caller can fall back to the CPU copy path.
///
/// wgpu-native forbids combining MAP_READ with STORAGE on a single buffer, so
/// the compute output cannot be mapped directly; the staging-texture copy is
/// the standard readback shape.
RendererBitmap ReadGeodeTextureSnapshotGpu(const std::shared_ptr<geode::GeodeDevice>& device,
                                           const wgpu::Texture& texture, uint32_t width,
                                           uint32_t height,
                                           const std::function<bool()>& shouldCancel,
                                           bool& outTimedOut) {
  RendererBitmap bitmap;
  outTimedOut = false;
  const geode::GeodeSnapshotReadbackPipeline& readbackPipeline = device->snapshotReadbackPipeline();
  if (!readbackPipeline.valid()) {
    return bitmap;
  }

  // Pooled staging texture + readback buffer, keyed by size. Repeat snapshots
  // at the same dimensions reuse the entry, so steady-state readback
  // allocates nothing.
  geode::SnapshotReadbackResources resources =
      device->acquireSnapshotReadbackResources(width, height);
  if (resources.empty()) {
    return bitmap;
  }
  const uint32_t bytesPerRow = alignBytesPerRow(width * 4u);
  const uint64_t mapSize = static_cast<uint64_t>(bytesPerRow) * static_cast<uint64_t>(height);

  geode::GeodeWgpuAdapterDevice& runtime = device->adapterDevice();

  // The source is the caller's own render target, which the runtime has not named; it is
  // borrowed for this one submission and the caller keeps ownership.
  gpu::Result<gpu::Texture> importedInput =
      runtime.importExternalTexture(texture, gpu::Extent2d{width, height},
                                    gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::Sampled);
  if (importedInput.hasError()) {
    return bitmap;
  }
  const gpu::Texture inputTexture = std::move(importedInput).result();
  gpu::Result<gpu::TextureView> importedView = runtime.createTextureView(
      inputTexture, gpu::TextureViewDescriptor{"RendererGeodeReadbackInputView"});
  if (importedView.hasError()) {
    return bitmap;
  }
  const gpu::TextureView inputView = std::move(importedView).result();

  using ReadbackBinding = gpu::shader::programs::SnapshotUnpremultiplyBinding;
  gpu::Result<gpu::BindGroup> bindGroup = runtime.createBindGroup(gpu::BindGroupDescriptor{
      "RendererGeodeReadbackBG",
      readbackPipeline.bindGroupLayout(),
      {gpu::BindGroupEntry{static_cast<uint32_t>(ReadbackBinding::InputTexture),
                           gpu::TextureViewBinding{inputView}},
       gpu::BindGroupEntry{static_cast<uint32_t>(ReadbackBinding::OutputTexture),
                           gpu::TextureViewBinding{resources.stagingView}}}});
  if (bindGroup.hasError()) {
    return bitmap;
  }

  // Record the unpremultiply compute pass and the staging-texture copy into one command buffer.
  gpu::Result<std::unique_ptr<gpu::CommandEncoder>> encoderResult = runtime.createCommandEncoder();
  if (encoderResult.hasError()) {
    return bitmap;
  }
  const std::unique_ptr<gpu::CommandEncoder> encoder = std::move(encoderResult).result();

  gpu::Result<gpu::ComputePassEncoder*> passResult =
      encoder->beginComputePass(gpu::ComputePassDescriptor{"RendererGeodeReadbackPass"});
  if (passResult.hasError()) {
    return bitmap;
  }
  gpu::ComputePassEncoder* pass = passResult.result();
  // The dispatch is sized from the same generated constants the pipeline declares, so the grid
  // cannot disagree with the size compiled into the shader. A stale larger copy here would
  // under-dispatch and leave the tail of the destination unwritten.
  constexpr uint32_t kWorkgroupX = gpu::shader::programs::kSnapshotUnpremultiplyWorkgroupSize;
  constexpr uint32_t kWorkgroupY = gpu::shader::programs::kSnapshotUnpremultiplyWorkgroupSize;
  if (pass->setPipeline(readbackPipeline.pipeline()).hasError() ||
      pass->setBindGroup(0, bindGroup.result()).hasError() ||
      pass->dispatchWorkgroups((width + kWorkgroupX - 1) / kWorkgroupX,
                               (height + kWorkgroupY - 1) / kWorkgroupY, 1)
          .hasError() ||
      pass->end().hasError()) {
    return bitmap;
  }

  if (encoder
          ->copyTextureToBuffer(gpu::TexelCopyTextureInfo{resources.staging}, resources.readback,
                                gpu::TexelCopyBufferLayout{0, bytesPerRow, height},
                                gpu::Extent2d{width, height})
          .hasError()) {
    return bitmap;
  }

  gpu::Result<gpu::CommandBuffer> commands = encoder->finish();
  if (commands.hasError()) {
    return bitmap;
  }
  // Standalone, not the frame's encoder: this readback has its own map to wait on, and a
  // snapshot is routinely asked for while another renderer's frame is open. Its sources were
  // submitted before it was asked for and its only writes are its own staging texture and
  // readback buffer, so it shares nothing with the spans that frame is still recording.
  //
  // The runtime counts its own submit, so there is no explicit tick here; a second one would
  // double-count every snapshot against the per-frame submission ceilings.
  if (runtime.submitStandalone(std::move(commands).result()).hasError()) {
    return bitmap;
  }

  ReadbackMapResult mapResult =
      MapAndWaitReadback(device, resources.readback, mapSize, shouldCancel);
  const ReadbackMapStatus mapStatus = mapResult.status;
  if (mapStatus != ReadbackMapStatus::Success) {
    // Cancelled, timed out, lost, or failed: on cancellation and timeout the
    // helper already unmapped and destroyed the readback buffer, and on a
    // lost device the map was never requested; either way the entry cannot
    // be pooled. The caller uses outTimedOut to avoid a second full-deadline
    // wait against an unresponsive device.
    outTimedOut =
        mapStatus == ReadbackMapStatus::TimedOut || mapStatus == ReadbackMapStatus::DeviceLost;
    return bitmap;
  }

  const gpu::Result<std::span<const uint8_t>> mappedBytes =
      device->adapterDevice().mappedBytes(mapResult.mapping);
  if (mappedBytes.hasError()) {
    // A mapped-range/buffer-size mismatch is reported here rather than raising a validation
    // error. Unmap and drop the entry (do not pool a buffer whose sizing math disagreed with
    // ours) and let the CPU path take over.
    (void)device->adapterDevice().unmapBuffer(std::move(mapResult.mapping));
    return bitmap;
  }
  const uint8_t* mapped = mappedBytes.result().data();

  // The staging texture already holds straight-alpha RGBA, so the CPU only
  // strips row padding.
  bitmap.dimensions = Vector2i(static_cast<int>(width), static_cast<int>(height));
  bitmap.rowBytes = static_cast<size_t>(width) * 4u;
  bitmap.alphaType = AlphaType::Unpremultiplied;
  bitmap.pixels.resize(bitmap.rowBytes * height);
  for (uint32_t y = 0; y < height; ++y) {
    if (shouldCancel && shouldCancel()) {
      (void)device->adapterDevice().unmapBuffer(std::move(mapResult.mapping));
      return {};
    }
    std::memcpy(bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes,
                mapped + static_cast<size_t>(y) * bytesPerRow, bitmap.rowBytes);
  }
  (void)device->adapterDevice().unmapBuffer(std::move(mapResult.mapping));
  device->releaseSnapshotReadbackResources(std::move(resources));
  return bitmap;
}

}  // namespace

static RendererBitmap ReadGeodeTextureSnapshot(const std::shared_ptr<geode::GeodeDevice>& device,
                                               const wgpu::Texture& texture, Vector2i dimensions,
                                               wgpu::TextureFormat format,
                                               AlphaType sourceAlphaType,
                                               const std::function<bool()>& shouldCancel) {
  RendererBitmap bitmap;
  // Close the traversal-to-snapshot race before allocating a readback buffer or submitting any GPU
  // work. A main-document request may arrive immediately after the thumbnail finishes drawing.
  if (shouldCancel && shouldCancel()) {
    return bitmap;
  }
  if (!device || !texture || dimensions.x <= 0 || dimensions.y <= 0) {
    return bitmap;
  }

  const uint32_t width = static_cast<uint32_t>(dimensions.x);
  const uint32_t height = static_cast<uint32_t>(dimensions.y);

  // GPU unpremultiply path: premultiplied RGBA8Unorm render targets only. The
  // compute shader produces straight RGBA regardless of the texture's memory
  // layout, but BGRA targets keep the proven CPU path for now.
  // Gate on the texture's REAL format as well as the caller-declared one:
  // the compute pass binds a view that inherits the texture's actual format,
  // so an sRGB surface declared as RGBA8Unorm would be silently linearized
  // by textureLoad and re-quantized into wrong bytes with no validation
  // error, where the CPU copy path degrades only to a channel-order bug.
  if (sourceAlphaType == AlphaType::Premultiplied && format == wgpu::TextureFormat::RGBA8Unorm &&
      texture.getFormat() == format &&
      (static_cast<WGPUTextureUsage>(texture.getUsage()) &
       static_cast<WGPUTextureUsage>(wgpu::TextureUsage::TextureBinding)) != 0u) {
    bool gpuTimedOut = false;
    RendererBitmap gpuBitmap =
        ReadGeodeTextureSnapshotGpu(device, texture, width, height, shouldCancel, gpuTimedOut);
    if (!gpuBitmap.empty()) {
      return gpuBitmap;
    }
    // The GPU path returned empty: cancelled, timed out, or failed. A
    // cancellation must return here so a superseding request is not delayed
    // by a second GPU round-trip. A timeout must also return: the device
    // just spent the full deadline not delivering a map, and the CPU copy
    // path would begin another full-deadline wait against the same
    // unresponsive device, turning a 10 s stall into 20 s. Only a genuine
    // map failure falls back to the CPU copy path below.
    if ((shouldCancel && shouldCancel()) || gpuTimedOut) {
      return bitmap;
    }
  }

  const uint32_t bytesPerRow = alignBytesPerRow(width * 4u);

  // Allocate readback buffer. Created through the runtime, which counts the allocation itself,
  // so there is no explicit tick here.
  const uint64_t readbackSize = static_cast<uint64_t>(bytesPerRow) * static_cast<uint64_t>(height);
  gpu::Result<gpu::Buffer> createdReadback = device->adapterDevice().createBuffer(
      gpu::BufferDescriptor{"RendererGeodeReadback", readbackSize,
                            gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead});
  if (createdReadback.hasError()) {
    return bitmap;
  }
  gpu::Buffer readback = std::move(createdReadback).result();

  // Copy texture -> readback buffer. The source is the caller's own render target, borrowed for
  // this one submission; the caller keeps ownership.
  geode::GeodeWgpuAdapterDevice& runtime = device->adapterDevice();
  gpu::Result<gpu::Texture> importedSource = runtime.importExternalTexture(
      texture, gpu::Extent2d{width, height}, geode::GpuTextureFormatFromWgpu(format),
      gpu::TextureUsage::CopySrc);
  if (importedSource.hasError()) {
    return bitmap;
  }
  const gpu::Texture sourceTexture = std::move(importedSource).result();

  gpu::Result<std::unique_ptr<gpu::CommandEncoder>> encoderResult = runtime.createCommandEncoder();
  if (encoderResult.hasError()) {
    return bitmap;
  }
  const std::unique_ptr<gpu::CommandEncoder> encoder = std::move(encoderResult).result();
  if (encoder
          ->copyTextureToBuffer(gpu::TexelCopyTextureInfo{sourceTexture}, readback,
                                gpu::TexelCopyBufferLayout{0, bytesPerRow, height},
                                gpu::Extent2d{width, height})
          .hasError()) {
    return bitmap;
  }
  gpu::Result<gpu::CommandBuffer> commands = encoder->finish();
  if (commands.hasError()) {
    return bitmap;
  }
  // Standalone for the same reason as the GPU path above, and the runtime counts its own submit.
  if (runtime.submitStandalone(std::move(commands).result()).hasError()) {
    return bitmap;
  }

  ReadbackMapResult mapResult = MapAndWaitReadback(device, readback, readbackSize, shouldCancel);
  if (mapResult.status != ReadbackMapStatus::Success) {
    return bitmap;
  }

  const gpu::Result<std::span<const uint8_t>> mappedBytes =
      device->adapterDevice().mappedBytes(mapResult.mapping);
  if (mappedBytes.hasError()) {
    // A size mismatch between the map request and the buffer is reported here rather than
    // raising a validation error; never memcpy from it.
    (void)device->adapterDevice().unmapBuffer(std::move(mapResult.mapping));
    return bitmap;
  }
  const uint8_t* mapped = mappedBytes.result().data();

  // Strip row padding and unpremultiply alpha so the consumer gets a tightly
  // packed *straight-alpha* RGBA buffer. `GeoEncoder::fillPath` premultiplies
  // paint RGB by alpha before upload to match the blend pipeline's
  // premultiplied storage, but `RendererBitmap` - like Skia's and
  // tiny-skia's `takeSnapshot()` outputs - is defined as straight RGBA.
  // Returning raw texture bytes would darken semi-transparent content and
  // break cross-backend parity.
  bitmap.dimensions = Vector2i(static_cast<int>(width), static_cast<int>(height));
  bitmap.rowBytes = static_cast<size_t>(width) * 4u;
  bitmap.alphaType = AlphaType::Unpremultiplied;
  bitmap.pixels.resize(bitmap.rowBytes * height);
  const bool sourceIsBgra = IsBgraTextureFormat(format);
  for (uint32_t y = 0; y < height; ++y) {
    if (shouldCancel && shouldCancel()) {
      (void)device->adapterDevice().unmapBuffer(std::move(mapResult.mapping));
      return {};
    }
    const uint8_t* srcRow = mapped + static_cast<size_t>(y) * bytesPerRow;
    uint8_t* dstRow = bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t srcR = sourceIsBgra ? srcRow[x * 4 + 2] : srcRow[x * 4 + 0];
      const uint8_t srcG = srcRow[x * 4 + 1];
      const uint8_t srcB = sourceIsBgra ? srcRow[x * 4 + 0] : srcRow[x * 4 + 2];
      const uint8_t srcA = srcRow[x * 4 + 3];
      if (sourceAlphaType == AlphaType::Unpremultiplied) {
        dstRow[x * 4 + 0] = srcR;
        dstRow[x * 4 + 1] = srcG;
        dstRow[x * 4 + 2] = srcB;
        dstRow[x * 4 + 3] = srcA;
        continue;
      }
      if (srcA == 0u) {
        dstRow[x * 4 + 0] = 0u;
        dstRow[x * 4 + 1] = 0u;
        dstRow[x * 4 + 2] = 0u;
        dstRow[x * 4 + 3] = 0u;
        continue;
      }
      if (srcA == 255u) {
        dstRow[x * 4 + 0] = srcR;
        dstRow[x * 4 + 1] = srcG;
        dstRow[x * 4 + 2] = srcB;
        dstRow[x * 4 + 3] = 255u;
        continue;
      }
      // Round-nearest unpremultiply: straight = (premul * 255 + alpha/2) / alpha.
      const unsigned a = srcA;
      const unsigned half = a >> 1u;
      dstRow[x * 4 + 0] = static_cast<uint8_t>(std::min(255u, (srcR * 255u + half) / a));
      dstRow[x * 4 + 1] = static_cast<uint8_t>(std::min(255u, (srcG * 255u + half) / a));
      dstRow[x * 4 + 2] = static_cast<uint8_t>(std::min(255u, (srcB * 255u + half) / a));
      dstRow[x * 4 + 3] = srcA;
    }
  }
  (void)device->adapterDevice().unmapBuffer(std::move(mapResult.mapping));
  return bitmap;
}

RendererBitmap RendererGeodeTextureSnapshot::takeSnapshot() const {
  return ReadGeodeTextureSnapshot(device_, texture_, dimensions_, format_, alphaType_,
                                  /*shouldCancel=*/{});
}

RendererBitmap RendererGeode::takeSnapshotInterruptibly(
    const std::function<bool()>& shouldCancel) const {
  if (!impl_->device || !impl_->target || impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    return RendererBitmap{};
  }
  return ReadGeodeTextureSnapshot(impl_->device, impl_->target,
                                  Vector2i(impl_->pixelWidth, impl_->pixelHeight),
                                  impl_->textureFormat, AlphaType::Premultiplied, shouldCancel);
}

bool RendererGeode::deviceLost() const {
  return impl_->device && impl_->device->isDeviceLost();
}

namespace {

/// Translate the Geode wait vocabulary into the backend-neutral one the
/// renderer interface publishes.
///
/// A new enumerator on either side is only a -Wswitch warning here, not a
/// build error, so the fallthrough must not resolve to `None`: `None` is
/// published as "the driver declared this loss, no deadline expired", and a
/// wait site quietly wearing that label points a report at the wrong
/// subsystem. Report it as `Unknown` instead, which reads as itself in the
/// published diagnostics.
GpuWaitTimeoutSite NeutralWaitSite(geode::GpuWaitSite site) {
  switch (site) {
    case geode::GpuWaitSite::None: return GpuWaitTimeoutSite::None;
    case geode::GpuWaitSite::ReadbackMap: return GpuWaitTimeoutSite::ReadbackMap;
    case geode::GpuWaitSite::QueueIdle: return GpuWaitTimeoutSite::QueueIdle;
  }
  return GpuWaitTimeoutSite::Unknown;
}

}  // namespace

RendererReadbackStats RendererGeode::consumeReadbackStats() {
  if (!impl_->device) {
    return {};
  }
  const geode::GeodeDevice::ReadbackStats stats = impl_->device->consumeReadbackStats();
  return RendererReadbackStats{
      .count = stats.count,
      .pollIterations = stats.pollIterations,
      .usedTimedWaitAny = stats.usedTimedWaitAny,
      .deviceLost = stats.deviceLost,
      .timedOutWaitSite = NeutralWaitSite(stats.timedOutWaitSite),
      .timedOutWaitMs = stats.timedOutWaitMs,
  };
}

}  // namespace donner::svg
