#include "donner/svg/renderer/RendererGeode.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Path.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/base/Transform.h"
#include "donner/base/Utils.h"
#include "donner/base/Vector2.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/core/Gradient.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/FilterRegionContext.h"
#include "donner/svg/renderer/HeadlessDevicePool.h"
#include "donner/svg/renderer/PatternTile.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/TextureHandOutQueue.h"
#include "donner/svg/renderer/geode/GeoEncoder.h"
#include "donner/svg/renderer/geode/GeodeAnchoredSampling.h"
#include "donner/svg/renderer/geode/GeodeBufferPool.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeFillTolerance.h"
#include "donner/svg/renderer/geode/GeodeFilterEngine.h"
#include "donner/svg/renderer/geode/GeodeGlyphResidency.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePathCacheComponent.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"
#include "donner/svg/renderer/geode/GeodePerDevice.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeResidentImageComponent.h"
#include "donner/svg/renderer/geode/GeodeResidentPathComponent.h"
#include "donner/svg/renderer/geode/GeodeResourceBudget.h"
#include "donner/svg/renderer/geode/GeodeStrokeTolerance.h"
#include "donner/svg/resources/ImageResource.h"
#ifdef DONNER_TEXT_ENABLED
#include "donner/base/MathUtils.h"
#include "donner/svg/renderer/PlacedTextGeometry.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"
#include "donner/svg/text/TextEngineHelpers.h"
#include "donner/svg/text/TextLayoutParams.h"
#include "donner/svg/text/TextPathWarp.h"
#endif

namespace donner::svg {

namespace {
bool SnapshotExtentFits(Vector2i content, Vector2i allocation) {
  return content.x > 0 && content.y > 0 && content.x <= allocation.x && content.y <= allocation.y;
}

Vector2i SnapshotAllocationExtent(const gpu::Extent2d& size) {
  if (size.width > uint32_t(std::numeric_limits<int>::max()) ||
      size.height > uint32_t(std::numeric_limits<int>::max())) {
    return Vector2i::Zero();
  }
  return {static_cast<int>(size.width), static_cast<int>(size.height)};
}

/// Capabilities that let a snapshot's texture be presented or read back.
constexpr gpu::TextureUsage kSnapshotUsableCapabilities =
    gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc;

/// Whether a snapshot may name a texture of this format: the presentation and readback paths
/// handle the two 8-bit RGBA orderings and nothing else.
/// @param format Format to test.
bool IsSnapshotFormat(gpu::TextureFormat format) {
  return format == gpu::TextureFormat::RGBA8Unorm || format == gpu::TextureFormat::BGRA8Unorm;
}

/// Whether \p owner still names \p texture as something a consumer may sample.
/// @param owner Device that owns the texture. @param texture Handle to resolve.
bool SnapshotTextureIsSampleable(const gpu::Device& owner, const gpu::Texture& texture) {
  const gpu::Result<gpu::TextureDescriptor> descriptor = owner.textureDescriptor(texture);
  return descriptor.hasResult() && IsSnapshotFormat(descriptor.result().format) &&
         gpu::HasAllFlags(descriptor.result().usage, gpu::TextureUsage::Sampled);
}
}  // namespace

struct RendererGeodeTextureSnapshot::Backing {
  std::shared_ptr<geode::GeodeDevice> device;
  gpu::Texture runtimeTexture;
  /// Made on the producer's thread at adoption, so another context can register the texture
  /// without reading the producer's tables. Empty on a backend that cannot share textures.
  gpu::TextureExport exported;

  ~Backing() {
    // Released before the texture is handed back, so a texture nothing else holds keeps its
    // immediate backing release.
    exported = gpu::TextureExport();
    if (runtimeTexture.isValid() && device) {
      UTILS_RELEASE_ASSERT(device->deferDestroyTextureBacking(std::move(runtimeTexture)));
    }
  }
};

RendererGeodeTextureSnapshot RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
    std::shared_ptr<geode::GeodeDevice> device, gpu::Texture&& texture, Vector2i dimensions,
    gpu::TextureFormat format, AlphaType alphaType) {
  RendererGeodeTextureSnapshot result;
  if (!device || !device->runtimeDevice().ownsTextureBacking(texture)) {
    return result;
  }
  const gpu::Result<gpu::TextureDescriptor> descriptor =
      device->runtimeDevice().textureDescriptor(texture);
  if (descriptor.hasError() ||
      (format != gpu::TextureFormat::RGBA8Unorm && format != gpu::TextureFormat::BGRA8Unorm) ||
      descriptor.result().format != format || descriptor.result().sampleCount != 1 ||
      (descriptor.result().usage & kSnapshotUsableCapabilities) == gpu::TextureUsage::None ||
      !SnapshotExtentFits(dimensions, SnapshotAllocationExtent(descriptor.result().size))) {
    return result;
  }
  // A failed export leaves the snapshot usable on its own device; only naming it on another
  // context, including a capture, is refused.
  gpu::Result<gpu::TextureExport> exported = device->runtimeDevice().exportTexture(texture);
  result.device_ = std::move(device);
  result.backing_ = std::make_shared<Backing>();
  result.backing_->device = result.device_;
  result.backing_->runtimeTexture = std::move(texture);
  if (exported.hasResult()) {
    result.backing_->exported = std::move(exported).result();
  }
  result.allocationDimensions_ = SnapshotAllocationExtent(descriptor.result().size);
  result.runtimeFormat_ = format;
  result.dimensions_ = dimensions;
  result.alphaType_ = alphaType;
  return result;
}

RendererGeodeTextureSnapshot::RendererGeodeTextureSnapshot(
    RendererGeodeTextureSnapshot&& other) noexcept {
  *this = std::move(other);
}

bool RendererGeodeTextureSnapshot::isValid() const {
  return runtimeTexture() != nullptr && runtimeFormat_.has_value() &&
         SnapshotExtentFits(dimensions_, allocationDimensions_);
}

const gpu::Texture* RendererGeodeTextureSnapshot::runtimeTexture() const {
  if (backing_ && backing_->runtimeTexture.isValid()) {
    return &backing_->runtimeTexture;
  }
  return borrowedGpuTexture_.isValid() ? &borrowedGpuTexture_ : nullptr;
}

gpu::Texture RendererGeodeTextureSnapshot::takeRuntimeRegistrationForTesting() {
  return backing_ ? std::move(backing_->runtimeTexture) : gpu::Texture();
}

const gpu::TextureExport* RendererGeodeTextureSnapshot::textureExport() const {
  return backing_ != nullptr && backing_->exported.isValid() ? &backing_->exported : nullptr;
}

uint64_t RendererGeodeTextureSnapshot::deviceId() const {
  const gpu::Texture* runtime = runtimeTexture();
  return runtime != nullptr ? runtime->deviceId() : 0;
}

bool RendererGeodeTextureSnapshot::setDimensions(Vector2i dimensions) {
  if (!SnapshotExtentFits(dimensions, allocationDimensions_)) {
    return false;
  }
  dimensions_ = dimensions;
  return true;
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
  backing_ = std::move(other.backing_);
  borrowedGpuTexture_ = std::move(other.borrowedGpuTexture_);
  allocationDimensions_ = std::exchange(other.allocationDimensions_, Vector2i::Zero());
  runtimeFormat_ = std::exchange(other.runtimeFormat_, std::nullopt);
  dimensions_ = std::exchange(other.dimensions_, Vector2i::Zero());
  alphaType_ = std::exchange(other.alphaType_, AlphaType::Premultiplied);
  return *this;
}

void RendererGeodeTextureSnapshot::destroyOwnedBacking() noexcept {
  backing_.reset();
  borrowedGpuTexture_ = {};
  allocationDimensions_ = Vector2i::Zero();
  runtimeFormat_.reset();
}

RendererGeodeTextureSnapshot RendererGeodeTextureSnapshot::BorrowCurrentFrame(
    const gpu::Device& owner, const gpu::Texture& runtimeTexture, Vector2i dimensions) {
  RendererGeodeTextureSnapshot result;
  const gpu::Result<gpu::TextureDescriptor> descriptor = owner.textureDescriptor(runtimeTexture);
  if (descriptor.hasError() || !IsSnapshotFormat(descriptor.result().format)) {
    return result;
  }
  result.borrowedGpuTexture_ = gpu::Texture::CreateForBackend(
      runtimeTexture.slotIndex(), runtimeTexture.generation(), runtimeTexture.deviceId());
  result.allocationDimensions_ = SnapshotAllocationExtent(descriptor.result().size);
  result.runtimeFormat_ = descriptor.result().format;
  (void)result.setDimensions(dimensions);
  return result;
}

namespace {

/// The unit path bounds used by `objectBoundingBox` gradient coordinates,
/// matching the CPU-renderer helper.
const Box2d kUnitPathBounds(Vector2d::Zero(), Vector2d(1, 1));

/// Source UV rect that samples an entire texture.
const Box2d kWholeTextureUv(Vector2d::Zero(), Vector2d(1, 1));

/// Accept trigonometric roundoff only when it cannot move a mask edge appreciably in device pixels.
/// The coefficient bound alone is insufficient for large local regions.
bool IsMaskEdgeAxisAligned(double offAxis, double alongAxis, double localExtent) {
  constexpr double kCoefficientRoundoff = 4.0 * std::numeric_limits<double>::epsilon();
  constexpr double kMaximumDeviceDrift = 1e-9;
  return offAxis == 0.0 || (std::abs(offAxis) <= kCoefficientRoundoff * std::abs(alongAxis) &&
                            std::abs(offAxis * localExtent) <= kMaximumDeviceDrift);
}

/// Whether a mask region needs exact GPU clipping beyond its transformed bounding box.
/// @param maskBounds Optional mask region in local coordinates.
/// @param deviceFromLocal Transform from local coordinates to device pixels.
bool RequiresMaskRegionClip(const std::optional<Box2d>& maskBounds,
                            const Transform2d& deviceFromLocal) {
  if (!maskBounds) {
    return false;
  }

  const auto& data = deviceFromLocal.data;
  const bool axisAligned = (IsMaskEdgeAxisAligned(data[1], data[0], maskBounds->width()) &&
                            IsMaskEdgeAxisAligned(data[2], data[3], maskBounds->height())) ||
                           (IsMaskEdgeAxisAligned(data[0], data[1], maskBounds->width()) &&
                            IsMaskEdgeAxisAligned(data[3], data[2], maskBounds->height()));
  return !axisAligned;
}

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

bool IsBgraTextureFormat(gpu::TextureFormat format) {
  return format == gpu::TextureFormat::BGRA8Unorm;
}

// NOTE: `transformPath` now lives in the shared (text-gated) PlacedTextGeometry
// header so both backends share one definition. The
// only geode callers (glyph + decoration placement) are inside the
// DONNER_TEXT_ENABLED region.

#ifdef DONNER_TEXT_ENABLED
/// Whether \p glyph has a vector outline for Geode to draw: it is not `.notdef` and its face is
/// not bitmap-only.
bool HasVectorGlyph(const TextGlyph& glyph, const GlyphFaceScale& face) {
  return glyph.glyphIndex != 0 && !face.bitmapOnly;
}

std::optional<RendererTextMaterializationBudget::Cost> GlyphPredecodeCost(
    const FontManager::GlyphOutlineComplexity& complexity, std::size_t expansion = 1u) {
  const std::size_t kCommandCopiesPerVertex = 6u * expansion;
  const std::size_t kPointCopiesPerVertex = 9u * expansion;
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

// Every gradient draw the frame's stop budget admits fits one stop ramp, so the encoder's own
// refusal of a larger ramp is never what limits a document.
static_assert(RendererDrawBudget::kMaximumGradientStops <= geode::GeoEncoder::kMaxGradientRampStops,
              "a gradient the frame budget admits must fit one stop ramp");

std::optional<uint32_t> checkedFilterRasterDimension(double value) {
  if (!std::isfinite(value) || value <= 0.0 ||
      value > static_cast<double>(components::kMaximumFilterSurfaceDimension)) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(std::max(1.0, std::ceil(value)));
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
/// Path::strokeToFill. Arcs retain the same miter fallback as the tiny-skia backend.
LineJoin toLineJoin(StrokeLinejoin join) {
  switch (join) {
    case StrokeLinejoin::Miter: return LineJoin::Miter;
    case StrokeLinejoin::MiterClip: return LineJoin::MiterClip;
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

/// Whether a radial gradient's start and end circles count as identical, the one case in which a
/// canvas two-point gradient (which SVG 2 `fr` follows) paints nothing, using the tolerances of the
/// tiny-skia radial gradient the software renderer draws with.
/// For concentric circles (centers within 2^-15), tiny-skia treats radii within 2^-15 of each other
/// as identical circles; otherwise it draws a start radius of at most 2^-15 as an ordinary radial
/// gradient, and paints nothing when the radii are within 2^-12 of each other (its rejection of two
/// radii both at most 2^-12 falls inside that case).
/// Keep this identical in meaning to the software renderer's copy so both backends drop the same
/// draws.
bool radialCirclesCoincide(const Vector2d& center, double radius, const Vector2d& focalCenter,
                           double focalRadius) {
  constexpr double kDegenerateTolerance = 1.0 / (1 << 15);
  constexpr double kConcentricRadiusTolerance = 1.0 / (1 << 12);
  if (!NearZero((center - focalCenter).length(), kDegenerateTolerance)) {
    return false;
  }
  return NearEquals(radius, focalRadius, kDegenerateTolerance) ||
         (focalRadius > kDegenerateTolerance &&
          NearEquals(radius, focalRadius, kConcentricRadiusTolerance));
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

/// How a gradient resolver charges the stops it converts for one draw, and what it reports.
struct GradientStopCharge {
  /// Charges the draw's stops to the frame before any is converted; empty to convert without
  /// charging. Returns false when the frame's budget refuses them.
  std::function<bool(std::size_t)> reserve;
  /// Counts the stops converted; may be null.
  std::uint64_t* conversions = nullptr;
  /// Set when `reserve` refused the draw: nothing was converted and the draw must not happen.
  bool refused = false;
  /// Set when `reserve` charged the draw's stops.
  bool charged = false;

  /// Charges \p stops. @return Whether the draw may convert them.
  bool admit(std::size_t stops) {
    if (!reserve) {
      return true;
    }
    refused = !reserve(stops);
    charged = !refused;
    return charged;
  }
};

/// Translate the gradient's stop list into the encoder wire format, every stop included.
void buildGradientStops(const components::ComputedGradientComponent& computedGradient,
                        const css::RGBA currentColor, float opacity,
                        std::vector<geode::LinearGradientParams::Stop>& stopsStorage,
                        GradientStopCharge& charge) {
  const size_t stopCount = computedGradient.stops.size();
  if (charge.conversions != nullptr) {
    *charge.conversions += stopCount;
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

/// True when \p server references a pattern element.
bool paintReferenceIsPattern(const components::ResolvedPaintServer& server) {
  const auto* ref = std::get_if<components::PaintResolvedReference>(&server);
  return ref != nullptr && ref->reference.handle.all_of<components::ComputedPatternComponent>();
}

/// The solid color a text draw paints for \p server at \p opacity, or nullopt when it paints no
/// solid color. A paint server reference paints its server rather than its fallback color wherever
/// the text draw can paint that server: a gradient (one that cannot be drawn falls back where it is
/// drawn), or a pattern when the driver staged a tile for it, as \p patternStaged says: the tile
/// of a span's own pattern, or the text element's. Only a pattern reference with no staged tile
/// paints its fallback color.
std::optional<css::RGBA> TextSolidPaint(const components::ResolvedPaintServer& server,
                                        double opacity, const css::RGBA& currentColor,
                                        bool patternStaged) {
  const float alpha = static_cast<float>(opacity);
  if (const auto* solid = std::get_if<PaintServer::Solid>(&server)) {
    return solid->color.resolve(currentColor, alpha);
  }
  const auto* ref = std::get_if<components::PaintResolvedReference>(&server);
  if (ref == nullptr || !ref->fallback.has_value() || patternStaged ||
      paintReferenceIsGradient(server)) {
    return std::nullopt;
  }
  return ref->fallback->resolve(currentColor, alpha);
}
#endif  // DONNER_TEXT_ENABLED

/// Attempt to resolve a `PaintResolvedReference` into a concrete linear-gradient
/// draw specification for the Geode encoder. Returns `std::nullopt` for any
/// non-linear / malformed / degenerate gradient; the caller should fall back
/// to the gradient's fallback color or skip the draw.
std::optional<geode::LinearGradientParams> resolveLinearGradientParams(
    const components::PaintResolvedReference& ref, const Box2d& pathBounds, const Box2d& viewBox,
    const css::RGBA currentColor, float opacity,
    std::vector<geode::LinearGradientParams::Stop>& stopsStorage, GradientStopCharge& charge) {
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

  if (computedGradient->stops.empty() || !charge.admit(computedGradient->stops.size())) {
    return std::nullopt;
  }

  buildGradientStops(*computedGradient, currentColor, opacity, stopsStorage, charge);

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

/// A gradient paint server reference resolved against its geometry once, so that every draw it
/// paints shares one charge and one conversion of its stops. The parameters point into the stop
/// storage the resolution was given.
struct ResolvedGradientPaint {
  /// Set once the reference was resolved. A draw handed an unresolved paint resolves the
  /// reference itself.
  bool resolved = false;
  /// Set when the frame's stop budget refused the gradient, or the frame had already refused a
  /// draw: nothing is painted with it.
  bool refused = false;
  /// The linear gradient, when the reference is one.
  std::optional<geode::LinearGradientParams> linear;
  /// The radial resolution, when the reference is a radial gradient.
  std::optional<ResolvedRadialGradient> radial;
  /// The stop ramp the encoder bound for this resolution's draws, so every draw made from it in
  /// one encoder, such as each glyph of a text run, shares one ramp lookup. Filled by the encoder
  /// during a draw, hence mutable; reset with the resolution, whose stops it was looked up from.
  mutable geode::GradientRampCache rampCache;

  /// The linear parameters, or null.
  const geode::LinearGradientParams* linearParams() const {
    return linear.has_value() ? &*linear : nullptr;
  }
  /// The radial parameters, or null; null too for a zero-radius radial, which paints one color.
  const geode::RadialGradientParams* radialParams() const {
    return radial.has_value() && radial->gradient.has_value() ? &*radial->gradient : nullptr;
  }
  /// Whether the paint is a gradient the fill batch can carry.
  bool batchable() const { return linearParams() != nullptr || radialParams() != nullptr; }
};

/// The single color a zero-radius radial gradient paints, the last stop's.
ResolvedRadialGradient ZeroRadiusRadialFill(
    const components::ComputedGradientComponent& computedGradient, const css::RGBA currentColor,
    float opacity) {
  ResolvedRadialGradient out;
  const auto& lastStop = computedGradient.stops.back();
  const css::RGBA base = lastStop.color.resolve(currentColor, opacity);
  // Multiply in stop-opacity per SVG2 (stop-color * stop-opacity) and then
  // the overall paint opacity factor that `buildGradientStops` also honors.
  const double stopOpacity = std::clamp<double>(lastStop.opacity, 0.0, 1.0);
  out.solidFallback =
      css::RGBA(base.r, base.g, base.b, static_cast<uint8_t>(std::round(base.a * stopOpacity)));
  return out;
}

/// Whether a radial gradient with these radii may be drawn: neither radius is negative, and the
/// frame's budget admits its \p stops. Negative radii are invalid, unlike the valid zero-radius
/// last-stop case, so both are validated before either degeneracy rule and r=0 cannot hide an
/// invalid fr. The stops are charged before either degeneracy rule too, as on the CPU renderer, so
/// a zero-radius gradient, or one whose circles coincide, is charged like any other.
bool AdmitRadialGradient(double radius, double focalRadius, std::size_t stops,
                         GradientStopCharge& charge) {
  if (radius < 0.0 || focalRadius < 0.0) {
    return false;
  }
  return charge.admit(stops);
}

/// Same shape as @ref resolveLinearGradientParams, but for radial gradients.
/// Returns `nullopt` if the referenced entity isn't a radial gradient. If it
/// IS a radial gradient but is degenerate (zero outer radius), returns a
/// populated `solidFallback` with the last stop's color, matching the removed
/// full-Skia backend. If the focal and outer circles are identical, returns
/// an empty result (both fields unset) so the caller drops the draw.
std::optional<ResolvedRadialGradient> resolveRadialGradientParams(
    const components::PaintResolvedReference& ref, const Box2d& pathBounds, const Box2d& viewBox,
    const css::RGBA currentColor, float opacity,
    std::vector<geode::RadialGradientParams::Stop>& stopsStorage, GradientStopCharge& charge) {
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
  const double focalRadius =
      resolveGradientCoord(radial->fr, frame->coordBounds, frame->numbersArePercent);
  if (!AdmitRadialGradient(radius, focalRadius, computedGradient->stops.size(), charge)) {
    return ResolvedRadialGradient{};
  }
  // SVG2: a radius of zero collapses the gradient to a single point. Match
  // the removed full-Skia backend's behavior of painting a solid color equal to the LAST
  // stop in the gradient - this keeps elements visible for valid degenerate
  // radial gradients (e.g., `r="0"` with a single visible color).
  if (radius == 0.0) {
    return ZeroRadiusRadialFill(*computedGradient, currentColor, opacity);
  }

  const Vector2d center =
      resolveGradientCoords(radial->cx, radial->cy, frame->coordBounds, frame->numbersArePercent);
  // SVG 2: if `fx` / `fy` aren't specified, they coincide with `cx` / `cy`.
  // Resolved on the spot - keeps the geometry resolution local to the
  // shader's coordinate system.
  const Vector2d focalCenter =
      resolveGradientCoords(radial->fx.value_or(radial->cx), radial->fy.value_or(radial->cy),
                            frame->coordBounds, frame->numbersArePercent);

  // A start circle that contains the end circle still paints the whole plane: the circles are
  // nested, so the radial `t` solve in the gradient shaders finds exactly one root with a
  // non-negative radius at every point. Only identical circles paint nothing.
  if (radialCirclesCoincide(center, radius, focalCenter, focalRadius)) {
    return ResolvedRadialGradient{};
  }

  buildGradientStops(*computedGradient, currentColor, opacity, stopsStorage, charge);

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
  /// Free textures, handed out in the order the most recent frame took them.
  TextureHandOutQueue<gpu::Texture> free;
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
 *
 * A bucket hands its textures out in the order the most recent frame took them, whatever order
 * they came back in, so a frame that repeats the previous frame's acquisitions gets back the same
 * texture for each one. Caches keyed on texture identity, such as the filter engine's views and
 * bind groups, therefore stay valid across unchanged frames.
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
      bucket.free.drain([&](gpu::Texture&& texture) { destroyPooledTexture(std::move(texture)); });
    }
  }

  void beginFrame() {
    std::lock_guard lock(mutex_);
    ++currentFrameIndex_;
    forgetWorkingSetAfterIdle();
    recentFrameReleases_.push_back(std::exchange(releasedSinceFrameStart_, 0));
    if (recentFrameReleases_.size() > kBudgetWindowFrames) {
      recentFrameReleases_.pop_front();
    }
    lastActivityTime_ = clock_();
    const uint64_t workingSet =
        *std::max_element(recentFrameReleases_.begin(), recentFrameReleases_.end());
    budgetBytes_ = std::clamp(workingSet, kTexturePoolBudgetBytes, kMaximumTexturePoolBudgetBytes);
    evictStalePoolBuckets();
    (void)evictPoolEntriesToFit(0);
    // A texture kept out longer than a bucket stays alive comes back with no hand-out order.
    std::erase_if(handedOut_, [&](const auto& entry) {
      return currentFrameIndex_ - entry.second.frame > kBucketEvictAfterFrames;
    });
  }

  /**
   * Drop free textures to relieve memory pressure. \ref RendererGeodeTexturePoolTrim::ToFloor
   * forgets the recent working set and keeps at most the starting budget;
   * \ref RendererGeodeTexturePoolTrim::All drops every free texture. A trim also applies the idle
   * decay. Textures in use are never affected.
   */
  void trim(RendererGeodeTexturePoolTrim level) {
    std::lock_guard lock(mutex_);
    forgetWorkingSetAfterIdle();
    if (level == RendererGeodeTexturePoolTrim::IdleOnly) {
      (void)evictPoolEntriesToFit(0);
      return;
    }
    // Releases since the last frame began belong to the working set being forgotten too; the next
    // frame would otherwise record them and raise the budget again.
    recentFrameReleases_.clear();
    releasedSinceFrameStart_ = 0;
    budgetBytes_ = level == RendererGeodeTexturePoolTrim::All ? 0 : kTexturePoolBudgetBytes;
    (void)evictPoolEntriesToFit(0);
    budgetBytes_ = kTexturePoolBudgetBytes;
  }

  /// Replace the clock idle decay reads, for a test.
  void setClockForTesting(std::function<std::chrono::steady_clock::time_point()> clock) {
    std::lock_guard lock(mutex_);
    clock_ = std::move(clock);
    lastActivityTime_ = clock_();
  }

  [[nodiscard]] RendererGeodeTexturePoolStats stats() const {
    std::lock_guard lock(mutex_);
    RendererGeodeTexturePoolStats result;
    result.maximumBudgetBytes = kMaximumTexturePoolBudgetBytes;
    result.textureCount = pooledTextureCount_;
    result.bytes = pooledTextureBytes_;
    result.budgetBytes = budgetBytes_;
    for (const auto& [unusedKey, bucket] : buckets_) {
      (void)unusedKey;
      if (!bucket.free.empty()) {
        ++result.bucketCount;
      }
    }
    return result;
  }

  /// Called with a pooled texture's runtime identity just before the pool destroys it.
  using DestroyListener = std::function<void(const gpu::ResourceIdentity& texture)>;

  /**
   * Register \p listener to hear of every texture this pool destroys, before the destroy. A
   * renderer holding views of textures it returned to the pool releases them there, because any
   * renderer sharing the device can make the pool destroy them: by trimming it, by releasing a
   * texture that evicts others to fit the budget, or by beginning frames until a bucket goes stale.
   *
   * The listener runs with this pool's lock held, on the thread making that call, which the
   * device's one-thread-at-a-time contract serializes with the listener's own renderer. It must not
   * call back into this pool.
   *
   * @param listener Callback receiving the identity of each texture about to be destroyed.
   * @return Token for \ref removeDestroyListener.
   */
  uint64_t addDestroyListener(DestroyListener listener) {
    std::lock_guard lock(mutex_);
    const uint64_t token = ++destroyListenerTokens_;
    destroyListeners_.emplace(token, std::move(listener));
    return token;
  }

  /// Stop calling the listener registered under \p token.
  /// @param token Token \ref addDestroyListener returned.
  void removeDestroyListener(uint64_t token) {
    std::lock_guard lock(mutex_);
    destroyListeners_.erase(token);
  }

  /**
   * Tell every destroy listener that \p texture, a texture of this pool's device that the pool
   * does not own, is about to be destroyed, as \ref destroyPooledTexture tells them of its own. The
   * device's handle retirement calls this for each texture it releases, so a texture kept across
   * frames outside the pool, such as a decoded image's, is reported to every renderer on the device
   * the same way.
   *
   * @param texture Identity of the texture about to be destroyed.
   */
  void notifyTextureDestroyed(const gpu::ResourceIdentity& texture) {
    std::lock_guard lock(mutex_);
    for (const auto& [unusedToken, listener] : destroyListeners_) {
      (void)unusedToken;
      listener(texture);
    }
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
        gpu::Texture texture = bucket.free.takeNext();
        pooledTextureBytes_ -= textureByteSize(key);
        --pooledTextureCount_;
        recordHandOut(texture);
        return texture;
      }
    }

    gpu::Result<gpu::Texture> created = device_->runtimeDevice().createTexture(desc);
    if (created.hasError()) {
      return gpu::Texture{};
    }
    gpu::Texture texture = std::move(created).result();
    std::lock_guard lock(mutex_);
    recordHandOut(texture);
    return texture;
  }

  /// Returns \p texture to its bucket, or destroys it when a ceiling rejects it.
  /// @param texture Texture to pool; consumed either way.
  /// @param desc Descriptor \p texture was acquired with.
  void release(gpu::Texture texture, const gpu::TextureDescriptor& desc) {
    if (!texture.isValid()) {
      return;
    }

    std::lock_guard lock(mutex_);
    TextureHandOutStamp stamp;
    if (const auto handOut = handedOut_.find(IdentityOf(texture)); handOut != handedOut_.end()) {
      stamp = handOut->second;
      handedOut_.erase(handOut);
    }

    const RendererGeodeTextureKey key = RendererGeodeTextureKey::From(desc);
    const auto existingBucket = buckets_.find(key);
    const uint64_t textureBytes = textureByteSize(key);
    releasedSinceFrameStart_ += textureBytes;
    lastActivityTime_ = clock_();
    if (existingBucket != buckets_.end() &&
        existingBucket->second.free.size() >= kMaxPoolEntriesPerKey &&
        textureBytes * (existingBucket->second.free.size() + 1) > budgetBytes_ / 4) {
      destroyPooledTexture(std::move(texture));
      return;
    }

    if (!evictPoolEntriesToFit(textureBytes)) {
      destroyPooledTexture(std::move(texture));
      return;
    }

    RendererGeodeTextureBucket& bucket = buckets_[key];
    bucket.lastUsedFrame = currentFrameIndex_;
    bucket.free.push(std::move(texture), stamp);
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
  // Surfaces of one size, such as the region-sized captures of many filtered instances in one
  // frame, may exceed the entry cap up to a quarter of the current budget, so a frame of them
  // reuses its previous frame's surfaces instead of recreating all but the first few.
  /// Retention budget a pool starts with and never goes below.
  static constexpr uint64_t kTexturePoolBudgetBytes = 64u * 1024u * 1024u;
  // The budget follows the most bytes released in one recent frame, up to this ceiling, so a
  // frame's whole working set, such as one transient surface per filtered element, stays pooled
  // for the next frame instead of being destroyed and recreated each frame. Browsers on phones
  // reclaim a tab whose GPU process grows, so the Wasm ceiling stays close to the floor.
#ifdef __EMSCRIPTEN__
  static constexpr uint64_t kMaximumTexturePoolBudgetBytes = 96u * 1024u * 1024u;
#else
  static constexpr uint64_t kMaximumTexturePoolBudgetBytes = 256u * 1024u * 1024u;
#endif
  // A device with no frame beginning and no texture returned for this long forgets its working
  // set: the next frame or trim drops the budget to the floor and evicts free textures beyond it.
  // Returns at the end of a frame count as activity, so a renderer whose frames take longer, or
  // one that renders on demand, keeps its pool across pauses shorter than this.
  static constexpr std::chrono::seconds kIdleWorkingSetDecay{2};

  void forgetWorkingSetAfterIdle() {
    if (clock_() - lastActivityTime_ < kIdleWorkingSetDecay) {
      return;
    }
    recentFrameReleases_.clear();
    releasedSinceFrameStart_ = 0;
    budgetBytes_ = kTexturePoolBudgetBytes;
  }
  static constexpr uint64_t kBucketEvictAfterFrames = 120;
  // Frames of release history the budget follows. Frames count beginFrame calls of every
  // renderer sharing the device, so with several renderers the window spans fewer app frames. It
  // matches the stale-bucket age: a bucket unused for longer than the window was not part of any
  // frame the budget still counts.
  static constexpr std::size_t kBudgetWindowFrames =
      static_cast<std::size_t>(kBucketEvictAfterFrames);

  static uint64_t textureByteSize(const RendererGeodeTextureKey& key) {
    return static_cast<uint64_t>(key.width) * key.height * key.sampleCount *
           gpu::TextureFormatBytesPerTexel(key.format);
  }

  /// Destroys the backend object behind \p texture, not just this pool's name for it, after every
  /// destroy listener has released what it holds of it.
  ///
  /// Dropping the handle alone releases the runtime slot and leaves the backend texture resident
  /// until the host runtime collects it, which is exactly where a succession of evicted pool
  /// entries shows up as retained memory.
  ///
  /// Every caller holds \ref mutex_, except the destructor, which runs once no renderer holds the
  /// pool and so no listener remains.
  /// @param texture Texture to destroy; consumed.
  void destroyPooledTexture(gpu::Texture&& texture) {
    if (!texture.isValid()) {
      return;
    }
    const gpu::ResourceIdentity identity{texture.slotIndex(), texture.generation()};
    for (const auto& [unusedToken, listener] : destroyListeners_) {
      (void)unusedToken;
      listener(identity);
    }
    const gpu::Status destroyed =
        device_->runtimeDevice().destroyTextureBacking(std::move(texture));
    (void)destroyed;  // A texture this pool owns is always live; a stale handle is already gone.
  }

  void destroyPoolBucket(const RendererGeodeTextureKey& key, RendererGeodeTextureBucket& bucket) {
    const std::size_t bucketCount = bucket.free.size();
    bucket.free.drain([&](gpu::Texture&& texture) { destroyPooledTexture(std::move(texture)); });
    pooledTextureBytes_ -= textureByteSize(key) * static_cast<uint64_t>(bucketCount);
    pooledTextureCount_ -= bucketCount;
  }

  bool evictPoolEntriesToFit(uint64_t incomingBytes) {
    if (incomingBytes > budgetBytes_) {
      return false;
    }

    while (pooledTextureBytes_ > budgetBytes_ - incomingBytes) {
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

      destroyPooledTexture(victim->second.free.takeLast());
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

  /// Records that \p texture was handed out now, so its return can be ordered by it.
  void recordHandOut(const gpu::Texture& texture) {
    handedOut_[IdentityOf(texture)] = TextureHandOutStamp{currentFrameIndex_, ++handOutCount_};
  }

  static uint64_t IdentityOf(const gpu::Texture& texture) {
    return (uint64_t{texture.slotIndex()} << 32) | texture.generation();
  }

  std::shared_ptr<geode::GeodeDevice> device_;
  mutable std::mutex mutex_;
  std::map<RendererGeodeTextureKey, RendererGeodeTextureBucket> buckets_;
  /// When each texture now out of the pool was handed out, keyed by its runtime identity.
  std::unordered_map<uint64_t, TextureHandOutStamp> handedOut_;
  /// Hand-outs so far, the source of \ref TextureHandOutStamp::order.
  uint64_t handOutCount_ = 0;
  uint64_t pooledTextureBytes_ = 0;
  std::size_t pooledTextureCount_ = 0;
  uint64_t currentFrameIndex_ = 0;
  /// Bytes returned to the pool since the last \ref beginFrame.
  uint64_t releasedSinceFrameStart_ = 0;
  /// Bytes returned in each recent frame, newest last.
  std::deque<uint64_t> recentFrameReleases_;
  /// Current retention budget; see \ref kMaximumTexturePoolBudgetBytes.
  uint64_t budgetBytes_ = kTexturePoolBudgetBytes;
  /// Clock idle decay reads.
  std::function<std::chrono::steady_clock::time_point()> clock_ = [] {
    return std::chrono::steady_clock::now();
  };
  /// When a frame last began or a texture was last returned.
  std::chrono::steady_clock::time_point lastActivityTime_ = std::chrono::steady_clock::now();
  /// Listeners told of each texture before it is destroyed, by registration token.
  std::map<uint64_t, DestroyListener> destroyListeners_;
  /// Tokens handed out so far.
  uint64_t destroyListenerTokens_ = 0;
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
  // Textures the device's retirement releases, such as decoded images kept across frames, reach
  // the same listeners as the pool's own, so every renderer drops its views of them first.
  // One live pool per device (this registry), so this replaces only a listener whose pool is gone.
  const std::weak_ptr<RendererGeodeTexturePool> weakPool = pool;
  device->handleRetirement()->setTextureReleaseListener(
      [weakPool](const gpu::ResourceIdentity& texture) {
        if (const std::shared_ptr<RendererGeodeTexturePool> live = weakPool.lock()) {
          live->notifyTextureDestroyed(texture);
        }
      },
      [weakPool] { return !weakPool.expired(); });
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
  geode::FilterTilePlan fullPlan;
  std::optional<geode::FilterTilePlan> localPlan;
  /// Raster pixels from device pixels when the graph runs on a reduced-resolution raster, which
  /// the buffer fields then describe; absent at full resolution.
  std::optional<Transform2d> reducedRasterFromDevice;
  /// Device pixels per raster pixel along each axis: 1 at full resolution.
  std::uint32_t rasterReduction = 1;
};

struct GeodeFilterBuffer {
  Box2d region;
  int width = 0;
  int height = 0;
  int offsetX = 0;
  int offsetY = 0;
};

std::optional<GeodeFilterBuffer> MakeGeodeFilterBuffer(const std::optional<Box2d>& filterRegion,
                                                       const FilterInputBuffer& layout,
                                                       int viewportWidth, int viewportHeight) {
  const GeodeFilterBuffer result{
      filterRegion.value_or(Box2d(Vector2d::Zero(), Vector2d(viewportWidth, viewportHeight))),
      layout.width, layout.height, layout.offsetX, layout.offsetY};
  if (result.width <= 0 || result.height <= 0) {
    return std::nullopt;
  }
  return result;
}

struct GeodeLocalRasterGeometry {
  double scaleX = 1.0;
  double scaleY = 1.0;
  double blurPadding = 0.0;
  Box2d paddedRegion;
  uint32_t width = 0;
  uint32_t height = 0;
};

/// The local raster for a filter that takes the transformed path. It resamples the capture from
/// device space, placing the capture's texel (0, 0) at its buffer offset, so a capture with input
/// margins also works.
std::optional<GeodeLocalRasterGeometry> ComputeGeodeLocalRasterGeometry(
    const components::FilterGraph& filterGraph, const std::optional<Box2d>& filterRegion,
    const Transform2d& deviceFromFilter, bool fullExecutionFits) {
  if (!UsesTransformedBlurPath(filterGraph, filterRegion, deviceFromFilter, fullExecutionFits)) {
    return std::nullopt;
  }
  const Vector2d localScale = TransformedPathLocalScale(deviceFromFilter);
  const double scaleX = localScale.x;
  const double scaleY = localScale.y;
  const double blurPadding = ComputeTransformedBlurPadding(filterGraph);
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

std::optional<geode::FilterTilePlan> ComputeGeodeLocalFilterPlan(
    const components::FilterGraph& filterGraph, const std::optional<Box2d>& filterRegion,
    const Transform2d& deviceFromFilter, bool fullExecutionFits,
    const geode::GeodeFilterEngine& engine) {
  const auto geometry = ComputeGeodeLocalRasterGeometry(filterGraph, filterRegion, deviceFromFilter,
                                                        fullExecutionFits);
  if (!geometry) {
    return std::nullopt;
  }
  const auto plan = engine.executionPlan(filterGraph, geometry->width, geometry->height,
                                         Transform2d::Scale(geometry->scaleX, geometry->scaleY));
  uint64_t work = 0;
  uint64_t bytes = 0;
  if (!components::FilterGraphExecutionCost(filterGraph, plan.workPixels(),
                                            components::FilterMemoryModel::GpuAllNodes, work, bytes,
                                            plan.pixels(), plan.tiles)) {
    return std::nullopt;
  }
  return plan;
}

struct GeodeFilterWorkBounds {
  uint64_t workPixels = 0;
  uint64_t memoryPixels = 0;
  uint64_t executions = 0;
  uint64_t additionalTextureBytes = 0;

  void include(const geode::FilterTilePlan& plan) {
    workPixels = std::max(workPixels, plan.workPixels());
    memoryPixels = std::max(memoryPixels, plan.pixels());
    executions = std::max(executions, plan.tiles);
    additionalTextureBytes = std::max(additionalTextureBytes, plan.additionalTextureBytes());
  }
};

/// Execution, capture and surface sizes for one candidate filter input layout.
struct GeodeFilterPlan {
  GeodeFilterBuffer buffer;                        ///< Input raster layout.
  geode::FilterTilePlan plan;                      ///< Full-raster execution plan.
  std::optional<geode::FilterTilePlan> localPlan;  ///< Transformed blur path plan.
  bool fullExecutionFits = false;                  ///< Whether the full plan fits limits.
  GeodeFilterWorkBounds bounds;                    ///< Work over the plans that may run.
  uint64_t captureBytes = 0;                       ///< Capture texture bytes.
  uint64_t graphBytes = 0;                         ///< Graph intermediate bytes.
  std::size_t surfaces = 0;                        ///< Retained GPU surfaces.
  bool costKnown = false;                          ///< Whether the graph cost resolved.
};

/// Plan a filter layer over \p buffer without reserving anything.
/// Backdrop textures a filter capture keeps for BackgroundImage and BackgroundAlpha.
struct GeodeBackdropCapture {
  uint64_t bytes = 0;     ///< RGBA8 bytes of the backdrop copy and any tile buffer for it.
  uint64_t surfaces = 0;  ///< Surfaces those textures occupy.
};

/// Plans the backdrop copy with the capture's layout, and the tile buffer a tiled execution
/// copies each tile of it into, when \p filterGraph reads the backdrop.
GeodeBackdropCapture PlanGeodeBackdropCapture(const components::FilterGraph& filterGraph,
                                              const geode::FilterTilePlan& plan) {
  if (!components::StandardInputUsage(filterGraph).readsBackdrop()) {
    return {};
  }
  GeodeBackdropCapture capture{uint64_t{plan.width} * plan.height * 4, 1};
  if (plan.tiles > 1) {
    capture.bytes += plan.pixels() * 4;
    ++capture.surfaces;
  }
  return capture;
}

GeodeFilterPlan PlanGeodeFilter(const components::FilterGraph& filterGraph,
                                const std::optional<Box2d>& filterRegion,
                                const Transform2d& deviceFromFilter,
                                const GeodeFilterBuffer& buffer,
                                const geode::GeodeFilterEngine& engine, bool allowLocalPlan = true,
                                bool preferTiles = false) {
  GeodeFilterPlan result{buffer, engine.executionPlan(filterGraph, buffer.width, buffer.height,
                                                      deviceFromFilter, preferTiles)};
  uint64_t plannedWork = 0;
  uint64_t plannedBytes = 0;
  result.fullExecutionFits = components::FilterGraphExecutionCost(
      filterGraph, result.plan.workPixels(), components::FilterMemoryModel::GpuAllNodes,
      plannedWork, plannedBytes, result.plan.pixels(), result.plan.tiles);
  result.localPlan = allowLocalPlan
                         ? ComputeGeodeLocalFilterPlan(filterGraph, filterRegion, deviceFromFilter,
                                                       result.fullExecutionFits, engine)
                         : std::nullopt;
  if (result.fullExecutionFits || !result.localPlan) {
    result.bounds.include(result.plan);
  }
  if (result.localPlan) {
    result.bounds.include(*result.localPlan);
  }
  const uint64_t localPixels =
      result.localPlan ? uint64_t{result.localPlan->width} * result.localPlan->height : 0;
  const uint64_t bufferPixels =
      static_cast<std::uint64_t>(buffer.width) * static_cast<std::uint64_t>(buffer.height);
  const GeodeBackdropCapture backdrop = PlanGeodeBackdropCapture(filterGraph, result.plan);
  result.captureBytes =
      bufferPixels * 4 + localPixels * 4 + result.bounds.additionalTextureBytes + backdrop.bytes;
  uint64_t graphWork = 0;
  result.costKnown = components::FilterGraphExecutionCost(
      filterGraph, result.bounds.workPixels, components::FilterMemoryModel::GpuAllNodes, graphWork,
      result.graphBytes, result.bounds.memoryPixels, result.bounds.executions);
  if (result.costKnown) {
    components::GpuFilterWorkingSet layout;
    (void)components::ComputeGpuFilterWorkingSet(filterGraph, layout);
    const bool tiled = result.bounds.executions > 1;
    result.surfaces =
        layout.floatTextures + 2 + (result.localPlan ? 1 : 0) + (tiled ? 2 : 0) + backdrop.surfaces;
    result.surfaces +=
        std::count_if(filterGraph.nodes.begin(), filterGraph.nodes.end(), [](const auto& node) {
          return std::holds_alternative<components::filter_primitive::Image>(node.primitive);
        });
  }
  return result;
}

/// Whether the frame's GPU surface budget can hold \p plan, when its cost is known.
bool GeodeFilterSurfacesFit(const GeodeFilterPlan& plan,
                            const components::FilterExecutionBudget& budget,
                            const RendererSurfaceBudget& surfaceBudget) {
  return !plan.costKnown ||
         (budget.reservedGpuSurfaces() <= RendererSurfaceBudget::kMaximumSurfaces &&
          surfaceBudget.canReserveBytes(
              plan.captureBytes + plan.graphBytes + budget.retainedGpuBytes(),
              plan.surfaces + budget.reservedGpuSurfaces()));
}

/// Whether \p plan fits every frame budget, without reserving or latching anything.
bool GeodeFilterPlanFits(const components::FilterGraph& filterGraph, const GeodeFilterPlan& plan,
                         const components::FilterExecutionBudget& budget,
                         const RendererSurfaceBudget& surfaceBudget,
                         const geode::GeodeFilterEngine& engine) {
  return GeodeFilterSurfacesFit(plan, budget, surfaceBudget) &&
         budget.canReserve(filterGraph, plan.bounds.workPixels,
                           components::FilterMemoryModel::GpuAllNodes, plan.captureBytes,
                           engine.retainedBufferBytes(), plan.bounds.memoryPixels,
                           plan.bounds.executions, plan.surfaces);
}

/// Coarsest reduced filter raster: one raster pixel per 16 x 16 device pixels.
constexpr std::uint32_t kMaximumFilterRasterReduction = 16;

/// Why \p plan cannot be admitted at full resolution, or `RejectionReason::None` when it can.
components::FilterExecutionBudget::RejectionReason GeodeFilterPlanRefusal(
    const components::FilterGraph& filterGraph, const GeodeFilterPlan& plan,
    const components::FilterExecutionBudget& budget, const RendererSurfaceBudget& surfaceBudget,
    const geode::GeodeFilterEngine& engine) {
  if (!GeodeFilterSurfacesFit(plan, budget, surfaceBudget)) {
    return components::FilterExecutionBudget::RejectionReason::MemoryLimit;
  }
  return budget.refusalReason(filterGraph, plan.bounds.workPixels,
                              components::FilterMemoryModel::GpuAllNodes, plan.captureBytes,
                              engine.retainedBufferBytes(), plan.bounds.memoryPixels,
                              plan.bounds.executions, plan.surfaces);
}

/// Whether \p filterGraph can run on a reduced raster and still be the picture a canvas reduced by
/// the same factor shows. A backdrop copy and a pre-rasterized feImage fragment keep the device
/// grid, and a graph whose transform needs the transformed blur path takes that path on the
/// reduced canvas, which a device-axis blur on the reduced capture cannot reproduce.
bool CanRunOnReducedRaster(const components::FilterGraph& filterGraph,
                           const std::optional<Box2d>& filterRegion,
                           const Transform2d& deviceFromFilter) {
  if (components::StandardInputUsage(filterGraph).readsBackdrop() ||
      UsesTransformedBlurPath(filterGraph, filterRegion, deviceFromFilter,
                              /*fullExecutionFits=*/true)) {
    return false;
  }
  return std::none_of(filterGraph.nodes.begin(), filterGraph.nodes.end(), [](const auto& node) {
    const auto* image = std::get_if<components::filter_primitive::Image>(&node.primitive);
    return image != nullptr && image->isFragmentReference;
  });
}

/// Rounds \p value down to a multiple of \p step, for any sign. @param step Positive step.
int FloorToMultiple(int value, int step) {
  const int quotient = value / step;
  return (quotient * step > value ? quotient - 1 : quotient) * step;
}

/// Admits \p filterGraph on a raster of \p buffer reduced by the smallest power of two, up to
/// \ref kMaximumFilterRasterReduction, that fits every frame limit. The raster's pixels are the
/// pixels of the canvas reduced by that factor, covering the capture, and the graph runs in them,
/// so each resolution-dependent parameter is evaluated as for that reduced canvas. Nothing is
/// reserved when no reduction fits.
std::optional<GeodeFilterAdmission> AdmitReducedGeodeFilter(
    const components::FilterGraph& filterGraph, const std::optional<Box2d>& filterRegion,
    const Transform2d& deviceFromFilter, const GeodeFilterBuffer& buffer,
    components::FilterExecutionBudget& budget, const RendererSurfaceBudget& surfaceBudget,
    const geode::GeodeFilterEngine& engine) {
  if (!CanRunOnReducedRaster(filterGraph, filterRegion, deviceFromFilter)) {
    return std::nullopt;
  }
  for (std::uint32_t reduction = 2; reduction <= kMaximumFilterRasterReduction; reduction *= 2) {
    const int step = static_cast<int>(reduction);
    // The capture spans device [-offset, size - offset); the raster spans the reduced canvas
    // pixels that cover it, so its pixel grid is the reduced canvas's.
    const int left = FloorToMultiple(-buffer.offsetX, step);
    const int top = FloorToMultiple(-buffer.offsetY, step);
    const int right = -FloorToMultiple(buffer.offsetX - buffer.width, step);
    const int bottom = -FloorToMultiple(buffer.offsetY - buffer.height, step);
    const double scale = 1.0 / reduction;
    const Transform2d rasterFromDevice =
        Transform2d::Translate(-left, -top) * Transform2d::Scale(scale, scale);
    const GeodeFilterBuffer reduced{buffer.region, (right - left) / step, (bottom - top) / step, 0,
                                    0};
    const GeodeFilterPlan plan =
        PlanGeodeFilter(filterGraph, filterRegion, deviceFromFilter * rasterFromDevice, reduced,
                        engine, /*allowLocalPlan=*/false);
    if (!plan.costKnown ||
        GeodeFilterPlanRefusal(filterGraph, plan, budget, surfaceBudget, engine) !=
            components::FilterExecutionBudget::RejectionReason::None) {
      continue;
    }
    auto reservation = budget.reserve(filterGraph, plan.bounds.workPixels,
                                      components::FilterMemoryModel::GpuAllNodes, plan.captureBytes,
                                      engine.retainedBufferBytes(), plan.bounds.memoryPixels,
                                      plan.bounds.executions, plan.surfaces);
    if (!reservation.has_value()) {
      return std::nullopt;
    }
    return GeodeFilterAdmission{
        reduced.region, reduced.width, reduced.height,   0,        0, false, false, *reservation,
        plan.plan,      std::nullopt,  rasterFromDevice, reduction};
  }
  return std::nullopt;
}

/// Whether \p plan's capture and intermediates fit the frame's surface limit with nothing else
/// held, the most a submitted chunk can free.
bool FitsAnEmptyFrame(const GeodeFilterPlan& plan, const RendererSurfaceBudget& surfaceBudget) {
  const std::uint64_t limit = surfaceBudget.limits().bytes;
  return plan.captureBytes <= limit && plan.graphBytes <= limit - plan.captureBytes;
}

/// Reserves the frame's budgets for \p plan and returns its admission, or nothing when the budget
/// refuses it.
std::optional<GeodeFilterAdmission> ReserveGeodeFilter(const components::FilterGraph& filterGraph,
                                                       const GeodeFilterPlan& plan,
                                                       components::FilterExecutionBudget& budget,
                                                       const geode::GeodeFilterEngine& engine) {
  auto reservation = budget.reserve(filterGraph, plan.bounds.workPixels,
                                    components::FilterMemoryModel::GpuAllNodes, plan.captureBytes,
                                    engine.retainedBufferBytes(), plan.bounds.memoryPixels,
                                    plan.bounds.executions, plan.surfaces);
  if (!reservation.has_value()) {
    return std::nullopt;
  }
  return GeodeFilterAdmission{
      plan.buffer.region,      plan.buffer.width,   plan.buffer.height,
      plan.buffer.offsetX,     plan.buffer.offsetY, plan.localPlan.has_value(),
      !plan.fullExecutionFits, *reservation,        plan.plan,
      plan.localPlan};
}

/// Admits \p filterGraph after its full-resolution plan over \p buffer was refused: split into
/// tiles at full resolution when its sampling halo allows and the tiles fit, otherwise on a
/// reduced-resolution raster (\ref AdmitReducedGeodeFilter).
std::optional<GeodeFilterAdmission> AdmitPastTheFullPlan(const components::FilterGraph& filterGraph,
                                                         const std::optional<Box2d>& filterRegion,
                                                         const Transform2d& deviceFromFilter,
                                                         const GeodeFilterBuffer& buffer,
                                                         components::FilterExecutionBudget& budget,
                                                         const RendererSurfaceBudget& surfaceBudget,
                                                         const geode::GeodeFilterEngine& engine) {
  const GeodeFilterPlan tiled =
      PlanGeodeFilter(filterGraph, filterRegion, deviceFromFilter, buffer, engine,
                      /*allowLocalPlan=*/true, /*preferTiles=*/true);
  if (tiled.plan.tiles > 1 && tiled.costKnown &&
      GeodeFilterPlanRefusal(filterGraph, tiled, budget, surfaceBudget, engine) ==
          components::FilterExecutionBudget::RejectionReason::None) {
    return ReserveGeodeFilter(filterGraph, tiled, budget, engine);
  }
  return AdmitReducedGeodeFilter(filterGraph, filterRegion, deviceFromFilter, buffer, budget,
                                 surfaceBudget, engine);
}

/// Whether a graph refused at full resolution for \p refusal may run on a reduced raster: a memory,
/// work or estimate refusal may, except a memory refusal that a submitted chunk can lift while
/// \p mayRequestChunk.
bool ReducedRasterMayAdmit(components::FilterExecutionBudget::RejectionReason refusal,
                           bool mayRequestChunk) {
  using RejectionReason = components::FilterExecutionBudget::RejectionReason;
  switch (refusal) {
    case RejectionReason::MemoryLimit: return !mayRequestChunk;
    case RejectionReason::WorkLimit:
    case RejectionReason::InvalidGraph: return true;
    case RejectionReason::None:
    case RejectionReason::ExecutionLimit:
    case RejectionReason::External: return false;
  }
  return false;
}

/// Admits \p filterGraph at full resolution when it fits the frame's limits. A graph that does not
/// fit runs in tiles or on a reduced-resolution raster instead (\ref AdmitPastTheFullPlan), except
/// while
/// \p mayRequestChunk and the refusal is one a submitted chunk can lift: the caller then submits
/// the chunk and asks again with \p mayRequestChunk false.
std::optional<GeodeFilterAdmission> AdmitGeodeFilter(
    const components::FilterGraph& filterGraph, const std::optional<Box2d>& filterRegion,
    const Transform2d& deviceFromFilter, int viewportWidth, int viewportHeight,
    components::FilterExecutionBudget& budget, const RendererSurfaceBudget& surfaceBudget,
    const geode::GeodeFilterEngine& engine, bool mayRequestChunk) {
  // Prefer a raster over the part of the filter region the viewport can show, with the graph's
  // input margin, but never let the margin turn an admissible filter into a frame-wide rejection:
  // fall back to the viewport layout when that raster would not fit.
  const FilterInputBuffer expanded = ComputeRegionFilterInputBuffer(
      filterGraph, filterRegion, deviceFromFilter, viewportWidth, viewportHeight);
  const std::optional<GeodeFilterBuffer> buffer =
      MakeGeodeFilterBuffer(filterRegion, expanded, viewportWidth, viewportHeight);
  if (!buffer.has_value()) {
    budget.refuseOversizedGraph();
    return std::nullopt;
  }
  GeodeFilterPlan plan =
      PlanGeodeFilter(filterGraph, filterRegion, deviceFromFilter, *buffer, engine);
  const bool largerThanViewport =
      std::uint64_t{static_cast<uint32_t>(expanded.width)} *
          static_cast<uint32_t>(expanded.height) >
      std::uint64_t{static_cast<uint32_t>(viewportWidth)} * static_cast<uint32_t>(viewportHeight);
  if (largerThanViewport &&
      !GeodeFilterPlanFits(filterGraph, plan, budget, surfaceBudget, engine)) {
    const GeodeFilterBuffer viewport{buffer->region, viewportWidth, viewportHeight, 0, 0};
    plan = PlanGeodeFilter(filterGraph, filterRegion, deviceFromFilter, viewport, engine);
  }
  if (ReducedRasterMayAdmit(
          GeodeFilterPlanRefusal(filterGraph, plan, budget, surfaceBudget, engine),
          mayRequestChunk && FitsAnEmptyFrame(plan, surfaceBudget))) {
    if (std::optional<GeodeFilterAdmission> admitted = AdmitPastTheFullPlan(
            filterGraph, filterRegion, deviceFromFilter, *buffer, budget, surfaceBudget, engine)) {
      return admitted;
    }
  }
  if (!GeodeFilterSurfacesFit(plan, budget, surfaceBudget)) {
    budget.requireMemoryChunk();
    return std::nullopt;
  }
  return ReserveGeodeFilter(filterGraph, plan, budget, engine);
}

/// Views of pooled render-target textures that outlive the frame that made them, so a cached bind
/// group can keep naming the view of a texture the pool hands back each frame.
///
/// A view is keyed by its texture's slot and generation. It lives while the owning renderer's
/// frames keep using its texture and is dropped at the start of the second frame that does not,
/// after the frame that last bound it has been submitted. The textures themselves go back to the
/// pool at every frame end; the renderer drops a texture's view when the pool, or the device's
/// handle retirement, reports the texture destroyed (\ref forget), and drops every view when a
/// frame ends without submitting. \ref beginFrame and \ref forget report each view they drop
/// before it goes, so an owner can first release what it built on the view. A view owns no memory
/// of its own, so the entries are bounded by the textures one frame uses, which the frame's surface
/// budget bounds, and charged to no byte budget.
class RetainedTextureViews {
public:
  /// @param label Label of the views created.
  explicit RetainedTextureViews(const char* label) : label_(label) {}

  /// The view of \p texture, created on first use and kept for this frame and the next.
  /// @param runtime Device that owns \p texture.
  /// @param texture Texture to view.
  /// @return The view, or null when the runtime refuses one.
  const gpu::TextureView* view(gpu::Device& runtime, const gpu::Texture& texture) {
    const Key key{texture.slotIndex(), texture.generation()};
    Entry& entry = views_[key];
    if (!entry.view.isValid()) {
      gpu::Result<gpu::TextureView> created =
          runtime.createTextureView(texture, gpu::TextureViewDescriptor{label_});
      if (created.hasError()) {
        views_.erase(key);
        return nullptr;
      }
      entry.view = std::move(created).result();
    }
    entry.lastUsedFrame = frame_;
    return &entry.view;
  }

  /// Starts a frame: drops the views the finished frame did not use.
  /// @param onDrop Called with the runtime identity of each view before it is dropped.
  template <typename OnDrop>
  void beginFrame(OnDrop&& onDrop) {
    ++frame_;
    for (auto it = views_.begin(); it != views_.end();) {
      if (it->second.lastUsedFrame + 1 < frame_) {
        onDrop(IdentityOf(it->second.view));
        it = views_.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// Drops the view of \p texture, if one is kept. Called when the texture is about to be
  /// destroyed, so its view goes with it instead of lingering until the owner's frames stop using
  /// it.
  /// @param texture Runtime identity of the texture.
  /// @param onDrop Called with the runtime identity of the view before it is dropped.
  template <typename OnDrop>
  void forget(const gpu::ResourceIdentity& texture, OnDrop&& onDrop) {
    const auto it = views_.find(Key{texture.slotIndex, texture.generation});
    if (it != views_.end()) {
      onDrop(IdentityOf(it->second.view));
      views_.erase(it);
    }
  }

  /// Drops every view.
  void clear() { views_.clear(); }

  /// Number of views kept.
  std::size_t size() const { return views_.size(); }

  /// Runtime identity of \p view, as a bind group built on it records it.
  static gpu::ResourceIdentity IdentityOf(const gpu::TextureView& view) {
    return gpu::ResourceIdentity{view.slotIndex(), view.generation()};
  }

private:
  using Key = std::pair<uint32_t, uint32_t>;
  struct Entry {
    gpu::TextureView view;       //!< View of the texture.
    uint64_t lastUsedFrame = 0;  //!< Last frame that used the texture.
  };
  const char* label_;
  /// Views by texture slot and generation. A map, so a view's address stays valid until its entry
  /// is dropped at a frame start.
  std::map<Key, Entry> views_;
  /// The owning renderer's own frame count. Every renderer of a device advances the device's frame
  /// generation, so this, not that, measures how many of the owner's frames a view went unused.
  uint64_t frame_ = 0;
};

/// The GPU residence one device keeps for a document: its geometry slab, record slab and glyph
/// cache. Declared in destruction order last-first, so cached glyphs return their slab ranges
/// before the slabs go.
struct DocumentDeviceResidency {
  std::shared_ptr<geode::GeodeResidentSlab> geometry;  //!< Resident path geometry.
  std::shared_ptr<geode::GeodeRecordSlab> records;     //!< Painter-ordered instance records.
  std::shared_ptr<geode::GeodeGlyphCache> glyphs;      //!< Resident glyph outlines.
};

/**
 * Cached clip-mask geometry of one clipped element, kept on the entity whose
 * `ComputedClipPathsComponent` holds its clip paths (\ref ResolvedClip::sourceEntity).
 *
 * Each clip shape, by its index in the resolved clip, keeps its encode at one device-derived
 * tolerance, and per device a resident slot holding that encode's geometry, its mask uniform and
 * its mask bind group, so an unchanged frame fills the clip mask without encoding, uploading or
 * creating a bind group. Each draw compares a shape's path and device-derived tolerance with
 * those of its cached encode and encodes again on any difference, and a clip with a different
 * shape count drops every shape. The fill rule (the referencing element's clip-rule) and the
 * transform are not part of the encode; they reach the GPU through the mask uniform, which is
 * compared and rewritten every draw. The component is removed when the entity's computed clip
 * paths are created, replaced or destroyed, so its memory and budget charges go with its clip.
 *
 * Only draws from the live document reach it: a render snapshot records clips without their
 * source entity, so a replayed frame keeps nothing here.
 *
 * Bounded by the document: one entry per clip shape per clipped element. Encodes and their path
 * copies are charged to the document's cache-byte budget and resident geometry to its
 * resident-byte budget; a shape either budget refuses is filled from a per-frame encode or the
 * per-frame arena.
 */
struct GeodeClipPathCacheComponent {
  /// One clip shape's encode.
  struct Shape {
    std::optional<geode::EncodedPath> encode;  //!< Encode at \ref tolerance, if cached.
    /// Tolerance of \ref encode; zero for a path without curves, whose encode is the same at
    /// every scale.
    double tolerance = 0.0;
    /// The path \ref encode was built from. A draw of any other path, even one with the same
    /// command and point counts, is encoded again.
    Path path;
    /// Identity of \ref encode, unique within the component; a slot uploaded from another
    /// identity is stale.
    std::uint64_t generation = 0;
    geode::GeodeGeometryCacheReservation reservation;  //!< Charge for \ref encode.
  };

  /// One clip shape's resident mask fill on one device.
  struct MaskSlot {
    geode::GeodeResidentSlot slot;  //!< Geometry, mask uniform and mask bind group.
    std::uint64_t generation = 0;   //!< \ref Shape::generation \ref slot was uploaded from.
  };

  /// Resident mask fills of every shape on one device, by shape index.
  struct DeviceSlots {
    std::vector<MaskSlot> shapes;  //!< One per clip shape.
  };

  std::vector<Shape> shapes;                   //!< Encodes, by clip shape index.
  std::uint64_t encodeClock = 0;               //!< Last \ref Shape::generation handed out.
  geode::GeodePerDevice<DeviceSlots> devices;  //!< Resident mask fills per drawing device.
};
// Host storage per clip shape: one shape record, and one slot record per drawing device.
static_assert(sizeof(GeodeClipPathCacheComponent::Shape) <= 576,
              "a clip shape's host record grew past its stated bound");
static_assert(sizeof(GeodeClipPathCacheComponent::MaskSlot) <= 576,
              "a clip mask slot's host record grew past its stated bound");
static_assert(!std::is_copy_constructible_v<GeodeClipPathCacheComponent> &&
                  std::is_nothrow_move_constructible_v<GeodeClipPathCacheComponent> &&
                  std::is_nothrow_move_assignable_v<GeodeClipPathCacheComponent>,
              "the registry moves components when it compacts, and a component owns its slots");

/// Per-entity component types that keep state per device, and the sweep that drops gone devices
/// from all of them.
template <typename... Components>
struct PerDeviceComponents {
  /// Drops gone devices' entries from every entity's \p Components.
  static void DropGone(Registry& registry) { (DropGoneFrom<Components>(registry), ...); }

private:
  template <typename Component>
  static void DropGoneFrom(Registry& registry) {
    for (auto&& [entity, component] : registry.view<Component>().each()) {
      (void)component.devices.dropGone();
    }
  }
};

/// Every per-entity component that keeps state per device in a `geode::GeodePerDevice`. A new one
/// must be listed here, or a gone device's slots on it would keep that device's slabs alive.
using DocumentPerDeviceComponents =
    PerDeviceComponents<geode::GeodeResidentPathComponent,
                        geode::GeodeTextInstanceResidencyComponent,
                        geode::GeodeTextShapeResidencyComponent, geode::GeodeResidentImageComponent,
                        GeodeClipPathCacheComponent>;

/**
 * Drops, from every entity of \p registry, the residence slots of devices that are gone.
 *
 * Nothing else drops them: looking an entity's slots up drops nothing. Without this, an entity
 * would keep a gone device's slots, and through them its slabs and their charge to the document's
 * geometry budget, for as long as the document lives. Called where a gone device's document-level
 * residence is dropped, so once per gone device.
 *
 * @param registry Registry of the document.
 */
void DropGoneDevicesFromEntities(Registry& registry) {
  DocumentPerDeviceComponents::DropGone(registry);
}

/// Residence of every device that draws a document, kept in the document's registry context.
///
/// A document can be drawn by renderers on several devices - an editor's render worker and its
/// UI thread's thumbnail pass, say. Each device keeps its own entry: one device drawing the
/// document neither releases nor rebuilds another's, and none of it is destroyed on another
/// device's thread (see `geode::GeodeHandleRetirement`).
struct DocumentResidency {
  geode::GeodePerDevice<DocumentDeviceResidency> devices;  //!< Residence per drawing device.
};

/// The axis a set of boxes is swept along: their longer extent, so a text run in either writing
/// mode is swept along its advance.
struct BoxSweepAxis {
  bool alongY = false;  //!< True to sweep along y, false along x.

  /// Returns the axis along the longer side of the boxes' union.
  /// @param boxes Boxes to sweep.
  static BoxSweepAxis LongerExtentOf(std::span<const Box2d> boxes) {
    if (boxes.empty()) {
      return {};
    }
    Box2d extent = boxes.front();
    for (const Box2d& box : boxes) {
      extent.addBox(box);
    }
    return {.alongY = extent.height() > extent.width()};
  }

  /// Returns the low end of \p box along the sweep axis.
  double low(const Box2d& box) const { return alongY ? box.topLeft.y : box.topLeft.x; }

  /// Returns the high end of \p box along the sweep axis.
  double high(const Box2d& box) const { return alongY ? box.bottomRight.y : box.bottomRight.x; }

  /// Returns the low end of \p box across the sweep axis.
  double crossLow(const Box2d& box) const { return alongY ? box.topLeft.x : box.topLeft.y; }

  /// Returns the high end of \p box across the sweep axis.
  double crossHigh(const Box2d& box) const {
    return alongY ? box.bottomRight.x : box.bottomRight.y;
  }
};

/**
 * Groups boxes by sweeping them along the longer extent of their union. Each open group keeps the
 * union of its boxes' bounds, and a box joins every open group whose bounds it touches, merging
 * them. Boxes that touch, directly or through other boxes, therefore share a group; a group can
 * also take in a box that touches only the union of the group's bounds, not any one box of it.
 *
 * Open groups' bounds stay pairwise disjoint, and every open group reaches the sweep position, so
 * their extents across the sweep axis are disjoint intervals: the groups a box touches are one
 * contiguous run of them. Each box is tested against the groups it merges plus at most one more,
 * and every merge removes a group, so N boxes cost fewer than 2N tests, wherever the boxes are.
 */
class BoxSweepGrouping {
public:
  /// @param boxes Boxes to group. The grouping borrows them, so they must outlive it.
  explicit BoxSweepGrouping(std::span<const Box2d> boxes UTILS_LIFETIME_BOUND)
      : boxes_(boxes), axis_(BoxSweepAxis::LongerExtentOf(boxes)), parent_(boxes.size()) {
    std::iota(parent_.begin(), parent_.end(), std::size_t{0});
  }

  /**
   * Returns, for each box, a representative of its group.
   *
   * @param tests Incremented once for each box tested against an open group's bounds.
   */
  std::vector<std::size_t> groups(std::uint64_t& tests) {
    std::vector<std::size_t> order(boxes_.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(order, [&](std::size_t a, std::size_t b) {
      return axis_.low(boxes_[a]) < axis_.low(boxes_[b]);
    });
    for (const std::size_t index : order) {
      closeGroupsEndingBefore(axis_.low(boxes_[index]));
      add(index, tests);
    }
    for (std::size_t index = 0; index < parent_.size(); ++index) {
      parent_[index] = find(index);
    }
    return std::move(parent_);
  }

private:
  /// An open group: the union of its boxes' bounds and one of its boxes.
  struct Group {
    Box2d bounds;          //!< Union of the group's boxes.
    std::size_t member;    //!< Index of one box of the group.
    std::uint64_t serial;  //!< Identifies this group among the closing entries.
  };

  /// A group's sweep end, for closing it once the sweep passes it.
  struct Closing {
    double high;           //!< The group's high end along the sweep axis.
    double key;            //!< The group's key in \ref open_.
    std::uint64_t serial;  //!< The group's \ref Group::serial.

    bool operator>(const Closing& other) const { return high > other.high; }
  };

  std::size_t find(std::size_t index) {
    while (parent_[index] != index) {
      parent_[index] = parent_[parent_[index]];
      index = parent_[index];
    }
    return index;
  }

  /// Closes the open groups whose sweep end is before \p low; merged-away groups are skipped.
  void closeGroupsEndingBefore(double low) {
    while (!closing_.empty() && closing_.top().high < low) {
      const Closing entry = closing_.top();
      closing_.pop();
      const auto it = open_.find(entry.key);
      if (it != open_.end() && it->second.serial == entry.serial) {
        open_.erase(it);
      }
    }
  }

  /// Adds box \p index, merging it with every open group whose bounds it touches.
  void add(std::size_t index, std::uint64_t& tests) {
    Group merged{boxes_[index], index, nextSerial_++};
    // Every open group starts at or before this box and ends at or after it along the sweep, so
    // a group touches the box exactly when their extents across the sweep meet. Groups are sorted
    // by their low cross extent and disjoint, so walk left from the last one starting at or before
    // the box's high cross extent until one ends before the merged low cross extent.
    auto it = open_.upper_bound(axis_.crossHigh(merged.bounds));
    while (it != open_.begin()) {
      const auto previous = std::prev(it);
      ++tests;
      if (axis_.crossHigh(previous->second.bounds) < axis_.crossLow(merged.bounds)) {
        break;
      }
      merged.bounds.addBox(previous->second.bounds);
      parent_[find(previous->second.member)] = find(index);
      it = open_.erase(previous);
    }
    const double key = axis_.crossLow(merged.bounds);
    closing_.push({axis_.high(merged.bounds), key, merged.serial});
    open_.emplace(key, merged);
  }

  std::span<const Box2d> boxes_;  //!< The boxes passed to the constructor, borrowed.
  BoxSweepAxis axis_;
  std::vector<std::size_t> parent_;
  std::map<double, Group> open_;  //!< Open groups by their low cross extent.
  std::priority_queue<Closing, std::vector<Closing>, std::greater<Closing>> closing_;
  std::uint64_t nextSerial_ = 0;
};

/// Device-pixel distance for simplifying the convex clip intersection below. Two intersection
/// vertices closer than this are one vertex, and a vertex closer than this to the chord between its
/// neighbours is dropped, which moves the clip boundary by at most about this distance per removed
/// vertex. The shader tests each pixel centre against the clip's half-planes, so this can only
/// change a pixel whose centre lies within that distance of the boundary: the same order as the
/// shader's own plane tolerance (1e-4) and float rounding. It keeps near-parallel edges from
/// counting as extra edges through floating-point noise alone.
constexpr double kClipPolygonEpsilonPx = 1e-3;

/// Twice the signed area of \p polygon; its sign gives the winding.
double SignedDoubleArea(std::span<const Vector2d> polygon) {
  double area = 0.0;
  for (std::size_t i = 0; i < polygon.size(); ++i) {
    const Vector2d& a = polygon[i];
    const Vector2d& b = polygon[(i + 1) % polygon.size()];
    area += a.x * b.y - b.x * a.y;
  }
  return area;
}

/// Signed distance of \p point from the edge \p a to \p b, positive on the inside of a polygon
/// with winding \p sign.
double EdgeDistance(const Vector2d& a, const Vector2d& b, const Vector2d& point, double sign) {
  const Vector2d edge = b - a;
  const double length = edge.length();
  if (length <= kClipPolygonEpsilonPx) {
    return 0.0;
  }
  const Vector2d offset = point - a;
  return sign * (edge.x * offset.y - edge.y * offset.x) / length;
}

/// Vertices of a convex clip polygon, in fixed inline storage so that folding a clip into the
/// clip stack allocates nothing.
///
/// Bound: the clip stack only ever intersects a polygon of at most four vertices (the active
/// polygon, kept only while it has at most four) with a clip quad of four edges. One
/// Sutherland-Hodgman step emits at most two vertices per input vertex (the vertex itself when
/// inside, and one crossing), so four steps from four vertices emit at most 4 * 2^4 = 64. For an
/// exactly convex subject a step adds at most one vertex (at most eight in all), but rounding in
/// the distances of near-collinear vertices can change sign more than twice around the polygon,
/// so the storage is sized by the construction bound, which holds whatever the rounding.
class ClipVertices {
public:
  /// Vertices of the subject polygon before any step.
  static constexpr std::size_t kMaxSubjectVertices = 4;
  /// Half-planes the subject is clipped by.
  static constexpr std::size_t kMaxClipEdges = 4;
  /// The construction bound above.
  static constexpr std::size_t kCapacity = kMaxSubjectVertices << kMaxClipEdges;
  static_assert(kCapacity == 64,
                "Four Sutherland-Hodgman steps from four vertices emit at most 64.");

  ClipVertices() = default;
  explicit ClipVertices(std::span<const Vector2d> vertices) {
    UTILS_RELEASE_ASSERT(vertices.size() <= kMaxSubjectVertices);
    for (const Vector2d& vertex : vertices) {
      push(vertex);
    }
  }

  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  const Vector2d& operator[](std::size_t i) const { return vertices_[i]; }
  void clear() { size_ = 0; }

  void push(const Vector2d& vertex) {
    UTILS_RELEASE_ASSERT_MSG(size_ < kCapacity, "Clip polygon exceeds its construction bound");
    vertices_[size_++] = vertex;
  }

  /// Removes the vertex at \p index, keeping the order of the rest.
  void erase(std::size_t index) {
    for (std::size_t i = index + 1; i < size_; ++i) {
      vertices_[i - 1] = vertices_[i];
    }
    --size_;
  }

private:
  std::array<Vector2d, kCapacity> vertices_{};
  std::size_t size_ = 0;
};

/// Clips the convex \p subject by the half-plane inside the edge \p a to \p b into \p result
/// (Sutherland-Hodgman step).
void ClipByEdge(const ClipVertices& subject, const Vector2d& a, const Vector2d& b, double sign,
                ClipVertices& result) {
  result.clear();
  for (std::size_t i = 0; i < subject.size(); ++i) {
    const Vector2d& current = subject[i];
    const Vector2d& next = subject[(i + 1) % subject.size()];
    const double currentDistance = EdgeDistance(a, b, current, sign);
    const double nextDistance = EdgeDistance(a, b, next, sign);
    if (currentDistance >= 0.0) {
      result.push(current);
    }
    if ((currentDistance >= 0.0) != (nextDistance >= 0.0)) {
      const double t = currentDistance / (currentDistance - nextDistance);
      result.push(current + (next - current) * t);
    }
  }
}

/// Removes vertices of the convex \p polygon that coincide with the previous one or lie on the
/// chord between their neighbours, both within kClipPolygonEpsilonPx.
void SimplifyConvexPolygon(ClipVertices& polygon) {
  bool changed = true;
  while (changed && polygon.size() >= 3) {
    changed = false;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
      const Vector2d& previous = polygon[(i + polygon.size() - 1) % polygon.size()];
      const Vector2d& next = polygon[(i + 1) % polygon.size()];
      const bool coincident = (polygon[i] - previous).length() <= kClipPolygonEpsilonPx;
      if (coincident ||
          std::abs(EdgeDistance(previous, next, polygon[i], 1.0)) <= kClipPolygonEpsilonPx) {
        polygon.erase(i);
        changed = true;
        break;
      }
    }
  }
}

/// Returns the intersection of the convex polygons \p subject (at most
/// ClipVertices::kMaxSubjectVertices vertices) and \p clip (at most ClipVertices::kMaxClipEdges
/// edges), simplified by SimplifyConvexPolygon. Fewer than three vertices means the intersection
/// is empty. Allocates nothing.
ClipVertices IntersectConvexPolygons(std::span<const Vector2d> subject,
                                     std::span<const Vector2d> clip) {
  UTILS_RELEASE_ASSERT(clip.size() <= ClipVertices::kMaxClipEdges);
  const double sign = SignedDoubleArea(clip) < 0.0 ? -1.0 : 1.0;
  std::array<ClipVertices, 2> buffers{ClipVertices(subject), ClipVertices()};
  std::size_t current = 0;
  for (std::size_t i = 0; i < clip.size() && !buffers[current].empty(); ++i) {
    ClipByEdge(buffers[current], clip[i], clip[(i + 1) % clip.size()], sign, buffers[1 - current]);
    current = 1 - current;
  }
  SimplifyConvexPolygon(buffers[current]);
  return buffers[current];
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
                             public geode::FilterTextureAllocator,
                             public geode::FrameCommandBufferSink,
                             public geode::FilterImageTextureSource {
  bool verbose = false;
  bool antialias = true;
  std::function<void()> offscreenCreationHookForTesting;
  bool failFilterFrameSuspensionForTesting = false;
  /// Closes of the frame's recorded draws to let through before one fails, as a backend refusal
  /// would; empty when no failure is armed.
  std::optional<size_t> failFrameEncoderCloseForTesting;
  /// One-shot: the next filter-budget split runs without a budget rejection asking for it.
  bool forceFilterBudgetChunkForTesting = false;
  /// Filter graphs this frame ran on a reduced-resolution raster.
  std::uint64_t filterReducedRasters = 0;
  /// Filter graphs this frame ran at full resolution in more than one tile.
  std::uint64_t filterTiledGraphs = 0;
  /// Simple path clips proven disjoint from their active target this frame.
  std::uint64_t simpleClipCulls = 0;
  /// Device pixels per raster pixel of this frame's coarsest reduced filter raster, or 1.
  std::uint32_t filterRasterReduction = 1;
  bool failFilterFrameRestoreForTesting = false;
  bool frameRecordingAbandoned = false;
  bool frameArenaWriteFailed = false;
  bool failNextLocalFilterRasterAllocationForTesting = false;
  bool failNextTransformedResampleArenaWriteForTesting = false;

  // Per-frame perf counters. Reset at `beginFrame`, read via
  // `lastFrameTimings()`; `GeodePerf_tests.cc` pins their ceilings.
  geode::GeodeCounters counters;

  struct CounterOwner {
    Impl& renderer;
  };
  std::shared_ptr<CounterOwner> counterOwnerToken;
  std::weak_ptr<CounterOwner> offscreenCounterOwner;
  uint64_t counterOwnerFrame = 0;
  bool offscreen = false;

  std::shared_ptr<components::FilterExecutionBudget> filterExecutionBudget =
      std::make_shared<components::FilterExecutionBudget>();
  bool ownsFilterExecutionBudget = true;
  std::shared_ptr<RendererFilterPreparationBudget> filterPreparationBudget =
      std::make_shared<RendererFilterPreparationBudget>();
  bool ownsFilterPreparationBudget = true;
  /// Per-path encoder limits; tests lower them to reach the limit with small paths.
  geode::GeodePathEncoder::Limits pathEncoderLimits;
  std::shared_ptr<geode::GeodeFrameGeometryBudget> geometryBudget =
      std::make_shared<geode::GeodeFrameGeometryBudget>();
  bool ownsGeometryBudget = true;
  std::shared_ptr<RendererSurfaceBudget> surfaceBudget = std::make_shared<RendererSurfaceBudget>();
  bool ownsSurfaceBudget = true;
  std::shared_ptr<RendererTextMaterializationBudget> textMaterializationBudget =
      std::make_shared<RendererTextMaterializationBudget>();
  bool ownsTextMaterializationBudget = true;
  /// Charges the frame's gradient stops under the CPU renderer's per-frame cap. Only
  /// `gradientStops` is charged here.
  std::shared_ptr<RendererDrawBudget> drawBudget = std::make_shared<RendererDrawBudget>();
  bool ownsDrawBudget = true;
  std::size_t frameResourceScopeDepth = 0;
  std::shared_ptr<geode::GeodeDocumentGeometryBudget::Limits> documentGeometryLimits =
      std::make_shared<geode::GeodeDocumentGeometryBudget::Limits>();
  /// The documents' geometry budgets, as `resourceStats` reports them for the frame. Shared by a
  /// renderer and its offscreen instances, and reset with the owner's frame budgets. Each of them
  /// keeps its own list of the budgets its open frame touched (`touchedDocumentBudgets`) and
  /// settles only those here.
  struct DocumentGeometryFrameState {
    using Budget = geode::GeodeDocumentGeometryBudget;
    /// Budgets one renderer's open frame is drawing, held only until that frame ends.
    using Touched = std::vector<std::shared_ptr<Budget>>;

    /// What a document's budget reported when the last frame that drew it ended.
    struct Settled {
      /// Which budget. Compared by owner and never locked, so the budget is not kept alive.
      std::weak_ptr<Budget> budget;
      std::uint64_t retainedBytes = 0;  //!< Cache plus resident bytes.
      bool rejected = false;            //!< Whether it had refused a request.
      std::uint64_t rampBytes = 0;      //!< Resident bytes of kept gradient stop ramps.
    };
    std::vector<Settled> settled;

    /// The documents' retained bytes and whether any refused a request.
    struct Figures {
      std::uint64_t retainedBytes = 0;  //!< Summed, saturating.
      bool rejected = false;            //!< Whether any budget had refused a request.
      std::uint64_t rampBytes = 0;      //!< Kept gradient stop ramp bytes, summed.
    };

    void reset() { settled.clear(); }

    /// Record what each budget in \p touched reports and let go of it. Call when the frame that
    /// touched them ends, while the documents it drew are still held: afterwards the renderer
    /// neither reads those budgets without holding their documents nor drops the last reference to
    /// one on its own thread. Only that frame's own list: another renderer's frame may still be
    /// drawing the documents in its list, and may still give back what it borrowed from them.
    /// @param touched The ending frame's budgets; empty on return.
    void settle(Touched& touched) {
      for (const std::shared_ptr<Budget>& budget : touched) {
        Settled figures{budget, budget->cacheBytes() + budget->residentBytes(), budget->rejected(),
                        budget->rampBytes()};
        const auto existing =
            std::find_if(settled.begin(), settled.end(),
                         [&](const Settled& value) { return SameBudget(value.budget, budget); });
        if (existing != settled.end()) {
          *existing = std::move(figures);
        } else {
          settled.push_back(std::move(figures));
        }
      }
      touched.clear();
    }

    /// Settled figures for documents \p touched does not name, live ones for those it does.
    /// @param touched Budgets the asking renderer's open frame is drawing.
    Figures figures(const Touched& touched) const {
      Figures result;
      const auto add = [&result](std::uint64_t bytes, bool rejected, std::uint64_t rampBytes) {
        result.retainedBytes =
            bytes > std::numeric_limits<std::uint64_t>::max() - result.retainedBytes
                ? std::numeric_limits<std::uint64_t>::max()
                : result.retainedBytes + bytes;
        result.rejected = result.rejected || rejected;
        // Ramp bytes are a part of the retained bytes, so they cannot overflow before those do.
        result.rampBytes += std::min(rampBytes, bytes);
      };
      for (const Settled& document : settled) {
        const bool drawing = std::any_of(touched.begin(), touched.end(), [&](const auto& budget) {
          return SameBudget(document.budget, budget);
        });
        if (!drawing) {
          add(document.retainedBytes, document.rejected, document.rampBytes);
        }
      }
      for (const std::shared_ptr<Budget>& budget : touched) {
        add(budget->cacheBytes() + budget->residentBytes(), budget->rejected(),
            budget->rampBytes());
      }
      return result;
    }

  private:
    static bool SameBudget(const std::weak_ptr<Budget>& settledBudget,
                           const std::shared_ptr<Budget>& budget) {
      return !settledBudget.owner_before(budget) && !budget.owner_before(settledBudget);
    }
  };
  std::shared_ptr<DocumentGeometryFrameState> documentGeometryFrameState =
      std::make_shared<DocumentGeometryFrameState>();
  /// Budgets of the documents this renderer's open frame is drawing. This renderer's own, never
  /// shared with its offscreen instances, so each frame settles exactly the documents it drew.
  DocumentGeometryFrameState::Touched touchedDocumentBudgets;

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
  // When valid, `beginFrame()` renders into this texture instead of creating its own offscreen
  // target. Identity only: the host retains ownership and keeps it alive.
  gpu::Texture hostTarget;
  bool preserveTargetOnBeginFrame = false;

  // Texture format for all render targets. Matches the GeodeDevice's configured
  // format (RGBA8Unorm for headless, host-specified for embedded mode).
  gpu::TextureFormat textureFormat = gpu::TextureFormat::RGBA8Unorm;

  // Per-frame resources, recreated in `beginFrame`.
  RenderViewport viewport;
  int pixelWidth = 0;
  int pixelHeight = 0;
  // Dimensions of `target` as it was created. When the next `beginFrame`
  // comes in at the same size,
  // the textures are reused; otherwise they're reallocated.
  int targetWidth = 0;
  int targetHeight = 0;
  /// Names whatever the renderer is drawing into right now: the primary target, or the layer,
  /// mask, pattern or filter texture that has taken its place mid-frame. Identity only - the
  /// owner of each of those keeps it alive, and this never releases anything.
  gpu::Texture target;
  /// Primary render target this renderer allocated, held as the runtime handle that owns it.
  /// Null while the embedder supplies the target instead.
  gpu::Texture ownedTarget;
  std::optional<RendererGeodeTextureSnapshot> borrowedTargetSnapshot;

  // The frame's command buffers, in the order they must execute. endFrame submits all of them
  // as one submission, so every recorded render pass (base + every pushed layer / filter / mask)
  // runs in program order off one queue handoff. A frame is split across buffers wherever its
  // recording interleaves with something recording its own: each GeoEncoder retire closes one,
  // and a filter execution inside the frame adds the chunks it closes here.
  std::vector<gpu::CommandBuffer> frameCommandBuffers;

  // Whether a frame is open for recording. Distinct from `frameGpuEncoder`, which is null while
  // a filter executes between the frame's own encoders.
  bool frameRecordingOpen = false;

  // Most command buffers this renderer puts in one submission.
  //
  // Well below what the runtime refuses, because the wgpu backend acquires a backend command
  // buffer per element and commits none of them until the whole span has encoded, while the
  // backend queue caps how many may be outstanding at once and blocks the encoding thread at
  // that cap. Measured on the wgpu Metal path (2026-09-21): a span of 54 blocks indefinitely,
  // and 32 blocks as well, while 16 leaves room for the readback contexts and sibling renderers
  // sharing the same queue. Ordinary frames run to a handful of buffers, so only a pathological
  // one - a filter graph of thousands of passes - is split across submissions at all, and the
  // split keeps recording order because submissions execute in the order they are made.
  static constexpr size_t kMaxFrameCommandBuffersPerSubmission = 16;

  // Runtime command encoder the frame is recording into right now.
  //
  // A runtime encoder is single-use: what it records becomes a command buffer when it is
  // finished, and the frame keeps that buffer until it submits. Every encoder stays alive until
  // the frame ends: a finished one records nothing more, but the GeoEncoders that recorded
  // through it still hold a reference to it.
  std::deque<std::unique_ptr<gpu::CommandEncoder>> frameGpuEncoders;
  gpu::CommandEncoder* frameGpuEncoder = nullptr;

  std::unordered_set<std::shared_ptr<RendererGeodeTextureSnapshot::Backing>> frameSnapshotBackings;
  /// Consumer-local borrowed registrations; the retained backing owns the source texture.
  std::unordered_map<const RendererGeodeTextureSnapshot::Backing*, gpu::Texture>
      frameSnapshotImports;

  /// This context's registration of another context's snapshot for the rest of the frame, made
  /// from the export the snapshot took on its producer's thread, or null when it is refused.
  /// @param key Backing the registration is cached under for the frame.
  /// @param exported The snapshot's export.
  const gpu::Texture* registerFrameSnapshot(const RendererGeodeTextureSnapshot::Backing* key,
                                            const gpu::TextureExport& exported) {
    auto found = frameSnapshotImports.find(key);
    if (found == frameSnapshotImports.end()) {
      gpu::Result<gpu::Texture> registered =
          geode::RegisterOrderedTexture(device->runtimeDevice(), exported);
      if (registered.hasError()) {
        return nullptr;
      }
      found = frameSnapshotImports.emplace(key, std::move(registered).result()).first;
    }
    return &found->second;
  }

  /// Samples a prepared `feImage` texture in this frame: keeps its backing alive until the frame
  /// submits and registers it here when another context produced it. Filter preparation renders
  /// on this renderer's own device, so the registration is reached only by a graph that carries
  /// another context's texture (`FeImageTextureFromAnotherContextIsRegisteredBeforeSampling`).
  /// @param primitive Primitive whose `imageTexture` is set.
  const gpu::Texture* imageTexture(const components::filter_primitive::Image& primitive) override {
    if (!device || primitive.imageTexture == nullptr ||
        primitive.imageTexture->backend() != RendererTextureSnapshotBackend::Geode) {
      return nullptr;
    }
    const auto& snapshot =
        static_cast<const RendererGeodeTextureSnapshot&>(*primitive.imageTexture);
    if (!snapshot.canSampleWith(*device)) {
      return nullptr;
    }
    if (snapshot.backing_) {
      frameSnapshotBackings.insert(snapshot.backing_);
    }
    const gpu::Texture* source = snapshot.runtimeTexture();
    if (source->deviceId() == device->runtimeDevice().deviceId()) {
      return source;
    }
    return snapshot.backing_
               ? registerFrameSnapshot(snapshot.backing_.get(), snapshot.backing_->exported)
               : nullptr;
  }

  /// The view of clip-mask texture \p texture, kept across frames (see \ref clipMaskViews). Views
  /// cover the whole texture, so this addresses exactly the texels any other full-texture view of
  /// the same texture would.
  /// @return The view, or null when the runtime refuses one.
  const gpu::TextureView* clipMaskView(const gpu::Texture& texture) {
    return clipMaskViews.view(device->runtimeDevice(), texture);
  }

  /// The texture a finished filter execution composites, or null when it produced nothing.
  ///
  /// A declined graph composites the caller's own capture unchanged, so the outcomes differ in
  /// which texture is drawn, not in whether one is drawn.
  /// @param result Outcome of the execution.
  /// @param sourceGraphic Capture the execution ran against.
  static const gpu::Texture* filterOutput(const geode::FilterExecutionResult& result,
                                          const gpu::Texture& sourceGraphic) {
    switch (result.kind) {
      case geode::FilterExecutionResult::Kind::Failed: return nullptr;
      case geode::FilterExecutionResult::Kind::SourceGraphic: return &sourceGraphic;
      case geode::FilterExecutionResult::Kind::Output: return &result.texture;
    }
    return nullptr;
  }

  /// The descriptor the composited filter texture was allocated with.
  /// @param result Outcome of the execution.
  /// @param sourceGraphicDesc Descriptor of the capture the execution ran against.
  static const gpu::TextureDescriptor& filterOutputDesc(
      const geode::FilterExecutionResult& result, const gpu::TextureDescriptor& sourceGraphicDesc) {
    return result.kind == geode::FilterExecutionResult::Kind::Output ? result.desc
                                                                     : sourceGraphicDesc;
  }

  /// An identity-only name for \p texture: it resolves to the same resource but releases
  /// nothing, so whoever owns the texture stays the only thing that can free it.
  /// @param texture Handle to alias.
  static gpu::Texture aliasOf(const gpu::Texture& texture) {
    return gpu::Texture::CreateForBackend(texture.slotIndex(), texture.generation(),
                                          texture.deviceId());
  }

  /// Runtime name for whatever the renderer is drawing into right now.
  const gpu::Texture& activeTarget() const { return target; }

  /// The runtime's spelling of the renderer's surface format.
  gpu::TextureFormat gpuTextureFormat() const { return textureFormat; }

  /// Whether \p texture may be the source of a copy, which an embedder-supplied target only is
  /// when the embedder gave it that capability.
  /// @param texture Live runtime texture handle.
  [[nodiscard]] bool canCopyFrom(const gpu::Texture& texture) const {
    const gpu::Result<gpu::TextureDescriptor> descriptor =
        device->runtimeDevice().textureDescriptor(texture);
    return descriptor.hasResult() &&
           gpu::HasAllFlags(descriptor.result().usage, gpu::TextureUsage::CopySrc);
  }

  /// Extent of the active render target, in texels, as its owning device recorded it.
  gpu::Extent2d targetExtent() const {
    const gpu::Result<gpu::TextureDescriptor> descriptor =
        device->runtimeDevice().textureDescriptor(target);
    UTILS_RELEASE_ASSERT_MSG(descriptor.hasResult(),
                             "The active render target is not a live texture of this device");
    return descriptor.result().size;
  }

  /// Open a runtime encoder for the frame to record into.
  [[nodiscard]] bool openFrameGpuEncoder() {
    gpu::Result<std::unique_ptr<gpu::CommandEncoder>> created =
        device->runtimeDevice().createCommandEncoder();
    if (!created.hasResult()) {
      return false;
    }
    frameGpuEncoders.push_back(std::move(created).result());
    frameGpuEncoder = frameGpuEncoders.back().get();
    return frameGpuEncoder != nullptr;
  }

  /// Submit what the frame holds so far and let the backend finish it before recording resumes.
  ///
  /// Splitting a frame is a cross-submit edge for whatever recorded the buffers on either side of
  /// it: a pass in the next submission may sample a storage texture the submitted one wrote, and
  /// on hardware Vulkan the automatic barrier for that races the async queue and produces
  /// nondeterministic large-area corruption. The wait is bounded, and a timeout is reported as a
  /// failure so the frame is abandoned rather than recorded against unproven work.
  [[nodiscard]] bool splitFrameSubmission() {
    if (frameArenaWriteFailed) {
      return false;
    }
    if (frameCommandBuffers.empty()) {
      return true;  // Nothing was split, so there is no edge to wait across.
    }
    if (!submitFrameCommandBuffers()) {
      return false;
    }
    return !device->isVulkan() || device->waitForQueueIdle() == geode::GpuWaitResult::Complete;
  }

  /// Add \p commandBuffer to the frame, splitting the submission first when the frame has
  /// already reached \ref kMaxFrameCommandBuffersPerSubmission.
  [[nodiscard]] bool appendFrameCommandBuffer(gpu::CommandBuffer commandBuffer) override {
    if (frameArenaWriteFailed) {
      return false;
    }
    if (frameCommandBuffers.size() >= kMaxFrameCommandBuffersPerSubmission &&
        !splitFrameSubmission()) {
      return false;
    }
    frameCommandBuffers.push_back(std::move(commandBuffer));
    return true;
  }

  /// Submit every command buffer the frame has accumulated as one submission, in recording order.
  [[nodiscard]] bool submitFrameCommandBuffers() {
    if (frameArenaWriteFailed) {
      return false;
    }
    if (frameCommandBuffers.empty()) {
      return true;
    }
    if (!device->waitForFrameSubmissionRoom()) {
      return false;
    }
    gpu::Result<uint64_t> submitted = device->runtimeDevice().submit(frameCommandBuffers);
    frameCommandBuffers.clear();
    if (submitted.hasError()) {
      std::fprintf(stderr, "[Geode] submitting the frame's command buffers failed: %s\n",
                   submitted.error().message.c_str());
      return false;
    }
    return true;
  }

  /// Close what the frame's runtime encoder has recorded into a command buffer of the frame and
  /// open a fresh one. Call before anything else records a command buffer of its own, so the two
  /// streams reach the queue in the order they were written.
  [[nodiscard]] bool flushFrameGpuEncoder() { return closeFrameGpuEncoder(true); }

  /// Close the frame's runtime encoder into the frame's command buffers.
  /// @param reopen Whether to open a fresh encoder for the frame to keep recording into.
  [[nodiscard]] bool closeFrameGpuEncoder(bool reopen) {
    if (frameGpuEncoder == nullptr) {
      return true;
    }
    if (failFrameEncoderCloseForTesting.has_value()) {
      if (*failFrameEncoderCloseForTesting == 0) {
        failFrameEncoderCloseForTesting.reset();
        // Refuse in the state a real handover failure leaves behind: the encoder is gone, so a
        // caller that records afterwards without noticing has nothing to record through. Failing
        // with one still installed would be a state this never reaches in production.
        frameGpuEncoder = nullptr;
        return false;
      }
      --*failFrameEncoderCloseForTesting;
    }
    if (frameGpuEncoder->recordedCommandCount() == 0) {
      // Finishing an encoder that recorded nothing would spend one of the submission's command
      // buffers on a buffer with no work in it, and the frame closes its encoder at every
      // boundary whether or not anything was drawn between two of them.
      if (reopen) {
        return true;
      }
      frameGpuEncoder = nullptr;
      return true;
    }
    gpu::Result<gpu::CommandBuffer> commandBuffer = frameGpuEncoder->finish();
    if (commandBuffer.hasError()) {
      std::fprintf(stderr, "[Geode] closing the frame's recorded draws failed: %s\n",
                   commandBuffer.error().message.c_str());
      return false;
    }
    frameGpuEncoder = nullptr;
    if (!appendFrameCommandBuffer(std::move(commandBuffer).result())) {
      return false;
    }
    return !reopen || openFrameGpuEncoder();
  }

  /// Close the frame's own recording before a filter execution records command buffers of its
  /// own, so the source rendering the filter samples is ordered ahead of it.
  [[nodiscard]] bool suspendFrameForFilter() {
    if (frameArenaWriteFailed) {
      return false;
    }
    if (std::exchange(failFilterFrameSuspensionForTesting, false)) {
      discardFrameGpuEncoder();
      return false;
    }
    if (!frameRecordingOpen || !closeFrameGpuEncoder(false)) {
      discardFrameGpuEncoder();
      return false;
    }
    return true;
  }

  [[nodiscard]] bool frameUploadRefused() const override { return frameArenaWriteFailed; }

  /// Opens the fresh runtime encoder that records the post-filter composite.
  [[nodiscard]] bool restoreFrameAfterFilter() {
    if (std::exchange(failFilterFrameRestoreForTesting, false)) {
      return false;
    }
    if (!device || device->isDeviceLost()) {
      return false;
    }
    if (!frameRecordingOpen || !openFrameGpuEncoder() || device->isDeviceLost()) {
      discardFrameGpuEncoder();
      return false;
    }
    return true;
  }

  /// Abandon this frame, dropping everything it recorded without submitting any of it.
  void discardFrameGpuEncoder() {
    frameGpuEncoders.clear();
    frameGpuEncoder = nullptr;
    frameCommandBuffers.clear();
    frameRecordingOpen = false;
    frameSnapshotImports.clear();
    frameSnapshotBackings.clear();
    if (device) {
      device->drainDeferredTextureBackings();
      device->releaseRetiredHandles();
    }
  }

  void abandonFrameRecordingAfterFilterFailure(const gpu::Texture& savedTarget) {
    target = aliasOf(savedTarget);
    encoder.reset();
    discardFrameGpuEncoder();
    retainPendingFrameReleasesAfterFailure();
    frameRecordingAbandoned = true;
  }

  /// Abandon the frame where there is no outer target to go back to, leaving the same state as
  /// the filter-failure path: nothing submitted, and everything the frame recorded against
  /// retained rather than returned to a reusable pool.
  void abandonFrameRecording() {
    const gpu::Texture currentTarget = aliasOf(target);
    abandonFrameRecordingAfterFilterFailure(currentTarget);
  }

  void markDeviceLostUnlessArenaUploadFailed(const char* reason) {
    if (!frameArenaWriteFailed) {
      device->markDeviceLost(reason);
    }
  }

  std::unique_ptr<geode::GeoEncoder> encoder;
  std::vector<std::unique_ptr<geode::GeoEncoder>> frameFinishedEncoders;

  void retireFinishedEncoder(std::unique_ptr<geode::GeoEncoder> finishedEncoder) {
    if (finishedEncoder) {
      frameFinishedEncoders.push_back(std::move(finishedEncoder));
    }
  }

  /// Installs \p nextEncoder as the active one, keeping whatever it replaces alive until the
  /// frame ends. A GeoEncoder owns runtime handles - its target view above all - that its
  /// recorded draws reference, and the frame's submission re-validates every one of them, so one
  /// destroyed mid-frame would fail the submission of work it had already recorded.
  /// @param nextEncoder Encoder to make active, or null to leave none active.
  void replaceActiveEncoder(std::unique_ptr<geode::GeoEncoder> nextEncoder) {
    retireFinishedEncoder(std::move(encoder));
    encoder = std::move(nextEncoder);
  }

  void retireActiveEncoder() {
    if (!encoder) {
      return;
    }
    encoder->finish();
    if (frameArenaWriteFailed) {
      abandonFrameRecording();
      return;
    }
    // Close before the encoder's recorded draws can outlive what they name: the resources a
    // draw references stay alive only as long as whoever recorded it keeps them, and a
    // subsequent encoder is free to re-upload over them.
    const bool closed = flushFrameGpuEncoder();
    retireFinishedEncoder(std::move(encoder));
    if (!closed) {
      // The frame has no encoder to record into any more, and every later push and pop records
      // through one. Abandon here so those take their abandoned path instead of recording
      // against nothing.
      device->markDeviceLost("the frame's recorded draws could not be closed");
      abandonFrameRecording();
    }
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
    PatternPaintSlot() = default;
    ~PatternPaintSlot() = default;

    // Move-only, explicitly. The tile this slot owns is a move-only runtime handle, so a copy
    // could never have worked; deleting it matters because a container member can advertise a
    // copy constructor that fails only once instantiated, and `std::vector` reallocation's
    // `move_if_noexcept` reaches for that copy whenever the element's move is not `noexcept`.
    PatternPaintSlot(const PatternPaintSlot&) = delete;
    PatternPaintSlot& operator=(const PatternPaintSlot&) = delete;
    PatternPaintSlot(PatternPaintSlot&&) = default;
    PatternPaintSlot& operator=(PatternPaintSlot&&) = default;

    /// Pooled tile this paint samples. The slot owns it until a fill or stroke consumes the
    /// paint, or the frame ends without one.
    gpu::Texture tile;
    /// Descriptor \ref tile was acquired with, so releasing it finds the same pool bucket.
    gpu::TextureDescriptor tileDesc;
    /// View of \ref tile from \ref patternTileViews, which outlives the frame so a resident
    /// fill's cached bind group can keep naming it.
    gpu::TextureViewRef tileView;
    Vector2d rasterTileSize;
    Transform2d targetFromRaster;
  };
  static_assert(!std::is_copy_constructible_v<PatternPaintSlot>,
                "A copyable pattern paint slot would let a container copy its tile handle.");

  /// Pattern tiles staged for the text spans of the next text draw that paint with their own
  /// pattern, keyed by span pattern slot.
  struct SpanPatternSlots {
    SpanPatternSlots() = default;
    ~SpanPatternSlots() = default;
    /// @see PatternPaintSlot for why the copy operations are deleted rather than left implicit.
    SpanPatternSlots(const SpanPatternSlots&) = delete;
    SpanPatternSlots& operator=(const SpanPatternSlots&) = delete;
    SpanPatternSlots(SpanPatternSlots&&) noexcept = default;
    SpanPatternSlots& operator=(SpanPatternSlots&&) noexcept = default;

    std::unordered_map<std::uint64_t, PatternPaintSlot> fill;    ///< Tiles of span fills.
    std::unordered_map<std::uint64_t, PatternPaintSlot> stroke;  ///< Tiles of span strokes.
  };
  static_assert(!std::is_copy_constructible_v<SpanPatternSlots>,
                "Copyable span pattern slots would let a container copy their tile handles.");
  static_assert(std::is_nothrow_move_constructible_v<SpanPatternSlots>,
                "Span pattern slots move with the pattern stack frame that saves them.");

  struct PatternStackFrame {
    PatternStackFrame() = default;
    ~PatternStackFrame() = default;

    /// @see PatternPaintSlot for why the copy operations are deleted rather than left implicit.
    PatternStackFrame(const PatternStackFrame&) = delete;
    PatternStackFrame& operator=(const PatternStackFrame&) = delete;
    PatternStackFrame(PatternStackFrame&&) = default;
    PatternStackFrame& operator=(PatternStackFrame&&) = default;

    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    gpu::Texture savedTarget;
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
    Box2d tileRect;                 // In pattern space (topLeft at origin).
    Transform2d targetFromPattern;  // Transform used when the tile is sampled.
    gpu::Texture tileTexture;       // Pooled tile, sampled after recording.
    /// Descriptor \ref tileTexture was acquired with, carried to the paint slot it promotes to.
    gpu::TextureDescriptor tileDesc;
    /// See \ref PatternPaintSlot::tileView.
    gpu::TextureViewRef tileView;
    int tilePixelWidth = 0;
    int tilePixelHeight = 0;
    // Scale factor applied to all `setTransform` calls while this frame is
    // active, mapping pattern-tile units to tile-texture pixels so the
    // encoder's viewport math works out.
    Vector2d rasterScale = Vector2d(1.0, 1.0);
    /// Filter captures open when the tile began. They hold the target the tile interrupted, not
    /// the tile, so only captures opened inside the tile move what is drawn into it.
    std::size_t filterStackDepth = 0;

    // Pattern paints pending at `beginPatternTile` time, saved so tile-content draws don't
    // consume the outer element's pattern slots (e.g. a `context-fill` pattern shared between
    // several consumers re-rendering the same tile subtree). Restored by `endPatternTile`.
    std::optional<PatternPaintSlot> savedPatternFillPaint;
    /// @see savedPatternFillPaint
    std::optional<PatternPaintSlot> savedPatternStrokePaint;
    /// @see savedPatternFillPaint
    SpanPatternSlots savedSpanPatternPaints;
  };
  static_assert(!std::is_copy_constructible_v<PatternStackFrame>,
                "A copyable pattern stack frame would let vector growth copy its tile handle.");
  std::vector<PatternStackFrame> patternStack;

  /// Views of pattern tile textures, so a resident pattern fill's cached bind group can keep naming
  /// the view of the tile it samples.
  RetainedTextureViews patternTileViews{"RendererGeodePatternTileView"};
  /// Views of path-clip mask textures, so a resident draw under a clip, and a clip shape's
  /// resident mask fill intersected with a nested clip, keep their cached bind groups across
  /// frames. The pool hands a clip mask the texture it had the frame before, so an unchanged
  /// frame looks up the same view.
  RetainedTextureViews clipMaskViews{"RendererGeodeClipMaskView"};

  /// A view of pattern tile \p texture that outlives this frame, created on first use.
  /// @return The view, or null when the runtime refuses one.
  gpu::TextureViewRef patternTileView(const gpu::Texture& texture) {
    const gpu::TextureView* view = patternTileViews.view(device->runtimeDevice(), texture);
    return view != nullptr ? gpu::TextureViewRef(*view) : gpu::TextureViewRef();
  }

  /// Start a frame's bookkeeping of retained views: views the finished frame did not use are
  /// dropped.
  void beginFrameRetainedViews() {
    patternTileViews.beginFrame([](const gpu::ResourceIdentity&) {});
    clipMaskViews.beginFrame([this](const gpu::ResourceIdentity& view) {
      releaseCompositeBindingsOfClipMaskView(view);
    });
  }

  void promotePatternTile(PatternStackFrame& frame, bool forStroke,
                          std::optional<std::uint64_t> spanSlot) {
    PatternPaintSlot slot;
    slot.tile = std::move(frame.tileTexture);
    slot.tileDesc = frame.tileDesc;
    slot.tileView = frame.tileView;
    slot.rasterTileSize = Vector2d(frame.tilePixelWidth, frame.tilePixelHeight);
    slot.targetFromRaster = TargetFromPatternRaster(frame.targetFromPattern, frame.rasterScale);

    restoreSavedPatternPaints(frame);
    if (spanSlot.has_value()) {
      auto& tiles = forStroke ? spanPatternPaints.stroke : spanPatternPaints.fill;
      if (const auto found = tiles.find(*spanSlot); found != tiles.end()) {
        releaseTextureAtFrameEnd(std::move(found->second.tile), found->second.tileDesc);
        tiles.erase(found);
      }
      tiles.emplace(*spanSlot, std::move(slot));
      return;
    }
    std::optional<PatternPaintSlot>& targetSlot = forStroke ? patternStrokePaint : patternFillPaint;
    releasePatternPaintSlot(targetSlot);
    targetSlot = std::move(slot);
  }

  /// Put back the pattern paints that were pending when \p frame's tile began.
  void restoreSavedPatternPaints(PatternStackFrame& frame) {
    patternFillPaint = std::move(frame.savedPatternFillPaint);
    patternStrokePaint = std::move(frame.savedPatternStrokePaint);
    spanPatternPaints = std::move(frame.savedSpanPatternPaints);
  }

  /// Hand every tile of \p slots back to the pool and empty them.
  void releaseSpanPatternPaints(SpanPatternSlots& slots) {
    for (auto* tiles : {&slots.fill, &slots.stroke}) {
      for (auto& [slot, tile] : *tiles) {
        releaseTextureAtFrameEnd(std::move(tile.tile), tile.tileDesc);
      }
      tiles->clear();
    }
  }

  /// The staged pattern tile that run \p runIndex of \p text paints with for its fill or stroke:
  /// the tile in its span's pattern slot, or, when its span has none, the text element's own
  /// pattern. Null when that tile was not staged.
  const PatternPaintSlot* runPatternTile(const components::ComputedTextComponent& text,
                                         std::size_t runIndex, bool forStroke) const {
    const std::optional<std::uint64_t> slot =
        runIndex < text.spans.size() ? (forStroke ? text.spans[runIndex].strokePatternSlot
                                                  : text.spans[runIndex].fillPatternSlot)
                                     : std::nullopt;
    if (!slot.has_value()) {
      const std::optional<PatternPaintSlot>& element =
          forStroke ? patternStrokePaint : patternFillPaint;
      return element.has_value() ? &*element : nullptr;
    }
    const auto& tiles = forStroke ? spanPatternPaints.stroke : spanPatternPaints.fill;
    const auto found = tiles.find(*slot);
    return found != tiles.end() ? &found->second : nullptr;
  }

  /// Hand a pattern paint's tile back to the pool and empty the slot.
  ///
  /// The tile was drawn into by this frame's encoders, so it goes back at the frame boundary
  /// rather than immediately: a mid-frame release would let the next acquire of the same bucket
  /// hand it out before the GPU has finished writing it.
  /// @param slot Slot to empty; left disengaged.
  void releasePatternPaintSlot(std::optional<PatternPaintSlot>& slot) {
    if (!slot.has_value()) {
      return;
    }
    releaseTextureAtFrameEnd(std::move(slot->tile), slot->tileDesc);
    slot.reset();
  }

  /// Hand back the tiles of pattern state this frame did not consume.
  ///
  /// A promoted pattern paint is claimed by the very next fill or stroke, and an open tile is
  /// closed by the `endPatternTile` matching its `beginPatternTile`. A document can deliver
  /// neither. Carrying either into the next frame would name a texture that frame is free to
  /// recycle, so both are retired here and the state that held them is dropped.
  /// What the retirement does with the state the outermost open tile interrupted.
  enum class OuterPatternState {
    kRestore,  //!< Put the renderer back on it; the frame still has work to do against it.
    kDrop,     //!< Release it; the frame it belonged to is over and the next one brings its own.
  };

  void retireUnconsumedPatternTilesAtFrameEnd(OuterPatternState outer) {
    if (!patternStack.empty()) {
      // The innermost tile owns the live clip stack: `beginPatternTile` saved the outer one and
      // started the tile unclipped, so what is live now was pushed inside the tile.
      releaseClipStackTexturesAtFrameEnd(clipStack);
    }
    for (std::size_t index = 0; index < patternStack.size(); ++index) {
      PatternStackFrame& frame = patternStack[index];
      // The encoder this tile interrupted recorded draws the frame is still going to submit, and
      // it owns the target view they name, so it outlives the tile rather than the stack entry.
      retireFinishedEncoder(std::move(frame.savedEncoder));
      releaseTextureAtFrameEnd(std::move(frame.tileTexture), frame.tileDesc);
      releasePatternPaintSlot(frame.savedPatternFillPaint);
      releasePatternPaintSlot(frame.savedPatternStrokePaint);
      releaseSpanPatternPaints(frame.savedSpanPatternPaints);
      if (index != 0) {
        // Every saved stack but the outermost belongs to an enclosing tile, which is going away
        // with it. The outermost one is the renderer's own and is settled below.
        releaseClipStackTexturesAtFrameEnd(frame.savedClipStack);
      }
    }
    if (!patternStack.empty()) {
      PatternStackFrame& outermost = patternStack.front();
      if (outer == OuterPatternState::kRestore) {
        // Put the renderer back on the state the outermost tile interrupted, so the rest of this
        // frame does not read a texture that just went back to the pool.
        target = aliasOf(outermost.savedTarget);
        pixelWidth = outermost.savedPixelWidth;
        pixelHeight = outermost.savedPixelHeight;
        deviceFromLocalTransform = outermost.savedDeviceFromLocalTransform;
        deviceFromLocalTransformStack.clear();
        clipStack = std::move(outermost.savedClipStack);
      } else {
        // The next frame supplies its own viewport, transform and target, so restoring the
        // abandoned frame's here would render it at the wrong size under a stale clip.
        releaseClipStackTexturesAtFrameEnd(outermost.savedClipStack);
        target = gpu::Texture();
      }
    }
    patternStack.clear();
    releasePatternPaintSlot(patternFillPaint);
    releasePatternPaintSlot(patternStrokePaint);
    releaseSpanPatternPaints(spanPatternPaints);
  }

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
  // repeat-render case because layer sizes are derived from the
  // extents of the targets they draw into, which don't change
  // between idle re-renders. A size-bucketing extension is a future
  // follow-up for viewport-resize scenarios.
  // --------------------------------------------------------------------

  std::shared_ptr<RendererGeodeTexturePool> texturePool;

  [[nodiscard]] RendererGeodeTexturePoolStats texturePoolStats() const {
    return texturePool ? texturePool->stats() : RendererGeodeTexturePoolStats{};
  }

  /// Detach a superseded primary target and free its backing at the next frame boundary.
  ///
  /// Releasing it here would free the target while work already recorded against it is still
  /// unsubmitted. Hand it to the retirement mailbox instead, which holds it through the current
  /// frame boundary and then destroys the backing rather than only releasing the runtime slot -
  /// a dropped handle leaves the allocation for the host runtime to collect, which is how a
  /// succession of resized targets stays resident on runtimes, notably browsers, that collect
  /// lazily.
  void retireOwnedTargetAtFrameBoundary() {
    target = gpu::Texture();
    targetWidth = 0;
    targetHeight = 0;
    if (!ownedTarget.isValid()) {
      return;
    }

    if (!device) {
      ownedTarget = gpu::Texture();
      return;
    }
    // The mailbox refuses only a handle that belongs to another device, which a target allocated
    // through this one never is; dropping it here instead would release the slot and leave the
    // allocation behind.
    UTILS_RELEASE_ASSERT(device->deferDestroyTextureBacking(std::move(ownedTarget)));
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
    encoderToConfigure.setArenaWriteFailureFlag(&frameArenaWriteFailed);
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

  void inheritCounterOwner(Impl& owner) {
    if (!owner.counterOwnerToken) {
      owner.counterOwnerToken = std::make_shared<CounterOwner>(CounterOwner{owner});
    }
    offscreenCounterOwner = owner.counterOwnerToken;
    offscreen = true;
  }

  void captureOffscreenCounterOwner() {
    const auto owner = offscreenCounterOwner.lock();
    counterOwnerFrame =
        owner && owner->renderer.frameGenerationOpen ? owner->renderer.currentFrameIndex : 0;
  }

  void finishOffscreenFrameCounters(bool hadOpenFrame) {
    if (!offscreen || !hadOpenFrame) {
      return;
    }
    // A retained child can outlive its owner or finish after the owning frame has closed.
    if (const auto owner = offscreenCounterOwner.lock();
        owner && owner->renderer.frameGenerationOpen &&
        owner->renderer.currentFrameIndex == counterOwnerFrame) {
      owner->renderer.counters.add(counters);
    }
    device->removeCounters(counters);
  }

  /// Close this renderer's open frame generation, if any.
  void closeFrameGeneration() {
    if (frameGenerationOpen && device) {
      device->endFrameGeneration(currentFrameIndex);
    }
    frameGenerationOpen = false;
  }

  /// Charges one surface of \p size against the frame's surface budget.
  /// @param size Extent of the surface about to be allocated, in texels.
  [[nodiscard]] bool reserveTextureSurface(
      const gpu::Extent2d& size, gpu::TextureFormat format = gpu::TextureFormat::RGBA8Unorm) {
    if (!device || size.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        size.height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        !surfaceBudget->reserve(static_cast<int>(size.width), static_cast<int>(size.height), 1,
                                gpu::TextureFormatBytesPerTexel(format))) {
      return false;
    }
    return true;
  }

  /// Acquire a pooled texture matching `desc`, or create a fresh one
  /// on miss. Always increments the `textureCreates` counter on miss;
  /// never on hit. Returns an invalid texture on device failure.
  gpu::Texture acquireTexture(const gpu::TextureDescriptor& desc) {
    if (!texturePool) {
      return gpu::Texture{};
    }
    // A parked texture keeps its surface charge, which passes to the reuser.
    if (gpu::Texture parked = takeFrameReusableTexture(desc); parked.isValid()) {
      return parked;
    }
    if (!reserveTextureSurface(desc.size, desc.format)) {
      return gpu::Texture{};
    }
    return texturePool->acquire(desc);
  }

  /// Takes a texture this renderer parked in its open frame with exactly \p desc, if any.
  gpu::Texture takeFrameReusableTexture(const gpu::TextureDescriptor& desc) {
    const RendererGeodeTextureKey key = RendererGeodeTextureKey::From(desc);
    const auto reusable = std::find_if(
        frameReusableTextures.begin(), frameReusableTextures.end(),
        [&](const PendingRelease& r) { return RendererGeodeTextureKey::From(r.desc) == key; });
    if (reusable == frameReusableTextures.end()) {
      return gpu::Texture{};
    }
    gpu::Texture texture = std::move(reusable->texture);
    frameReusableTextures.erase(reusable);
    return texture;
  }

  gpu::Texture acquireFilterTexture(const gpu::TextureDescriptor& desc) override {
    return acquireTexture(desc);
  }

  /// Return a texture to its pool bucket IMMEDIATELY. Caller must
  /// pass the same descriptor used to acquire, otherwise the next
  /// acquire with the original descriptor will miss the bucket.
  ///
  /// Prefer `releaseTextureAtFrameEnd` for textures whose GPU work was
  /// recorded into one of the frame's command buffers: releasing those
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
                               static_cast<int>(desc.size.height), 1,
                               gpu::TextureFormatBytesPerTexel(desc.format));
    UTILS_RELEASE_ASSERT(releasedSurface);
    recycleOrDestroyTexture(std::move(texture), desc);
  }

  void recycleOrDestroyTexture(gpu::Texture texture, const gpu::TextureDescriptor& desc) {
    if (texturePool) {
      texturePool->release(std::move(texture), desc);
    } else {
      // No pool to return it to, so the backend object is destroyed here rather than left
      // resident until the host runtime collects it, after any retained view of it is released.
      releaseRetainedViewsOf(gpu::ResourceIdentity{texture.slotIndex(), texture.generation()});
      const gpu::Status destroyed =
          device->runtimeDevice().destroyTextureBacking(std::move(texture));
      (void)destroyed;
    }
  }

  /// Defer a release until after submission, or retain it if the frame's submission is uncertain.
  /// Layer, filter, mask, and clip textures may still be named by recorded commands.
  struct PendingRelease {
    gpu::Texture texture;
    gpu::TextureDescriptor desc;
  };
  std::vector<PendingRelease> framePendingReleases;
  std::vector<PendingRelease> failedFilterTextures;

  void drainFencedTextureBackings() {
    if (device) {
      device->drainFencedTextureBackings();
    }
  }

  void retainRecoverableTexture(gpu::Texture texture, const gpu::TextureDescriptor& desc) {
    // The device destroys it once the fence passes, outside the pool, so no retained view of it
    // may stay behind.
    releaseRetainedViewsOf(gpu::ResourceIdentity{texture.slotIndex(), texture.generation()});
    const uint64_t fence = device->runtimeDevice().lastSubmittedSerial();
    if (!device->deferDestroyTextureBackingAfterSerial(std::move(texture), fence)) {
      failedFilterTextures.push_back({std::move(texture), desc});
    }
  }

  void retainTextureAfterAbandonedFrame(gpu::Texture texture, const gpu::TextureDescriptor& desc) {
    if (frameArenaWriteFailed && device && !device->isDeviceLost()) {
      retainRecoverableTexture(std::move(texture), desc);
    } else {
      retainTextureWithoutCompletionProof(std::move(texture), desc);
    }
  }

  void releaseTextureAtFrameEnd(gpu::Texture texture, const gpu::TextureDescriptor& desc) {
    if (!texture.isValid()) {
      return;
    }
    if (frameRecordingAbandoned) {
      if (frameArenaWriteFailed && device && !device->isDeviceLost()) {
        retainRecoverableTexture(std::move(texture), desc);
      } else {
        failedFilterTextures.push_back({std::move(texture), desc});
      }
      return;
    }
    framePendingReleases.push_back({std::move(texture), desc});
  }

  void releaseFilterTextureAtFrameEnd(gpu::Texture texture,
                                      const gpu::TextureDescriptor& desc) override {
    // An execution releases its intermediates once its work is recorded; a later filter this
    // renderer records in the frame can take them, so many small filters share a few textures
    // instead of each holding its own until the frame ends. They reach the shared pool only after
    // the frame submits.
    releaseTextureWithinFrame(std::move(texture), desc);
  }

  /**
   * Make \p texture available to a later acquisition by this renderer in this frame, when every
   * command naming it is already recorded in this renderer's submission order: a later pass that
   * reuses it runs after them. The texture stays out of the device-shared pool, which another
   * renderer could take from and submit ahead of this frame, and which can destroy what it
   * cannot hold; it reaches the pool only once this frame is submitted. It stays charged to the
   * frame's surface budget while parked, since it is still live memory, and the charge passes to
   * its reuser.
   *
   * A texture a queue upload can write is deferred to the frame end instead, since an upload runs
   * ahead of recorded work that is not submitted yet; so is everything once the frame's recording
   * has failed, or while within-frame reuse is disabled for a test.
   */
  void releaseTextureWithinFrame(gpu::Texture texture, const gpu::TextureDescriptor& desc) {
    if (!texture.isValid()) {
      return;
    }
    if (!withinFrameReuseEnabled || frameRecordingAbandoned || frameArenaWriteFailed ||
        !frameRecordingOpen || gpu::HasAllFlags(desc.usage, gpu::TextureUsage::CopyDst)) {
      releaseTextureAtFrameEnd(std::move(texture), desc);
      return;
    }
    frameReusableTextures.push_back({std::move(texture), desc});
  }

  /// Textures this renderer released within its open frame, reusable only by its own later
  /// acquisitions until the frame is submitted. Each is still charged to the surface budget.
  std::vector<PendingRelease> frameReusableTextures;
  /// Whether \ref releaseTextureWithinFrame makes textures reusable within the frame.
  bool withinFrameReuseEnabled = true;

  /// Hand the frame's unreused within-frame textures to the pool, releasing their surface charge,
  /// once the frame is submitted; or retain them when its submission is uncertain, as the frame's
  /// other pending releases are.
  void drainFrameReusableTextures(bool submissionUncertain) {
    for (PendingRelease& reusable : frameReusableTextures) {
      if (!submissionUncertain) {
        releaseTexture(std::move(reusable.texture), reusable.desc);
      } else if (frameArenaWriteFailed && device && !device->isDeviceLost()) {
        retainRecoverableTexture(std::move(reusable.texture), reusable.desc);
      } else {
        failedFilterTextures.push_back(std::move(reusable));
      }
    }
    frameReusableTextures.clear();
  }

  /// Keep unresolved frame resources alive until device teardown instead of recycling them.
  void retainTextureWithoutCompletionProof(gpu::Texture texture,
                                           const gpu::TextureDescriptor& desc) override {
    if (texture.isValid()) {
      failedFilterTextures.push_back({std::move(texture), desc});
    }
  }

  void retainPendingFrameReleasesAfterFailure() {
    drainFrameReusableTextures(/*submissionUncertain=*/true);
    for (PendingRelease& pending : framePendingReleases) {
      if (frameArenaWriteFailed && device && !device->isDeviceLost()) {
        retainRecoverableTexture(std::move(pending.texture), pending.desc);
      } else {
        failedFilterTextures.push_back(std::move(pending));
      }
    }
    framePendingReleases.clear();
  }

  void drainPendingReleases() {
    drainFrameReusableTextures(/*submissionUncertain=*/false);
    for (auto& pending : framePendingReleases) {
      releaseTexture(std::move(pending.texture), pending.desc);
    }
    framePendingReleases.clear();
  }

  /// Whether a memory-limited filter budget is holding bytes it can only free once the work
  /// naming them has reached the queue.
  [[nodiscard]] bool filterBudgetNeedsChunkSubmit() const {
    return filterExecutionBudget->rejectionReason() ==
               components::FilterExecutionBudget::RejectionReason::MemoryLimit &&
           filterChunkFreesMemory();
  }

  /// Whether submitting the frame's work so far would free filter memory the budget still holds.
  [[nodiscard]] bool filterChunkFreesMemory() const {
    return filterExecutionBudget->activeGpuReservations() == 0 &&
           filterExecutionBudget->retainedGpuBytes() != 0;
  }

  /// Whether the frame can be split mid-frame to submit a filter budget chunk.
  [[nodiscard]] bool canSubmitFilterBudgetChunk() const {
    return device && frameRecordingOpen && target.isValid() && filterStack.empty();
  }

  /// Whether a filter refused for memory can be admitted again after \ref submitFilterBudgetChunk.
  [[nodiscard]] bool filterBudgetChunkAvailable() const {
    return filterExecutionBudget->executions() != 0 && canSubmitFilterBudgetChunk() &&
           filterChunkFreesMemory();
  }

  bool submitFilterBudgetChunk() {
    const bool forced = std::exchange(forceFilterBudgetChunkForTesting, false);
    if (!canSubmitFilterBudgetChunk() || (!forced && !filterBudgetNeedsChunkSubmit())) {
      return false;
    }

    retireActiveEncoder();
    if (frameRecordingAbandoned) {
      return false;  // Closing the frame's recorded draws failed, which abandoned the frame.
    }
    // A memory-limited filter budget only frees its retained bytes once the work holding them has
    // reached the queue, so this splits the frame the same way reaching the bound on one
    // submission does.
    if (!closeFrameGpuEncoder(false) || !splitFrameSubmission()) {
      // The frame has no encoder to record into any more, and every later push and pop records
      // through one. The caller reads a refusal here as "no room for this filter" and carries on
      // drawing, so the abandonment has to be declared rather than reported through the return.
      markDeviceLostUnlessArenaUploadFailed(
          "the frame's recorded draws could not be split mid-frame");
      abandonFrameRecording();
      return false;
    }
    frameGpuEncoders.clear();

    frameFinishedEncoders.clear();
    drainPendingReleases();

    if (!openFrameGpuEncoder()) {
      device->markDeviceLost("the frame's encoder could not be reopened after a mid-frame split");
      abandonFrameRecording();
      return false;
    }
    replaceActiveEncoder(std::make_unique<geode::GeoEncoder>(*device, *pipeline, *gradientPipeline,
                                                             *imagePipeline, activeTarget(),
                                                             targetExtent(), *frameGpuEncoder));
    configurePathEncoder(*encoder);
    encoder->setLoadPreserve();
    updateEncoderScissor();
    return filterExecutionBudget->beginChunkAfterSubmit();
  }

  /// Composite index of a composite that keeps nothing across frames, such as a placeholder layer.
  static constexpr size_t kNoComposite = std::numeric_limits<size_t>::max();

  /// A saved encoder + target state for an in-progress isolated layer
  /// (`pushIsolatedLayer` / `popIsolatedLayer`). When the driver begins a
  /// group with non-identity opacity or a non-Normal blend mode, we
  /// redirect subsequent draws into an offscreen texture of the same size
  /// as the current target. On `pop`, the offscreen is composited back
  /// onto the saved target with the stored opacity.
  struct LayerStackFrame {
    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    gpu::Texture savedTarget;
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
    /// Index into \ref layerComposites for this layer's composite.
    size_t compositeIndex = kNoComposite;
  };
  static_assert(!std::is_copy_constructible_v<LayerStackFrame>,
                "A copyable layer stack frame would let vector growth copy its texture handles.");
  std::vector<LayerStackFrame> layerStack;

  /**
   * The uniform buffers and bind groups one kind of full-target composite keeps from frame to
   * frame, indexed by the order the frame draws those composites in, which is stable while the
   * document is unchanged.
   *
   * The texture pool hands each size's textures out in the order the last frame took them, so an
   * unchanged frame composites the same textures in the same order and every binding still matches
   * (`LayerCompositesKeepTheirTexturesAcrossUnchangedFrames`). A changed document shifts the
   * indices; a composite then finds different textures or uniforms and rebuilds, which is correct,
   * only not free.
   *
   * A sequence keeps at most \ref kMaxEntries entries; composites past that in one frame allocate
   * their uniform and bind group for that frame only. The surface budget does not bound how many
   * composites a frame draws: sibling layers take turns with one surface, and a frame split by a
   * memory-limited filter budget returns the surfaces of the layers it already composited
   * (`SiblingLayersPastTheSurfaceBudgetAreAllAdmitted`,
   * `LayerCompositeRetainedStateIsCappedAcrossFrameSplits`). Entries the frame did not reach are
   * dropped once it is submitted, and every entry is released when the texture pool is trimmed to
   * nothing (`LayerCompositeEntriesFollowTheLastFrame`). Entries release their views before any
   * texture they view is destroyed: the pool reports each texture to \ref releaseRetainedViewsOf
   * before destroying it, whichever renderer's call caused that
   * (`RetainedCompositesReleaseViewsBeforeThePoolDestroysTheirTextures`); a texture destroyed
   * outside the pool, after a failed frame or with no pool, is reported the same way; and a frame
   * that ends without submitting releases every entry's views
   * (`RetainedCompositesReleaseViewsWhenAFrameIsAbandoned`). So an entry's view destroy is always
   * requested before its texture's; the runtime defers both past the submissions using them, and
   * it refuses any use of a view whose texture is gone in any case.
   */
  struct RetainedCompositeSequence {
    /// Most entries one sequence keeps; composites past it in a frame draw with per-frame state.
    /// The image blit sequence uses the same cap for its kept-texture blits.
    static constexpr size_t kMaxEntries = RendererSurfaceBudget::kMaximumSurfaces;

    struct Entry {
      geode::GeodeTextureEncoder::RetainedQuadBinding binding;
      /// The texture this frame's composite drew from; empty until it draws.
      std::optional<gpu::ResourceIdentity> texture;
    };
    static_assert(!std::is_copy_constructible_v<Entry> &&
                      std::is_nothrow_move_constructible_v<Entry>,
                  "Vector growth must move entries, never copy their GPU handles.");
    /// Host bytes one entry may take. With \ref kMaxEntries entries per sequence this bounds the
    /// host storage a sequence retains, and a renderer keeps four sequences.
    static constexpr size_t kMaxEntryHostBytes = 512;
    static_assert(sizeof(Entry) <= kMaxEntryHostBytes);

    std::vector<Entry> entries;
    /// Bindings of dropped entries, holding only their uniform buffers, for the next entries a
    /// frame claims. A renderer whose frames alternate between documents, or between a
    /// compositor's layers, drops the entries of the frames that draw no such composite; reusing
    /// their buffers keeps the next frame that does from creating them again. Together with
    /// \ref entries they never exceed \ref kMaxEntries.
    std::vector<geode::GeodeTextureEncoder::RetainedQuadBinding> spareBindings;
    /// Entries claimed so far in the open frame.
    size_t claimedThisFrame = 0;

    /// Claim the next entry for a composite of this frame, or \ref kNoComposite past
    /// \ref kMaxEntries.
    size_t claim() {
      if (claimedThisFrame >= kMaxEntries) {
        return kNoComposite;
      }
      const size_t index = claimedThisFrame++;
      if (index >= entries.size()) {
        entries.resize(index + 1);
        if (!spareBindings.empty()) {
          entries[index].binding = std::move(spareBindings.back());
          spareBindings.pop_back();
        }
      }
      entries[index].texture.reset();
      return index;
    }

    /// The binding entry \p index draws with, after recording \p texture as its source.
    geode::GeodeTextureEncoder::RetainedQuadBinding* binding(size_t index,
                                                             const gpu::Texture& texture) {
      if (index == kNoComposite) {
        return nullptr;
      }
      entries[index].texture = gpu::ResourceIdentity{texture.slotIndex(), texture.generation()};
      return &entries[index].binding;
    }

    /// Drop the entries the frame did not reach, keeping their uniform buffers as spares. Only once
    /// nothing recorded can still be submitted: a dropped entry's bind group must not be named by a
    /// pending submission.
    void dropUnclaimed() {
      while (entries.size() > claimedThisFrame) {
        entries.back().binding.releaseTextureViews();
        spareBindings.push_back(std::move(entries.back().binding));
        entries.pop_back();
      }
    }

    /// Start counting a new frame's entries, dropping any the last frame did not reach.
    void beginFrame() {
      dropUnclaimed();
      claimedThisFrame = 0;
    }

    /// Release every entry and spare buffer. Only while no frame is recording.
    void clear() {
      entries.clear();
      spareBindings.clear();
      claimedThisFrame = 0;
    }

    /// Release the views and bind group of every entry viewing \p texture, keeping its uniform
    /// buffer.
    void releaseViewsOf(const gpu::ResourceIdentity& texture) {
      for (Entry& entry : entries) {
        if (entry.binding.viewsTexture(texture)) {
          entry.binding.releaseTextureViews();
        }
      }
    }

    /// Release the views and bind group of every entry, keeping its uniform buffer.
    void releaseAllViews() {
      for (Entry& entry : entries) {
        entry.binding.releaseTextureViews();
      }
    }

    /// Release the views and bind group of every entry whose bind group names the clip-mask view
    /// \p view, keeping its uniform buffer. The renderer keeps clip-mask views across frames, so
    /// an unchanged frame reuses these bind groups; it calls this before it drops a view.
    void releaseBindingsOfClipMaskView(const gpu::ResourceIdentity& view) {
      for (Entry& entry : entries) {
        if (entry.binding.bindsClipMaskView(view)) {
          entry.binding.releaseTextureViews();
        }
      }
    }

    /// Entries whose binding holds a texture view or a bind group.
    size_t viewHolderCount() const {
      return static_cast<size_t>(std::count_if(entries.begin(), entries.end(), [](const Entry& e) {
        return e.binding.holdsTextureViews();
      }));
    }

    /// The source of each entry claimed in the last frame, in claim order.
    std::vector<std::optional<gpu::ResourceIdentity>> textures() const {
      std::vector<std::optional<gpu::ResourceIdentity>> result;
      for (size_t i = 0; i < std::min(entries.size(), claimedThisFrame); ++i) {
        result.push_back(entries[i].texture);
      }
      return result;
    }
  };
  /// Composites of the isolated layers pushed in a frame, in push order.
  RetainedCompositeSequence layerComposites;
  /// Composites of the filter results drawn in a frame, in draw order.
  RetainedCompositeSequence filterOutputComposites;
  /// Blits of the image textures kept across frames that a frame draws, in draw order. A kept
  /// texture is written only when it is created, so an entry that finds the same texture samples
  /// the pixels it was built for.
  RetainedCompositeSequence imageBlits;
  /// Composites of the `<mask>` contents a frame draws through their masks, in the order the frame
  /// pops the masks. Both textures of such a composite are drawn by the frame that composites them.
  RetainedCompositeSequence maskComposites;
  /// Set by a pool trim to nothing while a frame records; the frame's end then releases every
  /// retained composite entry.
  bool releaseRetainedCompositesAtFrameEnd = false;

  /// Every retained composite sequence of this renderer.
  std::array<RetainedCompositeSequence*, 4> retainedCompositeSequences() {
    return {&layerComposites, &filterOutputComposites, &imageBlits, &maskComposites};
  }

  /// Release every retained composite entry. Only while no frame is recording.
  void clearRetainedComposites() {
    for (RetainedCompositeSequence* sequence : retainedCompositeSequences()) {
      sequence->clear();
    }
  }

  /// Start counting a new frame's entries in every sequence, dropping any the last frame did not
  /// reach.
  void beginFrameRetainedComposites() {
    for (RetainedCompositeSequence* sequence : retainedCompositeSequences()) {
      sequence->beginFrame();
    }
  }

  /// Release every retained composite entry now, or at the open frame's end if one is recording.
  void releaseRetainedComposites() {
    if (frameRecordingOpen) {
      releaseRetainedCompositesAtFrameEnd = true;
      return;
    }
    clearRetainedComposites();
  }

  /// After the frame's submission: drop the entries it did not reach, and release every entry if a
  /// pool trim asked for it during the frame. A frame that was abandoned releases every entry's
  /// views first: what it touched is retained or destroyed outside the pool.
  void finishRetainedCompositeFrame() {
    if (std::exchange(releaseRetainedCompositesAtFrameEnd, false)) {
      clearRetainedComposites();
      return;
    }
    if (frameRecordingAbandoned || frameArenaWriteFailed) {
      releaseAllRetainedViews();
    }
    for (RetainedCompositeSequence* sequence : retainedCompositeSequences()) {
      sequence->dropUnclaimed();
    }
  }

  /// Release the bind group of every retained composite that names the clip-mask view \p view,
  /// which \ref clipMaskViews is about to drop.
  void releaseCompositeBindingsOfClipMaskView(const gpu::ResourceIdentity& view) {
    for (RetainedCompositeSequence* sequence : retainedCompositeSequences()) {
      sequence->releaseBindingsOfClipMaskView(view);
    }
  }

  /// Registration of \ref releaseRetainedViewsOf with the texture pool; zero when not registered.
  uint64_t texturePoolDestroyListener = 0;

  /**
   * Release every retained composite view of \p texture, which the shared texture pool is about to
   * destroy. A retained entry's views outlive the frame that made them, while its textures return
   * to the pool at that frame's end, so the pool may destroy one at a call from any renderer on
   * this device. Textures the open frame recorded draws from stay out of the pool until it ends,
   * so this never touches a bind group a pending command names.
   */
  void releaseRetainedViewsOf(const gpu::ResourceIdentity& texture) {
    for (RetainedCompositeSequence* sequence : retainedCompositeSequences()) {
      sequence->releaseViewsOf(texture);
    }
    patternTileViews.forget(texture, [](const gpu::ResourceIdentity&) {});
    clipMaskViews.forget(texture, [this](const gpu::ResourceIdentity& view) {
      releaseCompositeBindingsOfClipMaskView(view);
    });
  }

  /// Release the views and bind group of every retained entry, keeping the uniform buffers. For a
  /// frame that ends without submitting: its textures leave the pool's path, and its clip mask
  /// views go with it.
  void releaseAllRetainedViews() {
    for (RetainedCompositeSequence* sequence : retainedCompositeSequences()) {
      sequence->releaseAllViews();
    }
    patternTileViews.clear();
    clipMaskViews.clear();
  }

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
    gpu::Texture savedTarget;
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
    // Outer clipping belongs to the completed composite, not to either capture. A prepared
    // region clip already incorporates the outer path-mask coverage and is sampled only once.
    std::vector<ClipStackEntry> savedClipStack;
    std::vector<ClipStackEntry> compositeRegionClip;
    // Only rejected region admission remains active while capturing, to fail closed immediately.
    bool hasRegionClip = false;
    ColorInterpolation maskColorInterpolation = ColorInterpolation::SRGB;
  };
  static_assert(!std::is_copy_constructible_v<MaskStackFrame>,
                "A copyable mask stack frame would let vector growth copy its texture handles.");
  std::vector<MaskStackFrame> maskStack;

  std::optional<PatternPaintSlot> patternFillPaint;
  std::optional<PatternPaintSlot> patternStrokePaint;
  /// Tiles of text spans' own patterns, staged for the next text draw.
  SpanPatternSlots spanPatternPaints;

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
  /// half-planes on top of the scissor rect. The shader tests one polygon of
  /// at most four edges, so `FoldClipPolygon` intersects the stacked
  /// polygons and keeps the intersection while it has at most four edges;
  /// beyond that it falls back as that function describes.
  /// The polygon the shader tests for a clip stack: \ref count corners, none when \ref count is
  /// zero, and \ref empty when the stacked polygons do not overlap.
  struct ActiveClipPolygon {
    std::array<Vector2d, 4> corners{};
    std::size_t count = 0;
    bool empty = false;
  };

  /// The clip state of a stack up to and including one entry, cached on that entry when it is
  /// pushed. It is computed from the fold of the entry below and the entry itself, so a push folds
  /// one entry and a pop folds nothing: the entry below already holds its own fold.
  struct ClipFold {
    /// An entry is known empty, its coverage could not be produced, or polygons do not overlap.
    bool drawsNothing = false;
    /// Intersection of every valid `pixelRect`; none when no entry has one.
    std::optional<Box2d> scissor;
    ActiveClipPolygon polygon;
    /// Mask of the topmost entry that has one; both null when none does. They name the same
    /// objects as that entry's \ref ClipStackEntry::maskResolveTextureHandle and view, which stay
    /// put while the entry moves, and that entry is popped no earlier than the one caching them.
    const gpu::Texture* maskTexture = nullptr;
    const gpu::TextureView* maskView = nullptr;
  };

  struct ClipStackEntry {
    ClipStackEntry() = default;
    ~ClipStackEntry() = default;

    // Move-only, explicitly. The mask textures this entry owns are move-only runtime handles, so
    // a copy could never have worked; deleting it matters because the `std::deque` member below
    // advertises a copy constructor that fails only once instantiated, and a deque's move is
    // `noexcept` in libc++ but not in libstdc++, so `std::vector` reallocation's
    // `move_if_noexcept` reaches for that copy on one of them and not the other.
    ClipStackEntry(const ClipStackEntry&) = delete;
    ClipStackEntry& operator=(const ClipStackEntry&) = delete;
    ClipStackEntry(ClipStackEntry&&) = default;
    ClipStackEntry& operator=(ClipStackEntry&&) = default;

    Box2d pixelRect;
    bool valid = false;
    bool hasPolygon = false;
    /// From ResolvedClip::boundsFallbackWhenStacked.
    bool boundsFallbackWhenStacked = false;
    /// The clip's bounded geometry is proven disjoint from the active target.
    bool knownEmpty = false;
    /// The clip's coverage could not be produced, so nothing under it draws.
    bool allocationRejected = false;
    Vector2d polygonCorners[4];
    /// Path-clip mask. When non-null these name a 1-sample texture sampled
    /// by the fill / gradient pipelines through their clip-mask bindings.
    /// The texture is allocated per `pushClip` call and parked in
    /// `maskLayerTextures`, which keeps it alive until `popClip`.
    ///
    /// Each nested reference clips only its referencing shape. Sibling results are unioned
    /// into the owning layer's mask; the outermost layer supplies the final coverage.
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
    /// The clip state of the stack up to and including this entry; set by \ref pushClipEntry.
    ClipFold fold;
  };

  /// State for an in-progress filter layer. Captures all draws
  /// between `pushFilterLayer` / `popFilterLayer` into an offscreen texture,
  /// then runs the stored `FilterGraph` through `GeodeFilterEngine` and
  /// composites the result back onto the outer target.
  struct FilterStackFrame {
    std::unique_ptr<geode::GeoEncoder> savedEncoder;
    gpu::Texture savedTarget;
    gpu::Texture layerTexture;
    /// Descriptors captured at push for texture-pool release.
    gpu::TextureDescriptor layerDesc = {};
    components::FilterGraph filterGraph;
    geode::FilterTilePlan fullFilterPlan;
    std::optional<geode::FilterTilePlan> localFilterPlan;
    components::FilterExecutionBudget::Reservation filterReservation;
    Box2d filterRegion;
    Transform2d deviceFromFilter;  // Full CTM at push time.
    std::optional<Transform2d> localCaptureFromParent;
    /// Raster pixels from device pixels of a capture reduced to fit the frame's limits.
    std::optional<Transform2d> reducedRasterFromDevice;
    bool transformedCaptureReserved = false;
    bool localRasterRequiredForBudget = false;
    int filterBufferOffsetX = 0;  // Input margin past the outer target's left edge.
    int filterBufferOffsetY = 0;  // Input margin past the outer target's top edge.
    /// BackgroundImage input in the capture's texel layout, when the graph reads the backdrop.
    gpu::Texture backdropTexture;
    gpu::TextureDescriptor backdropDesc = {};
    std::vector<ClipStackEntry> savedClipStack;
    bool allocationRejected = false;
  };
  // Non-copyability here is incidental: the move-only `savedEncoder` and `layerTexture` members
  // supply it, not an explicit `= delete`. The assert pins it because `savedClipStack` holds
  // entries whose copy is a hard error; if both members ever became copyable, `std::vector`
  // reallocation would reach for the copy on a standard library whose deque move is not
  // `noexcept`, and the failure would surface inside the standard library rather than here.
  static_assert(!std::is_copy_constructible_v<FilterStackFrame>,
                "A copyable filter stack frame would let vector growth copy its clip entries.");
  std::vector<ClipStackEntry> clipStack;
  std::vector<FilterStackFrame> filterStack;
  std::size_t rejectedFilterDepth = 0;

  /// The target beneath one open isolated, mask or filter layer, and the filter-stack depth at its
  /// push. A filter stage that reads the backdrop skips the filtered element's own layers through
  /// these records.
  struct LayerBackdropRecord {
    gpu::Texture target;          //!< Alias of the target the layer composites onto.
    std::size_t filterDepth = 0;  //!< Filter frames open when the layer was pushed.
  };
  std::vector<LayerBackdropRecord> layerBackdropRecords;

  void pushLayerBackdropRecord() {
    layerBackdropRecords.push_back({aliasOf(target), filterStack.size()});
  }

  void popLayerBackdropRecord() {
    if (!layerBackdropRecords.empty()) {
      layerBackdropRecords.pop_back();
    }
  }

  /// Records a clear of \p backdrop and a copy of the backdrop \p filterGraph reads into it, in
  /// the texel layout of a capture admitted as \p admission. The newest record names the target
  /// this filter layer composites onto. The outer target's draws must already be recorded.
  void recordFilterBackdrop(const components::FilterGraph& filterGraph,
                            const GeodeFilterAdmission& admission, const gpu::Texture& backdrop,
                            const gpu::Extent2d& extent) {
    auto clearEncoder = std::make_unique<geode::GeoEncoder>(
        *device, *pipeline, *gradientPipeline, *imagePipeline, backdrop, extent, *frameGpuEncoder);
    configureEncoder(*clearEncoder);
    clearEncoder->clear(css::RGBA(0, 0, 0, 0));
    clearEncoder->finish();
    retireFinishedEncoder(std::move(clearEncoder));
    if (layerBackdropRecords.empty()) {
      return;
    }
    const std::size_t newest = layerBackdropRecords.size() - 1;
    const LayerBackdropRecord& record =
        layerBackdropRecords[newest - std::min<std::size_t>(filterGraph.backdropLayerSkip, newest)];
    if (!record.target.isValid() || !canCopyFrom(record.target)) {
      return;  // An embedder target that refuses copies leaves the backdrop transparent.
    }
    const gpu::Result<gpu::TextureDescriptor> source =
        device->runtimeDevice().textureDescriptor(record.target);
    if (!source.hasResult()) {
      return;
    }
    // Each filter capture above the backdrop moved its contents by its input margin; this
    // capture's own margin moves them once more.
    int64_t offsetX = admission.bufferOffsetX;
    int64_t offsetY = admission.bufferOffsetY;
    for (std::size_t index = record.filterDepth; index < filterStack.size(); ++index) {
      offsetX += filterStack[index].filterBufferOffsetX;
      offsetY += filterStack[index].filterBufferOffsetY;
    }
    // Capture texel (x, y) is backdrop texel (x - offsetX, y - offsetY).
    const int64_t xBegin = std::max<int64_t>(0, offsetX);
    const int64_t yBegin = std::max<int64_t>(0, offsetY);
    const int64_t xEnd = std::min<int64_t>(extent.width, source.result().size.width + offsetX);
    const int64_t yEnd = std::min<int64_t>(extent.height, source.result().size.height + offsetY);
    if (xBegin >= xEnd || yBegin >= yEnd) {
      return;
    }
    const gpu::Status copied = frameGpuEncoder->copyTextureToTexture(
        record.target, backdrop,
        gpu::Extent2d{static_cast<uint32_t>(xEnd - xBegin), static_cast<uint32_t>(yEnd - yBegin)},
        gpu::Origin2d{static_cast<uint32_t>(xBegin - offsetX),
                      static_cast<uint32_t>(yBegin - offsetY)},
        gpu::Origin2d{static_cast<uint32_t>(xBegin), static_cast<uint32_t>(yBegin)});
    UTILS_RELEASE_ASSERT_MSG(!copied.hasError(), "Failed to record the filter backdrop copy");
  }

  /// Maps draw coordinates through the active captures, from outermost to innermost.
  /// @param firstFrame First capture counted, excluding captures a pattern tile interrupted.
  [[nodiscard]] Transform2d filterCaptureTransform(std::size_t firstFrame = 0) const {
    Transform2d result;
    for (std::size_t index = firstFrame; index < filterStack.size(); ++index) {
      const auto& frame = filterStack[index];
      result = result * captureFromParent(frame);
    }
    return result;
  }

  /// Filter user space to the pixels of \p frame's capture, which the filter engine works in: a
  /// reduced raster's, or the device's shifted by any input margin past the top or left edge.
  static Transform2d captureFromFilter(const FilterStackFrame& frame) {
    if (frame.reducedRasterFromDevice) {
      return frame.deviceFromFilter * *frame.reducedRasterFromDevice;
    }
    if (frame.filterBufferOffsetX != 0 || frame.filterBufferOffsetY != 0) {
      return frame.deviceFromFilter *
             Transform2d::Translate(frame.filterBufferOffsetX, frame.filterBufferOffsetY);
    }
    return frame.deviceFromFilter;
  }

  /// Capture pixels from the pixels of the target \p frame composites onto.
  static Transform2d captureFromParent(const FilterStackFrame& frame) {
    if (frame.localCaptureFromParent) {
      return *frame.localCaptureFromParent;
    }
    if (frame.reducedRasterFromDevice) {
      return *frame.reducedRasterFromDevice;
    }
    return Transform2d::Translate(frame.filterBufferOffsetX, frame.filterBufferOffsetY);
  }

  /// Fills the clip paths in [\p begin, \p end) of \p clip, one layer of it, into \p mask with
  /// the active encoder, intersected with whatever nested mask the encoder has bound.
  /// @param mask Mask texture with the active target's extent.
  /// @param clip Clip whose paths are filled.
  /// @param begin Index of the layer's first path.
  /// @param end Index past the layer's last path.
  /// @param deviceFromLocal Transform of the clipped element's local space.
  /// @return False when the encoder refused the mask pass, which leaves the mask without coverage.
  [[nodiscard]] bool fillClipLayerMask(const gpu::Texture& mask, const ResolvedClip& clip,
                                       size_t begin, size_t end, const Transform2d& deviceFromLocal,
                                       gpu::LoadOp loadOp) {
    if (!encoder->beginMaskPass(mask, loadOp)) {
      return false;
    }
    for (size_t s = begin; s < end; ++s) {
      const ClipPathShape& shape = clip.clipPaths[s];
      const Transform2d deviceFromShape =
          clip.clipPathUnitsTransform * shape.parentFromEntity * deviceFromLocal;
      encoder->setTransform(deviceFromShape);
      fillClipShapeIntoMask(clip, s, shape.path, shape.fillRule, deviceFromShape);
    }
    encoder->endMaskPass();
    return true;
  }

  /// Fills \p path, shape \p index of \p clip, into the open mask pass from its cached encode and
  /// resident slot (see `GeodeClipPathCacheComponent`), or, for a clip without a source entity, a
  /// shape a budget refuses, or a path the encoder's limits reject, from a per-frame encode.
  /// @param clip Clip the shape belongs to.
  /// @param index Index of the shape in \p clip: a clip path, or a group marker whose coverage is
  ///   unioned through \p path.
  /// @param path Path filled.
  /// @param rule Fill rule.
  /// @param deviceFromShape Transform the encoder fills the shape with.
  void fillClipShapeIntoMask(const ResolvedClip& clip, std::size_t index, const Path& path,
                             FillRule rule, const Transform2d& deviceFromShape) {
    const EntityHandle source = clip.sourceEntity;
    if (!source.valid() || geometryBudget->rejected()) {
      encoder->fillPathIntoMask(path, rule);
      return;
    }
    Registry& registry = *source.registry();
    GeodeClipPathCacheComponent& cache = clipPathCache(source, clip.clipPaths.size());
    GeodeClipPathCacheComponent::Shape& entry = cache.shapes[index];
    const double tolerance = geode::FillCubicToleranceFor(deviceFromShape);
    if (!ClipShapeEncodeServes(entry, path, tolerance) &&
        !encodeClipShape(cache, entry, path, rule, tolerance, registry)) {
      return;
    }
    fillResidentClipShape(cache, index, rule, registry);
  }

  /// The clip geometry cache on \p source, holding \p shapeCount shapes. A cache holding another
  /// number of shapes is for another clip, so it forgets every shape; generations keep counting,
  /// so no slot matches a new encode.
  GeodeClipPathCacheComponent& clipPathCache(EntityHandle source, std::size_t shapeCount) {
    ensureCacheInvalidationWired(*source.registry());
    auto& cache = source.get_or_emplace<GeodeClipPathCacheComponent>();
    if (cache.shapes.size() != shapeCount) {
      cache.shapes.clear();
      cache.shapes.resize(shapeCount);
      cache.devices.forEach(
          [](GeodeClipPathCacheComponent::DeviceSlots& slots) { slots.shapes.clear(); });
    }
    return cache;
  }

  /// True when \p entry holds the encode \p path would get at \p tolerance.
  static bool ClipShapeEncodeServes(const GeodeClipPathCacheComponent::Shape& entry,
                                    const Path& path, double tolerance) {
    return entry.encode.has_value() && entry.path == path &&
           (entry.tolerance == 0.0 || entry.tolerance == tolerance);
  }

  /// Encodes \p path into \p entry at \p tolerance. When the encoder's limits reject the path or
  /// the document's cache budget refuses to keep it, fills it into the open mask pass from a
  /// per-frame encode instead.
  /// @return True when \p entry holds the new encode; false when the shape was already filled.
  bool encodeClipShape(GeodeClipPathCacheComponent& cache,
                       GeodeClipPathCacheComponent::Shape& entry, const Path& path, FillRule rule,
                       double tolerance, Registry& registry) {
    device->countPathEncode();
    geode::EncodedPath encoded =
        geode::GeodePathEncoder::encode(path, rule, tolerance, pathEncoderLimits);
    if (encoded.rejected()) {
      encoder->fillPathIntoMask(path, rule);
      return false;
    }
    // The kept copy of the path is charged with its encode.
    const std::size_t encodedBytes = encoded.retainedBytes();
    const std::size_t pathBytes =
        path.retainedBytes().value_or(std::numeric_limits<std::size_t>::max());
    if (encodedBytes > std::numeric_limits<std::size_t>::max() - pathBytes ||
        !entry.reservation.replace(documentGeometryBudget(registry), encodedBytes + pathBytes)) {
      // Past the document's cache budget: this frame fills from its own encode.
      clipScratchEncode = std::move(encoded);
      encoder->fillPathIntoMask(path, rule, &*clipScratchEncode);
      return false;
    }
    entry.encode = std::move(encoded);
    entry.path = path;
    const bool curved = std::any_of(
        path.commands().begin(), path.commands().end(),
        [](const Path::Command& command) { return command.verb == Path::Verb::CurveTo; });
    entry.tolerance = curved ? tolerance : 0.0;
    entry.generation = ++cache.encodeClock;
    return true;
  }

  /// Fills shape \p index of \p cache into the open mask pass from this device's resident slot of
  /// it, which is reset first when it was uploaded from another encode or into another slab.
  void fillResidentClipShape(GeodeClipPathCacheComponent& cache, std::size_t index, FillRule rule,
                             Registry& registry) {
    const GeodeClipPathCacheComponent::Shape& entry = cache.shapes[index];
    std::vector<GeodeClipPathCacheComponent::MaskSlot>& slots =
        cache.devices.forDevice(deviceKey()).shapes;
    slots.resize(cache.shapes.size());
    GeodeClipPathCacheComponent::MaskSlot& maskSlot = slots[index];
    std::shared_ptr<geode::GeodeResidentSlab> slab = residentSlab(registry);
    if (maskSlot.generation != entry.generation || maskSlot.slot.slab.get() != slab.get()) {
      // reset() parks the old range and bind group in their slab until no frame that may have
      // recorded them is open.
      maskSlot.slot.reset();
      maskSlot.generation = entry.generation;
    }
    maskSlot.slot.slab = std::move(slab);
    encoder->fillPathIntoMaskResident(maskSlot.slot, *entry.encode, rule, currentFrameIndex);
  }

  /// Finds adjacent sibling shapes that do not have their own nested reference.
  static std::size_t clipSiblingRunBegin(const ResolvedClip& clip, std::size_t begin, int layer,
                                         std::size_t lowerBound) {
    while (begin > lowerBound && clip.clipPaths[begin - 1].layer == layer &&
           clip.clipPaths[begin - 1].groupPathCount == 0 &&
           (begin - lowerBound < 2 || clip.clipPaths[begin - 2].layer <= layer)) {
      --begin;
    }
    return begin;
  }

  /// Intersects each referencing shape with its nested coverage before unioning siblings.
  const gpu::Texture* buildClipLayerMask(const ResolvedClip& clip, ClipStackEntry& entry,
                                         std::size_t& cursor, int layer,
                                         const gpu::Texture* ancestorMask,
                                         const Transform2d& deviceFromLocal,
                                         std::size_t lowerBound = 0) {
    const gpu::TextureDescriptor descriptor{
        RcString("RendererGeodeClipMask"), targetExtent(), gpu::TextureFormat::RGBA8Unorm,
        gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::Sampled};
    gpu::Texture mask = acquireTexture(descriptor);
    if (!mask.isValid()) {
      entry.allocationRejected = true;
      return nullptr;
    }
    // Allocate before descending so the surface budget also bounds recursion depth.
    entry.maskLayerTextures.push_back({std::move(mask), descriptor});
    const gpu::Texture& destination = entry.maskLayerTextures.back().texture;
    gpu::LoadOp loadOp = gpu::LoadOp::Clear;
    while (cursor > lowerBound && clip.clipPaths[cursor - 1].layer == layer) {
      const std::size_t end = cursor;
      std::size_t begin = --cursor;
      if (clip.clipPaths[begin].groupPathCount != 0) {
        const gpu::Texture* group = buildClipGroupMask(clip, entry, cursor, layer, lowerBound,
                                                       ancestorMask, deviceFromLocal);
        if (!unionClipMask(clip, begin, destination, group, loadOp)) {
          entry.allocationRejected = true;
          return nullptr;
        }
        loadOp = gpu::LoadOp::Load;
        continue;
      }
      const gpu::Texture* nested = ancestorMask;
      if (cursor > lowerBound && clip.clipPaths[cursor - 1].layer > layer) {
        nested = buildClipLayerMask(clip, entry, cursor, clip.clipPaths[cursor - 1].layer,
                                    ancestorMask, deviceFromLocal, lowerBound);
        if (!nested) {
          return nullptr;
        }
      } else {
        begin = clipSiblingRunBegin(clip, begin, layer, lowerBound);
        cursor = begin;
      }
      if (!bindNestedClipMask(nested)) {
        entry.allocationRejected = true;
        return nullptr;
      }
      if (!fillClipLayerMask(destination, clip, begin, end, deviceFromLocal, loadOp)) {
        entry.allocationRejected = true;
        return nullptr;
      }
      loadOp = gpu::LoadOp::Load;
    }
    return &destination;
  }

  /// Binds \p nested, the coverage the next mask fills intersect with, or clears the encoder's clip
  /// mask when it is null. @return False when the runtime refuses a view of \p nested.
  bool bindNestedClipMask(const gpu::Texture* nested) {
    if (nested == nullptr) {
      encoder->clearClipMask();
      return true;
    }
    const gpu::TextureView* view = clipMaskView(*nested);
    if (view == nullptr) {
      return false;
    }
    encoder->setClipMask(*nested, *view);
    return true;
  }

  /// Unions already-computed coverage into a mask without leaving GPU memory, by filling the
  /// whole target through \p coverage. The fill is a unit square scaled to the target, so its
  /// encode is the same at every target size and is kept as shape \p markerIndex of \p clip.
  /// @param clip Clip being built.
  /// @param markerIndex Index of the group marker in \p clip whose coverage is unioned.
  bool unionClipMask(const ResolvedClip& clip, std::size_t markerIndex,
                     const gpu::Texture& destination, const gpu::Texture* coverage,
                     gpu::LoadOp loadOp) {
    const gpu::TextureView* coverageView = coverage != nullptr ? clipMaskView(*coverage) : nullptr;
    if (coverageView == nullptr) {
      return false;
    }
    encoder->setClipMask(*coverage, *coverageView);
    if (!encoder->beginMaskPass(destination, loadOp)) {
      return false;
    }
    const gpu::Extent2d extent = targetExtent();
    const Transform2d deviceFromUnit = Transform2d::Scale(extent.width, extent.height);
    encoder->setTransform(deviceFromUnit);
    fillClipShapeIntoMask(clip, markerIndex, unitSquarePath, FillRule::NonZero, deviceFromUnit);
    encoder->endMaskPass();
    return true;
  }

  /// The unit square a clip group's coverage is unioned through.
  const Path unitSquarePath = PathBuilder().addRect(Box2d::FromXYWH(0, 0, 1, 1)).build();

  /// Applies a clipPath-level reference to its complete counted child union.
  const gpu::Texture* buildClipGroupMask(const ResolvedClip& clip, ClipStackEntry& entry,
                                         std::size_t& cursor, int layer, std::size_t lowerBound,
                                         const gpu::Texture* ancestorMask,
                                         const Transform2d& deviceFromLocal) {
    const std::size_t count = clip.clipPaths[cursor].groupPathCount;
    if (count > cursor - lowerBound || clip.clipPaths[cursor - 1].layer <= layer) {
      entry.allocationRejected = true;
      return nullptr;
    }
    const std::size_t childEnd = cursor;
    const std::size_t childBegin = cursor - count;
    cursor = childBegin;
    const gpu::Texture* inherited = ancestorMask;
    if (cursor > lowerBound && clip.clipPaths[cursor - 1].layer > layer) {
      inherited = buildClipLayerMask(clip, entry, cursor, clip.clipPaths[cursor - 1].layer,
                                     ancestorMask, deviceFromLocal, lowerBound);
      if (!inherited) {
        return nullptr;
      }
    }
    const std::size_t resumeCursor = cursor;
    cursor = childEnd;
    const gpu::Texture* result =
        buildClipLayerMask(clip, entry, cursor, clip.clipPaths[cursor - 1].layer, inherited,
                           deviceFromLocal, childBegin);
    if (cursor != childBegin) {
      entry.allocationRejected = true;
      return nullptr;
    }
    cursor = resumeCursor;
    return result;
  }

  /// Builds the path mask while preserving the active ancestor clip and main-pass transform.
  void buildClipMask(const ResolvedClip& clip, ClipStackEntry& entry) {
    const Transform2d savedDeviceFromLocal = deviceFromLocalTransform;
    // The topmost mask on the stack, which the top entry's fold caches.
    const gpu::Texture* ancestorMask = clipStackFold().maskTexture;
    std::size_t cursor = clip.clipPaths.size();
    entry.maskResolveTextureHandle = buildClipLayerMask(
        clip, entry, cursor, clip.clipPaths.back().layer, ancestorMask, savedDeviceFromLocal);
    if (entry.maskResolveTextureHandle && cursor == 0) {
      entry.maskResolveViewHandle = clipMaskView(*entry.maskResolveTextureHandle);
    }
    if (entry.maskResolveViewHandle == nullptr) {
      entry.allocationRejected = true;
      entry.maskResolveTextureHandle = nullptr;
    }
    encoder->clearClipMask();
    encoder->setTransform(savedDeviceFromLocal);
  }

  struct SimpleClipHull {
    Box2d bounds;
    double margin = 0.0;
  };

  static bool fitsGpuFloat(double value) {
    return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
  }

  static bool fitsGpuFloat(const Vector2d& value) {
    return fitsGpuFloat(value.x) && fitsGpuFloat(value.y);
  }

  static bool fitsGpuFloat(const Transform2d& value) {
    return std::all_of(std::begin(value.data), std::end(value.data),
                       [](double coefficient) { return fitsGpuFloat(coefficient); });
  }

  static bool isLinearClipPath(const Path& path) {
    return std::all_of(
        path.commands().begin(), path.commands().end(), [](const Path::Command& command) {
          return command.verb == Path::Verb::MoveTo || command.verb == Path::Verb::LineTo ||
                 command.verb == Path::Verb::ClosePath;
        });
  }

  /// Whether \p shape is a bounded-work, non-nested linear clip candidate.
  static bool isSimpleClipCullCandidate(const ClipPathShape& shape) {
    constexpr std::size_t kMaximumCommands = 64;
    constexpr std::size_t kMaximumPoints = 3 * kMaximumCommands;
    return shape.layer == 0 && shape.groupPathCount == 0 && !shape.path.commands().empty() &&
           shape.path.commands().size() <= kMaximumCommands && isLinearClipPath(shape.path) &&
           !shape.path.points().empty() && shape.path.points().size() <= kMaximumPoints;
  }

  /// Conservative device-space control-point hull and raster arithmetic margin for \p shape.
  static std::optional<SimpleClipHull> simpleClipHull(const ClipPathShape& shape,
                                                      const Transform2d& deviceFromShape) {
    constexpr double kAntialiasMargin = 2.0;
    constexpr double kFloatOperationsMargin = 32.0;
    const std::span<const Vector2d> points = shape.path.points();
    if (!fitsGpuFloat(points.front())) {
      return std::nullopt;
    }
    const Vector2d firstDevicePoint = deviceFromShape.transformPosition(points.front());
    if (!fitsGpuFloat(firstDevicePoint)) {
      return std::nullopt;
    }

    Box2d deviceHull = Box2d::CreateEmpty(firstDevicePoint);
    double maximumLocalX = 0.0;
    double maximumLocalY = 0.0;
    for (const Vector2d& point : points) {
      if (!fitsGpuFloat(point)) {
        return std::nullopt;
      }
      maximumLocalX = std::max(maximumLocalX, std::abs(point.x));
      maximumLocalY = std::max(maximumLocalY, std::abs(point.y));
      const Vector2d devicePoint = deviceFromShape.transformPosition(point);
      if (!fitsGpuFloat(devicePoint)) {
        return std::nullopt;
      }
      deviceHull.addPoint(devicePoint);
    }
    const double maximumDeviceX = std::abs(deviceFromShape.data[0]) * maximumLocalX +
                                  std::abs(deviceFromShape.data[2]) * maximumLocalY +
                                  std::abs(deviceFromShape.data[4]);
    const double maximumDeviceY = std::abs(deviceFromShape.data[1]) * maximumLocalX +
                                  std::abs(deviceFromShape.data[3]) * maximumLocalY +
                                  std::abs(deviceFromShape.data[5]);
    if (!fitsGpuFloat(maximumDeviceX) || !fitsGpuFloat(maximumDeviceY)) {
      return std::nullopt;
    }
    const double arithmeticMargin = kFloatOperationsMargin * std::numeric_limits<float>::epsilon() *
                                    std::max(maximumDeviceX, maximumDeviceY);
    if (!std::isfinite(arithmeticMargin)) {
      return std::nullopt;
    }
    return SimpleClipHull{deviceHull, kAntialiasMargin + arithmeticMargin};
  }

  static bool missesTarget(const SimpleClipHull& hull, const gpu::Extent2d& extent) {
    return hull.bounds.bottomRight.x < -hull.margin || hull.bounds.bottomRight.y < -hull.margin ||
           hull.bounds.topLeft.x > static_cast<double>(extent.width) + hull.margin ||
           hull.bounds.topLeft.y > static_cast<double>(extent.height) + hull.margin;
  }

  /// Whether a bounded, non-nested linear path clip is conservatively outside the active target.
  ///
  /// Path bounds are linear in the path size, so this only inspects small paths. The margin covers
  /// antialiasing plus the float conversion and arithmetic used by the GPU path pipeline. Any
  /// non-finite or unbounded input keeps the exact mask path.
  bool simpleClipMissesTarget(const ResolvedClip& clip) const {
    if (clip.clipPaths.size() != 1) {
      return false;
    }
    const ClipPathShape& shape = clip.clipPaths.front();
    if (!isSimpleClipCullCandidate(shape)) {
      return false;
    }
    const Transform2d deviceFromShape =
        clip.clipPathUnitsTransform * shape.parentFromEntity * deviceFromLocalTransform;
    if (!fitsGpuFloat(deviceFromShape)) {
      return false;
    }
    const std::optional<SimpleClipHull> hull = simpleClipHull(shape, deviceFromShape);
    return hull && missesTarget(*hull, targetExtent());
  }

  bool initializeClipEntry(const ResolvedClip& clip, ClipStackEntry& entry) {
    if (rejectedFilterDepth != 0) {
      pushClipEntry(std::move(entry));
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
    entry.boundsFallbackWhenStacked = clip.boundsFallbackWhenStacked;
    return true;
  }

  void restoreFilterCaptureTransform(const FilterStackFrame& frame) {
    if (frame.localCaptureFromParent || frame.reducedRasterFromDevice) {
      deviceFromLocalTransform = deviceFromLocalTransform * captureFromParent(frame).inverse();
    }
  }

  bool readyForFilterCapture() const {
    return device && pipeline && gradientPipeline && imagePipeline && encoder && filterEngine;
  }

  void pushRejectedFilterFrame() {
    FilterStackFrame frame;
    frame.savedEncoder = std::move(encoder);
    frame.savedTarget = aliasOf(target);
    frame.allocationRejected = true;
    // The pop restores the clip stack from the frame, so the frame must hold it.
    frame.savedClipStack = std::move(clipStack);
    clipStack.clear();
    filterStack.push_back(std::move(frame));
    ++rejectedFilterDepth;
  }

  bool beginFilterCapture(const components::FilterGraph& filterGraph,
                          const GeodeFilterAdmission& admission,
                          const Transform2d& deviceFromFilter) {
    const auto geometry =
        admission.localPlan && CanCaptureFilterSourceLocally(filterGraph, deviceFromFilter)
            ? ComputeGeodeLocalRasterGeometry(filterGraph, admission.region, deviceFromFilter,
                                              !admission.localRasterRequired)
            : std::nullopt;
    const gpu::Extent2d captureSize =
        geometry ? gpu::Extent2d{geometry->width, geometry->height}
                 : gpu::Extent2d{static_cast<uint32_t>(admission.bufferWidth),
                                 static_cast<uint32_t>(admission.bufferHeight)};
    const gpu::TextureDescriptor textureDesc{
        "RendererGeodeFilterLayer", captureSize, gpuTextureFormat(),
        gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::Sampled |
            gpu::TextureUsage::CopySrc};
    gpu::Texture layerTexture = acquireTexture(textureDesc);
    if (!layerTexture.isValid()) {
      return false;
    }
    const bool readsBackdrop = components::StandardInputUsage(filterGraph).readsBackdrop();
    const gpu::TextureDescriptor backdropDesc{
        "RendererGeodeFilterBackdrop", textureDesc.size, gpuTextureFormat(),
        gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::Sampled |
            gpu::TextureUsage::CopySrc | gpu::TextureUsage::CopyDst};
    gpu::Texture backdropTexture = readsBackdrop ? acquireTexture(backdropDesc) : gpu::Texture();
    if (readsBackdrop && !backdropTexture.isValid()) {
      releaseTextureAtFrameEnd(std::move(layerTexture), textureDesc);
      return false;
    }

    encoder->finish();
    if (readsBackdrop) {
      recordFilterBackdrop(filterGraph, admission, backdropTexture, backdropDesc.size);
    }
    FilterStackFrame frame;
    frame.savedEncoder = std::move(encoder);
    frame.savedTarget = aliasOf(target);
    frame.layerDesc = textureDesc;
    frame.filterGraph = filterGraph;
    frame.fullFilterPlan = admission.fullPlan;
    frame.localFilterPlan = admission.localPlan;
    frame.filterReservation = admission.reservation;
    frame.filterRegion = admission.region;
    frame.deviceFromFilter = deviceFromFilter;
    frame.transformedCaptureReserved = admission.transformedCaptureReserved;
    frame.localRasterRequiredForBudget = admission.localRasterRequired;
    frame.reducedRasterFromDevice = admission.reducedRasterFromDevice;
    if (geometry) {
      frame.localCaptureFromParent =
          FilterRasterFromDevice(deviceFromFilter, geometry->paddedRegion.topLeft,
                                 Vector2d(geometry->scaleX, geometry->scaleY));
    } else {
      frame.filterBufferOffsetX = admission.bufferOffsetX;
      frame.filterBufferOffsetY = admission.bufferOffsetY;
    }
    if (readsBackdrop) {
      frame.backdropTexture = std::move(backdropTexture);
      frame.backdropDesc = backdropDesc;
    }
    frame.savedClipStack = std::move(clipStack);
    clipStack.clear();

    frame.layerTexture = std::move(layerTexture);
    target = aliasOf(frame.layerTexture);
    auto newEncoder =
        std::make_unique<geode::GeoEncoder>(*device, *pipeline, *gradientPipeline, *imagePipeline,
                                            target, textureDesc.size, *frameGpuEncoder);
    deviceFromLocalTransform = deviceFromLocalTransform * captureFromParent(frame);
    filterStack.push_back(std::move(frame));
    configurePathEncoder(*newEncoder);
    newEncoder->clear(css::RGBA(0, 0, 0, 0));
    replaceActiveEncoder(std::move(newEncoder));
    ClipStackEntry filterClipEntry;
    filterClipEntry.pixelRect = deviceFromLocalTransform.transformBox(admission.region);
    filterClipEntry.valid = true;
    pushClipEntry(std::move(filterClipEntry));
    updateEncoderScissor();
    return true;
  }

  enum class TransformedFilterResult { NotApplicable, Composited, FrameAbandoned };

  std::optional<GeodeLocalRasterGeometry> transformedFilterGeometry(
      const FilterStackFrame& frame) const {
    if (!frame.transformedCaptureReserved || !frame.localFilterPlan || !filterEngine ||
        frame.filterGraph.empty()) {
      return std::nullopt;
    }
    return ComputeGeodeLocalRasterGeometry(frame.filterGraph, frame.filterRegion,
                                           frame.deviceFromFilter,
                                           !frame.localRasterRequiredForBudget);
  }

  /// Composites a filtered local raster back to the device through \p deviceFromLocal.
  static void drawLocalRasterComposite(geode::GeoEncoder& encoder, const gpu::Texture& composited,
                                       const GeodeLocalRasterGeometry& geometry,
                                       const Transform2d& deviceFromLocal) {
    encoder.setTransform(deviceFromLocal);
    const Box2d rasterRect = Box2d::FromXYWH(0.0, 0.0, static_cast<double>(geometry.width),
                                             static_cast<double>(geometry.height));
    // The raster moves with the content, so anchor at the device pixel holding its origin: a
    // whole-pixel device move moves the anchor with it and leaves every weight unchanged. An
    // origin beyond the integer pixel range has no anchor and samples with plain bilinear.
    if (const std::optional<Vector2i> anchor =
            geode::AnchorPixelFor(deviceFromLocal.transformPosition(Vector2d::Zero()))) {
      encoder.drawTextureAnchored(composited, rasterRect, kWholeTextureUv, 1.0,
                                  /*sourceIsPremultiplied=*/true, *anchor);
    } else {
      encoder.drawTexture(composited, rasterRect, kWholeTextureUv, 1.0, /*pixelated=*/false,
                          /*sourceIsPremultiplied=*/true);
    }
    encoder.setTransform(Transform2d());
  }

  struct SharpShadowPass {
    TransformedShadowGraphs graphs;
    Box2d shadowRegion;
    geode::FilterTilePlan localPlan;
    geode::FilterTilePlan mergePlan;
    components::FilterExecutionBudget::Reservation shadowReservation;
    components::FilterExecutionBudget::Reservation mergeReservation;
    gpu::Texture projectedShadow;
    gpu::TextureDescriptor projectionDesc;
  };

  std::optional<SharpShadowPass> prepareSharpShadow(const FilterStackFrame& frame,
                                                    components::FilterStandardInput foreground,
                                                    const GeodeLocalRasterGeometry& geometry,
                                                    const Box2d& localRegion,
                                                    const Transform2d& localFromFilter,
                                                    const Transform2d& bufferFromFilter) {
    auto graphs = SplitTransformedShadow(frame.filterGraph, foreground);
    const Box2d shadowRegion = graphs.preservePaddedBlur
                                   ? Box2d(Vector2d::Zero(), geometry.paddedRegion.size())
                                   : localRegion;
    const GeodeFilterBuffer localBuffer{shadowRegion, static_cast<int>(geometry.width),
                                        static_cast<int>(geometry.height), 0, 0};
    const GeodeFilterBuffer deviceBuffer{frame.filterRegion,
                                         static_cast<int>(frame.layerDesc.size.width),
                                         static_cast<int>(frame.layerDesc.size.height),
                                         frame.filterBufferOffsetX, frame.filterBufferOffsetY};
    const GeodeFilterPlan shadow =
        PlanGeodeFilter(graphs.shadow, shadowRegion, localFromFilter, localBuffer, *filterEngine);
    const GeodeFilterPlan merge = PlanGeodeFilter(graphs.merge, frame.filterRegion,
                                                  bufferFromFilter, deviceBuffer, *filterEngine);
    if (!shadow.fullExecutionFits || !merge.fullExecutionFits || !shadow.costKnown ||
        !merge.costKnown) {
      return std::nullopt;
    }

    // Try the two extra executions together. A graph that cannot afford the split keeps the
    // device-raster fallback, without latching a frame-wide filter rejection.
    auto budget = *filterExecutionBudget;
    const auto reserve = [&](const components::FilterGraph& graph, GeodeFilterPlan plan,
                             uint64_t existingPixels)
        -> std::optional<components::FilterExecutionBudget::Reservation> {
      plan.captureBytes -= existingPixels * 4;
      --plan.surfaces;  // The existing source capture was reserved when the layer opened.
      if (!GeodeFilterPlanFits(graph, plan, budget, *surfaceBudget, *filterEngine)) {
        return std::nullopt;
      }
      return budget.reserve(graph, plan.bounds.workPixels,
                            components::FilterMemoryModel::GpuAllNodes, plan.captureBytes,
                            filterEngine->retainedBufferBytes(), plan.bounds.memoryPixels,
                            plan.bounds.executions, plan.surfaces);
    };
    auto shadowReservation =
        reserve(graphs.shadow, shadow, uint64_t{geometry.width} * geometry.height);
    auto mergeReservation = reserve(
        graphs.merge, merge, uint64_t{frame.layerDesc.size.width} * frame.layerDesc.size.height);
    if (!shadowReservation || !mergeReservation) {
      return std::nullopt;
    }

    gpu::TextureDescriptor desc = frame.layerDesc;
    desc.label = RcString("ProjectedFilterShadow");
    gpu::Texture projectedShadow = acquireTexture(desc);
    if (!projectedShadow.isValid()) {
      return std::nullopt;
    }
    *filterExecutionBudget = budget;
    return SharpShadowPass{std::move(graphs),
                           shadowRegion,
                           shadow.plan,
                           merge.plan,
                           *shadowReservation,
                           *mergeReservation,
                           std::move(projectedShadow),
                           std::move(desc)};
  }

  void releaseSharpShadow(SharpShadowPass& pass) {
    filterExecutionBudget->release(pass.mergeReservation);
    filterExecutionBudget->release(pass.shadowReservation);
    releaseTextureAtFrameEnd(std::move(pass.projectedShadow), pass.projectionDesc);
  }

  /// Project only the blurred branch. The original capture never leaves its pixel grid, and the
  /// regular feMerge executor applies the authored working color space before the parent clip.
  std::optional<geode::FilterExecutionResult> mergeTransformedShadow(
      const FilterStackFrame& frame, SharpShadowPass& pass, const gpu::Texture& filteredShadow,
      const GeodeLocalRasterGeometry& geometry, const Transform2d& bufferFromLocal,
      const Transform2d& bufferFromFilter) {
    auto projection = std::make_unique<geode::GeoEncoder>(
        *device, *pipeline, *gradientPipeline, *imagePipeline, pass.projectedShadow,
        pass.projectionDesc.size, *frameGpuEncoder);
    configureEncoder(*projection);
    drawLocalRasterComposite(*projection, filteredShadow, geometry, bufferFromLocal);
    projection->finish();
    retireFinishedEncoder(std::move(projection));
    if (!suspendFrameForFilter()) {
      if (!frameArenaWriteFailed) {
        device->markDeviceLost("frame commands could not be submitted before shadow merge");
      }
      return std::nullopt;
    }
    auto result =
        filterEngine->execute(pass.graphs.merge, frame.layerTexture, frame.layerDesc,
                              frame.filterRegion, bufferFromFilter, *this, nullptr, pass.mergePlan,
                              this, &pass.projectedShadow, &pass.projectionDesc, nullptr, this);
    if (!restoreFrameAfterFilter()) {
      device->markDeviceLost("frame encoder could not be restored after shadow merge");
      retainTextureWithoutCompletionProof(std::move(result.texture), result.desc);
      return std::nullopt;
    }
    return result;
  }

  void drawDeviceFilterOutput(const FilterStackFrame& frame, const gpu::Texture& filteredTexture,
                              const gpu::TextureDescriptor& filteredDesc) {
    geode::GeodeTextureEncoder::RetainedQuadBinding* retained =
        filterOutputComposites.binding(filterOutputComposites.claim(), filteredTexture);
    if (frame.reducedRasterFromDevice) {
      // Nearest sampling gives each device pixel the raster pixel that covers its center, so the
      // result is the filtered raster at its own resolution, with no interpolation added.
      encoder->setTransform(frame.reducedRasterFromDevice->inverse());
      encoder->drawTexture(filteredTexture,
                           Box2d::FromXYWH(0.0, 0.0, static_cast<double>(filteredDesc.size.width),
                                           static_cast<double>(filteredDesc.size.height)),
                           kWholeTextureUv, 1.0, /*pixelated=*/true,
                           /*sourceIsPremultiplied=*/true, retained);
      syncTransform();
      return;
    }
    const gpu::Extent2d outerExtent = targetExtent();
    const FilterInputBuffer buffer{static_cast<int>(frame.layerDesc.size.width),
                                   static_cast<int>(frame.layerDesc.size.height),
                                   frame.filterBufferOffsetX, frame.filterBufferOffsetY};
    if (!buffer.exceedsViewport(static_cast<int>(outerExtent.width),
                                static_cast<int>(outerExtent.height))) {
      encoder->blitFullTarget(filteredTexture, 1.0, retained);
      return;
    }
    // An expanded or inset capture maps texture pixel (0, 0) to device (-offsetX, -offsetY).
    // Nearest sampling preserves those original pixels.
    encoder->setTransform(Transform2d());
    encoder->drawTexture(
        filteredTexture,
        Box2d::FromXYWH(-static_cast<double>(buffer.offsetX), -static_cast<double>(buffer.offsetY),
                        static_cast<double>(filteredDesc.size.width),
                        static_cast<double>(filteredDesc.size.height)),
        kWholeTextureUv, 1.0, /*pixelated=*/true,
        /*sourceIsPremultiplied=*/true, retained);
    syncTransform();
  }

  void beginFilterComposite(const FilterStackFrame& frame) {
    target = aliasOf(frame.savedTarget);
    auto compositeEncoder =
        std::make_unique<geode::GeoEncoder>(*device, *pipeline, *gradientPipeline, *imagePipeline,
                                            frame.savedTarget, targetExtent(), *frameGpuEncoder);
    configurePathEncoder(*compositeEncoder);
    compositeEncoder->setLoadPreserve();
    replaceActiveEncoder(std::move(compositeEncoder));
    updateEncoderScissor();
  }

  TransformedFilterResult compositeSharpShadow(FilterStackFrame& frame, SharpShadowPass& sharp,
                                               geode::FilterExecutionResult localFiltered,
                                               gpu::Texture localTexture,
                                               const gpu::TextureDescriptor& localDesc,
                                               const GeodeLocalRasterGeometry& geometry,
                                               const Transform2d& bufferFromLocal,
                                               const Transform2d& bufferFromFilter) {
    if (localFiltered.kind != geode::FilterExecutionResult::Kind::Output) {
      releaseSharpShadow(sharp);
      releaseTextureAtFrameEnd(std::move(localFiltered.texture), localFiltered.desc);
      releaseTextureAtFrameEnd(std::move(localTexture), localDesc);
      return TransformedFilterResult::NotApplicable;
    }

    auto merged = mergeTransformedShadow(frame, sharp, localFiltered.texture, geometry,
                                         bufferFromLocal, bufferFromFilter);
    if (!merged) {
      retainTextureAfterAbandonedFrame(std::move(localFiltered.texture), localFiltered.desc);
      retainTextureAfterAbandonedFrame(std::move(localTexture), localDesc);
      retainTextureAfterAbandonedFrame(std::move(frame.layerTexture), frame.layerDesc);
      abandonFrameRecordingAfterFilterFailure(frame.savedTarget);
      releaseSharpShadow(sharp);
      return TransformedFilterResult::FrameAbandoned;
    }

    releaseSharpShadow(sharp);
    releaseTextureAtFrameEnd(std::move(localFiltered.texture), localFiltered.desc);
    releaseTextureAtFrameEnd(std::move(localTexture), localDesc);
    if (merged->kind != geode::FilterExecutionResult::Kind::Output) {
      return TransformedFilterResult::NotApplicable;
    }
    beginFilterComposite(frame);
    drawDeviceFilterOutput(frame, merged->texture, merged->desc);
    releaseTextureAtFrameEnd(std::move(merged->texture), merged->desc);
    return TransformedFilterResult::Composited;
  }

  void resampleFilterCapture(const FilterStackFrame& frame, const gpu::Texture& localTexture,
                             const gpu::TextureDescriptor& localDesc,
                             const Transform2d& localFromDevice, const Box2d& localRegionPixels) {
    auto resampleEncoder =
        std::make_unique<geode::GeoEncoder>(*device, *pipeline, *gradientPipeline, *imagePipeline,
                                            localTexture, localDesc.size, *frameGpuEncoder);
    configureEncoder(*resampleEncoder);
    if (std::exchange(failNextTransformedResampleArenaWriteForTesting, false)) {
      resampleEncoder->injectNextArenaWriteFailureForTesting();
    }
    // The filter region bounds SourceGraphic, so the capture beyond it must not reach the padding
    // the blur reads. The region is axis-aligned in this local space even when the device
    // transform skews or rotates it; round outward like the CPU executor's source crop.
    const auto regionX0 = static_cast<int32_t>(std::floor(localRegionPixels.topLeft.x));
    const auto regionY0 = static_cast<int32_t>(std::floor(localRegionPixels.topLeft.y));
    resampleEncoder->setScissorRect(
        regionX0, regionY0,
        static_cast<int32_t>(std::ceil(localRegionPixels.bottomRight.x)) - regionX0,
        static_cast<int32_t>(std::ceil(localRegionPixels.bottomRight.y)) - regionY0);
    resampleEncoder->setTransform(localFromDevice);
    // The capture's texel (0, 0) is device (-offsetX, -offsetY) when the input buffer extends
    // past the viewport's top-left corner. The local raster follows the filter region while the
    // capture follows the device grid, so a whole-pixel device move shifts the capture under the
    // raster; anchoring the mapping at the raster origin keeps every bilinear weight unchanged.
    resampleEncoder->drawTextureAnchored(
        frame.layerTexture,
        Box2d::FromXYWH(-static_cast<double>(frame.filterBufferOffsetX),
                        -static_cast<double>(frame.filterBufferOffsetY),
                        static_cast<double>(frame.layerDesc.size.width),
                        static_cast<double>(frame.layerDesc.size.height)),
        kWholeTextureUv, 1.0, /*sourceIsPremultiplied=*/true, Vector2i::Zero());
    resampleEncoder->finish();
    retireFinishedEncoder(std::move(resampleEncoder));
  }

  static std::pair<const gpu::Texture&, const gpu::TextureDescriptor&> localFilterSource(
      const FilterStackFrame& frame, const gpu::Texture& resampled,
      const gpu::TextureDescriptor& resampledDesc) {
    if (frame.localCaptureFromParent) {
      return {frame.layerTexture, frame.layerDesc};
    }
    return {resampled, resampledDesc};
  }

  geode::FilterExecutionResult evaluateTransformedFilter(const FilterStackFrame& frame,
                                                         const gpu::Texture& localTexture,
                                                         const gpu::TextureDescriptor& localDesc,
                                                         const Box2d& localFilterRegion,
                                                         const Transform2d& localFromFilter,
                                                         const SharpShadowPass* sharp) {
    if (!sharp) {
      return filterEngine->execute(frame.filterGraph, localTexture, localDesc, localFilterRegion,
                                   localFromFilter, *this, nullptr, frame.localFilterPlan, this,
                                   nullptr, nullptr, nullptr, this);
    }
    // The source resample already uses the authored clip. An explicit drop shadow can work over
    // the padding so its blur crosses the authored region until the offset has moved it.
    return filterEngine->execute(sharp->graphs.shadow, localTexture, localDesc, sharp->shadowRegion,
                                 localFromFilter, *this, nullptr, sharp->localPlan, this, nullptr,
                                 nullptr, &localFilterRegion, this);
  }

  std::optional<geode::FilterExecutionResult> executeLocalRasterFilter(
      FilterStackFrame& frame, gpu::Texture& localTexture, const gpu::TextureDescriptor& localDesc,
      const Box2d& localFilterRegion, const Transform2d& localDeviceFromFilter,
      SharpShadowPass* sharp) {
    if (!suspendFrameForFilter()) {
      if (!frameArenaWriteFailed) {
        device->markDeviceLost("frame commands could not be submitted before filter execution");
      }
      retainTextureAfterAbandonedFrame(std::move(localTexture), localDesc);
      retainTextureAfterAbandonedFrame(std::move(frame.layerTexture), frame.layerDesc);
      frame.localRasterRequiredForBudget = true;
      if (frameGpuEncoder == nullptr && !frameArenaWriteFailed && !restoreFrameAfterFilter()) {
        device->markDeviceLost("frame encoder could not be restored after filter suspension");
      }
      abandonFrameRecordingAfterFilterFailure(frame.savedTarget);
      if (sharp) {
        releaseSharpShadow(*sharp);
      }
      return std::nullopt;
    }
    const auto [source, sourceDesc] = localFilterSource(frame, localTexture, localDesc);
    geode::FilterExecutionResult localFiltered = evaluateTransformedFilter(
        frame, source, sourceDesc, localFilterRegion, localDeviceFromFilter, sharp);
    if (!restoreFrameAfterFilter()) {
      device->markDeviceLost("frame encoder could not be restored after filter execution");
      retainTextureWithoutCompletionProof(std::move(localFiltered.texture), localFiltered.desc);
      retainTextureWithoutCompletionProof(std::move(localTexture), localDesc);
      retainTextureWithoutCompletionProof(std::move(frame.layerTexture), frame.layerDesc);
      abandonFrameRecordingAfterFilterFailure(frame.savedTarget);
      if (sharp) {
        releaseSharpShadow(*sharp);
      }
      return std::nullopt;
    }
    return localFiltered;
  }

  std::optional<gpu::Texture> allocateFilterResample(const FilterStackFrame& frame,
                                                     const gpu::TextureDescriptor& desc) {
    if (frame.localCaptureFromParent) {
      return gpu::Texture();
    }
    if (std::exchange(failNextLocalFilterRasterAllocationForTesting, false)) {
      return std::nullopt;
    }
    gpu::Texture texture = acquireTexture(desc);
    if (!texture.isValid()) {
      return std::nullopt;
    }
    return texture;
  }

  static std::optional<components::FilterStandardInput> localShadowForeground(
      const FilterStackFrame& frame) {
    if (frame.localCaptureFromParent) {
      return std::nullopt;
    }
    return ApplicableTransformedShadow(frame.filterGraph, frame.deviceFromFilter,
                                       !frame.localRasterRequiredForBudget);
  }

  TransformedFilterResult tryCompositeTransformedFilter(FilterStackFrame& frame) {
    const std::optional<GeodeLocalRasterGeometry> geometry = transformedFilterGeometry(frame);
    if (!geometry.has_value()) {
      return TransformedFilterResult::NotApplicable;
    }

    const gpu::TextureDescriptor localDesc{
        "RendererGeodeBlurLocal", gpu::Extent2d{geometry->width, geometry->height},
        gpuTextureFormat(),
        gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::Sampled |
            gpu::TextureUsage::CopySrc};
    auto resampled = allocateFilterResample(frame, localDesc);
    if (!resampled) {
      return TransformedFilterResult::NotApplicable;
    }
    gpu::Texture localTexture = std::move(*resampled);
    const Transform2d localFromDevice =
        FilterRasterFromDevice(frame.deviceFromFilter, geometry->paddedRegion.topLeft,
                               Vector2d(geometry->scaleX, geometry->scaleY));
    const Transform2d localDeviceFromFilter =
        Transform2d::Scale(geometry->scaleX, geometry->scaleY);
    const Box2d localFilterRegion(Vector2d(geometry->blurPadding, geometry->blurPadding),
                                  Vector2d(geometry->blurPadding + frame.filterRegion.width(),
                                           geometry->blurPadding + frame.filterRegion.height()));
    const Transform2d bufferFromDevice =
        Transform2d::Translate(frame.filterBufferOffsetX, frame.filterBufferOffsetY);
    const Transform2d bufferFromFilter = frame.deviceFromFilter * bufferFromDevice;
    std::optional<SharpShadowPass> sharp;
    if (const auto foreground = localShadowForeground(frame)) {
      sharp = prepareSharpShadow(frame, *foreground, *geometry, localFilterRegion,
                                 localDeviceFromFilter, bufferFromFilter);
      if (!sharp) {
        releaseTextureWithinFrame(std::move(localTexture), localDesc);
        return TransformedFilterResult::NotApplicable;
      }
    }
    if (localTexture.isValid()) {
      resampleFilterCapture(frame, localTexture, localDesc, localFromDevice,
                            localDeviceFromFilter.transformBox(localFilterRegion));
    }
    auto filtered = executeLocalRasterFilter(frame, localTexture, localDesc, localFilterRegion,
                                             localDeviceFromFilter, sharp ? &*sharp : nullptr);
    if (!filtered) {
      return TransformedFilterResult::FrameAbandoned;
    }
    geode::FilterExecutionResult localFiltered = std::move(*filtered);

    const Transform2d deviceFromLocal =
        Transform2d::Scale(1.0 / geometry->scaleX, 1.0 / geometry->scaleY) *
        Transform2d::Translate(geometry->paddedRegion.topLeft.x, geometry->paddedRegion.topLeft.y) *
        frame.deviceFromFilter;
    if (sharp) {
      return compositeSharpShadow(frame, *sharp, std::move(localFiltered), std::move(localTexture),
                                  localDesc, *geometry, deviceFromLocal * bufferFromDevice,
                                  bufferFromFilter);
    }
    beginFilterComposite(frame);
    const auto [source, sourceDesc] = localFilterSource(frame, localTexture, localDesc);
    if (const gpu::Texture* composited = filterOutput(localFiltered, source)) {
      drawLocalRasterComposite(*encoder, *composited, *geometry, deviceFromLocal);
    }
    releaseTextureAtFrameEnd(std::move(localFiltered.texture), localFiltered.desc);
    releaseTextureAtFrameEnd(std::move(localTexture), localDesc);
    frame.localRasterRequiredForBudget = false;
    return TransformedFilterResult::Composited;
  }

  /// Executes one ordinary filter graph between the submitted source and following frame encoder.
  std::optional<geode::FilterExecutionResult> executeFilterGraph(
      FilterStackFrame& frame, const Transform2d& bufferDeviceFromFilter) {
    geode::FilterExecutionResult filtered;
    filtered.kind = geode::FilterExecutionResult::Kind::SourceGraphic;
    if (frame.localRasterRequiredForBudget || frame.localCaptureFromParent) {
      // There is no usable device capture when the graph required or already captured its local
      // raster. Fail closed if that execution could not run.
      filterExecutionBudget->reject();
      filtered.kind = geode::FilterExecutionResult::Kind::Failed;
      return filtered;
    }
    if (!filterEngine || frame.filterGraph.empty() ||
        static_cast<std::uint64_t>(frame.layerDesc.size.width) * frame.layerDesc.size.height >
            components::kMaximumFilterSurfacePixels) {
      return filtered;
    }

    if (!suspendFrameForFilter()) {
      if (frameArenaWriteFailed) {
        filterExecutionBudget->release(frame.filterReservation);
        retainTextureAfterAbandonedFrame(std::move(frame.layerTexture), frame.layerDesc);
        abandonFrameRecordingAfterFilterFailure(frame.savedTarget);
        return std::nullopt;
      }
      device->markDeviceLost("frame commands could not be submitted before filter execution");
      retainTextureWithoutCompletionProof(std::move(frame.layerTexture), frame.layerDesc);
      filtered.kind = geode::FilterExecutionResult::Kind::Failed;
    } else {
      filtered = filterEngine->execute(
          frame.filterGraph, frame.layerTexture, frame.layerDesc, frame.filterRegion,
          bufferDeviceFromFilter, *this, nullptr, frame.fullFilterPlan, this,
          frame.backdropTexture.isValid() ? &frame.backdropTexture : nullptr, &frame.backdropDesc,
          nullptr, this);
    }
    if (frameGpuEncoder == nullptr && !restoreFrameAfterFilter()) {
      device->markDeviceLost("frame encoder could not be restored after filter execution");
      filterExecutionBudget->release(frame.filterReservation);
      retainTextureWithoutCompletionProof(std::move(filtered.texture), filtered.desc);
      retainTextureWithoutCompletionProof(std::move(frame.layerTexture), frame.layerDesc);
      abandonFrameRecordingAfterFilterFailure(frame.savedTarget);
      return std::nullopt;
    }
    return filtered;
  }

  void suspendMaskClipsForCapture(MaskStackFrame& frame) {
    frame.savedClipStack = std::move(clipStack);
    clipStack.clear();
    if (frame.hasRegionClip) {
      // Preserve admission timing and empty capture on failure. Successful region coverage must
      // not be applied to each mask primitive or multiplied again by an isolated layer.
      pushClipEntry(std::move(frame.compositeRegionClip.back()));
      frame.compositeRegionClip.clear();
    }
    updateEncoderScissor();
  }

  /// Folds \p entry into \p active. The shader tests one polygon of at most four edges. Each
  /// polygon entry is intersected with the polygon already active; an intersection of at most four
  /// edges is exact and becomes the active polygon. A larger one cannot be carried: an entry marked
  /// boundsFallbackWhenStacked then keeps the enclosing polygon and clips only through its
  /// device-space bounds in the scissor, and any other entry replaces the active polygon. Carrying
  /// the full intersection is tracked in #2627.
  /// @param intersections Counts the polygon intersection this performs, at most one.
  static void FoldClipPolygon(ActiveClipPolygon& active, const ClipStackEntry& entry,
                              uint64_t& intersections) {
    if (!entry.hasPolygon || active.empty) {
      return;
    }
    const std::span<const Vector2d> quad(entry.polygonCorners);
    if (active.count == 0) {
      std::copy(quad.begin(), quad.end(), active.corners.begin());
      active.count = quad.size();
      return;
    }
    ++intersections;
    const ClipVertices intersection = IntersectConvexPolygons(
        std::span<const Vector2d>(active.corners.data(), active.count), quad);
    if (intersection.size() < 3) {
      active.empty = true;
    } else if (intersection.size() <= active.corners.size()) {
      for (std::size_t i = 0; i < intersection.size(); ++i) {
        active.corners[i] = intersection[i];
      }
      active.count = intersection.size();
    } else if (!entry.boundsFallbackWhenStacked) {
      std::copy(quad.begin(), quad.end(), active.corners.begin());
      active.count = quad.size();
    }
  }

  /// The clip state of a stack whose top is \p entry over a stack whose state is \p below: the
  /// scissor rectangles intersected, the polygon folded by FoldClipPolygon, the topmost mask.
  /// @param intersections Counts the polygon intersections performed, at most one.
  static ClipFold FoldClipEntry(const ClipFold& below, const ClipStackEntry& entry,
                                uint64_t& intersections) {
    ClipFold fold = below;
    if (entry.maskResolveViewHandle != nullptr) {
      // Only the topmost path-clip mask is bound; nested path-clip intersections are a TODO
      // (would need multiple clip-mask bindings in the fragment shader or a per-clip
      // compositing pass).
      fold.maskTexture = entry.maskResolveTextureHandle;
      fold.maskView = entry.maskResolveViewHandle;
    }
    if (fold.drawsNothing) {
      return fold;
    }
    if (entry.knownEmpty) {
      fold.drawsNothing = true;
      return fold;
    }
    if (entry.allocationRejected) {
      fold.drawsNothing = true;
      return fold;
    }
    if (entry.valid) {
      if (!fold.scissor.has_value()) {
        fold.scissor = entry.pixelRect;
      } else {
        // Intersect: take the overlap of the two rectangles.
        const Box2d& active = *fold.scissor;
        const double x0 = std::max(active.topLeft.x, entry.pixelRect.topLeft.x);
        const double y0 = std::max(active.topLeft.y, entry.pixelRect.topLeft.y);
        const double x1 = std::min(active.bottomRight.x, entry.pixelRect.bottomRight.x);
        const double y1 = std::min(active.bottomRight.y, entry.pixelRect.bottomRight.y);
        fold.scissor = Box2d(Vector2d(x0, y0), Vector2d(std::max(x0, x1), std::max(y0, y1)));
      }
    }
    FoldClipPolygon(fold.polygon, entry, intersections);
    if (fold.polygon.empty) {
      // The stacked polygons do not overlap, so nothing under them draws.
      fold.drawsNothing = true;
    }
    return fold;
  }

  /// Pushes \p entry onto `clipStack`, caching on it the fold of the stack it tops. Every push
  /// goes through here, including an entry moved back from a saved stack, whose cached fold
  /// described the stack it was pushed onto before. Restoring a whole saved stack keeps each
  /// entry over the entries it was folded with, so their folds stay valid.
  void pushClipEntry(ClipStackEntry&& entry) {
    entry.fold = FoldClipEntry(clipStackFold(), entry, counters.clipPolygonIntersections);
    clipStack.push_back(std::move(entry));
  }

  /// The clip state of the whole of `clipStack`: the fold cached on its top entry, or the
  /// unclipped state when it is empty.
  const ClipFold& clipStackFold() const {
    static const ClipFold kUnclipped;
    return clipStack.empty() ? kUnclipped : clipStack.back().fold;
  }

  /// Sends \p active to the encoder, repeating the last vertex of a triangle: the zero-length edge
  /// that leaves is a half-plane every sample is inside of.
  void applyClipPolygon(const ActiveClipPolygon& active) {
    if (active.count == 0) {
      encoder->clearClipPolygon();
      return;
    }
    Vector2d corners[4];
    for (std::size_t i = 0; i < 4; ++i) {
      corners[i] = active.corners[std::min(i, active.count - 1)];
    }
    encoder->setClipPolygon(corners);
  }

  /// Apply the clip state cached on the top of `clipStack` to the active encoder: the scissor
  /// rectangle, the polygon `FoldClipPolygon` built (if any) through `setClipPolygon`, and the
  /// topmost path-clip mask. Called whenever the clip stack or the encoder changes; it folds
  /// nothing itself.
  void updateEncoderScissor() {
    if (!encoder) {
      return;
    }
    const ClipFold& fold = clipStackFold();
    if (fold.drawsNothing) {
      encoder->clearClipPolygon();
      encoder->clearClipMask();
      encoder->setScissorRect(0, 0, 0, 0);
      return;
    }
    applyClipPolygon(fold.polygon);

    if (fold.maskView != nullptr) {
      // Pass the parent texture alongside the view so the encoder keeps
      // the Vulkan resource alive even after this clip-stack entry is
      // destroyed. The 1-arg setClipMask overload accidentally left the
      // view dangling across `popClip`→`pop_back`→destructor →
      // `updateEncoderScissor` sequences; see issue #551.
      encoder->setClipMask(*fold.maskTexture, *fold.maskView);
    } else {
      encoder->clearClipMask();
    }

    if (!fold.scissor.has_value()) {
      encoder->clearScissorRect();
      return;
    }
    const Box2d& active = *fold.scissor;
    // Convert the Box2d corners to integer pixel coords. Floor on topLeft
    // and ceil on bottomRight so we don't accidentally clip fractional
    // edge pixels that the rasterizer still wants to cover.
    const int32_t x = static_cast<int32_t>(std::floor(active.topLeft.x));
    const int32_t y = static_cast<int32_t>(std::floor(active.topLeft.y));
    const int32_t w = std::max(0, static_cast<int32_t>(std::ceil(active.bottomRight.x)) - x);
    const int32_t h = std::max(0, static_cast<int32_t>(std::ceil(active.bottomRight.y)) - y);
    encoder->setScissorRect(x, y, w, h);
  }

  // Stub-state latches - set on first warning in verbose mode so each
  // unimplemented feature logs exactly once per renderer.
  bool warnedLayer = false;
  bool warnedGradient = false;
  bool warnedText = false;

  /// The solid color of \p server at \p effectiveOpacity: a solid's color, or a paint server
  /// reference's fallback color. Returns nullopt for None and for a reference without a fallback.
  std::optional<css::RGBA> resolveSolidPaint(const components::ResolvedPaintServer& server,
                                             double effectiveOpacity) {
    if (std::holds_alternative<PaintServer::None>(server)) {
      return std::nullopt;
    }
    const css::RGBA currentColor = paint.currentColor;
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

  /// The encoder-facing paint for \p slot at \p opacity.
  ///
  /// Its shader coordinates and repeat period are both in raster-tile pixels. The fill shader's
  /// sample position is path-local, which is also the pattern target space supplied by the
  /// driver, so no device-space reconstruction is needed.
  ///
  /// The result borrows \p slot's tile rather than naming a copy of it, so it has to reach the
  /// encoder before the slot is released. Every caller consumes it in the same statement.
  /// @param slot Promoted pattern tile to sample.
  /// @param opacity Opacity to apply to the sampled texels.
  geode::GeoEncoder::PatternPaint buildPatternPaint(const PatternPaintSlot& slot,
                                                    double opacity) const {
    geode::GeoEncoder::PatternPaint p;
    p.tile = &slot.tile;
    p.tileView = slot.tileView;
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

  /// A failed upload refuses later draws until the next frame resets the recording state.
  [[nodiscard]] bool canRecordGeometry() const {
    return device && encoder && !frameArenaWriteFailed;
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

  /// Configure a path-capable encoder and map captured geometry back to root pixels.
  /// Pattern-tile source geometry does not belong to the root overlay.
  void configurePathEncoder(geode::GeoEncoder& pathEncoder, bool collectGeometry = true) {
    configureEncoder(pathEncoder);
    if (debugGeometryOverlay && collectGeometry) {
      pathEncoder.setGeometryDebugSink(this, filterCaptureTransform().inverse());
    }
  }

  /// Capture the exact post-vertex Slug triangles at the GPU submission
  /// boundary. This reproduces `SlugFillSource.h`'s half-pixel miter dilation
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
    if (!debugGeometryOverlay || geometryDebugEdges.empty() || !target.isValid() ||
        frameGpuEncoder == nullptr) {
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

    replaceActiveEncoder(std::make_unique<geode::GeoEncoder>(*device, *pipeline, *gradientPipeline,
                                                             *imagePipeline, activeTarget(),
                                                             targetExtent(), *frameGpuEncoder));
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
    /// A batch requires `PathShape::canDeferGeometry()`. Its computed path and cached derivatives
    /// must remain unchanged until the flush: erasure can repoint paged storage at another entity,
    /// and replacement can change geometry while preserving its address. External-use occurrences
    /// keep their cache identity but decline deferral, so drawPath consumes their geometry.
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
      /// Last-written bytes of `recordSlotOverride`: across frames for a
      /// persistent occurrence record (text glyphs, a slot's repeats), or within
      /// this frame for a repeat's temporary record, so its batch's flush does
      /// not publish it a second time. Non-null turns the override write into a
      /// skip-when-unchanged write. Null for the temporary slot of a converted
      /// singleton, which is always written.
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
    /// The uniform a batch of repeats drawn under a clip polygon binds, which
    /// carries the polygon; no value for every other batch, which binds the
    /// shared batch uniform.
    std::optional<geode::GeoEncoder::ClipUniformBinding> sceneClipUniform;
    /// GPU-residence slot for the encode this batch started on - the source
    /// entity's fill slot for a fill, its stroke slot for a stroked outline.
    /// Used only by the size-1 flush path; a size >= 2 flush routes through
    /// the instanced arena path, which has no per-entity residence.
    geode::GeodeResidentSlot* residentSlot = nullptr;
  };
  std::optional<PendingBatch> pendingBatch;

  /// Record-slab slots allocated this frame for same-entity repeat draws
  /// (markers, repeated `<use>`): one record per draw, so earlier recorded
  /// batches keep their own content at submit time. Freed (deferred) when
  /// the frame ends, by `releaseFrameLoans`; the slab merges the frees at
  /// its next frame.
  /// Temporary per-frame record slots for same-frame repeat draws. A deque
  /// keeps element addresses stable (callers hold a pointer to the newest
  /// entry while later draws append), and each entry carries the slab it
  /// was allocated from so a multi-document frame frees every slot into
  /// its own slab rather than whichever document draw() saw last.
  struct SceneTempRecordSlot {
    geode::GeodeRecordSlab::Slot slot;
    std::shared_ptr<geode::GeodeRecordSlab> slab;
    /// Bytes written to `slot` this frame, for callers that publish through it.
    std::vector<uint8_t> lastRecord;
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
  /// `sceneTempRecordSlots` so its address stays valid until the frame ends
  /// and returns it to `slab`. Returns null when `slab` is absent or the
  /// device cannot back the allocation.
  /// @param slab Record slab to allocate from.
  /// @param recordCache When non-null, receives the slot's within-frame record cache.
  const geode::GeodeRecordSlab::Slot* allocateTempRecordSlot(
      const std::shared_ptr<geode::GeodeRecordSlab>& slab,
      std::vector<uint8_t>** recordCache = nullptr) {
    if (!slab) {
      return nullptr;
    }
    geode::GeodeRecordSlab::Slot tempSlot;
    if (!slab->allocateSlot(*device, tempSlot)) {
      return nullptr;
    }
    sceneTempRecordSlots.push_back(SceneTempRecordSlot{tempSlot, slab, {}});
    if (recordCache != nullptr) {
      *recordCache = &sceneTempRecordSlots.back().lastRecord;
    }
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

  /// Drop an entity's resident image textures when its `LoadedImageComponent` is replaced or
  /// destroyed. Connected to entt's `on_update` / `on_destroy` signals like
  /// \ref onComputedPathChanged.
  static void onLoadedImageChanged(Registry& registry, Entity entity);

  /// Drop an entity's cached clip geometry when its computed clip paths are created, replaced or
  /// destroyed. Connected to entt's `on_construct` / `on_update` / `on_destroy` signals.
  static void onComputedClipPathsChanged(Registry& registry, Entity entity);

  /// Scratch buffer for the no-source-entity stroke path. `getStrokeDerived`
  /// uses this as stable storage when there's no `GeodePathCacheComponent`
  /// to live on (e.g. `drawRect` / `drawEllipse` convenience draws). Only
  /// one active draw at a time, so a single slot is safe.
  Path strokeScratchPath;
  std::optional<geode::EncodedPath> fillScratchEncode;
  std::optional<geode::EncodedPath> strokeScratchEncode;
  /// Encode of the clip shape being filled when the document's cache budget refuses to keep it.
  std::optional<geode::EncodedPath> clipScratchEncode;
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
    /// Index of the `GeodePathCacheComponent::fillVariants` entry holding a persistent encode,
    /// which also selects its resident slots.
    std::size_t variant = 0;
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
    /// Index of the `GeodePathCacheComponent::strokeVariants` entry holding a persistent outline,
    /// which also selects its resident slots.
    std::size_t variant = 0;
    /// Stroke pieces use NonZero so their covered regions form a union.
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
    return reserveFrameGeometry(logicalDraws, items * logicalDraws,
                                static_cast<std::uint64_t>(retainedBytes) * logicalDraws);
  }

  bool canEncodeGeometry() const override {
    return admittingQueuedDraws || !geometryBudget->rejected();
  }

  /// Charges frame geometry. While the pending batch flushes, its draws, all queued before any
  /// refusal latched the frame, are admitted against the limits alone: drawn in order they would
  /// have been admitted before the refusal, as on the CPU renderer.
  bool reserveFrameGeometry(std::size_t draws, std::size_t items, std::uint64_t bytes) {
    return admittingQueuedDraws ? geometryBudget->reserveQueued(draws, items, bytes)
                                : geometryBudget->reserve(draws, items, bytes);
  }

  /// Set while `flushPendingBatch` draws the batch; see `reserveFrameGeometry`.
  bool admittingQueuedDraws = false;

  /// A resident instance retains its record for the frame, not a copy of the geometry, which the
  /// document's resident budget already holds.
  bool admitResidentInstance(const geode::EncodedPath& encoded) override {
    if (encoded.rejected()) {
      geometryBudget->reject();
      return false;
    }
    if (encoded.empty()) {
      return true;
    }
    return reserveFrameGeometry(1u, encoded.geometryItemCount(), sizeof(geode::InstanceRecord));
  }

  void releaseResidentInstance(const geode::EncodedPath& encoded) override {
    if (encoded.empty() || encoded.rejected()) {
      return;
    }
    geometryBudget->release(1u, encoded.geometryItemCount(), sizeof(geode::InstanceRecord));
  }

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

  /// Encodes \p path at the current device tolerance, without admitting it to the frame.
  geode::EncodedPath encodeAtDeviceTolerance(const Path& path, FillRule rule) {
    device->countPathEncode();
    return geode::GeodePathEncoder::encode(
        path, rule, geode::FillCubicToleranceFor(deviceFromLocalTransform), pathEncoderLimits);
  }

  std::optional<geode::EncodedPath> encodeGeometry(const Path& path, FillRule rule) {
    if (geometryBudget->rejected()) {
      return std::nullopt;
    }
    geode::EncodedPath encoded = encodeAtDeviceTolerance(path, rule);
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

  /**
   * Like \ref encodeTransientGeometry, but when the encoder's limits reject \p path, returns null
   * and leaves the frame admitted, so the caller can encode the path's parts instead.
   */
  const geode::EncodedPath* encodeTransientGeometryOrSplit(const Path& path, FillRule rule) {
    if (geometryBudget->rejected()) {
      return &rejectedGeometry;
    }
    geode::EncodedPath encoded = encodeAtDeviceTolerance(path, rule);
    if (encoded.rejected()) {
      return nullptr;
    }
    transientTextEncodes.push_back(std::move(encoded));
    return &transientTextEncodes.back();
  }

  /**
   * Expands each glyph outline's stroke, skipping glyphs whose stroke is empty. Each expanded
   * outline's storage is charged to the frame geometry budget as it is produced, so a chunk is
   * never expanded past the budget before its strokes are encoded: once a charge fails, the frame
   * is rejected, nothing more is expanded, and no outlines are returned. Release the returned
   * outlines' charges with \ref releaseTextStrokeOutlines once they are drawn.
   *
   * @param glyphs Placed glyph outlines, in order.
   * @param style Stroke to expand.
   */
  std::vector<Path> strokeGlyphOutlines(std::span<const Path> glyphs, const StrokeStyle& style) {
    std::vector<Path> outlines;
    outlines.reserve(glyphs.size());
    for (const Path& glyph : glyphs) {
      if (geometryBudget->rejected()) {
        break;
      }
      Path stroked = glyph.strokeToFill(style, strokeFlattenTolerance());
      if (stroked.empty()) {
        continue;
      }
      countStrokeOutline(stroked);
      const std::optional<std::size_t> bytes = stroked.retainedBytes();
      if (!bytes.has_value() || !geometryBudget->reserve(0u, 0u, *bytes)) {
        geometryBudget->reject();
        break;
      }
      outlines.push_back(std::move(stroked));
    }
    if (geometryBudget->rejected()) {
      releaseTextStrokeOutlines(outlines);
      return {};
    }
    return outlines;
  }

  /// Releases the frame geometry charges \ref strokeGlyphOutlines made for \p outlines.
  void releaseTextStrokeOutlines(std::span<const Path> outlines) {
    for (const Path& outline : outlines) {
      geometryBudget->release(0u, 0u, outline.retainedBytes().value_or(0u));
    }
  }

  /// Most painted shapes one text element keeps per device; see `geode::GeodeTextShapeCache`.
  std::size_t textShapeLimit = geode::GeodeTextShapeCache::kDefaultMaximumShapes;

  /// The retained shapes one text draw paints from, in paint order. A null `cache` sends every
  /// shape of the draw through a per-frame encode: the text has no source entity, or a frame that
  /// has not submitted drew the element already (an earlier draw in this frame, or an outer frame
  /// around an offscreen pass), whose recorded draws its shapes must keep serving.
  struct TextShapeCursor {
    /// Whether the draw keeps its shapes; false sends every shape through a per-frame encode.
    bool keeps = false;
    /// The element's shapes on this device; null until the element keeps its first shape here.
    geode::GeodeTextShapeCache* cache = nullptr;
    Registry* registry = nullptr;  //!< Registry of the element.
    Entity element = entt::null;   //!< The element's entity.
    std::size_t next = 0;          //!< Ordinal of the next shape.
  };

  /// What one painted text shape draws from: its encode, and its resident slots when the shape is
  /// retained. A null encode means the encoder refused a splittable shape whole.
  struct TextShape {
    const geode::EncodedPath* encoded = nullptr;               //!< Encode to draw.
    geode::GeodeResidentSlot* slot = nullptr;                  //!< Solid and pattern residence.
    geode::GeodeResidentGradientSlot* gradientSlot = nullptr;  //!< Gradient residence.
  };

#ifdef DONNER_TEXT_ENABLED
  /// The glyph fill of a text run, which paints a decoration that has no recorded origin.
  struct TextRunFillPaint {
    css::RGBA solid = css::RGBA(0, 0, 0, 0);  //!< Solid fill, transparent when not a solid.
    /// Gradient fill server, or null when the run's fill is not a gradient.
    const components::ResolvedPaintServer* gradient = nullptr;
    bool pattern = false;  //!< Whether the run fills with the staged element pattern.
    double opacity = 1.0;  //!< Opacity of a gradient or pattern fill.
  };

  /// The paint of one text decoration origin's lines. Gradients map onto the text element's
  /// bounds, as the glyphs' gradients do.
  struct TextDecorationPaint {
    css::RGBA solidFill = css::RGBA(0, 0, 0, 0);  //!< Solid fill, transparent for none.
    /// Gradient fill server, or null when the fill is not a gradient.
    const components::ResolvedPaintServer* gradientFill = nullptr;
    bool patternFill = false;              //!< Whether the fill samples the staged element pattern.
    double fillOpacity = 1.0;              //!< Opacity of a gradient or pattern fill.
    std::optional<css::RGBA> solidStroke;  //!< Solid stroke color, when stroked with one.
    /// Gradient stroke server, or null when the stroke is not a gradient.
    const components::ResolvedPaintServer* gradientStroke = nullptr;
    double strokeOpacity = 1.0;              //!< Opacity of a gradient stroke.
    std::optional<StrokeStyle> strokeStyle;  //!< Set when the decoration is stroked.
    PaintOrder paintOrder;                   //!< Order of the decoration's fill and stroke.
  };

  /**
   * Resolves the paint of \p decoration, declared on an element whose lines are drawn over a span
   * of opacity \p spanOpacity. SVG 2 section 11.11 paints a decoration with the fill and stroke of
   * the element that declares it.
   *
   * @param decoration Declaring origin.
   * @param spanOpacity Opacity of the span the lines are drawn over.
   * @param runFill The run's own fill, for a decoration without a recorded origin; null otherwise.
   */
  TextDecorationPaint resolveTextDecorationPaint(const components::TextDecorationOrigin& decoration,
                                                 double spanOpacity,
                                                 const TextRunFillPaint* runFill) {
    TextDecorationPaint result;
    result.paintOrder = decoration.paintOrder;
    resolveTextDecorationFill(decoration, spanOpacity, runFill, result);
    resolveTextDecorationStroke(decoration, spanOpacity, result);
    return result;
  }

  /**
   * The fill half of \ref resolveTextDecorationPaint.
   *
   * @param decoration Declaring origin.
   * @param spanOpacity Opacity of the span the lines are drawn over.
   * @param runFill The run's own fill, for a decoration without a recorded origin; null otherwise.
   * @param result Receives the fill paint and its opacity.
   */
  void resolveTextDecorationFill(const components::TextDecorationOrigin& decoration,
                                 double spanOpacity, const TextRunFillPaint* runFill,
                                 TextDecorationPaint& result) {
    if (runFill != nullptr) {
      result.solidFill = runFill->solid;
      result.gradientFill = runFill->gradient;
      result.patternFill = runFill->pattern && patternFillPaint.has_value();
      result.fillOpacity = runFill->opacity;
      return;
    }
    result.fillOpacity = decoration.fillOpacity * spanOpacity;
    if (paintReferenceIsGradient(decoration.fill)) {
      result.gradientFill = &decoration.fill;
    } else if (patternFillPaint.has_value() && decoration.usesPreparedFill(paint.fill)) {
      result.patternFill = true;
    } else if (const std::optional<css::RGBA> color =
                   resolveSolidPaint(decoration.fill, result.fillOpacity)) {
      result.solidFill = *color;
    }
  }

  /**
   * The stroke half of \ref resolveTextDecorationPaint.
   *
   * @param decoration Declaring origin.
   * @param spanOpacity Opacity of the span the lines are drawn over.
   * @param result Receives the stroke paint, its opacity and its style.
   */
  void resolveTextDecorationStroke(const components::TextDecorationOrigin& decoration,
                                   double spanOpacity, TextDecorationPaint& result) {
    if (!(decoration.strokeWidth > 0.0) ||
        std::holds_alternative<PaintServer::None>(decoration.stroke)) {
      return;
    }
    result.strokeOpacity = decoration.strokeOpacity * spanOpacity;
    if (paintReferenceIsGradient(decoration.stroke)) {
      result.gradientStroke = &decoration.stroke;
    } else {
      result.solidStroke = resolveSolidPaint(decoration.stroke, result.strokeOpacity);
      if (!result.solidStroke.has_value() || result.solidStroke->a == 0) {
        result.solidStroke.reset();
        return;
      }
    }
    StrokeParams strokeParams;
    strokeParams.strokeWidth = decoration.strokeWidth;
    result.strokeStyle = toStrokeStyle(strokeParams);
  }

  /**
   * Paints one decoration line \p path, in the text element's local space, with
   * \p decorationPaint. A gradient resolves against \p textBoundsPath, the text element's bounds,
   * for both the fill and the stroke, as the glyphs' gradients do.
   *
   * @param path Decoration line outline.
   * @param textBoundsPath The text element's bounds as a path.
   * @param decorationPaint Resolved paint of the declaring origin.
   * @param shapes The text draw's retained shapes, which the line and its stroke draw from.
   */
  void paintTextDecorationPath(const Path& path, const Path& textBoundsPath,
                               const TextDecorationPaint& decorationPaint,
                               TextShapeCursor& shapes) {
    if (path.empty()) {
      return;
    }
    const auto fillDecoration = [&]() {
      if (decorationPaint.gradientFill != nullptr) {
        const TextShape shape = textShape(shapes, path);
        drawPaintedPathAgainst(textBoundsPath, path, *decorationPaint.gradientFill,
                               decorationPaint.fillOpacity, FillRule::NonZero, shape.encoded,
                               shape.slot, shape.gradientSlot);
      } else if (decorationPaint.patternFill) {
        const TextShape shape = textShape(shapes, path);
        releaseGradientRamp(shape.gradientSlot);
        syncTransform();
        emitPatternFill(path, FillRule::NonZero,
                        buildPatternPaint(*patternFillPaint, decorationPaint.fillOpacity),
                        shape.encoded, shape.slot);
      } else if (decorationPaint.solidFill.a != 0) {
        const TextShape shape = textShape(shapes, path);
        releaseGradientRamp(shape.gradientSlot);
        emitSolidFill(path, decorationPaint.solidFill, FillRule::NonZero, shape.encoded,
                      shape.slot);
      }
    };
    const auto strokeDecoration = [&]() {
      if (!decorationPaint.strokeStyle.has_value()) {
        return;
      }
      // Device-aware flattening, as in every other renderer-side stroke.
      const Path stroked =
          path.strokeToFill(*decorationPaint.strokeStyle, strokeFlattenTolerance());
      if (stroked.empty()) {
        return;
      }
      countStrokeOutline(stroked);
      const TextShape shape = textShape(shapes, stroked);
      if (decorationPaint.gradientStroke != nullptr) {
        drawPaintedPathAgainst(textBoundsPath, stroked, *decorationPaint.gradientStroke,
                               decorationPaint.strokeOpacity, FillRule::NonZero, shape.encoded,
                               shape.slot, shape.gradientSlot);
      } else {
        releaseGradientRamp(shape.gradientSlot);
        emitSolidFill(stroked, *decorationPaint.solidStroke, FillRule::NonZero, shape.encoded,
                      shape.slot);
      }
    };
    // The decoration's own fill/stroke order is the declaring element's `paint-order`.
    PaintFillAndStroke(decorationPaint.paintOrder, fillDecoration, strokeDecoration);
  }

  /**
   * Draws the glyphs of \p run whose face has only bitmap glyphs, such as color emoji. Each glyph
   * draws its decoded strike as a textured quad through the image pipeline, placed and scaled from
   * the strike to the glyph's size as `RendererTinySkia::drawText` places it, at the span's
   * opacity. The pixels come from the font on the CPU and are uploaded; nothing is read back.
   *
   * @param textEngine Engine providing bitmap glyph metadata and pixels.
   * @param text Text whose spans give each run's opacity.
   * @param runIndex Index of \p run, and of its span.
   * @param run Laid-out run.
   * @param runScale `scaleForPixelHeight` of the run's face at its used size.
   * @param runBitmapOnly Whether the run's face has only bitmap glyphs.
   * @return False when the frame's text budget rejected a glyph; the text draws nothing further.
   */
  bool drawBitmapGlyphs(TextEngine& textEngine, const components::ComputedTextComponent& text,
                        std::size_t runIndex, const TextRun& run, float runScale,
                        bool runBitmapOnly) {
    // A run of an outline face draws bitmap glyphs only for glyphs a fallback face draws.
    if (!runBitmapOnly &&
        std::none_of(run.glyphs.begin(), run.glyphs.end(),
                     [](const TextGlyph& glyph) { return static_cast<bool>(glyph.font); })) {
      return true;
    }
    const double opacity = runIndex < text.spans.size() ? text.spans[runIndex].opacity : 1.0;
    for (const TextGlyph& glyph : run.glyphs) {
      if (glyph.glyphIndex == 0) {
        continue;  // `.notdef`, skipped as tiny-skia skips it.
      }
      const GlyphFaceScale face = ResolveGlyphFace(textEngine, run, glyph, runScale, runBitmapOnly);
      if (!face.bitmapOnly) {
        continue;
      }
      bool rejected = false;
      std::optional<TextBackend::BitmapGlyph> bitmap =
          AdmitBitmapGlyph(textEngine, face.font, glyph.glyphIndex, face.scale,
                           *textMaterializationBudget, rejected);
      if (rejected) {
        return false;
      }
      if (bitmap.has_value()) {
        drawBitmapGlyph(glyph, std::move(*bitmap), opacity);
      }
    }
    return true;
  }

  /// Draws one decoded bitmap glyph; see \ref drawBitmapGlyphs.
  void drawBitmapGlyph(const TextGlyph& glyph, TextBackend::BitmapGlyph bitmap, double opacity) {
    const Transform2d deviceFromBitmap =
        text_engine_detail::computeLocalFromBitmapGlyph(glyph, bitmap) * deviceFromLocalTransform;
    const Box2d bitmapRect(Vector2d::Zero(), Vector2d(bitmap.width, bitmap.height));
    encoder->setTransform(deviceFromBitmap);
    // No specification governs resampling a glyph strike: `image-rendering` applies to images,
    // not glyphs. Strikes use the kernel of an `<image>` with the default `image-rendering:
    // auto`, as the software renderer does.
    encoder->drawPremultipliedImage(
        ImageResource{std::move(bitmap.rgbaPixels), bitmap.width, bitmap.height}, bitmapRect,
        opacity, ImageRendering::Auto);
    encoder->setTransform(deviceFromLocalTransform);
  }
#endif  // DONNER_TEXT_ENABLED

  /// Most stroke outline points joined into one text stroke shape: far below the path encoder's
  /// 1 << 20 limit, and it bounds how wide one shape of a long, heavily stroked line can get.
  static constexpr std::size_t kMaximumTextStrokeGroupPoints = std::size_t{1} << 16;
  /// \ref kMaximumTextStrokeGroupPoints, unless a test lowers it.
  std::size_t textStrokeGroupPointLimit = kMaximumTextStrokeGroupPoints;

  /// Bounds of each outline, grown by one device pixel, so outlines whose grown bounds do not meet
  /// share no pixel.
  std::vector<Box2d> grownTextStrokeBounds(std::span<const Path> outlines) const {
    const Transform2d localFromDevice = deviceFromLocalTransform.inverse();
    const double margin = std::max(localFromDevice.transformVector(Vector2d(1.0, 0.0)).length(),
                                   localFromDevice.transformVector(Vector2d(0.0, 1.0)).length());
    std::vector<Box2d> bounds;
    bounds.reserve(outlines.size());
    for (const Path& outline : outlines) {
      bounds.push_back(outline.bounds().inflatedBy(margin));
    }
    return bounds;
  }

  /**
   * Returns, for each box, a representative of its group from \ref BoxSweepGrouping: boxes that
   * touch, directly or through other boxes, share a group. Each box tested against a group's
   * bounds is counted in `counters.textStrokeBoundsTests`.
   *
   * @param boxes Boxes to group.
   */
  std::vector<std::size_t> touchingBoxSets(std::span<const Box2d> boxes) {
    return BoxSweepGrouping(boxes).groups(counters.textStrokeBoundsTests);
  }

  /**
   * Groups the stroke outlines of one text chunk into shapes. Outlines whose bounds, grown by a
   * device pixel, touch, directly or through other outlines of the chunk, form one group, wherever
   * they are in the run; outlines in different groups share no pixel. A group can also take in an
   * outline that touches only the union of the group's bounds (see \ref BoxSweepGrouping); that
   * outline shares no pixel with the group, so joining it changes how many shapes are drawn, not
   * which pixels are covered or how often. A group is split, in run order, where its outline
   * points would pass \ref textStrokeGroupPointLimit, and a pixel two parts both cover is then
   * painted twice.
   *
   * @param outlines Stroke outlines of one text chunk's glyphs, in run order.
   * @return Each group's outline indices, in run order.
   */
  std::vector<std::vector<std::size_t>> textStrokeGroups(std::span<const Path> outlines) {
    const std::vector<std::size_t> sets = touchingBoxSets(grownTextStrokeBounds(outlines));
    constexpr std::size_t kNoGroup = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> openGroupOfSet(outlines.size(), kNoGroup);
    std::vector<std::vector<std::size_t>> groups;
    std::vector<std::size_t> groupPoints;
    for (std::size_t index = 0; index < outlines.size(); ++index) {
      std::size_t& group = openGroupOfSet[sets[index]];
      const std::size_t points = outlines[index].points().size();
      if (group == kNoGroup || groupPoints[group] + points > textStrokeGroupPointLimit) {
        group = groups.size();
        groups.emplace_back();
        groupPoints.push_back(0);
      }
      groups[group].push_back(index);
      groupPoints[group] += points;
    }
    return groups;
  }

  /**
   * Draws the stroke outlines of one text chunk's glyphs, joining each group from
   * \ref textStrokeGroups into one NonZero shape, so a pixel where the strokes of touching glyphs
   * overlap is covered once. A group the encoder rejects is drawn outline by outline, so overlaps
   * inside it are painted twice, as when every glyph was its own shape; an outline that cannot be
   * encoded on its own rejects the frame, as any path does.
   *
   * @param outlines Stroke outlines of the chunk's glyphs, in run order.
   * @param shapes The text draw's retained shapes, which each drawn shape takes the next of.
   * @param draw Paints a shape: called with the joined outline and its \ref TextShape.
   */
  template <typename DrawFn>
  void drawTextStrokeOutlines(std::span<const Path> outlines, TextShapeCursor& shapes,
                              const DrawFn& draw) {
    for (const std::vector<std::size_t>& group : textStrokeGroups(outlines)) {
      if (group.size() == 1) {
        const Path& outline = outlines[group.front()];
        draw(outline, textShape(shapes, outline));
        continue;
      }
      PathBuilder builder;
      for (const std::size_t index : group) {
        builder.addPath(outlines[index]);
      }
      const Path joined = builder.build();
      if (const TextShape shape = textShape(shapes, joined, /*splittable=*/true);
          shape.encoded != nullptr) {
        draw(joined, shape);
        continue;
      }
      for (const std::size_t index : group) {
        draw(outlines[index], textShape(shapes, outlines[index]));
      }
    }
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
    // The encode a draw uses depends only on its own tolerance bucket, never on which scales
    // drew the entity before, so an entity drawn at several scales renders the same every frame.
    const double tolerance = geode::FillCubicToleranceFor(deviceFromLocalTransform);
    if (const std::optional<std::size_t> hit = cache.fillVariantFor(tolerance)) {
      auto& variant = cache.fillVariants[*hit];
      variant.lastUse = ++cache.fillUseClock;
      variant.lastUseFrame = currentFrameIndex;
      variant.lastUseDeviceId = device->deviceId();
      return {&*variant.encode, true, *hit};
    }

    std::optional<geode::EncodedPath> encoded = encodeGeometry(path, rule);
    if (!encoded.has_value()) {
      return {&rejectedGeometry, false};
    }
    const std::optional<std::size_t> replaceable = fillVariantToReplace(cache);
    if (!replaceable) {
      // Every variant may still be read by an open frame. Replacing one would make this frame's
      // buckets evict each other on every draw, or pull an encode from under an outer frame's
      // pending batch, so this draw uses a scratch encode and the cached variants and their
      // residence stay put.
      fillScratchEncode = std::move(*encoded);
      return {&*fillScratchEncode, false};
    }
    const std::size_t index = *replaceable;
    auto& variant = cache.fillVariants[index];
    const std::size_t retainedBytes = encoded->retainedBytes();
    if (retainedBytes != std::numeric_limits<std::size_t>::max() &&
        variant.reservation.replace(documentBudget, retainedBytes)) {
      if (variant.encode && pendingBatchReferences(&*variant.encode)) {
        flushPendingBatch();
      }
      if (auto* resident = source.try_get<geode::GeodeResidentPathComponent>()) {
        // Every device's residence for this variant was uploaded from the encode being replaced.
        resident->devices.forEach([index](geode::GeodeResidentPathSlots& slots) {
          slots.fillSlots[index].reset();
          slots.gradientFillSlots[index].reset();
        });
      }
      variant.tolerance = std::any_of(path.commands().begin(), path.commands().end(),
                                      [](const Path::Command& command) {
                                        return command.verb == Path::Verb::CurveTo;
                                      })
                              ? tolerance
                              : 0.0;
      variant.encode = std::move(*encoded);
      variant.lastUse = ++cache.fillUseClock;
      variant.lastUseFrame = currentFrameIndex;
      variant.lastUseDeviceId = device->deviceId();
      return {&*variant.encode, true, index};
    }
    fillScratchEncode = std::move(*encoded);
    return {&*fillScratchEncode, false};
  }

  /// True when a cache variant last used in frame generation \p stamp of device \p stampDeviceId
  /// may still be read by a frame of this device that has not ended: the current frame, or an
  /// outer frame still open around an offscreen pass (`createOffscreenInstance` shares the device
  /// and its generations), which can hold a pending batch pointing at the variant. A stamp from
  /// another device is not checked: its generations are not comparable with this device's, and
  /// the per-entity caches rely on callers not drawing one document on two devices at once.
  [[nodiscard]] bool cacheVariantInUse(std::uint64_t stamp, std::uint64_t stampDeviceId) const {
    return stampDeviceId == device->deviceId() &&
           (stamp == currentFrameIndex || device->frameStampClaimed(stamp));
  }

  /// Index of the fill variant a new tolerance bucket replaces: an empty one when there is one,
  /// otherwise the least recently used one no open frame may still read (`cacheVariantInUse`).
  /// Nullopt when every variant may still be read.
  std::optional<std::size_t> fillVariantToReplace(
      const geode::GeodePathCacheComponent& cache) const {
    std::optional<std::size_t> index;
    for (std::size_t i = 0; i < cache.fillVariants.size(); ++i) {
      const auto& variant = cache.fillVariants[i];
      if (!variant.encode.has_value()) {
        return i;
      }
      if (!cacheVariantInUse(variant.lastUseFrame, variant.lastUseDeviceId) &&
          (!index || variant.lastUse < cache.fillVariants[*index].lastUse)) {
        index = i;
      }
    }
    return index;
  }

  AdmittedEncode getFillEncodeForPaint(EntityHandle source, const Path& path, FillRule rule) {
    if (!paint.drawFillComponent || std::holds_alternative<PaintServer::None>(paint.fill)) {
      return {&emptyGeometry, false};
    }
    return getFillEncode(source, path, rule);
  }

  /// Document whose frame \ref drawDocument is drawing live under its write access, for
  /// \ref settleDocumentResidency at the frame's start. Null outside such a draw, and for a
  /// concurrent-DOM document, whose frames replay a snapshot that must not touch the live document.
  Registry* frameDocument = nullptr;

  /**
   * Take back what this device's slabs for the document in \p registry released before this frame
   * (freed ranges and records, retired bind groups, stop ramps and image textures), whether or not
   * the frame draws anything that looks the slabs up. Without it, a document whose last resident
   * draw went away would keep those until a later draw used its slabs again. Creates nothing; skips
   * a frame that opened inside another frame on this device, as the lookups do.
   *
   * @param registry Registry of the document the frame draws, whose write access the drawing
   *   thread holds for the whole frame, or null when no such document is being drawn.
   */
  void settleDocumentResidency(Registry* registry) {
    if (registry == nullptr || device->oldestOpenFrameGeneration() < currentFrameIndex) {
      return;
    }
    const auto* residencyPtr = registry->ctx().find<std::shared_ptr<DocumentResidency>>();
    if (residencyPtr == nullptr) {
      return;
    }
    DocumentDeviceResidency* residency = (*residencyPtr)->devices.find(device->deviceId());
    if (residency == nullptr) {
      return;
    }
    if (residency->geometry) {
      residency->geometry->beginFrame(currentFrameIndex);
    }
    if (residency->records) {
      residency->records->beginFrame(currentFrameIndex);
    }
  }

  /// This device's painter-ordered record slab (ordered batching) for the document in
  /// \p registry. One per device per document, like `residentSlab`, and freed with the registry.
  std::shared_ptr<geode::GeodeRecordSlab> recordSlab(Registry& registry) {
    std::shared_ptr<geode::GeodeDocumentGeometryBudget> documentBudget =
        documentGeometryBudget(registry);
    DocumentDeviceResidency& residency = documentResidency(registry);
    if (!residency.records) {
      residency.records = std::make_shared<geode::GeodeRecordSlab>(
          device->deviceId(), device->handleRetirement(), std::move(documentBudget));
    }
    // Take back the records freed before this frame, at most once per frame, as `residentSlab`
    // does for geometry. Doing it here rather than in draw() covers the driver entry points a
    // render worker uses, which never pass through draw(). A frame that opened inside another
    // frame on this device, as an offscreen pass does, leaves them for later: the enclosing frame
    // has not submitted yet, so a record freed while it was open may still be one its batches read.
    if (device->oldestOpenFrameGeneration() >= currentFrameIndex) {
      residency.records->beginFrame(currentFrameIndex);
    }
    return residency.records;
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
      // The slot has no record in this device's slab yet: allocate one there.
      slot.recordSlab = std::move(slab);
      slot.recordSlot = geode::GeodeRecordSlab::Slot{};
    }
    if (!slot.recordSlot.buffer.isValid()) {
      (void)slot.recordSlab->allocateSlot(*device, slot.recordSlot);
    }
    return slot.recordSlot.buffer.isValid();
  }

  /// Glyph-residency budget, in distinct cached outlines and summed retained outline plus encode
  /// bytes. The entry cap follows the shared glyph cap and the byte cap defaults from
  /// `GeodeGlyphCache`; a test shrinks them to reach eviction without building a font-sized
  /// working set.
  size_t glyphCacheMaxEntriesForTesting = std::numeric_limits<size_t>::max();
  uint64_t glyphCacheMaxRetainedBytes = geode::GeodeGlyphCache::kDefaultMaxRetainedBytes;

  /// Distinct glyph outlines one document may keep resident.
  size_t glyphCacheMaxEntries() const {
    return std::min(glyphCacheMaxEntriesForTesting, textMaterializationBudget->maximumGlyphs());
  }

  /// Non-cached glyphs needed after the document cache reaches its admission cap. The deque keeps
  /// entry addresses stable for the frame's pending scene batches; `releaseFrameLoans` clears it
  /// once the frame has submitted and its pending batch has flushed.
  std::deque<geode::GeodeGlyphResidentEntry> transientGlyphEntries;

  /// This device's glyph-outline residency for the document in \p registry. One per device per
  /// document, like `residentSlab`, because every cached entry holds that device's buffer and
  /// bind group.
  std::shared_ptr<geode::GeodeGlyphCache> glyphCache(Registry& registry) {
    std::shared_ptr<geode::GeodeDocumentGeometryBudget> documentBudget =
        documentGeometryBudget(registry);
    std::shared_ptr<geode::GeodeGlyphCache>& cache = documentResidency(registry).glyphs;
    if (!cache) {
      cache =
          std::make_shared<geode::GeodeGlyphCache>(device->deviceId(), std::move(documentBudget));
    }
    // Trim to budget at the first touch of each frame, the same shape (and for
    // the same reason) as the slabs' pending-free merge: dropping an entry
    // returns its slab range through the deferred free list, so the range only
    // becomes reusable at the NEXT merge, never inside a frame whose recorded
    // draws still read it. Gating here rather than at one draw entry point
    // covers the multi-document tile paths that never reach `draw()`.
    device->countGlyphResidencyEvictions(
        cache->beginFrame(currentFrameIndex, device->oldestOpenFrameGeneration(),
                          glyphCacheMaxEntries(), glyphCacheMaxRetainedBytes));
    return cache;
  }

  /// Per-occurrence record storage for one text element's glyph fills.
  ///
  /// `component` non-null means the element's persistent slots are in play and
  /// `next` is the ordinal of the occurrence about to be drawn. Null means
  /// this pass takes per-frame temporaries instead (no source entity, batching
  /// off, or the element already drew once this frame).
  struct TextRecordCursor {
    geode::GeodeOccurrenceRecords* component = nullptr;
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
    geode::GeodeOccurrenceRecords& component =
        handle.get_or_emplace<geode::GeodeTextInstanceResidencyComponent>().devices.forDevice(
            deviceKey());
    std::shared_ptr<geode::GeodeRecordSlab> slab = recordSlab(registry);
    if (component.recordSlab.get() != slab.get()) {
      // The held slots belong to another slab, so drop them and start fresh.
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
      if (cursor.next == occurrences.size()) {
        appendOccurrence(*cursor.component, registry);
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

  /// Grow \p records by one persistent slot, charging the document budget and
  /// allocating from its record slab. Leaves \p records unchanged when either
  /// refuses.
  void appendOccurrence(geode::GeodeOccurrenceRecords& records, Registry& registry) {
    if (!records.recordSlab) {
      return;
    }
    std::shared_ptr<geode::GeodeDocumentGeometryBudget> documentBudget =
        documentGeometryBudget(registry);
    if (!records.reserveOccurrence(documentBudget)) {
      return;
    }
    geode::GeodeRecordSlab::Slot slot;
    if (!records.recordSlab->allocateSlot(*device, slot)) {
      records.rollbackOccurrence();
      return;
    }
    if (!records.appendReservedOccurrence(slot)) {
      records.recordSlab->freeSlot(slot);
    }
  }

  /// Most persistent repeat records one resident slot keeps. Each costs one
  /// 256-byte record in the document's record slab plus
  /// `GeodeOccurrenceRecords::kProjectedBytesPerOccurrence` (a few hundred
  /// bytes) of CPU bookkeeping, both charged to the document's geometry budget,
  /// so a slot retains under 3 MiB however many vertices carry its marker.
  /// Repeats past the cap draw from per-frame temporary records: they still
  /// read the slot's resident geometry, but write their record every frame.
  static constexpr std::size_t kMaxRepeatOccurrencesPerSlot = 4096;
  /// The cap in force; tests may lower it.
  std::size_t repeatRecordLimit = kMaxRepeatOccurrencesPerSlot;

  /// The next persistent repeat occurrence of \p slot in this frame, appended to \p records when
  /// the frame has used every one it holds and the cap allows another; null when none can be had.
  geode::GeodeOccurrenceRecords::Occurrence* nextPersistentOccurrence(
      geode::GeodeResidentSlot& slot, geode::GeodeOccurrenceRecords& records, Registry& registry) {
    auto& occurrences = records.occurrences;
    if (slot.nextRepeatOccurrence == occurrences.size() && occurrences.size() < repeatRecordLimit) {
      appendOccurrence(records, registry);
    }
    if (slot.nextRepeatOccurrence >= occurrences.size()) {
      return nullptr;
    }
    return occurrences[slot.nextRepeatOccurrence++].get();
  }

  /// Record storage for a second or later draw of \p slot in this frame: the
  /// slot's next persistent repeat record, or a per-frame temporary when the
  /// persistent set is full, refused, or still read by another open frame.
  /// Leaves \p outSlot null when no record can be had at all, and when
  /// \p outOccurrence is given and only a temporary could be had.
  ///
  /// Repeats of a slot arrive in the same order on every unchanged frame, so
  /// the `i`-th repeat keeps writing record `i` and its compare-before-write
  /// skips the write. Anything that moves or repaints a repeat - the path's
  /// vertices, a stroke-width change under `markerUnits="strokeWidth"`, the
  /// marker's attributes, a context paint - changes its record bytes and so
  /// rewrites exactly the records that changed.
  /// @param outOccurrence When non-null, receives the persistent occurrence the
  ///   record belongs to; such a caller takes no temporary record.
  void nextRepeatRecord(geode::GeodeResidentSlot& slot, Registry& registry,
                        const geode::GeodeRecordSlab::Slot*& outSlot,
                        std::vector<uint8_t>*& outCache,
                        geode::GeodeOccurrenceRecords::Occurrence** outOccurrence = nullptr) {
    outSlot = nullptr;
    outCache = nullptr;
    if (outOccurrence != nullptr) {
      *outOccurrence = nullptr;
    }
    // The repeat's record is published through `slot.recordSlab`, so the records come from that
    // same slab: wiring the slot first makes the two one object by construction.
    if (!ensureRecordSlot(slot, registry)) {
      return;
    }
    const std::shared_ptr<geode::GeodeRecordSlab>& slab = slot.recordSlab;
    if (!slot.repeatRecords) {
      slot.repeatRecords = std::make_unique<geode::GeodeOccurrenceRecords>();
    }
    geode::GeodeOccurrenceRecords& records = *slot.repeatRecords;
    if (records.recordSlab.get() != slab.get()) {
      // The held slots belong to another slab, so drop them and start fresh.
      records.freeRecordSlots();
      records.recordSlab = slab;
    }
    bool persistent = true;
    if (records.lastFrame != currentFrameIndex) {
      if (records.lastFrame >= device->oldestOpenFrameGeneration()) {
        // Another frame that wrote these records has not submitted (an outer
        // frame around an offscreen pass of the same document). Rewriting them
        // would retransform its recorded draws.
        persistent = false;
      } else {
        records.lastFrame = currentFrameIndex;
        slot.nextRepeatOccurrence = 0;
      }
    }
    if (geode::GeodeOccurrenceRecords::Occurrence* occurrence =
            persistent ? nextPersistentOccurrence(slot, records, registry) : nullptr) {
      outSlot = &occurrence->slot;
      outCache = &occurrence->lastRecord;
      if (outOccurrence != nullptr) {
        *outOccurrence = occurrence;
      }
      return;
    }
    if (outOccurrence != nullptr) {
      return;
    }
    outSlot = allocateTempRecordSlot(slab, &outCache);
  }

  /// Whether a draw of \p slot now would be its second or later draw in an
  /// open frame: the slot's uniform or primary record is already claimed, or
  /// its first draw is still waiting in the pending batch.
  bool isRepeatDraw(const geode::GeodeResidentSlot& slot) const {
    return device->frameStampClaimed(slot.lastResidentFrame) ||
           device->frameStampClaimed(slot.lastSceneFrame) ||
           (pendingBatch.has_value() && pendingBatch->mode == PendingBatch::Mode::SameEntity &&
            pendingBatch->residentSlot == &slot);
  }

  /// Offer a draw to the repeat path when it is a same-frame repeat of a resident slot - a marker
  /// drawn at several vertices, a `<use>` source drawn again. The slot's uniform and primary record
  /// serve only its first draw of the frame, so without this a repeat falls back to the per-frame
  /// arena: alone, or through the instanced append, whose instances are arena data too. Gradient
  /// paint lives in the slot's single paint block, so a gradient repeat keeps the existing paths.
  /// The target-state guards are the ordered batch's own, except that a repeat also draws under a
  /// clip polygon, through a uniform its occurrence keeps (see `tryAppendRepeatInstance`).
  /// @return True when the draw was taken.
  bool tryAppendRepeatDraw(Registry* sourceRegistry, const geode::EncodedPath* encoded,
                           geode::GeodeResidentSlot* residentSlot, const Path& path,
                           const geode::GeoEncoder::ScenePaint& paint, FillRule rule) {
    if (!kEnableSceneBatching || paint.isGradient() || residentSlot == nullptr ||
        encoded == nullptr || sourceRegistry == nullptr || encoder->hasActiveClipMask() ||
        encoder->hasOpenMaskPass() || !isRepeatDraw(*residentSlot)) {
      return false;
    }
    return tryAppendRepeatInstance(*sourceRegistry, *encoded, *residentSlot, path, paint.color,
                                   rule);
  }

  /// Draw a repeat of \p slot through a record of its own over the slot's
  /// resident geometry, joining the pending ordered batch when its record
  /// extends it and opening a new one otherwise. Returns false, having drawn
  /// nothing, when the repeat cannot take that form.
  ///
  /// The first draw of the frame keeps the slot's uniform and primary record;
  /// every later one owns a distinct record, so no record a recorded draw reads
  /// is rewritten later in the frame and painter order is the order of the
  /// records in each batch, as for any other ordered batch.
  ///
  /// A clip polygon reaches the shader only through the batch-level uniform,
  /// not the record, so a repeat under one also takes a uniform of its own,
  /// kept on its persistent occurrence and rewritten only when its polygon
  /// changes. A repeat that cannot have both, such as one past the record cap,
  /// is not taken here.
  bool tryAppendRepeatInstance(Registry& registry, const geode::EncodedPath& encoded,
                               geode::GeodeResidentSlot& slot, const Path& path,
                               const css::RGBA& color, FillRule rule) {
    const bool underClipPolygon = encoder->hasClipPolygon();
    const geode::GeodeRecordSlab::Slot* recordSlot = nullptr;
    std::vector<uint8_t>* recordCache = nullptr;
    geode::GeodeOccurrenceRecords::Occurrence* occurrence = nullptr;
    nextRepeatRecord(slot, registry, recordSlot, recordCache,
                     underClipPolygon ? &occurrence : nullptr);
    if (recordSlot == nullptr) {
      return false;
    }
    std::optional<geode::GeoEncoder::ClipUniformBinding> clipUniform;
    if (underClipPolygon) {
      clipUniform =
          encoder->publishOccurrenceClipUniform(*slot.repeatRecords, *occurrence, slot.slab);
      if (!clipUniform.has_value()) {
        return false;
      }
    }
    const uint64_t clipVersion = encoder->clipStateVersion();
    settlePendingSingletonForRepeat(clipVersion);
    geode::GeoEncoder::SceneRecordState recordState;
    if (!encoder->ensureResidentSceneRecord(slot, encoded, geode::GeoEncoder::ScenePaint{color},
                                            rule, deviceFromLocalTransform, recordSlot, recordCache,
                                            &recordState)) {
      return false;
    }
    appendOrOpenSceneBatch(
        PendingBatch::SceneInstance{&slot, &encoded, &path, color, rule, deviceFromLocalTransform,
                                    encoded.boundingDrawVertexCount(), recordSlot, recordCache,
                                    recordState},
        *recordSlot, clipVersion, clipUniform);
    slot.lastSceneFrame = currentFrameIndex;
    return true;
  }

  /// Before a repeat joins a batch: a pending singleton, typically the repeated slot's own first
  /// draw, becomes the first instance of a scene batch, so a run of repeats still draws as one
  /// batch. One that cannot convert is flushed now, taking its solo resident path before the repeat
  /// claims the slot. Under a clip polygon a singleton is always flushed: it has no occurrence to
  /// keep the polygon's uniform on, and its solo resident draw carries the polygon itself.
  /// @param clipVersion Encoder clip-state version of the repeat.
  void settlePendingSingletonForRepeat(uint64_t clipVersion) {
    if (!pendingBatch.has_value() || pendingBatch->mode != PendingBatch::Mode::SameEntity) {
      return;
    }
    if (pendingBatch->deviceFromLocalTransforms.size() != 1 ||
        pendingBatch->sameEntityClipVersion != clipVersion || encoder->hasClipPolygon() ||
        !convertPendingSingletonToScene(clipVersion)) {
      flushPendingBatch();
    }
  }

  /// Append \p instance to the pending scene batch when its record extends that batch under the
  /// same clip state; otherwise flush what is pending and open a new scene batch holding it.
  /// @param instance Instance whose record and geometry are already ensured.
  /// @param recordSlot Record slot \p instance draws from.
  /// @param clipVersion Encoder clip-state version of \p instance.
  /// @param clipUniform Uniform \p instance published for the clip polygon it is drawn under, or
  ///   no value when none is active. Instances under one clip state publish identical bytes, so
  ///   a batch binds the uniform of the instance that opened it.
  void appendOrOpenSceneBatch(
      const PendingBatch::SceneInstance& instance, const geode::GeodeRecordSlab::Slot& recordSlot,
      uint64_t clipVersion,
      const std::optional<geode::GeoEncoder::ClipUniformBinding>& clipUniform) {
    const uint64_t chunkId = instance.slot->bufferId;
    if (pendingBatch.has_value() && pendingBatch->mode == PendingBatch::Mode::Scene &&
        pendingBatch->sceneChunkBufferId == chunkId &&
        pendingBatch->sceneRecordBufferId == recordSlot.bufferId &&
        pendingBatch->sceneClipVersion == clipVersion &&
        pendingBatch->sceneClipUniform.has_value() == clipUniform.has_value() &&
        pendingBatch->sceneFirstInstance + pendingBatch->sceneInstances.size() ==
            recordSlot.index) {
      pendingBatch->sceneInstances.push_back(instance);
      pendingBatch->sceneVertexCount =
          std::max(pendingBatch->sceneVertexCount, instance.vertexCount);
      return;
    }
    flushPendingBatch();
    pendingBatch = PendingBatch{};
    pendingBatch->mode = PendingBatch::Mode::Scene;
    pendingBatch->sceneChunkBuffer = instance.slot->buffer;
    pendingBatch->sceneChunkBytes =
        instance.slot->slab != nullptr ? instance.slot->slab->chunkBytesForId(chunkId) : 0;
    pendingBatch->sceneRecordBuffer = recordSlot.buffer;
    pendingBatch->sceneChunkBufferId = chunkId;
    pendingBatch->sceneRecordBufferId = recordSlot.bufferId;
    pendingBatch->sceneFirstInstance = recordSlot.index;
    pendingBatch->sceneFirstRecordOffset = recordSlot.offset;
    pendingBatch->sceneClipVersion = clipVersion;
    pendingBatch->sceneClipUniform = clipUniform;
    pendingBatch->sceneVertexCount = instance.vertexCount;
    pendingBatch->sceneInstances.push_back(instance);
  }

  /**
   * The gradient residence a glyph shape of a run paints \p gradient from: none when the run's
   * resolved gradient has more stops than a draw carries inline. Such a gradient reads a stop ramp,
   * and the run's per-frame draws share one ramp lookup through the resolution's ramp cache, where
   * each resident draw would look its slot's ramp up again. A draw that resolves its own gradient,
   * as a decoration line does, looks its ramp up once whether it draws resident or per frame.
   *
   * @param shape The shape about to be painted.
   * @param gradient The run's resolved gradient, or null when the draw resolves its own.
   */
  static geode::GeodeResidentGradientSlot* TextShapeGradientSlot(
      const TextShape& shape, const ResolvedGradientPaint* gradient) {
    if (gradient != nullptr) {
      std::size_t stops = 0;
      if (const geode::LinearGradientParams* linear = gradient->linearParams()) {
        stops = linear->stops.size();
      } else if (const geode::RadialGradientParams* radial = gradient->radialParams()) {
        stops = radial->stops.size();
      }
      if (stops > geode::GeoEncoder::kMaxSceneGradientStops) {
        return nullptr;
      }
    }
    return shape.gradientSlot;
  }

  /// Opens the retained shapes of the text element rooted at \p textRoot for one draw.
  /// @param registry Registry of the element.
  /// @param textRoot The element's entity, or null when the text has none.
  TextShapeCursor beginTextShapes(Registry& registry, Entity textRoot) {
    TextShapeCursor cursor;
    if (textRoot == entt::null) {
      return cursor;
    }
    EntityHandle handle(registry, textRoot);
    if (!handle) {
      return cursor;
    }
    // Text that paints only solid glyph fills never tries to keep a shape, so the component is
    // created only at the element's first attempt to keep one (\ref textShapeCacheFor), and the
    // frame that creates it claims it then.
    if (auto* component = handle.try_get<geode::GeodeTextShapeResidencyComponent>()) {
      if (geode::GeodeTextShapeCache* cache = component->devices.find(device->deviceId())) {
        if (cache->lastFrame >= device->oldestOpenFrameGeneration()) {
          return cursor;
        }
        cache->lastFrame = currentFrameIndex;
        cursor.cache = cache;
      }
    }
    cursor.keeps = true;
    cursor.registry = &registry;
    cursor.element = textRoot;
    return cursor;
  }

  /// The element's shapes on this device for \p cursor's draw, created at the first attempt to keep
  /// a shape (an attempt the cap or budget then refuses leaves it empty and uncharged) and marked
  /// as drawn by this frame.
  geode::GeodeTextShapeCache& textShapeCacheFor(TextShapeCursor& cursor) {
    if (cursor.cache == nullptr) {
      cursor.cache = &EntityHandle(*cursor.registry, cursor.element)
                          .get_or_emplace<geode::GeodeTextShapeResidencyComponent>()
                          .devices.forDevice(deviceKey());
      cursor.cache->lastFrame = currentFrameIndex;
    }
    return *cursor.cache;
  }

  /// Drops the shapes past the last one \p cursor's draw reached.
  void endTextShapes(const TextShapeCursor& cursor) {
    if (cursor.cache != nullptr) {
      cursor.cache->trim(cursor.next);
    }
  }

  /**
   * The encode and residence of the next painted shape of a text draw, \p path in the element's
   * space: the retained shape when it holds exactly this path at the current tolerance, else a new
   * encode, retained when the element's shape cap and the document's cache budget allow and drawn
   * for this frame only otherwise.
   *
   * @param cursor The draw's shapes; see \ref beginTextShapes.
   * @param path Shape to paint.
   * @param splittable Whether the caller can draw \p path in parts: then an encode the encoder's
   *   limits refuse returns a null encode, leaving the frame admitted, instead of rejecting it.
   */
  TextShape textShape(TextShapeCursor& cursor, const Path& path, bool splittable = false) {
    if (!cursor.keeps) {
      return {splittable ? encodeTransientGeometryOrSplit(path, FillRule::NonZero)
                         : encodeTransientGeometry(path, FillRule::NonZero)};
    }
    const std::size_t index = cursor.next++;
    const double tolerance = geode::FillCubicToleranceFor(deviceFromLocalTransform);
    geode::GeodeTextShapeCache::Shape* shape =
        cursor.cache != nullptr ? cursor.cache->find(index, path, FillRule::NonZero, tolerance)
                                : nullptr;
    if (shape == nullptr) {
      if (geometryBudget->rejected()) {
        return {&rejectedGeometry};
      }
      geode::EncodedPath encoded = encodeAtDeviceTolerance(path, FillRule::NonZero);
      if (encoded.rejected()) {
        if (splittable) {
          // The caller draws the parts instead, from the ordinals this one would have taken, so a
          // whole the encoder refuses costs its encode attempt each frame and nothing more.
          cursor.next = index;
          return {nullptr};
        }
        geometryBudget->reject();
        return {&rejectedGeometry};
      }
      geode::GeodeTextShapeCache& cache = textShapeCacheFor(cursor);
      shape = cache.store(index, path, FillRule::NonZero, tolerance, encoded,
                          documentGeometryBudget(*cursor.registry), textShapeLimit);
      if (shape == nullptr) {
        // Past the cap or refused by the budget: this draw encodes for the frame, its ordinal is
        // left empty, and the later shapes of the element are still kept.
        cache.release(index);
        transientTextEncodes.push_back(std::move(encoded));
        return {&transientTextEncodes.back()};
      }
    }
    // Wire the slots to this device's slab for the document, as the per-entity residence getters
    // do; a slot resident in another slab re-uploads into this one.
    std::shared_ptr<geode::GeodeResidentSlab> slab = residentSlab(*cursor.registry);
    if (shape->slot.slab.get() != slab.get()) {
      shape->slot.reset();
    }
    shape->slot.slab = slab;
    if (shape->gradientSlot.slab.get() != slab.get()) {
      shape->gradientSlot.reset();
    }
    shape->gradientSlot.slab = std::move(slab);
    return {&shape->encoded, &shape->slot, &shape->gradientSlot};
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
  void admitTextRuns(Registry& registry, std::vector<TextRun>& runs) {
    // Trim the document's glyph cache before admission can reject the runs, so a lowered glyph
    // cap also shrinks the cache on frames whose text it rejects.
    (void)glyphCache(registry);
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

  bool appendWarpedGlyph(std::vector<Path>& paths, FontManager& fontManager,
                         const TextEngine& textEngine, const TextRun& run, const TextGlyph& glyph,
                         float scale, double tolerance) {
    bool hasComplexity = false;
    const FontHandle font = GlyphFont(run, glyph);
    if (!admitGlyphMiss(fontManager, font, glyph.glyphIndex, hasComplexity,
                        kMaximumTextPathWarpExpansion)) {
      return false;
    }
    Path placed = PlacedGlyphOutline(textEngine, font, run, glyph, scale, tolerance);
    if (placed.empty()) {
      return true;
    }
    if (!textMaterializationBudget->reservePathCopy(placed)) {
      return false;
    }
    paths.push_back(std::move(placed));
    return true;
  }

  /**
   * Charges one glyph-cache miss against the frame's text budget before its outline is decoded:
   * the font's predecode bound when it reports one (an untrusted font must), and the entry the miss
   * keeps for the rest of the frame, cached or not. The entry is all an outline-less glyph costs,
   * so it is what bounds misses on those.
   *
   * @param fontManager Font source for the complexity bound and trust.
   * @param font Font of the missed glyph.
   * @param glyphIndex Glyph index within \p font.
   * @param[out] hasComplexity Whether the font reported a predecode bound.
   * @return False when the budget rejects the miss.
   */
  bool admitGlyphMiss(FontManager& fontManager, FontHandle font, int glyphIndex,
                      bool& hasComplexity, std::size_t expansion = 1u) {
    const std::optional<FontManager::GlyphOutlineComplexity> complexity =
        fontManager.glyphOutlineComplexity(font, glyphIndex);
    hasComplexity = complexity.has_value();
    if (complexity.has_value()) {
      const std::optional<RendererTextMaterializationBudget::Cost> cost =
          GlyphPredecodeCost(*complexity, expansion);
      if (!cost.has_value() || !textMaterializationBudget->reserve(*cost)) {
        return false;
      }
    } else if (!fontManager.isTrustedFont(font)) {
      textMaterializationBudget->reject();
      return false;
    }
    return textMaterializationBudget->reserve(
        {.bytes = geode::GeodeGlyphCache::kEntryOverheadBytes});
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
      bool hasComplexity = false;
      if (!admitGlyphMiss(fontManager, font, glyphIndex, hasComplexity)) {
        return nullptr;
      }

      // Admission has to happen before outline decoding and before insertion. The frame-start
      // trim cannot bound a cache that begins below its limit and creates a large working set in
      // one frame; entries touched by that open frame are intentionally ineligible for eviction.
      // Reclaim only entries no open frame can still reference, then fail closed if the new entry
      // still would exceed the configured count cap.
      const size_t maxEntries = glyphCacheMaxEntries();
      bool cacheAdmissionAvailable = maxEntries != 0u && cache->admissionOpen();
      if (cacheAdmissionAvailable && cache->size() >= maxEntries) {
        const size_t evicted = cache->evictToBudget(device->oldestOpenFrameGeneration(),
                                                    maxEntries - 1u, glyphCacheMaxRetainedBytes);
        device->countGlyphResidencyEvictions(evicted);
        if (cache->size() >= maxEntries) {
          cache->closeAdmission();
          cacheAdmissionAvailable = false;
        }
      }
      Path outline = buildOutline();
      if (!hasComplexity && !outline.empty()) {
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
                                          device->oldestOpenFrameGeneration(), maxEntries,
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
    const auto existing =
        std::find_if(touchedDocumentBudgets.begin(), touchedDocumentBudgets.end(),
                     [&](const auto& value) { return value.get() == budget.get(); });
    if (existing == touchedDocumentBudgets.end()) {
      touchedDocumentBudgets.push_back(budget);
    }
    return budget;
  }

  /// This renderer's device, as the key of state kept per device.
  geode::GeodeDeviceKey deviceKey() const {
    return geode::GeodeDeviceKey{device->deviceId(), device->handleRetirement()};
  }

  /// This device's residence for the document in \p registry, created empty on first use.
  DocumentDeviceResidency& documentResidency(Registry& registry) {
    auto* residencyPtr = registry.ctx().find<std::shared_ptr<DocumentResidency>>();
    if (residencyPtr == nullptr) {
      residencyPtr = &registry.ctx().emplace<std::shared_ptr<DocumentResidency>>(
          std::make_shared<DocumentResidency>());
    }
    geode::GeodePerDevice<DocumentDeviceResidency>& devices = (*residencyPtr)->devices;
    // The one place a document drops gone devices, together with their slots on every entity:
    // `forDevice` itself drops nothing, so a context closing on another thread in between leaves
    // its entry for the next call to drop and sweep.
    if (devices.dropGone() != 0) {
      DropGoneDevicesFromEntities(registry);
    }
    return devices.forDevice(deviceKey());
  }

  /// This device's residence slots on \p source, created empty on first use.
  geode::GeodeResidentPathSlots& residentPathSlots(EntityHandle source) {
    return source.get_or_emplace<geode::GeodeResidentPathComponent>().devices.forDevice(
        deviceKey());
  }

  /**
   * This device's resident texture for \p image, uploading it when the entity keeps none for the
   * image's current pixels.
   *
   * Only an image drawn from its source entity's `LoadedImageComponent` is kept: the texture is
   * keyed by that component's revision and the image size, both compared before every draw, so a
   * texture uploaded from other pixels is never drawn. Returns null, leaving the draw to upload a
   * texture for this frame only, when there is no such entity, when the document's resident limit
   * refuses the texture's bytes, or when the device refuses the texture.
   *
   * @param source Entity the image was loaded onto, or null.
   * @param image Pixels being drawn; the encoder accepts them.
   */
  const gpu::Texture* residentImageTexture(EntityHandle source, const ImageResource& image) {
    const components::LoadedImageComponent* loaded = LoadedImageDrawnAs(source, image);
    if (loaded == nullptr) {
      return nullptr;
    }
    Registry& registry = *source.registry();
    ensureCacheInvalidationWired(registry);
    std::shared_ptr<geode::GeodeResidentSlab> slab = residentSlab(registry);
    geode::GeodeImageResidency& residency =
        source.get_or_emplace<geode::GeodeResidentImageComponent>().devices.forDevice(deviceKey());
    if (const geode::GeodeResidentImage* current = residency.image();
        current != nullptr && residency.slab() == slab &&
        keptImageMatches(*current, loaded->revision(), image)) {
      return &current->texture;
    }
    // Stale or absent: let go of the old texture first, so its charge does not count against the
    // replacement once the frames that sampled it are done.
    residency.replace(slab, nullptr);
    std::shared_ptr<geode::GeodeResidentImage> kept =
        uploadKeptImage(slab->documentBudget(), loaded->revision(), image);
    if (kept == nullptr) {
      return nullptr;
    }
    const gpu::Texture* texture = &kept->texture;
    residency.replace(std::move(slab), std::move(kept));
    return texture;
  }

  /// The loaded image component of \p source when \p image is its decoded image, or null.
  /// @param source Entity the image was loaded onto, or null.
  /// @param image Pixels being drawn.
  static const components::LoadedImageComponent* LoadedImageDrawnAs(EntityHandle source,
                                                                    const ImageResource& image) {
    if (!source) {
      return nullptr;
    }
    const auto* loaded = source.try_get<components::LoadedImageComponent>();
    if (loaded == nullptr || !loaded->image.has_value() || &*loaded->image != &image) {
      return nullptr;
    }
    return loaded;
  }

  /// Whether \p kept holds the pixels of \p image at \p revision, on this renderer's device.
  /// @param kept Kept texture.
  /// @param revision `LoadedImageComponent::revision()` of the image being drawn.
  /// @param image Pixels being drawn.
  bool keptImageMatches(const geode::GeodeResidentImage& kept, std::uint64_t revision,
                        const ImageResource& image) const {
    return kept.revision == revision && kept.width == image.width && kept.height == image.height &&
           kept.texture.deviceId() == device->runtimeDevice().deviceId();
  }

  /// Upload \p image to a texture kept across frames, charged to \p budget, or null when the
  /// budget refuses its bytes or the device refuses the texture.
  /// @param budget Document budget to charge.
  /// @param revision `LoadedImageComponent::revision()` of the image.
  /// @param image Pixels to upload; the encoder accepts them.
  std::shared_ptr<geode::GeodeResidentImage> uploadKeptImage(
      const std::shared_ptr<geode::GeodeDocumentGeometryBudget>& budget, std::uint64_t revision,
      const ImageResource& image) {
    const std::uint64_t bytes = geode::GeodeResidentImage::TextureBytes(image.width, image.height);
    if (!budget || !budget->reserveImageBytes(bytes)) {
      return nullptr;
    }
    auto kept = std::make_shared<geode::GeodeResidentImage>();
    kept->chargedBudget = budget;
    kept->chargedBytes = bytes;
    kept->texture = encoder->uploadImageTexture(image);
    if (!kept->texture.isValid()) {
      return nullptr;
    }
    kept->revision = revision;
    kept->width = image.width;
    kept->height = image.height;
    kept->owner = device->handleRetirement();
    return kept;
  }

  /// This device's resident slab for the document in \p registry: one growable chunk set per
  /// device per document, freed with the registry. Shared so a slot keeps its slab alive.
  std::shared_ptr<geode::GeodeResidentSlab> residentSlab(Registry& registry) {
    std::shared_ptr<geode::GeodeDocumentGeometryBudget> documentBudget =
        documentGeometryBudget(registry);
    std::shared_ptr<geode::GeodeResidentSlab>& slab = documentResidency(registry).geometry;
    if (!slab) {
      slab = std::make_shared<geode::GeodeResidentSlab>(
          device->deviceId(), device->handleRetirement(), std::move(documentBudget));
    }
    // An older open frame may still reference the retired bind groups and geometry ranges.
    if (device->oldestOpenFrameGeneration() >= currentFrameIndex) {
      slab->beginFrame(currentFrameIndex);
    }
    return slab;
  }

  /// This device's GPU-residence slot for fill encode variant \p variant of `source` (see
  /// `GeodePathCacheComponent::fillVariants`). Returns null for a null
  /// source (editor/overlay draws stay on the arena path). The slot lives on a
  /// `GeodeResidentPathComponent` beside the CPU encode cache and is invalidated by the same
  /// listener.
  geode::GeodeResidentSlot* residentFillSlot(EntityHandle source, std::size_t variant,
                                             bool solidPaint = false) {
    if (!source) {
      return nullptr;
    }
    ensureCacheInvalidationWired(*source.registry());
    geode::GeodeResidentPathSlots& slots = residentPathSlots(source);
    if (solidPaint) {
      // A solid or pattern fill binds no stop ramp, so the gradient slot of the same fill lets go
      // of any it kept from an earlier gradient paint.
      releaseGradientRamp(&slots.gradientFillSlots[variant]);
    }
    geode::GeodeResidentSlot& slot = slots.fillSlots[variant];
    std::shared_ptr<geode::GeodeResidentSlab> slab = residentSlab(*source.registry());
    if (slot.slab.get() != slab.get()) {
      // The slot's residence lives in another slab, so it must re-upload into this one. reset()
      // keeps the old slab alive through this slot until the assignment below.
      slot.reset();
    }
    slot.slab = std::move(slab);
    return &slot;
  }

  /// This device's residence slots for stroke variant \p variant of `source` (see
  /// `GeodePathCacheComponent::strokeVariants`), created empty on first use.
  geode::GeodeResidentStrokeSlots& residentStrokeSlots(EntityHandle source, std::size_t variant) {
    std::unique_ptr<geode::GeodeResidentStrokeSlots>& slots =
        residentPathSlots(source).strokeSlots[variant];
    if (!slots) {
      slots = std::make_unique<geode::GeodeResidentStrokeSlots>();
    }
    return *slots;
  }

  /// GPU-residence slot for stroke variant \p variant of `source`. See `residentFillSlot`.
  geode::GeodeResidentSlot* residentStrokeSlot(EntityHandle source, std::size_t variant) {
    if (!source) {
      return nullptr;
    }
    ensureCacheInvalidationWired(*source.registry());
    geode::GeodeResidentStrokeSlots& slots = residentStrokeSlots(source, variant);
    // Only a solid stroke takes this slot, and it binds no stop ramp, so the gradient stroke slot
    // lets go of any it kept from an earlier gradient paint.
    releaseGradientRamp(&slots.gradient);
    geode::GeodeResidentSlot& slot = slots.solid;
    std::shared_ptr<geode::GeodeResidentSlab> slab = residentSlab(*source.registry());
    if (slot.slab.get() != slab.get()) {
      slot.reset();
    }
    slot.slab = std::move(slab);
    return &slot;
  }

  /// GPU-residence slot for the gradient-painted stroke of stroke variant \p variant of
  /// `source`. See `residentGradientFillSlot`; the slot holds the cached stroke-outline encode
  /// plus the resolved gradient uniform.
  geode::GeodeResidentGradientSlot* residentGradientStrokeSlot(EntityHandle source,
                                                               std::size_t variant) {
    if (!source) {
      return nullptr;
    }
    ensureCacheInvalidationWired(*source.registry());
    geode::GeodeResidentGradientSlot& slot = residentStrokeSlots(source, variant).gradient;
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
  geode::GeodeResidentGradientSlot* residentGradientFillSlot(EntityHandle source,
                                                             std::size_t variant) {
    if (!source) {
      return nullptr;
    }
    ensureCacheInvalidationWired(*source.registry());
    geode::GeodeResidentGradientSlot& slot = residentPathSlots(source).gradientFillSlots[variant];
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

  /// Emit a pattern fill on the same terms as \ref emitSolidFill: through \p residentSlot when
  /// there is one, a cached encode and a tile view that outlives the frame, through the per-frame
  /// arena otherwise.
  void emitPatternFill(const Path& drawPath, FillRule rule,
                       const geode::GeoEncoder::PatternPaint& patternPaint,
                       const geode::EncodedPath* precomputedEncoded,
                       geode::GeodeResidentSlot* residentSlot) {
    if (residentSlot != nullptr && precomputedEncoded != nullptr &&
        patternPaint.tileView.isValid()) {
      encoder->fillPathPatternResident(*residentSlot, *precomputedEncoded, rule, patternPaint,
                                       currentFrameIndex);
    } else {
      encoder->fillPathPattern(drawPath, rule, patternPaint, precomputedEncoded);
    }
  }

  /// The resident slot a fill of \p source draws from, or null when it has none: only a solid
  /// fill, a batchable gradient fill or a pattern fill with a persistent encode is resident. The
  /// slot is shared by the batch size-1 flush, the cross-entity scene batch and the direct
  /// `fillResolved` path, so such a fill re-uploads zero geometry on an unchanged frame however it
  /// ends up being drawn; a pattern fill draws from the same slot, binding its tile.
  /// @param source Entity whose fill is drawn.
  /// @param admission The fill's encode admission.
  /// @param solidFill Whether the fill paint is a solid color.
  /// @param gradientBatchable Whether the fill paint is a gradient the scene batch accepts.
  geode::GeodeResidentSlot* residentFillSlotForPaint(EntityHandle source,
                                                     const AdmittedEncode& admission,
                                                     bool solidFill, bool gradientBatchable) {
    const bool patternFill = patternFillPaint.has_value();
    if (!admission.persistent || !(solidFill || gradientBatchable || patternFill)) {
      return nullptr;
    }
    return residentFillSlot(source, admission.variant, solidFill || patternFill);
  }

  /// Stroke \p source with the staged stroke pattern, consuming it: \p stroked is drawn through
  /// the entity's resident stroke slot when its encode is persistent.
  /// @param source Entity whose stroke is drawn.
  /// @param stroked The stroke's outline and encode.
  void emitPatternStroke(EntityHandle source, const StrokeDerived& stroked) {
    // This element's fill may still be sitting in the pending batch; drain it
    // so the pattern stroke lands on top of it rather than under it.
    flushPendingBatch();
    syncTransform();
    emitPatternFill(*stroked.strokedPath, stroked.fillRule,
                    buildPatternPaint(*patternStrokePaint, paint.strokeOpacity), stroked.encoded,
                    stroked.persistent ? residentStrokeSlot(source, stroked.variant) : nullptr);
    releasePatternPaintSlot(patternStrokePaint);
  }

  /// Pack a 2D affine into the 8-float wire format the shader expects
  /// (two `vec4f` rows, `(a, c, e, 0)` / `(b, d, f, 0)` - see
  /// `struct InstanceTransform` in `donner/gpu/shader/programs/SlugFillSource.h`).
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

  [[nodiscard]] bool pendingBatchCannotFlush() const {
    return frameArenaWriteFailed || !pendingBatch.has_value() ||
           pendingBatch->mode == PendingBatch::Mode::None ||
           (pendingBatch->mode == PendingBatch::Mode::SameEntity &&
            pendingBatch->deviceFromLocalTransforms.empty()) ||
           (pendingBatch->mode == PendingBatch::Mode::Scene &&
            pendingBatch->sceneInstances.empty());
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
    if (pendingBatchCannotFlush() || !encoder) {
      pendingBatch.reset();
      return;
    }
    const Transform2d savedDeviceFromLocalTransform = deviceFromLocalTransform;
    PendingBatch batch = std::move(*pendingBatch);
    pendingBatch.reset();
    // Nothing is queued once the frame is rejected (see `tryAppendOrStartBatch`), so every draw
    // here came before any refusal and is admitted as it would have been then.
    struct QueuedAdmission {
      bool& flag;
      bool previous;
      ~QueuedAdmission() { flag = previous; }
    } queuedAdmission{admittingQueuedDraws, std::exchange(admittingQueuedDraws, true)};

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
      binding.clipUniform = batch.sceneClipUniform;

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
  /// Whether the pending batch is a single draw of another slot under the clip state
  /// \p clipVersion, which a scene batch can take as its first instance.
  bool pendingSingletonConvertible(std::uint64_t clipVersion,
                                   const geode::GeodeResidentSlot* residentSlot) const {
    return pendingBatch.has_value() && pendingBatch->mode == PendingBatch::Mode::SameEntity &&
           pendingBatch->deviceFromLocalTransforms.size() == 1 &&
           pendingBatch->sameEntityClipVersion == clipVersion &&
           pendingBatch->residentSlot != residentSlot;
  }

  /// Queue a draw into the pending batch, or report that it must be drawn directly.
  ///
  /// Once the frame's geometry has been refused nothing more is queued: the pending batch, every
  /// draw of which came before the refusal, is drawn, and the caller's direct draw is refused, so
  /// a later refusal never drops an earlier draw.
  bool tryAppendOrStartBatch(Registry* sourceRegistry, Entity sourceEntity, const Path& path,
                             const geode::GeoEncoder::ScenePaint& paint, FillRule rule,
                             const geode::EncodedPath* encoded,
                             geode::GeodeResidentSlot* residentSlot,
                             bool allowInstancedAppend = true) {
    if (geometryBudget->rejected()) {
      flushPendingBatch();
      return false;
    }
    // The record-sourced paint fields are the only place a gradient can live
    // in a batch, so a gradient draw is offered to the ordered path and
    // nothing else: the instanced same-entity path and the size-1 singleton
    // flush both take their paint from the draw-level uniform, which carries
    // one colour and no gradient.
    const css::RGBA& color = paint.color;
    const bool gradientPaint = paint.isGradient();

    // A same-frame repeat of a resident slot takes a record of its own over the slot's geometry.
    if (tryAppendRepeatDraw(sourceRegistry, encoded, residentSlot, path, paint, rule)) {
      return true;
    }

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
    // starts as SameEntity(1); the second, different entity converts it).
    // A same-frame repeat that reaches this point could not take a record of
    // its own above, and keeps the instanced path or the solo flush's arena
    // fallback, which are exact for repeated draws.
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
    const bool haveConvertibleSingleton = pendingSingletonConvertible(clipVersion, residentSlot);
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
      // one entity therefore stay unconverted here; such a repeat takes a
      // record of its own in `tryAppendRepeatInstance` before reaching this
      // point, and one that could not have one leaves the singleton to flush
      // solo while this draw opens a fresh one.
      if (pendingBatch.has_value() && pendingBatch->mode == PendingBatch::Mode::SameEntity &&
          pendingBatch->deviceFromLocalTransforms.size() == 1 &&
          pendingBatch->sameEntityClipVersion == clipVersion &&
          pendingBatch->residentSlot != residentSlot) {
        if (convertPendingSingletonToScene(clipVersion)) {
          if (pendingBatch->sceneChunkBufferId == chunkId &&
              pendingBatch->sceneRecordBufferId == recordBufId &&
              pendingBatch->sceneFirstInstance + 1 == recordIndex) {
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

  /// Turn the pending size-1 same-entity singleton into the first instance of
  /// a scene batch drawn from a record: its primary record on its first scene
  /// use this frame, or a fresh temporary one when an earlier batch of this
  /// frame already reads the primary. Returns false, leaving the singleton
  /// pending, when it cannot take scene form.
  ///
  /// @param clipVersion Encoder clip-state version the singleton was queued
  ///   under, which the new batch carries.
  bool convertPendingSingletonToScene(uint64_t clipVersion) {
    if (pendingBatch->residentSlot == nullptr || pendingBatch->encoded == nullptr ||
        pendingBatch->sourceRegistry == nullptr) {
      return false;
    }
    PendingBatch::SceneInstance first{pendingBatch->residentSlot,
                                      pendingBatch->encoded,
                                      pendingBatch->path,
                                      pendingBatch->color,
                                      pendingBatch->rule,
                                      pendingBatch->deviceFromLocalTransforms.front(),
                                      pendingBatch->encoded->boundingDrawVertexCount()};
    // The singleton reached the batcher without being scene eligible itself,
    // so it may still hold a slot from a slab this device does not own
    // (another device rendered the document in between): ensure it against
    // the current slab before resolving.
    const geode::GeodeRecordSlab::Slot* firstRecordSlotPtr = nullptr;
    if (!ensureRecordSlot(*first.slot, *pendingBatch->sourceRegistry) ||
        !resolveSceneRecordSlot(*first.slot, firstRecordSlotPtr) ||
        !encoder->ensureResidentSceneRecord(*first.slot, *first.encoded,
                                            geode::GeoEncoder::ScenePaint{first.color}, first.rule,
                                            first.deviceFromLocal, firstRecordSlotPtr,
                                            /*overrideRecordCache=*/nullptr, &first.recordState)) {
      return false;
    }
    first.slot->lastSceneFrame = currentFrameIndex;
    // Carry the resolved slot on the instance so the flush-time re-ensure
    // targets the same record this batch draws from.
    first.recordSlotOverride = firstRecordSlotPtr;
    const geode::GeodeRecordSlab::Slot& effectiveFirstRecordSlot =
        firstRecordSlotPtr != nullptr ? *firstRecordSlotPtr : first.slot->recordSlot;
    const uint64_t firstChunkId = first.slot->bufferId;
    pendingBatch = PendingBatch{};
    pendingBatch->mode = PendingBatch::Mode::Scene;
    pendingBatch->sceneChunkBuffer = first.slot->buffer;
    pendingBatch->sceneChunkBytes =
        first.slot->slab != nullptr ? first.slot->slab->chunkBytesForId(firstChunkId) : 0;
    pendingBatch->sceneRecordBuffer = effectiveFirstRecordSlot.buffer;
    pendingBatch->sceneChunkBufferId = firstChunkId;
    pendingBatch->sceneRecordBufferId = effectiveFirstRecordSlot.bufferId;
    pendingBatch->sceneFirstInstance = effectiveFirstRecordSlot.index;
    pendingBatch->sceneFirstRecordOffset = effectiveFirstRecordSlot.offset;
    pendingBatch->sceneClipVersion = clipVersion;
    pendingBatch->sceneVertexCount = first.vertexCount;
    pendingBatch->sceneInstances.push_back(first);
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

  /// Flushes the pending batch when it still draws from stroke variant \p index of \p source,
  /// then drops that variant's resident slots on every device, so replacing the variant cannot
  /// leave a stale resident binding.
  void releaseStrokeVariant(EntityHandle source, geode::GeodePathCacheComponent& cache,
                            std::size_t index) {
    const std::unique_ptr<geode::GeodePathCacheComponent::StrokeVariant>& variant =
        cache.strokeVariants[index];
    if (variant && pendingBatchReferences(&variant->strokedEncode)) {
      flushPendingBatch();
    }
    if (auto* resident = source.try_get<geode::GeodeResidentPathComponent>()) {
      // Every device's residence for this variant was uploaded from the outline being replaced.
      resident->devices.forEach([index](geode::GeodeResidentPathSlots& slots) {
        if (slots.strokeSlots[index]) {
          slots.strokeSlots[index]->solid.reset();
          slots.strokeSlots[index]->gradient.reset();
        }
      });
    }
  }

  /// Index of the stroke variant an outline for \p style replaces. A style that already holds
  /// `kStrokeVariantsPerStyle` outlines (other tolerance buckets or host transforms) replaces
  /// its own least recently used one, so zooming through scale buckets cannot fill the cache with
  /// one style's outlines. Otherwise an empty variant, or else the least recently used one. Only
  /// variants no open frame may still read (`cacheVariantInUse`) are candidates; nullopt when
  /// there is none.
  /// @param cache Stroke cache to choose in.
  /// @param style Stroke style of the outline to store.
  std::optional<std::size_t> strokeVariantToReplace(const geode::GeodePathCacheComponent& cache,
                                                    const StrokeStyle& style) const {
    const auto sameStyle = [&style](const geode::GeodePathCacheComponent::StrokeVariant& variant) {
      return variant.strokeKey == style;
    };
    const auto anyStyle = [](const geode::GeodePathCacheComponent::StrokeVariant&) { return true; };
    const std::size_t sameStyleCount = static_cast<std::size_t>(
        std::count_if(cache.strokeVariants.begin(), cache.strokeVariants.end(),
                      [&](const auto& variant) { return variant && sameStyle(*variant); }));
    if (sameStyleCount >= geode::GeodePathCacheComponent::kStrokeVariantsPerStyle) {
      return leastRecentlyUsedReplaceableStrokeVariant(cache, sameStyle);
    }
    for (std::size_t i = 0; i < cache.strokeVariants.size(); ++i) {
      if (!cache.strokeVariants[i]) {
        return i;
      }
    }
    return leastRecentlyUsedReplaceableStrokeVariant(cache, anyStyle);
  }

  /// Index of the least recently used stroke variant of \p cache that satisfies \p matches and
  /// that no open frame may still read (`cacheVariantInUse`), or nullopt when there is none.
  template <typename Predicate>
  std::optional<std::size_t> leastRecentlyUsedReplaceableStrokeVariant(
      const geode::GeodePathCacheComponent& cache, const Predicate& matches) const {
    std::optional<std::size_t> index;
    for (std::size_t i = 0; i < cache.strokeVariants.size(); ++i) {
      const auto& variant = cache.strokeVariants[i];
      if (variant && matches(*variant) &&
          !cacheVariantInUse(variant->lastUseFrame, variant->lastUseDeviceId) &&
          (!index || variant->lastUse < cache.strokeVariants[*index]->lastUse)) {
        index = i;
      }
    }
    return index;
  }

  /// Stores \p candidate in the stroke variant of \p cache that `strokeVariantToReplace` picks,
  /// charging its bytes to the document budget in place of the variant it replaces.
  /// @param source Entity owning \p cache.
  /// @param cache Stroke cache of \p source.
  /// @param candidate Outline to store; moved from only when stored.
  /// @return Index of the stored variant, or nullopt when every variant may still be read by an
  ///   open frame or the budget refused the outline. The cache is unchanged then.
  std::optional<std::size_t> storeStrokeVariant(
      EntityHandle source, geode::GeodePathCacheComponent& cache,
      std::unique_ptr<geode::GeodePathCacheComponent::StrokeVariant>& candidate) {
    const std::optional<std::size_t> replaceable =
        strokeVariantToReplace(cache, candidate->strokeKey);
    const std::optional<std::size_t> retainedBytes = candidate->retainedBytes();
    if (!replaceable.has_value() || !retainedBytes.has_value()) {
      return std::nullopt;
    }
    std::unique_ptr<geode::GeodePathCacheComponent::StrokeVariant>& slot =
        cache.strokeVariants[*replaceable];
    // A replaced variant hands its charge over, so the budget never holds both outlines.
    geode::GeodeGeometryCacheReservation& reservation =
        slot ? slot->reservation : candidate->reservation;
    if (!reservation.replace(documentGeometryBudget(*source.registry()), *retainedBytes)) {
      return std::nullopt;
    }
    releaseStrokeVariant(source, cache, *replaceable);
    if (slot) {
      candidate->reservation = std::move(slot->reservation);
    }
    slot = std::move(candidate);
    return replaceable;
  }

  /// Stroke-side of the cache. Builds (or reuses) the `strokeToFill` output and its encode in
  /// one of `source`'s `GeodePathCacheComponent::strokeVariants`, keyed by the stroke style, the
  /// device-derived flattening tolerance, and the host transform of a `non-scaling-stroke`
  /// centerline. Returns a `StrokeDerived` pointing into the cached variant, or into
  /// `strokeScratchPath` for a draw with no entity, past the document budget, or past the
  /// variants when every one may still be read by an open frame. The caller must check
  /// `strokedPath == nullptr` for the "zero-stroke" case.
  StrokeDerived getStrokeDerived(EntityHandle source, const Path& geometry,
                                 const StrokeStyle& strokeStyle,
                                 const std::optional<Transform2d>& hostFromLocal) {
    StrokeDerived result;
    // Device-aware flattening tolerance for this draw. Part of the cache key: a zoom change that
    // crosses a scale bucket must re-flatten instead of serving the outline tessellated for the
    // old scale.
    const double flattenTolerance = strokeFlattenTolerance();
    const auto fromVariant = [&](geode::GeodePathCacheComponent& cache, std::size_t index) {
      geode::GeodePathCacheComponent::StrokeVariant& variant = *cache.strokeVariants[index];
      variant.lastUse = ++cache.strokeUseClock;
      variant.lastUseFrame = currentFrameIndex;
      variant.lastUseDeviceId = device->deviceId();
      result.strokedPath = &variant.strokedPath;
      result.encoded = &variant.strokedEncode;
      result.persistent = true;
      result.variant = index;
      result.fillRule = variant.strokeFillRule;
      return result;
    };
    if (source) {
      ensureCacheInvalidationWired(*source.registry());
      auto& cache = source.get_or_emplace<geode::GeodePathCacheComponent>();
      if (const std::optional<std::size_t> hit =
              cache.strokeVariantFor(strokeStyle, flattenTolerance, hostFromLocal)) {
        return fromVariant(cache, *hit);
      }

      Path stroked = geometry.strokeToFill(strokeStyle, flattenTolerance);
      if (stroked.empty()) {
        return result;  // strokedPath stays null.
      }
      countStrokeOutline(stroked);
      const FillRule fillRule = FillRule::NonZero;
      std::optional<geode::EncodedPath> encoded = encodeGeometry(stroked, fillRule);
      if (!encoded.has_value()) {
        return result;
      }
      auto candidate = std::make_unique<geode::GeodePathCacheComponent::StrokeVariant>(
          geode::GeodePathCacheComponent::StrokeVariant{
              .strokeKey = strokeStyle,
              .flattenTolerance = flattenTolerance,
              .hostFromLocal = hostFromLocal,
              .strokedPath = std::move(stroked),
              .strokedEncode = std::move(*encoded),
              .strokeFillRule = fillRule,
          });
      if (const std::optional<std::size_t> stored = storeStrokeVariant(source, cache, candidate)) {
        return fromVariant(cache, *stored);
      }
      // Every variant may still be read by an open frame, or the document budget refused the
      // outline: draw from a per-frame outline and leave the cached variants and their residence
      // in place.
      strokeScratchPath = std::move(candidate->strokedPath);
      strokeScratchEncode = std::move(candidate->strokedEncode);
      result.strokedPath = &strokeScratchPath;
      result.encoded = &*strokeScratchEncode;
      result.fillRule = fillRule;
      return result;
    }
    // No-entity fallback: compute and admit into Impl-local scratch buffers.
    strokeScratchPath = geometry.strokeToFill(strokeStyle, flattenTolerance);
    if (strokeScratchPath.empty()) {
      return result;
    }
    countStrokeOutline(strokeScratchPath);
    result.strokedPath = &strokeScratchPath;
    result.fillRule = FillRule::NonZero;
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
  /// Charges one gradient draw's \p stops to the frame, as the CPU renderer charges each gradient
  /// it instantiates. Past the frame's cap the draw is refused and, as on the CPU renderer, the
  /// rest of the frame with it: the frame's geometry is rejected.
  /// @return Whether the draw may proceed.
  bool reserveGradientStops(std::size_t stops) {
    if (drawBudget->reserve({.gradientStops = stops})) {
      return true;
    }
    geometryBudget->reject();
    return false;
  }

  /// Lets \p slot go of the stop ramp and ramp bind group it holds, if any; null is a no-op.
  ///
  /// A shape whose fill or stroke turns solid, or whose fill turns to a pattern or draws through
  /// the fill batch, releases its ramp here. One whose paint turns to none does not reach its
  /// gradient slot, so the slot keeps its ramp until the shape's geometry changes or its residence
  /// is removed. Every kept ramp is charged once, texture and stop key, to its document's resident
  /// limit (`GeodeDocumentGeometryBudget::kMaximumResidentBytes`), and a slot whose ramp would pass
  /// it draws from a ramp uploaded for each frame instead of keeping one.
  static void releaseGradientRamp(geode::GeodeResidentGradientSlot* slot) {
    if (slot != nullptr && slot->ramp != nullptr) {
      slot->replaceRamp(nullptr, gpu::BindGroup());
    }
  }

  /// A charge that reserves a gradient's stops against the frame's budget before they are
  /// converted, and counts the conversions in the frame's counters.
  GradientStopCharge gradientStopCharge() {
    GradientStopCharge charge;
    charge.reserve = [this](std::size_t stops) { return reserveGradientStops(stops); };
    charge.conversions = &counters.gradientStopConversions;
    return charge;
  }

  /// Resolves \p ref against \p geometryBounds into \p out, converting its stops into \p stops
  /// once \p charge admits them. Once the frame's stop budget or geometry budget has refused a
  /// draw, the frame draws nothing more, as on the CPU renderer, so nothing is resolved or
  /// converted and \p out is marked refused.
  void resolveGradient(const components::PaintResolvedReference& ref, const Box2d& geometryBounds,
                       float opacity, std::vector<geode::LinearGradientParams::Stop>& stops,
                       GradientStopCharge& charge, ResolvedGradientPaint& out) {
    out = ResolvedGradientPaint{};
    out.resolved = true;
    if (drawBudget->rejected() || geometryBudget->rejected()) {
      out.refused = true;
      return;
    }
    out.linear = resolveLinearGradientParams(ref, geometryBounds, paint.viewBox, paint.currentColor,
                                             opacity, stops, charge);
    if (!out.linear.has_value() && !charge.refused) {
      out.radial = resolveRadialGradientParams(ref, geometryBounds, paint.viewBox,
                                               paint.currentColor, opacity, stops, charge);
    }
    out.refused = charge.refused;
  }

  /// Resolves the fill batch's gradient into \p out, charging its stops, so the batch can carry it
  /// and a fill that leaves the batch draws from the same resolution. A gradient with more stops
  /// than the batch's paint block carries never batches, so it is left unresolved for the
  /// ordinary emit path. @return Whether the batch can carry the gradient.
  bool resolveBatchGradient(const Path& geometryPath, const components::ResolvedPaintServer& server,
                            double effectiveOpacity, ResolvedGradientPaint& out) {
    const auto* ref = std::get_if<components::PaintResolvedReference>(&server);
    const auto* computedGradient =
        ref != nullptr && ref->reference.handle
            ? ref->reference.handle.try_get<components::ComputedGradientComponent>()
            : nullptr;
    if (computedGradient == nullptr ||
        computedGradient->stops.size() > geode::GeoEncoder::kMaxSceneGradientStops) {
      return false;
    }
    GradientStopCharge charge = gradientStopCharge();
    resolveGradient(*ref, geometryPath.bounds(), static_cast<float>(effectiveOpacity),
                    gradientStopScratch, charge, out);
    return out.batchable();
  }

#ifdef DONNER_TEXT_ENABLED
  /// The text element's own gradient fill and stroke, resolved once per text draw. The
  /// parameters point into the stop vectors here, so the struct outlives every glyph draw of the
  /// text element.
  struct TextElementGradients {
    std::vector<geode::LinearGradientParams::Stop> fillStops;
    std::vector<geode::LinearGradientParams::Stop> strokeStops;
    ResolvedGradientPaint fill;
    ResolvedGradientPaint stroke;
  };

  /// Resolves the text element's own gradient fill and stroke once per text draw, charging and
  /// converting their stops, as the CPU renderer does when it builds the element's paints. A run
  /// that paints with the element's paint draws from these and is not charged again.
  /// @param textBounds The text element's bounds, which the gradients map against.
  /// @param stroked Whether the element's stroke has a positive width.
  /// @param patternFill Whether the driver staged a pattern for the element's fill.
  void resolveTextElementGradients(const Box2d& textBounds, bool stroked, bool patternFill,
                                   TextElementGradients& out) {
    resolvePaintGradient(!patternFill, paint.fill, textBounds, paint.fillOpacity, out.fillStops,
                         out.fill);
    resolvePaintGradient(stroked && !patternStrokePaint.has_value(), paint.stroke, textBounds,
                         paint.strokeOpacity, out.strokeStops, out.stroke);
  }

  /// Resolves \p server into \p out when \p enabled and it is a paint server reference, charging
  /// its stops; leaves \p out unresolved otherwise. A reference that is not a gradient charges
  /// nothing.
  void resolvePaintGradient(bool enabled, const components::ResolvedPaintServer& server,
                            const Box2d& bounds, double opacity,
                            std::vector<geode::LinearGradientParams::Stop>& stops,
                            ResolvedGradientPaint& out) {
    const auto* ref = std::get_if<components::PaintResolvedReference>(&server);
    if (!enabled || ref == nullptr) {
      return;
    }
    GradientStopCharge charge = gradientStopCharge();
    resolveGradient(*ref, bounds, static_cast<float>(opacity), stops, charge, out);
  }

  /// The gradient paint a text run draws with. A run with a paint of its own resolves it once
  /// against the text bounds, charging its stops, as the CPU renderer does for a span's paint
  /// server reference, whether or not the run has glyphs to paint. A run that paints with the text
  /// element's paint draws from \p elementPaint, charged once per text draw.
  /// @param painted Whether the run paints this component at all; null is returned when not.
  /// @param ownPaint Whether the run's paint is its span's own rather than the element's.
  /// @param stops Storage for the run's converted stops, which must outlive the run's draws.
  const ResolvedGradientPaint* runGradientPaint(
      bool painted, bool ownPaint, const components::ResolvedPaintServer& server,
      const Box2d& textBounds, double opacity,
      std::vector<geode::LinearGradientParams::Stop>& stops, ResolvedGradientPaint& runPaint,
      const ResolvedGradientPaint& elementPaint) {
    if (!painted) {
      return nullptr;
    }
    if (!ownPaint) {
      return &elementPaint;
    }
    resolvePaintGradient(true, server, textBounds, opacity, stops, runPaint);
    return &runPaint;
  }
#endif  // DONNER_TEXT_ENABLED

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
                    geode::GeodeResidentGradientSlot* gradientResidentSlot = nullptr,
                    const ResolvedGradientPaint* resolvedGradient = nullptr) {
    if (!encoder) {
      return;
    }
    // Pattern dispatch comes first: a pattern slot is populated by the
    // driver via `endPatternTile`, and is consumed by the very next fill or
    // stroke draw (matching the CPU-renderer semantics).
    if (patternFillPaint.has_value()) {
      syncTransform();
      emitPatternFill(path, rule, buildPatternPaint(*patternFillPaint, paint.fillOpacity),
                      precomputedEncoded, residentSlot);
      releasePatternPaintSlot(patternFillPaint);
      releaseGradientRamp(gradientResidentSlot);
      return;
    }
    const double effectiveOpacity = paint.fillOpacity;
    drawPaintedPath(path, paint.fill, effectiveOpacity, rule, precomputedEncoded, residentSlot,
                    gradientResidentSlot, resolvedGradient);
  }

  /// Core dispatch: given a path and a resolved paint server, emit the
  /// appropriate fill call (solid color or gradient).
  void drawPaintedPath(const Path& path, const components::ResolvedPaintServer& server,
                       double effectiveOpacity, FillRule rule,
                       const geode::EncodedPath* precomputedEncoded = nullptr,
                       geode::GeodeResidentSlot* residentSlot = nullptr,
                       geode::GeodeResidentGradientSlot* gradientResidentSlot = nullptr,
                       const ResolvedGradientPaint* resolvedGradient = nullptr) {
    drawPaintedPathAgainst(path, path, server, effectiveOpacity, rule, precomputedEncoded,
                           residentSlot, gradientResidentSlot, resolvedGradient);
  }

  /// Draws \p linear over \p drawPath; its stops were charged when they were converted.
  /// @param rampCache Stop ramp cache of the resolution \p linear comes from.
  void drawLinearGradient(const Path& drawPath, const geode::LinearGradientParams& linear,
                          FillRule rule, const geode::EncodedPath* precomputedEncoded,
                          geode::GeodeResidentGradientSlot* gradientResidentSlot,
                          geode::GradientRampCache* rampCache) {
    syncTransform();
    if (gradientResidentSlot != nullptr && precomputedEncoded != nullptr) {
      encoder->fillPathLinearGradientResident(*gradientResidentSlot, *precomputedEncoded, linear,
                                              rule, currentFrameIndex);
    } else {
      encoder->fillPathLinearGradient(drawPath, linear, rule, precomputedEncoded, rampCache);
    }
  }

  /// Draws a resolved radial gradient over \p drawPath, or the single color a
  /// zero radius collapses it to; its stops were charged when it was resolved.
  /// @param resolved The radial resolution; empty when the reference is not a
  ///   radial gradient.
  /// @param rampCache Stop ramp cache of the resolution.
  /// @return False when there is no usable radial gradient and the caller should
  ///   paint the reference's fallback instead; true when it was drawn.
  bool drawRadialGradient(const Path& drawPath,
                          const std::optional<ResolvedRadialGradient>& resolved, FillRule rule,
                          const geode::EncodedPath* precomputedEncoded,
                          geode::GeodeResidentSlot* residentSlot,
                          geode::GeodeResidentGradientSlot* gradientResidentSlot,
                          geode::GradientRampCache* rampCache) {
    if (!resolved.has_value() ||
        (!resolved->gradient.has_value() && !resolved->solidFallback.has_value())) {
      return false;
    }
    const ResolvedRadialGradient& radial = *resolved;
    syncTransform();
    if (radial.solidFallback.has_value()) {
      // SVG2 degenerate radial (r=0): paint the last stop color as a
      // solid fill so the element remains visible.
      emitSolidFill(drawPath, *radial.solidFallback, rule, precomputedEncoded, residentSlot);
    } else if (gradientResidentSlot != nullptr && precomputedEncoded != nullptr) {
      encoder->fillPathRadialGradientResident(*gradientResidentSlot, *precomputedEncoded,
                                              *radial.gradient, rule, currentFrameIndex);
    } else {
      encoder->fillPathRadialGradient(drawPath, *radial.gradient, rule, precomputedEncoded,
                                      rampCache);
    }
    return true;
  }

  /// Draws \p resolved over \p drawPath, unless it was refused or the frame has refused a draw
  /// since it was resolved.
  /// @return True when it was drawn or refused; false when it is not a usable gradient and the
  ///   caller should paint the reference's fallback.
  bool drawResolvedGradient(const ResolvedGradientPaint& resolved, const Path& drawPath,
                            FillRule rule, const geode::EncodedPath* precomputedEncoded,
                            geode::GeodeResidentSlot* residentSlot,
                            geode::GeodeResidentGradientSlot* gradientResidentSlot) {
    if (resolved.refused || drawBudget->rejected()) {
      return true;
    }
    if (resolved.linear.has_value()) {
      drawLinearGradient(drawPath, *resolved.linear, rule, precomputedEncoded, gradientResidentSlot,
                         &resolved.rampCache);
      return true;
    }
    return drawRadialGradient(drawPath, resolved.radial, rule, precomputedEncoded, residentSlot,
                              gradientResidentSlot, &resolved.rampCache);
  }

  /// Draws \p ref over \p drawPath as a linear or radial gradient: from \p preResolved when the
  /// caller resolved it already, else resolved against \p geometryPath here, its stops charged to
  /// the frame before they are converted (see `resolveGradient`).
  /// @return True when the reference was drawn or refused; false when it is not a usable
  ///   gradient and the caller should paint its fallback.
  bool drawGradientReference(const components::PaintResolvedReference& ref,
                             const Path& geometryPath, const Path& drawPath, float opacity,
                             FillRule rule, const geode::EncodedPath* precomputedEncoded,
                             geode::GeodeResidentSlot* residentSlot,
                             geode::GeodeResidentGradientSlot* gradientResidentSlot,
                             const ResolvedGradientPaint* preResolved) {
    if (preResolved != nullptr && preResolved->resolved) {
      return drawResolvedGradient(*preResolved, drawPath, rule, precomputedEncoded, residentSlot,
                                  gradientResidentSlot);
    }
    ResolvedGradientPaint resolved;
    GradientStopCharge charge = gradientStopCharge();
    resolveGradient(ref, geometryPath.bounds(), opacity, gradientStopScratch, charge, resolved);
    return drawResolvedGradient(resolved, drawPath, rule, precomputedEncoded, residentSlot,
                                gradientResidentSlot);
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
                              geode::GeodeResidentGradientSlot* gradientResidentSlot = nullptr,
                              const ResolvedGradientPaint* resolvedGradient = nullptr) {
    if (!encoder || drawPath.empty()) {
      return;
    }
    if (std::holds_alternative<PaintServer::None>(server)) {
      return;
    }

    const css::RGBA currentColor = paint.currentColor;
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
      if (drawGradientReference(*ref, geometryPath, drawPath, opacity, rule, precomputedEncoded,
                                residentSlot, gradientResidentSlot, resolvedGradient)) {
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
                     "has no fallback (patterns and sweep gradients are not drawn here)\n";
        warnedGradient = true;
      }
    }
  }

  // The cache-invalidation listener is a free function with no dependency
  // on `this`, so Impl teardown intentionally leaves it attached. Connections
  // die with the `Registry` they live on, and calling `.disconnect<&fn>()` from
  // a renderer dtor would UB when the registry was destroyed first.

  /// Drops a runtime texture handle without pooling it, for teardown paths where the pool and
  /// the surface budget are on their way out too.
  /// @param texture Handle to drop; left invalid.
  static void releaseRuntimeTexture(gpu::Texture& texture) { texture = gpu::Texture(); }

  static void releasePendingTexture(PendingRelease& release) {
    releaseRuntimeTexture(release.texture);
  }

  /// Hand every clip-mask texture on \p entries back to the pool at the frame boundary, and
  /// drop the entries. Used for clip state a frame ends still holding.
  /// @param entries Clip stack to empty.
  void releaseClipStackTexturesAtFrameEnd(std::vector<ClipStackEntry>& entries) {
    for (ClipStackEntry& entry : entries) {
      for (PendingRelease& release : entry.maskLayerTextures) {
        releaseTextureAtFrameEnd(std::move(release.texture), release.desc);
      }
      entry.maskLayerTextures.clear();
      entry.maskResolveTextureHandle = nullptr;
      entry.maskResolveViewHandle = nullptr;
    }
    entries.clear();
  }

  void retireOpenLayerStack(bool submitRecordedWork) {
    for (LayerStackFrame& frame : layerStack) {
      if (submitRecordedWork) {
        retireFinishedEncoder(std::move(frame.savedEncoder));
      }
      releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
    }
    layerStack.clear();
  }

  void retireOpenFilterStack(bool submitRecordedWork) {
    for (FilterStackFrame& frame : filterStack) {
      if (submitRecordedWork && frame.savedEncoder) {
        if (frame.allocationRejected) {
          frame.savedEncoder->finish();
        }
        retireFinishedEncoder(std::move(frame.savedEncoder));
      }
      releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
      releaseTextureAtFrameEnd(std::move(frame.backdropTexture), frame.backdropDesc);
      releaseClipStackTexturesAtFrameEnd(frame.savedClipStack);
      filterExecutionBudget->release(frame.filterReservation);
    }
    filterStack.clear();
    rejectedFilterDepth = 0;
  }

  void retireOpenMaskStack(bool submitRecordedWork) {
    for (MaskStackFrame& frame : maskStack) {
      if (submitRecordedWork) {
        retireFinishedEncoder(std::move(frame.savedEncoder));
      }
      releaseTextureAtFrameEnd(std::move(frame.maskTexture), frame.maskDesc);
      releaseTextureAtFrameEnd(std::move(frame.contentTexture), frame.contentDesc);
      releaseClipStackTexturesAtFrameEnd(frame.savedClipStack);
      releaseClipStackTexturesAtFrameEnd(frame.compositeRegionClip);
    }
    maskStack.clear();
  }

  /// Drop unclosed offscreen scopes without compositing their contents into the root target.
  /// Recorded encoders remain alive through submission; an abandoned frame can discard them.
  void retireOpenFrameStacksAtBoundary(bool submitRecordedWork) {
    releaseClipStackTexturesAtFrameEnd(clipStack);
    layerBackdropRecords.clear();
    if (layerStack.empty() && filterStack.empty() && maskStack.empty()) {
      return;
    }

    counters.unclosedLayerScopes += layerStack.size();
    counters.unclosedFilterScopes += filterStack.size();
    counters.unclosedMaskScopes += maskStack.size();
    retireOpenLayerStack(submitRecordedWork);
    retireOpenFilterStack(submitRecordedWork);
    retireOpenMaskStack(submitRecordedWork);

    geometryDebugEdges.clear();
    target = hostTarget.isValid() ? aliasOf(hostTarget) : aliasOf(ownedTarget);
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

  /// Give back what this frame borrowed from the documents it drew: the records that same-frame
  /// repeats took (`sceneTempRecordSlots`) and the glyphs made resident for this frame only
  /// (`transientGlyphEntries`). Both free into the documents' per-device slabs, and a glyph also
  /// gives back what it charged to its document's budget.
  ///
  /// `endFrame` calls this once the frame has submitted, while whoever drives the frame still holds
  /// its documents. Left for the next frame, it would run while the renderer holds another
  /// document, or none, against a thread that may be editing or destroying these. The budgets this
  /// renderer's frame touched are settled at the same point, for the same reason.
  void releaseFrameLoans() {
    for (const SceneTempRecordSlot& tempSlot : sceneTempRecordSlots) {
      tempSlot.slab->freeSlot(tempSlot.slot);
    }
    sceneTempRecordSlots.clear();
    transientGlyphEntries.clear();
    documentGeometryFrameState->settle(touchedDocumentBudgets);
  }

  void resetForBeginFrame(const RenderViewport& nextViewport) {
    // A frame that never reached endFrame submits nothing, and what it touched is retained or
    // returned below; its entries' views go first, with the bind groups naming its clip masks.
    if (frameRecordingOpen || frameRecordingAbandoned || frameArenaWriteFailed) {
      releaseAllRetainedViews();
    }
    borrowedTargetSnapshot.reset();
    if (device) {
      device->drainDeferredDestroys();
    }
    counters.reset();
    simpleClipCulls = 0;
    viewport = nextViewport;
    const Vector2i pixelSize = CheckedViewportPixels(nextViewport).value_or(Vector2i::Zero());
    pixelWidth = pixelSize.x;
    pixelHeight = pixelSize.y;
    deviceFromLocalTransform = Transform2d();
    deviceFromLocalTransformStack.clear();
    paint = PaintParams();
    encoder.reset();
    frameFinishedEncoders.clear();
    discardFrameGpuEncoder();
    // An unfinished frame can leave pattern tiles, clips, open scopes, and pending releases
    // behind. Retire them before resetting their budget. Unsubmitted work returns to the pool;
    // resources whose submission is uncertain stay retained until device teardown.
    retireUnconsumedPatternTilesAtFrameEnd(OuterPatternState::kDrop);
    retireOpenFrameStacksAtBoundary(/*submitRecordedWork=*/false);
    if (frameRecordingAbandoned) {
      retainPendingFrameReleasesAfterFailure();
    } else {
      drainPendingReleases();
    }
    geometryDebugEdges.clear();
    rejectedFilterDepth = 0;
    frameRecordingAbandoned = false;
    frameArenaWriteFailed = false;
    if (frameResourceScopeDepth == 0) {
      resetOwnedFrameBudgets();
    }
    closeFrameGeneration();
    // A sibling renderer may still have unsubmitted reads of the shared filter scratch.
    if (device && device->oldestOpenFrameGeneration() == std::numeric_limits<uint64_t>::max()) {
      device->filterEngine().beginFrame();
    }
    if (texturePool) {
      texturePool->beginFrame();
    }
    if (device) {
      currentFrameIndex = device->beginFrameGeneration();
      frameGenerationOpen = true;
      settleDocumentResidency(frameDocument);
    } else {
      ++currentFrameIndex;
    }
    beginFrameRetainedViews();
    lastDrawSourceEntity = entt::null;
    pendingBatch.reset();
    beginFrameRetainedComposites();
    // Nothing to give back after an endFrame. A frame that never reached one gives its loans back
    // here.
    releaseFrameLoans();
    transientTextEncodes.clear();
  }

  /// Counts a captured filter in the frame's reduced-raster or tiled statistics.
  void countAdmittedFilter(const GeodeFilterAdmission& admission) {
    if (admission.reducedRasterFromDevice) {
      ++filterReducedRasters;
      filterRasterReduction = std::max(filterRasterReduction, admission.rasterReduction);
    } else if (admission.fullPlan.tiles > 1) {
      ++filterTiledGraphs;
    }
  }

  void resetOwnedFrameBudgets() {
    if (ownsFilterExecutionBudget) {
      filterExecutionBudget->reset();
    }
    filterReducedRasters = 0;
    filterTiledGraphs = 0;
    filterRasterReduction = 1;
    if (ownsFilterPreparationBudget) {
      filterPreparationBudget->reset();
    }
    if (ownsGeometryBudget) {
      geometryBudget->reset();
      documentGeometryFrameState->reset();
      touchedDocumentBudgets.clear();
    }
    if (ownsSurfaceBudget) {
      surfaceBudget->reset();
    }
    if (ownsTextMaterializationBudget) {
      textMaterializationBudget->reset();
    }
    if (ownsDrawBudget) {
      drawBudget->reset();
    }
  }

  /// Whether the host's texture is a surface this renderer can draw a frame into, and its
  /// extent when it is.
  ///
  /// The device's own record of the texture is what is checked, so a handle that is stale or of
  /// another device is refused here rather than at the first pass that names it. Format and the
  /// render-attachment capability are the whole check: every texture with a record is 2D, single
  /// layer and single sample, because the runtime's texture model has no way to say otherwise
  /// and rejects any other sample count where textures are created. The remaining capabilities
  /// are not checked because the record carries the ones the texture actually has: a host that
  /// supplies a draw-only surface gets its frame, and the operations that surface cannot serve
  /// are refused where they are asked for.
  [[nodiscard]] std::optional<gpu::Extent2d> drawableHostTargetExtent() const {
    gpu::Result<gpu::TextureDescriptor> descriptor =
        device->runtimeDevice().textureDescriptor(hostTarget);
    if (descriptor.hasError() || descriptor.result().format != textureFormat ||
        !gpu::HasAllFlags(descriptor.result().usage, gpu::TextureUsage::RenderAttachment)) {
      return std::nullopt;
    }
    return descriptor.result().size;
  }

  bool prepareHostFrameTarget() {
    retireOwnedTargetAtFrameBoundary();
    const std::optional<gpu::Extent2d> extent = drawableHostTargetExtent();
    if (!extent || extent->width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        extent->height > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
      (void)surfaceBudget->reserve(-1, -1);
      target = gpu::Texture();
      return false;
    }
    pixelWidth = static_cast<int>(extent->width);
    pixelHeight = static_cast<int>(extent->height);
    if (!surfaceBudget->reserve(pixelWidth, pixelHeight)) {
      target = gpu::Texture();
      return false;
    }
    target = aliasOf(hostTarget);
    return true;
  }

  bool prepareOwnedFrameTarget() {
    if (!surfaceBudget->reserve(pixelWidth, pixelHeight)) {
      target = gpu::Texture();
      return false;
    }

    const bool canReuseTargets =
        ownedTarget.isValid() && targetWidth == pixelWidth && targetHeight == pixelHeight;
    if (!canReuseTargets) {
      retireOwnedTargetAtFrameBoundary();
      gpu::Result<gpu::Texture> created =
          device->runtimeDevice().createTexture(gpu::TextureDescriptor{
              "RendererGeodeTarget",
              gpu::Extent2d{static_cast<uint32_t>(pixelWidth), static_cast<uint32_t>(pixelHeight)},
              textureFormat,
              gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc |
                  gpu::TextureUsage::Sampled});
      if (!created.hasResult()) {
        target = gpu::Texture();
        return false;
      }
      // The runtime counts the creation itself, so there is no explicit tick here.
      ownedTarget = std::move(created).result();
      targetWidth = pixelWidth;
      targetHeight = pixelHeight;
    }
    target = aliasOf(ownedTarget);
    return true;
  }

  bool prepareFrameTarget() {
    if (!device || device->isDeviceLost() || !pipeline || !gradientPipeline || !imagePipeline ||
        pixelWidth <= 0 || pixelHeight <= 0) {
      retireOwnedTargetAtFrameBoundary();
      return false;
    }
    device->activateCounters(counters);
    if (hostTarget.isValid()) {
      return prepareHostFrameTarget();
    }
    return prepareOwnedFrameTarget();
  }

  void createFrameEncoder() {
    frameRecordingOpen = true;
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

  /// Drop every offscreen the open push/pop stacks still hold, and the stacks with them.
  ///
  /// Teardown, not a frame boundary: the pool these came from is going away too, so they are
  /// dropped rather than handed back to it.
  void releaseOpenStackTexturesAtTeardown() {
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
      releaseClipStackTextures(frame.savedClipStack);
      releaseClipStackTextures(frame.compositeRegionClip);
    }
    maskStack.clear();

    releaseClipStackTextures(clipStack);
    for (FilterStackFrame& frame : filterStack) {
      releaseRuntimeTexture(frame.layerTexture);
      releaseRuntimeTexture(frame.backdropTexture);
      releaseClipStackTextures(frame.savedClipStack);
    }
    filterStack.clear();
    layerBackdropRecords.clear();

    for (PatternStackFrame& frame : patternStack) {
      releaseRuntimeTexture(frame.tileTexture);
    }
    patternStack.clear();
    if (patternFillPaint.has_value()) {
      releaseRuntimeTexture(patternFillPaint->tile);
    }
    if (patternStrokePaint.has_value()) {
      releaseRuntimeTexture(patternStrokePaint->tile);
    }
    patternFillPaint.reset();
    patternStrokePaint.reset();
    for (auto* tiles : {&spanPatternPaints.fill, &spanPatternPaints.stroke}) {
      for (auto& [slot, tile] : *tiles) {
        releaseRuntimeTexture(tile.tile);
      }
      tiles->clear();
    }
    patternTileViews.clear();
    clipMaskViews.clear();
  }

  /// Give up this renderer's render targets at teardown.
  ///
  /// Dropping a handle would release the runtime slot and leave the backend allocation for the
  /// host runtime to collect, so a target this renderer allocated is destroyed explicitly; a
  /// series of renderers sharing one device would otherwise leave one resident apiece. An
  /// embedder's texture is only named here, so that handle is dropped.
  void releaseFrameTargetsAtTeardown() {
    if (device && ownedTarget.isValid()) {
      UTILS_RELEASE_ASSERT(device->deferDestroyTextureBacking(std::move(ownedTarget)));
    }
    ownedTarget = gpu::Texture();
  }

  /// Wait for the backend to go idle, unless the device is gone or lost.
  ///
  /// Bounded, and skipped once the device is lost, so renderer teardown never blocks on a hung
  /// driver.
  void waitForQueueIdleAtTeardown() {
#ifndef __EMSCRIPTEN__
    if (device && device->physicalDeviceOwner()->hasBackendDevice()) {
      device->waitForQueueIdle();
    }
#endif  // Browser submissions retain their resources; teardown must not wait on JS promises.
  }

  ~Impl() {
    counterOwnerToken.reset();
    encoder.reset();
    frameFinishedEncoders.clear();
    discardFrameGpuEncoder();
    closeFrameGeneration();

    waitForQueueIdleAtTeardown();
    // Retained views go before any texture this teardown destroys, inside the pool or not.
    clearRetainedComposites();
    if (texturePool) {
      texturePool->removeDestroyListener(std::exchange(texturePoolDestroyListener, 0));
    }
    drainFrameReusableTextures(frameRecordingAbandoned || !device || device->isDeviceLost());
    releaseOpenStackTexturesAtTeardown();
    releaseFrameTargetsAtTeardown();
    texturePool.reset();

    if (device) {
      device->drainDeferredDestroys();
      waitForQueueIdleAtTeardown();
      device->removeCounters(counters);
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
    texturePoolDestroyListener = texturePool->addDestroyListener(
        [this](const gpu::ResourceIdentity& texture) { releaseRetainedViewsOf(texture); });
    textureFormat = device->textureFormat();
    device->activateCounters(counters);
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
  // Destruction callbacks must not create pools while registry.destroy() iterates them.
  static_cast<void>(registry.storage<geode::GeodePathCacheComponent>());
  static_cast<void>(registry.storage<geode::GeodeResidentPathComponent>());
  static_cast<void>(registry.storage<geode::GeodeResidentImageComponent>());
  static_cast<void>(registry.storage<GeodeClipPathCacheComponent>());
  registry.on_update<components::ComputedPathComponent>().connect<&Impl::onComputedPathChanged>();
  registry.on_destroy<components::ComputedPathComponent>().connect<&Impl::onComputedPathChanged>();
  registry.on_update<components::LoadedImageComponent>().connect<&Impl::onLoadedImageChanged>();
  registry.on_destroy<components::LoadedImageComponent>().connect<&Impl::onLoadedImageChanged>();
  // A clip's cached geometry goes whenever its computed clip paths are made, replaced or dropped.
  registry.on_construct<components::ComputedClipPathsComponent>()
      .connect<&Impl::onComputedClipPathsChanged>();
  registry.on_update<components::ComputedClipPathsComponent>()
      .connect<&Impl::onComputedClipPathsChanged>();
  registry.on_destroy<components::ComputedClipPathsComponent>()
      .connect<&Impl::onComputedClipPathsChanged>();
  // Leaving the connection attached across renderer destruction is
  // intentional: our free-function listener has no `this`-capture,
  // so it's safe to outlive the renderer. Connections die with the
  // registry.
}

void RendererGeode::Impl::onComputedClipPathsChanged(Registry& registry, Entity entity) {
  // Slot reset parks bind groups and buffer ranges in their slabs until no older frame is open.
  registry.remove<GeodeClipPathCacheComponent>(entity);
}

void RendererGeode::Impl::onComputedPathChanged(Registry& registry, Entity entity) {
  // entt allows `remove` on a component the entity doesn't hold - it's a
  // cheap no-op in that case. We don't need an `all_of` guard.
  registry.remove<geode::GeodePathCacheComponent>(entity);
  // Drop the GPU residence in lock-step with the CPU encode cache.
  // Slot reset parks bind groups and buffer ranges in their slabs until no older frame is open.
  // Geometry can change during a shared-device offscreen draw before the outer frame submits.
  registry.remove<geode::GeodeResidentPathComponent>(entity);
}

void RendererGeode::Impl::onLoadedImageChanged(Registry& registry, Entity entity) {
  // Each device's texture parks in that device's slab until no open frame can still sample it.
  registry.remove<geode::GeodeResidentImageComponent>(entity);
}

namespace {

/// Most idle headless devices kept for renderers that do not bring their own.
constexpr std::size_t kMaxIdleHeadlessDevices = 4;

using HeadlessGeodeDevicePool = details::HeadlessDevicePool<geode::GeodeDevice>;

HeadlessGeodeDevicePool& SharedHeadlessGeodeDevicePool() {
  // Active leases retain a raw pointer to the pool. Keep the small bounded
  // cache alive through process teardown so global renderers cannot outlive it.
  static auto* pool = new HeadlessGeodeDevicePool(
      [] { return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless()); },
      kMaxIdleHeadlessDevices);
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

void RendererGeode::setMaximumGlyphs(std::size_t maximumGlyphs) {
#ifdef DONNER_TEXT_ENABLED
  impl_->textMaterializationBudget->setMaximumGlyphs(maximumGlyphs);
#else
  (void)maximumGlyphs;
#endif
}

std::size_t RendererGeode::maximumGlyphs() const {
#ifdef DONNER_TEXT_ENABLED
  return impl_->textMaterializationBudget->maximumGlyphs();
#else
  return 0;
#endif
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

void RendererGeode::trimTexturePool(RendererGeodeTexturePoolTrim level) {
  if (impl_->texturePool) {
    impl_->texturePool->trim(level);
  }
  if (level == RendererGeodeTexturePoolTrim::All) {
    impl_->releaseRetainedComposites();
  }
}

void RendererGeode::setTexturePoolClockForTesting(
    std::function<std::chrono::steady_clock::time_point()> clock) {
  if (impl_->texturePool) {
    impl_->texturePool->setClockForTesting(std::move(clock));
  }
}

void RendererGeode::setTargetTexture(const gpu::Texture& texture) {
  impl_->borrowedTargetSnapshot.reset();
  impl_->hostTarget = Impl::aliasOf(texture);
}

void RendererGeode::clearTargetTexture() {
  impl_->borrowedTargetSnapshot.reset();
  impl_->hostTarget = gpu::Texture();
  impl_->preserveTargetOnBeginFrame = false;
}

void RendererGeode::setPreserveTargetOnBeginFrame(bool preserve) {
  impl_->preserveTargetOnBeginFrame = preserve;
}

void RendererGeode::setAntialias(bool antialias) {
  impl_->antialias = antialias;
}

RendererGeode::~RendererGeode() = default;
RendererGeode::RendererGeode(RendererGeode&&) noexcept = default;
RendererGeode& RendererGeode::operator=(RendererGeode&&) noexcept = default;

void RendererGeode::draw(SVGDocument& document) {
  drawDocument(document, nullptr);
}

void RendererGeode::draw(SVGDocument& document, ParseWarningSink& warningSink) {
  drawDocument(document, &warningSink);
}

void RendererGeode::drawDocument(SVGDocument& document, ParseWarningSink* warningSink) {
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

  RendererDriver driver(*this, impl_->verbose, nullptr, nullptr, warningSink);
  // A concurrent-DOM document is captured into a snapshot and replayed, and replay must not touch
  // the live document, so its frames leave its residence alone; only a live draw, which holds write
  // access for the whole frame, settles it.
  if (document.threadingMode() != ThreadingMode::ConcurrentDom) {
    impl_->frameDocument = &document.registry();
  }
  driver.draw(document);
  impl_->frameDocument = nullptr;
}

std::vector<std::optional<gpu::ResourceIdentity>> RendererGeode::layerCompositeTexturesForTesting()
    const {
  return impl_->layerComposites.textures();
}

std::vector<std::optional<gpu::ResourceIdentity>>
RendererGeode::filterOutputCompositeTexturesForTesting() const {
  return impl_->filterOutputComposites.textures();
}

std::vector<std::optional<gpu::ResourceIdentity>> RendererGeode::maskCompositeTexturesForTesting()
    const {
  return impl_->maskComposites.textures();
}

RendererGeode::RetainedCompositeCounts RendererGeode::retainedCompositeCountsForTesting() const {
  return RetainedCompositeCounts{
      impl_->layerComposites.entries.size(), impl_->filterOutputComposites.entries.size(),
      impl_->imageBlits.entries.size(), impl_->maskComposites.entries.size()};
}

RendererGeode::RetainedCompositeCounts RendererGeode::retainedCompositeViewHoldersForTesting()
    const {
  return RetainedCompositeCounts{
      impl_->layerComposites.viewHolderCount(), impl_->filterOutputComposites.viewHolderCount(),
      impl_->imageBlits.viewHolderCount(), impl_->maskComposites.viewHolderCount()};
}

size_t RendererGeode::maxRetainedCompositesForTesting() {
  return Impl::RetainedCompositeSequence::kMaxEntries;
}

bool RendererGeode::sceneBatchingEnabledForTesting() {
  return kEnableSceneBatching;
}

void RendererGeode::setRepeatRecordLimitForTesting(std::size_t maximumRecords) {
  impl_->repeatRecordLimit = std::min(impl_->repeatRecordLimit, maximumRecords);
}

void RendererGeode::setGlyphResidencyBudgetForTesting(size_t maxEntries,
                                                      uint64_t maxRetainedBytes) {
  impl_->glyphCacheMaxEntriesForTesting = maxEntries;
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
       impl_->touchedDocumentBudgets) {
    document->setLimitsForTesting(*impl_->documentGeometryLimits);
  }
}

void RendererGeode::setPathEncoderItemLimitForTesting(std::size_t maximumItems) {
  impl_->pathEncoderLimits.maximumEncodedGeometryItems =
      std::min(geode::GeodePathEncoder::Limits{}.maximumEncodedGeometryItems, maximumItems);
}

void RendererGeode::setTextStrokeGroupPointLimitForTesting(std::size_t maximumPoints) {
  impl_->textStrokeGroupPointLimit = std::min(Impl::kMaximumTextStrokeGroupPoints, maximumPoints);
}

void RendererGeode::setTextShapeLimitForTesting(std::size_t maximumShapes) {
  impl_->textShapeLimit =
      std::min(geode::GeodeTextShapeCache::kDefaultMaximumShapes, maximumShapes);
}

void RendererGeode::setGradientStopBudgetForTesting(std::size_t maximumStops) {
  impl_->drawBudget->setGradientStopLimitForTesting(maximumStops);
}

void RendererGeode::setSurfaceBudgetForTesting(std::size_t maximumSurfaces,
                                               std::uint64_t maximumBytes) {
  impl_->surfaceBudget->setLimitsForTesting({.bytes = maximumBytes, .surfaces = maximumSurfaces});
}

void RendererGeode::replaceSurfaceBudgetLimitsForTesting(RendererSurfaceBudget::Limits limits) {
  impl_->surfaceBudget->replaceLimitsWithinDefaultsForTesting(limits);
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
  const Impl::DocumentGeometryFrameState::Figures documents =
      impl_->documentGeometryFrameState->figures(impl_->touchedDocumentBudgets);
  const std::uint64_t documentBytes = documents.retainedBytes;
  const bool documentRejected = documents.rejected;
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
      .filterGraphsRefused = impl_->filterExecutionBudget->refusedGraphs(),
      .filterBudgetRejected = impl_->filterExecutionBudget->rejected() ||
                              impl_->filterExecutionBudget->refusedGraphs() != 0,
      .filterReducedRasterSupported = true,
      .filterReducedRasters = impl_->filterReducedRasters,
      .filterRasterReduction = impl_->filterRasterReduction,
      .filterTiledGraphs = impl_->filterTiledGraphs,
      .geometryBudgetSupported = true,
      .geometryDraws = impl_->geometryBudget->draws(),
      .geometryItems = impl_->geometryBudget->items(),
      .geometryRetainedBytes = reportedGeometryBytes,
      .gradientRampRetainedBytes = documents.rampBytes,
      .geometryBudgetRejected = impl_->geometryBudget->rejected() || documentRejected,
      .surfaceBudgetSupported = true,
      .surfaceCount = impl_->surfaceBudget->surfaces(),
      .surfaceBytes = impl_->surfaceBudget->bytes(),
      .surfacePeakBytes = impl_->surfaceBudget->peakBytes(),
      .surfaceRefusedRequestBytes = impl_->surfaceBudget->refusedRequestBytes(),
      .surfaceLimitBytes = impl_->surfaceBudget->limitBytes(),
      .surfaceBudgetRejected = impl_->surfaceBudget->rejected(),
      .gradientStops = impl_->drawBudget->gradientStops(),
      .drawBudgetRejected = impl_->drawBudget->rejected(),
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

size_t RendererGeode::patternTileViewCountForTesting() const {
  return impl_->patternTileViews.size();
}

size_t RendererGeode::clipGeometryCacheCountForTesting(SVGDocument& document) const {
  return document.registry().view<GeodeClipPathCacheComponent>().size();
}

size_t RendererGeode::clipMaskViewCountForTesting() const {
  return impl_->clipMaskViews.size();
}

std::uint64_t RendererGeode::simpleClipCullCountForTesting() const {
  return impl_->simpleClipCulls;
}

size_t RendererGeode::residentGlyphCountForTesting(SVGDocument& document) {
  if (!impl_->device) {
    return 0;
  }
  auto* residencyPtr = document.registry().ctx().find<std::shared_ptr<DocumentResidency>>();
  if (residencyPtr == nullptr || !*residencyPtr) {
    return 0;
  }
  const DocumentDeviceResidency* residency =
      (*residencyPtr)->devices.find(impl_->device->deviceId());
  return residency != nullptr && residency->glyphs ? residency->glyphs->size() : 0;
}

RendererGeode::TextShapeResidency RendererGeode::textShapeResidencyForTesting(
    SVGDocument& document) {
  TextShapeResidency result;
  if (!impl_->device) {
    return result;
  }
  for (auto&& [entity, component] :
       document.registry().view<geode::GeodeTextShapeResidencyComponent>().each()) {
    if (const geode::GeodeTextShapeCache* cache =
            component.devices.find(impl_->device->deviceId())) {
      result.shapes += cache->keptShapes();
      result.chargedBytes += cache->retainedBytes();
    }
  }
  return result;
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
  impl_->captureOffscreenCounterOwner();
  if (impl_->device) {
    impl_->device->drainFencedTextureBackings();
    if (impl_->device->pendingFencedTextureBackings() != 0) {
      impl_->frameRecordingAbandoned = true;
      return;
    }
  }
  if (impl_->prepareFrameTarget()) {
    impl_->createFrameEncoder();
  }
}

void RendererGeode::endFrame() {
  const bool hadOpenFrame = impl_->frameGenerationOpen;
  // Flush any pending `<use>`-batch before closing out
  // the frame. Without this, the last run of batchable draws in the
  // frame would never emit.
  impl_->flushPendingBatch();

  // Before anything is submitted, so a submission failure retains these tiles along with the
  // rest of what the frame named rather than returning them to a pool.
  impl_->retireUnconsumedPatternTilesAtFrameEnd(Impl::OuterPatternState::kRestore);
  impl_->retireOpenFrameStacksAtBoundary(impl_->frameRecordingOpen &&
                                         !impl_->frameRecordingAbandoned &&
                                         !impl_->frameArenaWriteFailed);

  if (impl_->encoder) {
    // Ends the open render pass without submitting - shared-mode.
    impl_->retireActiveEncoder();
  }
  if (impl_->frameArenaWriteFailed && !impl_->frameRecordingAbandoned) {
    impl_->abandonFrameRecording();
  }

  // Replay the captured post-vertex Slug triangle edges only after every
  // normal paint, layer, filter, and mask composite. This makes the debug
  // wireframe globally topmost within the renderer's final target.
  impl_->emitGeometryDebugOverlay();
  if (impl_->encoder) {
    impl_->retireActiveEncoder();
  }

  // Hand the frame's command buffers to the queue together. After this one submit, all recorded
  // render passes (base + every pushed layer / filter / mask) execute on the GPU in program
  // order.
  if (impl_->frameRecordingOpen) {
    // That submission re-validates every resource the frame's command buffers name, so a handle
    // dropped anywhere in the frame fails here. Drop the frame, not the process: a document must
    // not be able to take the renderer down.
    const bool submitted = impl_->closeFrameGpuEncoder(false) && impl_->submitFrameCommandBuffers();
    if (!submitted) {
      impl_->device->markDeviceLost("the frame's command buffers could not be submitted");
    }
    impl_->frameRecordingOpen = false;
    impl_->frameGpuEncoders.clear();
    impl_->frameFinishedEncoders.clear();
    impl_->frameCommandBuffers.clear();
    // Every encoder that could still reach these aliases is gone, and the textures they name are
    // about to be recycled, so drop them before any of them can be handed out again.
    impl_->frameSnapshotImports.clear();
    impl_->frameSnapshotBackings.clear();
    impl_->device->drainDeferredTextureBackings();
    impl_->device->releaseRetiredHandles();
    if (!submitted) {
      // Whether any of what the frame recorded reached the queue is exactly what the failure
      // leaves unknown, so nothing it named may go back to a reusable pool.
      impl_->retainPendingFrameReleasesAfterFailure();
      impl_->frameRecordingAbandoned = true;
    }
  }
  impl_->finishRetainedCompositeFrame();

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

  // The documents the frame drew are still held, so give back what it borrowed from them now.
  impl_->releaseFrameLoans();
  impl_->drainFencedTextureBackings();

  impl_->deviceFromLocalTransform = Transform2d();
  impl_->deviceFromLocalTransformStack.clear();
  impl_->finishOffscreenFrameCounters(hadOpenFrame);
}

void RendererGeode::setTransform(const Transform2d& transform) {
  // Inside a pattern tile we're rasterising into a texture whose pixel
  // dimensions may not match the logical tile-space units. Pre-compose the
  // raster scale onto the transform so draws submitted to the tile encoder
  // map 1:1 onto the tile texture's pixel grid. (Matches
  // `RendererTinySkia::setTransform`'s `scaleTransformOutput` path.)
  //
  // Filter captures opened inside the tile move what is drawn into them, in tile pixels, as they
  // do outside a tile; captures opened before the tile hold the interrupted target instead.
  if (!impl_->patternStack.empty()) {
    const Impl::PatternStackFrame& pattern = impl_->patternStack.back();
    const Vector2d& scale = pattern.rasterScale;
    Transform2d scaled = transform;
    scaled.data[0] *= scale.x;
    scaled.data[2] *= scale.x;
    scaled.data[4] *= scale.x;
    scaled.data[1] *= scale.y;
    scaled.data[3] *= scale.y;
    scaled.data[5] *= scale.y;
    impl_->deviceFromLocalTransform =
        scaled * impl_->filterCaptureTransform(pattern.filterStackDepth);
    return;
  }
  impl_->deviceFromLocalTransform = transform * impl_->filterCaptureTransform();
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

  if (!clip.clipPaths.empty() && impl_->device && impl_->encoder && impl_->pixelWidth > 0 &&
      impl_->pixelHeight > 0) {
    if (impl_->simpleClipMissesTarget(clip)) {
      entry.knownEmpty = true;
      ++impl_->simpleClipCulls;
    } else {
      impl_->buildClipMask(clip, entry);
    }
  }

  impl_->pushClipEntry(std::move(entry));
  impl_->updateEncoderScissor();
}

void RendererGeode::popClip() {
  // Flush before popping so the batched draws stay
  // inside the clip region they were accumulated under.
  impl_->flushPendingBatch();

  if (!impl_->clipStack.empty()) {
    // Every draw under this clip is recorded, so a later clip of this frame may take its masks.
    Impl::ClipStackEntry& entry = impl_->clipStack.back();
    for (auto& release : entry.maskLayerTextures) {
      if (impl_->frameRecordingAbandoned) {
        impl_->retainTextureAfterAbandonedFrame(std::move(release.texture), release.desc);
      } else {
        impl_->releaseTextureWithinFrame(std::move(release.texture), release.desc);
      }
    }
    entry.maskLayerTextures.clear();
    impl_->clipStack.pop_back();
  }
  impl_->updateEncoderScissor();
}

void RendererGeode::pushIsolatedLayer(double opacity, MixBlendMode blendMode) {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  impl_->pushLayerBackdropRecord();

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
  // The layer takes the outer target's extent, not the surface's: the composite maps the whole
  // layer over the whole outer target, which inside a filter layer with an input margin is
  // larger than the surface.
  const gpu::TextureDescriptor td{"RendererGeodeIsolatedLayer", impl_->targetExtent(),
                                  impl_->gpuTextureFormat(),
                                  gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::Sampled |
                                      gpu::TextureUsage::CopySrc};
  gpu::Texture layerTexture = impl_->acquireTexture(td);
  if (!layerTexture.isValid()) {
    impl_->layerStack.push_back({});
    return;
  }
  const size_t compositeIndex = impl_->layerComposites.claim();

  // Finish the outer encoder so its queued draws land on the saved target
  // before we redirect subsequent work into the offscreen layer. Same
  // shape as `beginPatternTile` - two render-pass submissions ordered
  // serially on the queue.
  impl_->encoder->finish();

  Impl::LayerStackFrame frame;
  frame.savedEncoder = std::move(impl_->encoder);
  frame.savedTarget = Impl::aliasOf(impl_->target);
  frame.layerTexture = std::move(layerTexture);
  frame.layerDesc = td;
  frame.opacity = opacity;
  frame.blendMode = blendMode;
  frame.compositeIndex = compositeIndex;

  impl_->target = Impl::aliasOf(frame.layerTexture);
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->target, td.size, *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*newEncoder);
  newEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->replaceActiveEncoder(std::move(newEncoder));
  impl_->layerStack.push_back(std::move(frame));
  // The layer inherits the outer clip stack - reapply it to the new
  // encoder so scissors carry through.
  impl_->updateEncoderScissor();
}

void RendererGeode::popIsolatedLayer() {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  impl_->popLayerBackdropRecord();
  if (impl_->layerStack.empty()) {
    return;
  }
  Impl::LayerStackFrame frame = std::move(impl_->layerStack.back());
  impl_->layerStack.pop_back();

  if (!frame.layerTexture.isValid()) {
    return;  // Placeholder frame from the headless/error path.
  }
  // Finish the layer's render pass so the texture contents are ready. Closing it can fail, which
  // abandons the frame, so everything below is gated on the check after it rather than before.
  if (impl_->encoder) {
    impl_->retireActiveEncoder();
  }
  impl_->retireFinishedEncoder(std::move(frame.savedEncoder));
  if (impl_->frameRecordingAbandoned) {
    impl_->target = Impl::aliasOf(frame.savedTarget);
    impl_->replaceActiveEncoder(nullptr);
    impl_->retainTextureAfterAbandonedFrame(std::move(frame.layerTexture), frame.layerDesc);
    return;
  }

  // Restore outer target references.
  impl_->target = Impl::aliasOf(frame.savedTarget);

  if (frame.blendMode != MixBlendMode::Normal) {
    // SVG `mix-blend-mode`. The fragment shader needs the parent's current pixels as a
    // backdrop, but a render pass may not read from its own color attachment. Snapshot the
    // parent's 1-sample resolve target into a separate texture with a texture-to-texture copy,
    // then open a fresh parent encoder over the parent target; the load-op note further down
    // covers why that encoder preserves the target's contents. The snapshot is the whole parent
    // target, so the blend samples it texel for texel wherever the layer lands.
    const gpu::Extent2d backdropExtent = impl_->targetExtent();
    const gpu::TextureDescriptor snapDesc{"RendererGeodeBlendDstSnapshot", backdropExtent,
                                          impl_->gpuTextureFormat(),
                                          gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst};
    // Reading the backdrop copies out of the parent target, which an embedder-supplied one can
    // only serve if the embedder asked for that capability. A document must not be able to take
    // the renderer down by naming a blend mode, so a target that cannot be copied from falls
    // through to the unblended path below rather than recording a copy the runtime refuses.
    gpu::Texture snapshot =
        impl_->canCopyFrom(frame.savedTarget) ? impl_->acquireTexture(snapDesc) : gpu::Texture();

    if (snapshot.isValid()) {
      const gpu::Texture& savedTargetHandle = frame.savedTarget;
      const gpu::Texture& snapshotHandle = snapshot;

      // The layer's render pass ended when its encoder was retired above, so the copy can be
      // recorded outside a pass. It joins the frame's recorded stream after those draws and
      // before the composite recorded below, which is the order the backdrop has to be frozen in.
      const gpu::Status copied = impl_->frameGpuEncoder->copyTextureToTexture(
          savedTargetHandle, snapshotHandle, backdropExtent);
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
          savedTargetHandle, backdropExtent, *impl_->frameGpuEncoder);
      impl_->configurePathEncoder(*newEncoder);
      newEncoder->setLoadPreserve();
      impl_->replaceActiveEncoder(std::move(newEncoder));
      impl_->updateEncoderScissor();
      impl_->encoder->blitFullTargetBlended(
          frame.layerTexture, snapshotHandle, static_cast<uint32_t>(frame.blendMode), frame.opacity,
          impl_->layerComposites.binding(frame.compositeIndex, frame.layerTexture));
      // The blend just recorded is the layer's last reader, so a later surface of this frame may
      // take the layer. The snapshot waits for the frame's submission: its descriptor admits queue
      // uploads, which would run ahead of the recorded blend.
      impl_->releaseTextureWithinFrame(std::move(frame.layerTexture), frame.layerDesc);
      impl_->releaseTextureAtFrameEnd(std::move(snapshot), snapDesc);
      return;
    }
    // If the backdrop could not be captured fall through to the Normal path - at least the
    // layer content shows up even if unblended.
  }

  // Plain premultiplied source-over (the `Normal` case). Create a
  // fresh encoder that preserves its existing contents. Draw the layer
  // texture across the target with the stored opacity as compositing alpha.
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      frame.savedTarget, impl_->targetExtent(), *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*newEncoder);
  newEncoder->setLoadPreserve();
  impl_->replaceActiveEncoder(std::move(newEncoder));
  impl_->updateEncoderScissor();
  impl_->encoder->blitFullTarget(
      frame.layerTexture, frame.opacity,
      impl_->layerComposites.binding(frame.compositeIndex, frame.layerTexture));
  // The composite just recorded is the layer's last reader, so a later surface of this frame may
  // take the layer: a frame of many sibling layers holds one target-sized texture, not one each.
  impl_->releaseTextureWithinFrame(std::move(frame.layerTexture), frame.layerDesc);
}

void RendererGeode::pushFilterLayer(const components::FilterGraph& filterGraph,
                                    const std::optional<Box2d>& filterRegion) {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  impl_->pushLayerBackdropRecord();
  if (impl_->rejectedFilterDepth != 0) {
    impl_->pushRejectedFilterFrame();
    return;
  }
  if (!impl_->readyForFilterCapture()) {
    // A placeholder frame; its pop restores the clip stack it saves.
    Impl::FilterStackFrame placeholder;
    placeholder.savedClipStack = std::move(impl_->clipStack);
    impl_->clipStack.clear();
    impl_->filterStack.push_back(std::move(placeholder));
    return;
  }
  // The capture covers the target the result composites back onto, which is larger than the
  // surface when this filter nests inside one with an input margin.
  const gpu::Extent2d outerExtent = impl_->targetExtent();
  const int outerWidth = static_cast<int>(outerExtent.width);
  const int outerHeight = static_cast<int>(outerExtent.height);
  if (filterRegion.has_value() &&
      FilterRegionShowsNothing(*filterRegion, impl_->deviceFromLocalTransform, outerWidth,
                               outerHeight)) {
    // Nothing the layer produces can show, so neither its content nor its graph runs.
    impl_->pushRejectedFilterFrame();
    return;
  }
  std::optional<GeodeFilterAdmission> admission =
      AdmitGeodeFilter(filterGraph, filterRegion, impl_->deviceFromLocalTransform, outerWidth,
                       outerHeight, *impl_->filterExecutionBudget, *impl_->surfaceBudget,
                       *impl_->filterEngine, impl_->filterBudgetChunkAvailable());
  if (!admission.has_value() && impl_->filterExecutionBudget->executions() != 0 &&
      impl_->submitFilterBudgetChunk()) {
    admission = AdmitGeodeFilter(filterGraph, filterRegion, impl_->deviceFromLocalTransform,
                                 outerWidth, outerHeight, *impl_->filterExecutionBudget,
                                 *impl_->surfaceBudget, *impl_->filterEngine,
                                 /*mayRequestChunk=*/false);
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
  } else {
    impl_->countAdmittedFilter(*admission);
  }
}

void RendererGeode::popFilterLayer() {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  impl_->popLayerBackdropRecord();
  if (impl_->filterStack.empty()) {
    return;
  }
  Impl::FilterStackFrame frame = std::move(impl_->filterStack.back());
  impl_->filterStack.pop_back();
  // Every path below either records the backdrop's last read before returning or records none.
  // The texture returns to the pool at frame end, or is retained when the frame is abandoned.
  struct BackdropRelease {
    Impl& impl;
    Impl::FilterStackFrame& frame;
    ~BackdropRelease() {
      impl.releaseTextureAtFrameEnd(std::move(frame.backdropTexture), frame.backdropDesc);
    }
  } backdropRelease{*impl_, frame};
  impl_->clipStack = std::move(frame.savedClipStack);
  impl_->restoreFilterCaptureTransform(frame);

  if (frame.allocationRejected) {
    if (impl_->rejectedFilterDepth != 0) {
      --impl_->rejectedFilterDepth;
    }
    if (frame.savedEncoder) {
      impl_->target = Impl::aliasOf(frame.savedTarget);
      impl_->replaceActiveEncoder(std::move(frame.savedEncoder));
      impl_->updateEncoderScissor();
    }
    return;
  }

  if (!frame.layerTexture.isValid()) {
    return;  // Placeholder frame from the headless/error path.
  }
  // Finish the filter layer's render pass so the texture is ready. Closing it can fail, which
  // abandons the frame, so everything below is gated on the check after it rather than before.
  if (impl_->encoder) {
    impl_->retireActiveEncoder();
  }
  impl_->retireFinishedEncoder(std::move(frame.savedEncoder));
  if (impl_->frameRecordingAbandoned) {
    impl_->target = Impl::aliasOf(frame.savedTarget);
    impl_->replaceActiveEncoder(nullptr);
    impl_->retainTextureAfterAbandonedFrame(std::move(frame.layerTexture), frame.layerDesc);
    impl_->filterExecutionBudget->release(frame.filterReservation);
    return;
  }

  const Transform2d captureFromFilter = Impl::captureFromFilter(frame);

  const Impl::TransformedFilterResult transformed = impl_->tryCompositeTransformedFilter(frame);
  if (transformed == Impl::TransformedFilterResult::Composited) {
    impl_->filterExecutionBudget->release(frame.filterReservation);
    impl_->releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
    return;
  }
  if (transformed == Impl::TransformedFilterResult::FrameAbandoned) {
    impl_->filterExecutionBudget->release(frame.filterReservation);
    impl_->retainPendingFrameReleasesAfterFailure();
    return;
  }

  // Run the filter graph on the captured layer texture. Nothing runs it when the graph is empty,
  // in which case the capture itself is what gets composited back.
  std::optional<geode::FilterExecutionResult> execution =
      impl_->executeFilterGraph(frame, captureFromFilter);
  if (!execution.has_value()) {
    return;
  }
  geode::FilterExecutionResult filtered = std::move(*execution);
  const gpu::Texture* filteredTexture = Impl::filterOutput(filtered, frame.layerTexture);
  const gpu::TextureDescriptor& filteredDesc = Impl::filterOutputDesc(filtered, frame.layerDesc);

  impl_->beginFilterComposite(frame);
  if (filteredTexture == nullptr) {
    impl_->filterExecutionBudget->release(frame.filterReservation);
    impl_->releaseTextureAtFrameEnd(std::move(filtered.texture), filtered.desc);
    impl_->releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
    return;
  }
  impl_->drawDeviceFilterOutput(frame, *filteredTexture, filteredDesc);
  // Defer the composited texture's release to endFrame: the composite recorded a sample from it
  // into an encoder that has not reached the frame encoder yet. That is the output, or
  // `frame.layerTexture` when the graph produced none. A layer the execution consumed is read only
  // by work already recorded in order, so a later filter in this frame can take it.
  const bool compositedOutput = filtered.kind == geode::FilterExecutionResult::Kind::Output;
  impl_->releaseTextureAtFrameEnd(std::move(filtered.texture), filtered.desc);
  if (compositedOutput) {
    impl_->releaseTextureWithinFrame(std::move(frame.layerTexture), frame.layerDesc);
  } else {
    impl_->releaseTextureAtFrameEnd(std::move(frame.layerTexture), frame.layerDesc);
  }
  impl_->filterExecutionBudget->release(frame.filterReservation);
}

void RendererGeode::pushMask(const std::optional<Box2d>& maskBounds, MaskType maskType) {
  pushMask(maskBounds, maskType, ColorInterpolation::SRGB);
}

void RendererGeode::pushMask(const std::optional<Box2d>& maskBounds, MaskType maskType,
                             ColorInterpolation colorInterpolation) {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  impl_->pushLayerBackdropRecord();
  if (!impl_->device || !impl_->pipeline || !impl_->gradientPipeline || !impl_->imagePipeline ||
      !impl_->encoder || impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    // Headless / degenerate - push a placeholder so popMask stays balanced.
    impl_->maskStack.push_back({});
    return;
  }

  // The mask and its content take the outer target's extent, not the surface's: popMask maps both
  // over the whole outer target, which inside a filter layer with an input margin is larger
  // than the surface.
  const gpu::Extent2d outerExtent = impl_->targetExtent();
  const auto allocTexture = [&](const char* label, gpu::Texture& outTexture,
                                gpu::TextureDescriptor& outDesc) {
    outDesc = gpu::TextureDescriptor{RcString(label), outerExtent, impl_->gpuTextureFormat(),
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
  frame.maskColorInterpolation = colorInterpolation;

  // Flush the outer encoder's pending draws so they land before we
  // redirect subsequent commands into the mask capture.
  impl_->encoder->finish();

  frame.savedEncoder = std::move(impl_->encoder);
  frame.savedTarget = Impl::aliasOf(impl_->target);
  frame.phase = Impl::MaskStackFrame::Phase::Capturing;

  impl_->target = Impl::aliasOf(frame.maskTexture);
  auto captureEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->target, frame.maskDesc.size, *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*captureEncoder);
  captureEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->replaceActiveEncoder(std::move(captureEncoder));
  impl_->maskStack.push_back(std::move(frame));
  if (RequiresMaskRegionClip(maskBounds, impl_->deviceFromLocalTransform)) {
    // Admit the transformed region while outer clips are available: pushClip incorporates the
    // parent path-mask coverage into the prepared texture. Apply that texture only to the final
    // composite, after overlapping mask primitives and nested offscreen content have resolved.
    ResolvedClip regionClip;
    regionClip.clipPaths.push_back({.path = PathBuilder().addRect(*maskBounds).build()});
    pushClip(regionClip);
    Impl::MaskStackFrame& activeFrame = impl_->maskStack.back();
    activeFrame.hasRegionClip = impl_->clipStack.back().allocationRejected;
    activeFrame.compositeRegionClip.push_back(std::move(impl_->clipStack.back()));
    impl_->clipStack.pop_back();
  }
  impl_->suspendMaskClipsForCapture(impl_->maskStack.back());
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
  if (frame.hasRegionClip) {
    popClip();
    frame.hasRegionClip = false;
  }
  if (!frame.contentTexture.isValid() || !impl_->encoder) {
    frame.phase = Impl::MaskStackFrame::Phase::Content;
    return;
  }

  // Flush the mask-capture encoder so the mask texture is ready to
  // sample in popMask. Closing it can fail, which abandons the frame.
  impl_->retireActiveEncoder();
  if (impl_->frameRecordingAbandoned) {
    frame.phase = Impl::MaskStackFrame::Phase::Content;
    return;
  }

  impl_->target = Impl::aliasOf(frame.contentTexture);
  auto contentEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->target, frame.contentDesc.size, *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*contentEncoder);
  contentEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->replaceActiveEncoder(std::move(contentEncoder));
  frame.phase = Impl::MaskStackFrame::Phase::Content;
  impl_->updateEncoderScissor();
}

void RendererGeode::popMask() {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  impl_->popLayerBackdropRecord();
  if (impl_->maskStack.empty()) {
    return;
  }
  if (impl_->maskStack.back().hasRegionClip) {
    popClip();
    impl_->maskStack.back().hasRegionClip = false;
  }
  Impl::MaskStackFrame frame = std::move(impl_->maskStack.back());
  impl_->maskStack.pop_back();

  if (!frame.savedEncoder) {
    // Placeholder frame from the headless path - nothing to do.
    return;
  }
  // Finish the content encoder so its target is ready to sample. Closing it can fail, which
  // abandons the frame, so everything below is gated on the check after it rather than before.
  if (impl_->encoder) {
    impl_->retireActiveEncoder();
  }
  impl_->retireFinishedEncoder(std::move(frame.savedEncoder));
  impl_->releaseClipStackTexturesAtFrameEnd(impl_->clipStack);
  impl_->clipStack = std::move(frame.savedClipStack);
  if (impl_->frameRecordingAbandoned) {
    impl_->target = Impl::aliasOf(frame.savedTarget);
    impl_->replaceActiveEncoder(nullptr);
    impl_->retainTextureAfterAbandonedFrame(std::move(frame.maskTexture), frame.maskDesc);
    impl_->retainTextureAfterAbandonedFrame(std::move(frame.contentTexture), frame.contentDesc);
    impl_->releaseClipStackTexturesAtFrameEnd(frame.compositeRegionClip);
    return;
  }
  const bool hasCompositeRegionClip = !frame.compositeRegionClip.empty();
  if (hasCompositeRegionClip) {
    impl_->pushClipEntry(std::move(frame.compositeRegionClip.back()));
    frame.compositeRegionClip.clear();
  }

  // Restore the outer target and reopen a new encoder with load-preserve.
  impl_->target = Impl::aliasOf(frame.savedTarget);
  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      frame.savedTarget, impl_->targetExtent(), *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*newEncoder);
  newEncoder->setLoadPreserve();
  impl_->replaceActiveEncoder(std::move(newEncoder));
  impl_->updateEncoderScissor();

  // A path-captured region already carries edge coverage. Only the legacy axis-aligned path
  // needs a fragment-center bounds check; applying one again would discard partial coverage.
  std::optional<Box2d> pixelMaskBounds;
  if (frame.maskBounds.has_value() &&
      !RequiresMaskRegionClip(frame.maskBounds, frame.maskBoundsTransform)) {
    pixelMaskBounds = frame.maskBoundsTransform.transformBox(*frame.maskBounds);
  }

  // Composite content through the selected luminance or alpha mask onto the outer target.
  impl_->encoder->blitFullTargetMasked(
      frame.contentTexture, frame.maskTexture, frame.maskType, pixelMaskBounds,
      frame.maskColorInterpolation,
      impl_->maskComposites.binding(impl_->maskComposites.claim(), frame.contentTexture));
  if (hasCompositeRegionClip) {
    // The final blit is the only consumer; pop restores parent clipping for the next sibling.
    popClip();
  }

  // The composite just recorded is the last reader of both, so a later surface of this frame may
  // take them.
  impl_->releaseTextureWithinFrame(std::move(frame.maskTexture), frame.maskDesc);
  impl_->releaseTextureWithinFrame(std::move(frame.contentTexture), frame.contentDesc);
}

bool RendererGeode::beginPatternTile(const Box2d& tileRect, const Transform2d& targetFromPattern) {
  if (impl_->frameRecordingAbandoned || impl_->frameGpuEncoder == nullptr ||
      impl_->rejectedFilterDepth != 0 || !impl_->device || !impl_->pipeline) {
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

  // Pattern tile target sampled by the Slug fill shader when used as paint. It comes from the
  // transient pool, so a document repainting the same pattern every frame allocates once. The
  // pool hands a bucket's textures out in the order the previous frame took them, so on an
  // unchanged frame each tile draws into the texture it used before and a resident fill keeps the
  // bind group that names that texture's view.
  const gpu::TextureDescriptor tileDesc{
      "RendererGeodePatternTile",
      gpu::Extent2d{static_cast<uint32_t>(tilePixelWidth), static_cast<uint32_t>(tilePixelHeight)},
      impl_->gpuTextureFormat(),
      gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::Sampled |
          gpu::TextureUsage::CopySrc};
  gpu::Texture tileTexture = impl_->acquireTexture(tileDesc);
  if (!tileTexture.isValid()) {
    return false;
  }
  const gpu::TextureViewRef tileView = impl_->patternTileView(tileTexture);

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
  frame.savedTarget = Impl::aliasOf(impl_->target);
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
  frame.savedSpanPatternPaints = std::move(impl_->spanPatternPaints);
  impl_->patternFillPaint.reset();
  impl_->patternStrokePaint.reset();
  impl_->spanPatternPaints = Impl::SpanPatternSlots();
  frame.tileRect = tileRect;
  frame.targetFromPattern = targetFromPattern;
  frame.tileTexture = std::move(tileTexture);
  frame.tileDesc = tileDesc;
  frame.tileView = tileView;
  frame.tilePixelWidth = tilePixelWidth;
  frame.tilePixelHeight = tilePixelHeight;
  // Map pattern-tile units onto tile-texture pixels: this factor is applied
  // to every `setTransform` call while this frame is on the stack so the
  // encoder's pixelWidth/pixelHeight-based MVP renders the tile at its
  // native resolution.
  frame.rasterScale = rasterMetrics->rasterFromPatternScale;
  frame.filterStackDepth = impl_->filterStack.size();
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
  impl_->target = Impl::aliasOf(impl_->patternStack.back().tileTexture);
  impl_->deviceFromLocalTransformStack.clear();
  // Initialise the current transform to the raster scale so direct draws
  // issued before the driver's next `setTransform` still land in the
  // correct place on the tile texture.
  const Vector2d& rasterScale = impl_->patternStack.back().rasterScale;
  impl_->deviceFromLocalTransform = Transform2d::Scale(rasterScale.x, rasterScale.y);

  auto newEncoder = std::make_unique<geode::GeoEncoder>(
      *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
      impl_->patternStack.back().tileTexture, tileDesc.size, *impl_->frameGpuEncoder);
  impl_->configurePathEncoder(*newEncoder, /*collectGeometry=*/false);
  // Transparent clear so unpainted tile pixels contribute nothing.
  newEncoder->clear(css::RGBA(0, 0, 0, 0));
  impl_->replaceActiveEncoder(std::move(newEncoder));
  // Apply the (now-empty) clip stack so the fresh tile encoder has no scissor.
  impl_->updateEncoderScissor();

  // endPatternTile composes the logical pattern transform with this raster scale.
  return true;
}

void RendererGeode::endPatternTile(bool forStroke, std::optional<std::uint64_t> spanSlot) {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
  if (impl_->patternStack.empty()) {
    return;
  }

  Impl::PatternStackFrame frame = std::move(impl_->patternStack.back());
  impl_->patternStack.pop_back();
  // Filter captures nest inside tiles, so every capture the tile opened has closed by now and the
  // offsets setTransform applies inside the tile counted only those captures.
  assert(impl_->filterStack.size() == frame.filterStackDepth &&
         "A pattern tile must close every filter capture opened inside it");

  // Finish the tile's render pass so the texture contents are available
  // for sampling by subsequent draws.
  if (impl_->encoder) {
    impl_->retireActiveEncoder();
  }
  impl_->retireFinishedEncoder(std::move(frame.savedEncoder));
  // Restore outer state.
  impl_->target = Impl::aliasOf(frame.savedTarget);
  impl_->pixelWidth = frame.savedPixelWidth;
  impl_->pixelHeight = frame.savedPixelHeight;
  impl_->deviceFromLocalTransform = frame.savedDeviceFromLocalTransform;
  impl_->deviceFromLocalTransformStack = std::move(frame.savedDeviceFromLocalTransformStack);
  // Restore the outer clip stack (saved/cleared in beginPatternTile) so the
  // upcoming fill that samples this tile is scissored to the outer clip /
  // filter region again.
  impl_->clipStack = std::move(frame.savedClipStack);

  if (impl_->frameRecordingAbandoned) {
    impl_->replaceActiveEncoder(nullptr);
    impl_->restoreSavedPatternPaints(frame);
    impl_->retainTextureAfterAbandonedFrame(std::move(frame.tileTexture), frame.tileDesc);
    return;
  }

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
      frame.savedTarget.isValid()) {
    auto newEncoder = std::make_unique<geode::GeoEncoder>(
        *impl_->device, *impl_->pipeline, *impl_->gradientPipeline, *impl_->imagePipeline,
        frame.savedTarget, impl_->targetExtent(), *impl_->frameGpuEncoder);
    impl_->configurePathEncoder(*newEncoder);
    // Preserve existing target contents: the pattern subtree may have
    // submitted work on the outer target *before* the pattern tile opened
    // (via the finish() in beginPatternTile), so we must not clear it
    // again. GeoEncoder's constructor defaults to LoadOp::Clear with a
    // transparent clearValue; call setLoadPreserve() to switch it to
    // LoadOp::Load for the next render pass.
    newEncoder->setLoadPreserve();
    impl_->replaceActiveEncoder(std::move(newEncoder));
    // Re-apply the active clip stack to the freshly-created encoder.
    // The scissor state lived on the OLD encoder (finished inside
    // beginPatternTile) and doesn't carry over automatically - without
    // this call, a pattern capture triggered from inside a viewport
    // or `<use>` clip would leak the subsequent pattern fill outside
    // that clip rect. Mirrors the scissor restore in push/
    // popIsolatedLayer.
    impl_->updateEncoderScissor();
  } else {
    impl_->replaceActiveEncoder(nullptr);
  }

  impl_->promotePatternTile(frame, forStroke, spanSlot);
}

void RendererGeode::setPaint(const PaintParams& paint) {
  impl_->paint = paint;
}

void RendererGeode::drawPath(const PathShape& path, const StrokeParams& stroke) {
  // No-op mode guard (matches drawImage / drawText): with no device or no
  // active frame encoder, skip cleanly instead of dereferencing a null
  // device via getFillEncode() -> GeodeDevice::countPathEncode().
  if (!impl_->canRecordGeometry()) {
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
  // shared empty path for a null pointer. Deferred batches require canDeferGeometry(); other
  // paths are consumed before this call returns, even when they retain a persistent cache key.
  const Path& drawPathGeometry = path.pathOrEmpty();

  // Path-encode cache lookup for the fill. Null `sourceEntity` (editor
  // overlay, test-harness direct draws) returns nullptr and `GeoEncoder`
  // falls back to the inline encode path.
  const Impl::AdmittedEncode fillAdmission =
      impl_->getFillEncodeForPaint(path.sourceEntity, drawPathGeometry, path.fillRule);
  const geode::EncodedPath* fillEncoded = fillAdmission.encoded;

  // Try to append to a pending batch. Preconditions:
  //  - Source entity valid and geometry stable until the deferred draw flushes.
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
  const bool fillShapeBatchable = fillAdmission.persistent && path.canDeferGeometry() &&
                                  !impl_->patternFillPaint.has_value() &&
                                  impl_->encoder != nullptr && impl_->paint.drawFillComponent;

  // A gradient fill can batch too: the record carries its stop-ramp reference,
  // gradient transform and spread, so a run that alternates solid and gradient
  // paint collapses into one draw instead of one draw per shape plus a
  // pipeline switch at every boundary. The parameters are resolved here rather
  // than at emit time because the batcher needs them before it can decide.
  // A fill that leaves the batch draws from this same resolution, so its stops are charged and
  // converted once. Its parameters point into `gradientStopScratch`, which the next resolution
  // into that scratch overwrites, so `batchFill` is used only until this fill is drawn. Paints
  // that must outlive another resolution, such as a text run's fill and stroke, resolve into stop
  // vectors of their own instead.
  ResolvedGradientPaint batchFill;
  //
  // The state-driven reasons a gradient would be declined are cheap to test up
  // front, and testing them here keeps the ordinary emit path from re-resolving
  // a gradient the batcher just discarded. The remaining decline is a
  // same-frame repeat of one entity, which needs the residence slot to detect
  // and is rare enough not to be worth fetching one for.
  if (kEnableSceneBatching && referenceFill && fillShapeBatchable &&
      !impl_->encoder->hasActiveClipState() && !impl_->encoder->hasOpenMaskPass()) {
    impl_->resolveBatchGradient(drawPathGeometry, impl_->paint.fill, impl_->paint.fillOpacity,
                                batchFill);
  }
  const bool gradientFillBatchable = batchFill.batchable();

  const bool fillBatchable = (kEnableSceneBatching || !hasStroke) && fillShapeBatchable &&
                             (solidFill || gradientFillBatchable);

  // GPU residence: a persistent per-entity fill slot.
  geode::GeodeResidentSlot* residentFill = impl_->residentFillSlotForPaint(
      path.sourceEntity, fillAdmission, solidFill, gradientFillBatchable);

  // Gradient residence: a persistent per-entity gradient slot for
  // gradient-painted fills with a cached encode. Pattern references also
  // pass through here and simply never use the slot (the resident
  // gradient methods only run for resolved gradients).
  geode::GeodeResidentGradientSlot* residentGradientFill =
      (fillAdmission.persistent && referenceFill)
          ? impl_->residentGradientFillSlot(path.sourceEntity, fillAdmission.variant)
          : nullptr;

  bool fillAbsorbed = false;
  if (fillBatchable) {
    geode::GeoEncoder::ScenePaint scenePaint;
    if (solidFill) {
      const auto& solid = std::get<PaintServer::Solid>(impl_->paint.fill);
      scenePaint.color = solid.color.resolve(impl_->paint.currentColor,
                                             static_cast<float>(impl_->paint.fillOpacity));
    } else {
      scenePaint.linearGradient = batchFill.linearParams();
      scenePaint.radialGradient = batchFill.radialParams();
    }
    fillAbsorbed = impl_->tryAppendOrStartBatch(
        path.sourceEntity.registry(), path.sourceEntity.entity(), drawPathGeometry, scenePaint,
        path.fillRule, fillEncoded, residentFill, /*allowInstancedAppend=*/!hasStroke);
  }
  if (fillAbsorbed) {
    // A batched fill paints from its paint block, so a gradient ramp its entity's gradient slot
    // still holds from an earlier frame is no longer bound.
    impl_->releaseGradientRamp(residentGradientFill);
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
                          residentGradientFill, &batchFill);
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
  // All stroke pieces use NonZero and are composited together once, including
  // overlapping contours, caps, and dashes.
  //
  // The cache (`GeodePathCacheComponent::strokeVariants`) memoizes the `strokeToFill` output, its
  // encode, and the derived fill rule per stroke key. A cache hit skips all three computations.
  const StrokeStyle strokeStyle = toStrokeStyle(stroke);
  const Impl::StrokeDerived strokeDerived =
      impl_->getStrokeDerived(path.sourceEntity, drawPathGeometry, strokeStyle, path.hostFromLocal);
  if (!strokeDerived.strokedPath) {
    return;
  }
  const Path& strokedOutline = *strokeDerived.strokedPath;

  // Pattern dispatch comes first: a stroke pattern slot was populated by the
  // driver via `endPatternTile(forStroke=true)` and consumed here.
  if (impl_->patternStrokePaint.has_value()) {
    impl_->emitPatternStroke(path.sourceEntity, strokeDerived);
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
          ? impl_->residentStrokeSlot(path.sourceEntity, strokeDerived.variant)
          : nullptr;
  // Gradient residence for gradient-painted strokes: the cached
  // stroke-outline encode lives in a gradient slot so an unchanged outline
  // with an unchanged resolved gradient re-uploads zero geometry.
  geode::GeodeResidentGradientSlot* residentGradientStroke =
      (strokeDerived.persistent &&
       std::holds_alternative<components::PaintResolvedReference>(strokeServer))
          ? impl_->residentGradientStrokeSlot(path.sourceEntity, strokeDerived.variant)
          : nullptr;

  // A solid stroke is a solid fill of the cached stroke encode, so it is
  // offered to the batcher on the same terms as the fill above. Landing here
  // right after this element's own fill is what lets a run of stroked
  // elements collapse: the outline becomes the next instance instead of
  // ending the run.
  //
  // The stroked outline is retained by the batch until it flushes. That is
  // safe only when the caller permits deferral: an occurrence may replace this entity's
  // GeodePathCacheComponent::strokeVariants before another draw or the frame's eventual flush.
  if (kEnableSceneBatching && residentStroke != nullptr && strokeDerived.persistent &&
      path.canDeferGeometry() && !impl_->patternStrokePaint.has_value()) {
    const auto& solidStroke = std::get<PaintServer::Solid>(strokeServer);
    geode::GeoEncoder::ScenePaint strokePaint;
    strokePaint.color =
        solidStroke.color.resolve(impl_->paint.currentColor, static_cast<float>(effectiveOpacity));
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
  impl_->syncTransform();
  ImageRendering imageRendering = params.imageRendering;
  if (imageRendering == ImageRendering::Auto && params.imageRenderingPixelated) {
    imageRendering = ImageRendering::Pixelated;
  }
  if (!impl_->encoder->acceptsImage(image, params.targetRect)) {
    return;
  }
  if (const gpu::Texture* texture = impl_->residentImageTexture(params.sourceEntity, image)) {
    impl_->encoder->drawImageTexture(
        *texture, Vector2i(image.width, image.height), params.targetRect, 1.0, imageRendering,
        impl_->imageBlits.binding(impl_->imageBlits.claim(), *texture));
    return;
  }
  impl_->encoder->drawImage(image, params.targetRect, 1.0, imageRendering);
}

bool RendererGeodeTextureSnapshot::canSampleWith(const geode::GeodeDevice& device) const {
  if (!isValid()) {
    return false;
  }
  const gpu::Texture* runtime = runtimeTexture();
  if (runtime->deviceId() == device.runtimeDevice().deviceId()) {
    return SnapshotTextureIsSampleable(device.runtimeDevice(), *runtime);
  }
  // Reaching another context means registering this texture there, which needs it to still be
  // alive when that registration is made. A frame-local borrow gives no such promise - the
  // producer takes its target back at the frame boundary - so only an owning lease may cross. The
  // export describes the texture as the producer does, so the producer's tables are not read here.
  return backing_ != nullptr && backing_->exported.isValid() &&
         IsSnapshotFormat(backing_->exported.descriptor().format) &&
         gpu::HasAllFlags(backing_->exported.descriptor().usage, gpu::TextureUsage::Sampled);
}

bool RendererGeode::drawTextureSnapshot(const RendererTextureSnapshot& texture,
                                        const Box2d& targetRect, double opacity, bool pixelated) {
  if (!impl_->encoder || !impl_->device || opacity <= 0.0) {
    return false;
  }

  if (texture.backend() != RendererTextureSnapshotBackend::Geode) {
    return false;
  }
  const auto* geodeTexture = static_cast<const RendererGeodeTextureSnapshot*>(&texture);
  if (!geodeTexture->canSampleWith(*impl_->device)) {
    return false;
  }

  // A snapshot may report a content extent smaller than its backing texture (host uploads
  // keep an oversized allocation so a resized payload can reuse the texture). Sample only
  // that region, otherwise the unused remainder is squeezed into `targetRect` and every
  // tile edge lands in the wrong place.
  const Vector2i contentDimensions = geodeTexture->dimensions();
  const uint32_t textureWidth = static_cast<uint32_t>(geodeTexture->allocationDimensions().x);
  const uint32_t textureHeight = static_cast<uint32_t>(geodeTexture->allocationDimensions().y);
  const Box2d sourceUv(Vector2d::Zero(),
                       Vector2d(std::min(1.0, static_cast<double>(contentDimensions.x) /
                                                  static_cast<double>(textureWidth)),
                                std::min(1.0, static_cast<double>(contentDimensions.y) /
                                                  static_cast<double>(textureHeight))));

  impl_->flushPendingBatch();
  impl_->syncTransform();
  if (geodeTexture->backing_) {
    impl_->frameSnapshotBackings.insert(geodeTexture->backing_);
  }
  // `canSampleWith` admitted this snapshot, which means it names a texture.
  const gpu::Texture* source = geodeTexture->runtimeTexture();
  UTILS_RELEASE_ASSERT(source != nullptr);
  if (source->deviceId() != impl_->device->runtimeDevice().deviceId()) {
    source = impl_->registerFrameSnapshot(geodeTexture->backing_.get(),
                                          geodeTexture->backing_->exported);
    if (source == nullptr) {
      return false;
    }
  }
  impl_->encoder->drawTexture(*source, targetRect, sourceUv, opacity, pixelated,
                              geodeTexture->alphaType() == AlphaType::Premultiplied);
  return true;
}

namespace {

struct GlyphGeometryModes {
  bool warped;
  bool residentFill;
  bool placedPaths;
};

#ifdef DONNER_TEXT_ENABLED
/// Whether a run has no fill, stroke or decoration to paint, which leaves only its bitmap glyphs.
/// @param hasFillPaint Whether the run fills with a solid, gradient or pattern.
/// @param hasStrokePaint Whether the run strokes its glyphs.
/// @param decoration The run's decoration lines.
bool RunPaintsOnlyBitmapGlyphs(bool hasFillPaint, bool hasStrokePaint, TextDecoration decoration) {
  return !hasFillPaint && !hasStrokePaint && decoration == TextDecoration::None;
}
#endif  // DONNER_TEXT_ENABLED

GlyphGeometryModes ResolveGlyphGeometryModes(bool solidFill, bool gradientFill, bool patternFill,
                                             bool strokePaint, bool warped) {
  return {warped, solidFill && !gradientFill && !patternFill && !warped,
          gradientFill || patternFill || strokePaint || (solidFill && warped)};
}

#ifdef DONNER_TEXT_ENABLED
/// The solid color the glyphs of run \p runIndex of \p text fill with. A run whose span does not
/// own its fill takes \p textFill, the text element's solid fill (already scaled by its fill
/// opacity); a span whose own fill is none is not filled. A span's own fill is resolved at the
/// product of its `fill-opacity` and `opacity` by \ref TextSolidPaint, with \p patternStaged
/// saying whether the driver staged a fill pattern tile for the run. A pattern with no solid color
/// paints none: its staged tile paints it, and with no tile and no fallback color it paints
/// nothing. Any other fill with no solid color takes \p textFill, since its server is drawn
/// separately.
css::RGBA SpanFillColor(const components::ComputedTextComponent& text, std::size_t runIndex,
                        const css::RGBA& textFill, const css::RGBA& currentColor,
                        bool patternStaged) {
  if (runIndex >= text.spans.size() || !text.spans[runIndex].ownsFill()) {
    return textFill;
  }
  const auto& span = text.spans[runIndex];
  if (std::holds_alternative<PaintServer::None>(span.resolvedFill)) {
    return css::RGBA(0, 0, 0, 0);
  }
  return TextSolidPaint(span.resolvedFill, span.fillOpacity * span.opacity, currentColor,
                        patternStaged)
      .value_or(paintReferenceIsPattern(span.resolvedFill) ? css::RGBA(0, 0, 0, 0) : textFill);
}
#endif  // DONNER_TEXT_ENABLED

}  // namespace

void RendererGeode::drawText(Registry& registry, const components::ComputedTextComponent& text,
                             const TextParams& params) {
  impl_->flushPendingBatch();  // Flush any pending `<use>` batch.
#ifdef DONNER_TEXT_ENABLED
  // The element-level pattern fill and stroke slots and the span pattern slots are consumed exactly
  // once, on every way out of this draw, so a staged pattern cannot leak onto the next shape's
  // draw.
  struct PatternSlotRelease {
    Impl& impl;
    ~PatternSlotRelease() {
      impl.releasePatternPaintSlot(impl.patternFillPaint);
      impl.releasePatternPaintSlot(impl.patternStrokePaint);
      impl.releaseSpanPatternPaints(impl.spanPatternPaints);
    }
  } patternSlotRelease{*impl_};
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

  TextDrawGeometry drawGeometry = PrepareTextDrawGeometry(registry, text, params, textEngine);
  std::vector<TextRun>& runs = drawGeometry.runs;
  const Box2d& textBounds = drawGeometry.elementBounds;

  impl_->admitTextRuns(registry, runs);

  const Path textBoundsPath =
      textBounds.isEmpty() ? Path() : PathBuilder().addRect(textBounds).build();

  // Element-level pattern fill: the driver stages a `patternFillPaint` slot
  // (via renderPattern / endPatternTile) for a `<text fill="url(#pattern)">`,
  // which fills the glyph outlines with the pattern as `RendererTinySkia::drawText`
  // does; `patternSlotRelease` consumes it. A span's own pattern is staged in a span
  // pattern slot instead (see `Impl::runPatternTile`).
  const bool hasPatternFill = impl_->patternFillPaint.has_value();
  const bool hasPatternStroke = impl_->patternStrokePaint.has_value();
  const css::RGBA currentColor = impl_->paint.currentColor;
  Impl::TextElementGradients elementGradients;
  impl_->resolveTextElementGradients(textBounds, params.strokeParams.strokeWidth > 0.0,
                                     hasPatternFill, elementGradients);

  // Resolve a default fill colour from the text-element-level paint
  // state. Per-span fills override below when present.
  std::optional<css::RGBA> defaultFill =
      TextSolidPaint(impl_->paint.fill, impl_->paint.fillOpacity, currentColor, hasPatternFill);
  if (!defaultFill.has_value()) {
    // No solid fill and no fallback -- text is effectively invisible.
    // Still walk through in case a per-span fill kicks in.
    defaultFill = css::RGBA(0, 0, 0, 0);
  }

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
  // The element's painted shapes outside the glyph cache's solid fills, kept across frames.
  Impl::TextShapeCursor textShapes = impl_->beginTextShapes(registry, params.textRootEntity);

  // Each draw spends at most the per-draw geometry allowance, and only what the frame has left.
  std::size_t decorationWorkRemaining = std::min(
      Path::kMaximumGeometryQueryWork, impl_->textMaterializationBudget->remainingDecorationWork());
  for (size_t runIndex = 0; runIndex < runs.size(); ++runIndex) {
    const auto& run = runs[runIndex];
    if (run.font == FontHandle()) {
      continue;
    }

    const float spanFontSizePx = run.usedFontSizePx;

    const float scale = textEngine.scaleForPixelHeight(run.font, spanFontSizePx);
    const bool runBitmapOnly = textEngine.isBitmapOnly(run.font);
    if (!CanRenderTextRun(runBitmapOnly, scale)) {
      continue;
    }

    // The staged tiles this run's pattern fill and stroke sample: its span's own pattern, or the
    // text element's.
    const Impl::PatternPaintSlot* fillTile =
        impl_->runPatternTile(text, runIndex, /*forStroke=*/false);
    const Impl::PatternPaintSlot* strokeTile =
        impl_->runPatternTile(text, runIndex, /*forStroke=*/true);
    const css::RGBA spanFill =
        SpanFillColor(text, runIndex, *defaultFill, currentColor, fillTile != nullptr);
    const bool hasFill = spanFill.a != 0;

    // Effective per-run fill paint *server*: a span's own computed fill overrides
    // the element-level fill. This carries gradient refs that the solid
    // `SpanFillColor` collapses (it returns the inherited default for a ref) -
    // `drawPaintedPathAgainst` resolves them against the text bbox below (mirrors
    // `RendererTinySkia::drawText`). Patterns are NOT routed here: the driver
    // stages a `patternFillPaint` slot for a pattern fill, so `hasPatternFill`
    // distinguishes pattern from gradient (both are `PaintResolvedReference`).
    // A span's own computed fill overrides the element-level fill. Track whether
    // the chosen server came from the span (an override) vs. was inherited from
    // the element: a span-level gradient override must beat the element pattern,
    // which the driver only staged for the element. Mirrors
    // `RendererTinySkia::drawText`, where a span ref that instantiates as a
    // gradient wins unconditionally and only falls to `patternFillPaint_` when
    // it is not a gradient.
    const bool spanOverridesFill = runIndex < text.spans.size() && text.spans[runIndex].ownsFill();
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

    // A run paints a pattern fill only when its fill server is a paint server reference that is not
    // a gradient, from the tile staged for it: its span's own pattern, or the text element's. A
    // span whose own fill is a solid paints that solid, so a fully transparent one
    // (`fill-opacity="0"`, `transparent`) paints nothing rather than the element's pattern.
    // `!fillIsGradient` ensures a span's own gradient is drawn as a gradient (above) rather than a
    // pattern.
    const bool usePatternFill =
        fillTile != nullptr && !hasFill && !fillIsGradient &&
        std::holds_alternative<components::PaintResolvedReference>(fillServer);

    // Resolve per-run stroke. A span's computed stroke decides how its glyphs are stroked: the
    // span's computed style already inherits the text element's stroke properties where the span
    // sets none of its own, so a span whose stroke computes to `none`, or whose stroke-width
    // computes to zero, is not stroked even when the text element is. Solid is taken directly;
    // gradient/pattern strokes route through the server below. Mirrors
    // `RendererTinySkia::drawText`.
    std::optional<css::RGBA> strokeColor;
    std::optional<StrokeStyle> strokeStyle;
    const components::ResolvedPaintServer* strokeServer = &impl_->paint.stroke;
    double strokeOpacity = impl_->paint.strokeOpacity;
    bool spanOverridesStroke = false;
    {
      StrokeParams runStrokeParams = params.strokeParams;
      std::optional<css::RGBA> color = TextSolidPaint(
          impl_->paint.stroke, impl_->paint.strokeOpacity, currentColor, hasPatternStroke);
      bool runHasStroke = true;
      if (runIndex < text.spans.size()) {
        const auto& span = text.spans[runIndex];
        runStrokeParams.strokeWidth = span.strokeWidth;
        runStrokeParams.lineCap = span.strokeLinecap;
        runStrokeParams.lineJoin = span.strokeLinejoin;
        runStrokeParams.miterLimit = span.strokeMiterLimit;
        const std::span<const double> spanDash = span.strokeDashLengths();
        runStrokeParams.dashArray.assign(spanDash.begin(), spanDash.end());
        runStrokeParams.dashOffset = span.strokeDashOffset;
        runHasStroke = !std::holds_alternative<PaintServer::None>(span.resolvedStroke);
        if (runHasStroke) {
          spanOverridesStroke = true;
          strokeServer = &span.resolvedStroke;
          strokeOpacity = span.strokeOpacity * span.opacity;
          color = TextSolidPaint(span.resolvedStroke, span.strokeOpacity * span.opacity,
                                 currentColor, strokeTile != nullptr);
        }
      }
      if (runHasStroke && runStrokeParams.strokeWidth > 0.0) {
        // Solid stroke colour available → draw flat. Otherwise, a gradient/
        // pattern stroke server drives the paint below.
        if (color.has_value() && color->a != 0) {
          strokeColor = color;
        }
        // Only a solid with coverage or a paint server reference strokes the run. The element's
        // staged pattern stroke is not a stroke of its own: a span whose stroke is a fully
        // transparent solid (`stroke-opacity="0"`, `transparent`) paints nothing rather than the
        // element's pattern.
        const bool strokeIsServer =
            std::holds_alternative<components::PaintResolvedReference>(*strokeServer);
        if (strokeColor.has_value() || strokeIsServer) {
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
    // Paint a pattern stroke from the run's staged tile only when its stroke server is a paint
    // server reference (`strokeStyle` without `strokeColor` implies one) that is not a gradient: a
    // span's own gradient is drawn above.
    const bool usePatternStroke = strokeStyle.has_value() && !strokeColor.has_value() &&
                                  !strokeIsGradient && strokeTile != nullptr;
    const bool hasStrokePaint = strokeColor.has_value() || strokeIsGradient || usePatternStroke;

    // The run's gradient fill and stroke are each resolved at most once, fill first as on the CPU
    // renderer, before the run is skipped for having nothing to paint, and every glyph of the run
    // draws from them.
    std::vector<geode::LinearGradientParams::Stop> runFillStops;
    std::vector<geode::LinearGradientParams::Stop> runStrokeStops;
    ResolvedGradientPaint runFillResolution;
    ResolvedGradientPaint runStrokeResolution;
    const ResolvedGradientPaint* runFillGradient =
        impl_->runGradientPaint(true, spanOverridesFill, fillServer, textBounds, fillOpacity,
                                runFillStops, runFillResolution, elementGradients.fill);
    const ResolvedGradientPaint* runStrokeGradient = impl_->runGradientPaint(
        strokeStyle.has_value(), spanOverridesStroke, *strokeServer, textBounds, strokeOpacity,
        runStrokeStops, runStrokeResolution, elementGradients.stroke);

    const TextDecoration spanDecoration =
        runIndex < text.spans.size() ? text.spans[runIndex].textDecoration : params.textDecoration;
    if (RunPaintsOnlyBitmapGlyphs(hasFill || usePatternFill || fillIsGradient, hasStrokePaint,
                                  spanDecoration)) {
      // Bitmap glyphs draw without fill or stroke paint, as on the CPU renderer.
      if (!impl_->drawBitmapGlyphs(textEngine, text, runIndex, run, scale, runBitmapOnly)) {
        return;
      }
      continue;
    }

    // A solid fill draws each glyph from shared resident geometry: the outline
    // is cached once per glyph identity and the occurrence contributes only a
    // placement transform. Gradient, pattern, and stroke paint still consume a
    // glyph outline placed in the element's local space, so those keep
    // building placed paths.
    const GlyphGeometryModes glyphModes =
        ResolveGlyphGeometryModes(hasFill, fillIsGradient, usePatternFill, hasStrokePaint,
                                  static_cast<bool>(run.stretchedTextPath));

    // Glyph occurrences of this run, in paint order. Parallel to
    // `runGlyphPaths` when both are populated.
    struct RunGlyphOccurrence {
      geode::GeodeGlyphResidentEntry* entry = nullptr;
      Transform2d glyphFromLocal;
    };
    std::vector<RunGlyphOccurrence> runGlyphOccurrences;
    std::vector<Path> runGlyphPaths;
    TextChunkRanges runChunks;
    if (glyphModes.residentFill || glyphModes.placedPaths) {
      runGlyphOccurrences.reserve(run.glyphs.size());
    }
    if (glyphModes.placedPaths) {
      runGlyphPaths.reserve(run.glyphs.size());
    }
    // A run can reach here with no glyph paint at all - decoration lines only -
    // and must not then fetch, encode, and permanently cache an outline per
    // glyph for geometry nothing draws.
    const bool runNeedsGlyphGeometry = glyphModes.residentFill || glyphModes.placedPaths;
    for (const auto& glyph : runNeedsGlyphGeometry ? std::span<const TextGlyph>(run.glyphs)
                                                   : std::span<const TextGlyph>()) {
      runChunks.noteGlyph(glyph, runGlyphPaths.size());
      const GlyphFaceScale face = ResolveGlyphFace(textEngine, run, glyph, scale, runBitmapOnly);
      if (!HasVectorGlyph(glyph, face)) {
        // `.notdef` is skipped to match tiny-skia. A bitmap-only face has no outline budget, so
        // its glyphs are skipped before glyph admission; `drawBitmapGlyphs` draws them.
        continue;
      }

      if (glyphModes.warped) {
        if (!impl_->appendWarpedGlyph(
                runGlyphPaths, fontManager, textEngine, run, glyph, face.scale,
                geode::FillCubicToleranceFor(impl_->deviceFromLocalTransform))) {
          return;
        }
        continue;
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
      key.fontId = static_cast<uint64_t>(static_cast<uint32_t>(face.font.entity()));
      key.glyphIndex = static_cast<uint32_t>(glyph.glyphIndex);
      key.outlineScale = face.scale * glyph.fontSizeScale;
      key.stretchScaleX = glyph.stretchScaleX;
      key.stretchScaleY = glyph.stretchScaleY;
      key.rotateDegrees = glyph.rotateDegrees;
      key.fillTolerance = geode::FillCubicToleranceFor(impl_->deviceFromLocalTransform);
      key.allowSyntheticOblique = glyph.allowSyntheticOblique;

      geode::GeodeGlyphResidentEntry* entry = impl_->residentGlyphEntry(
          registry, fontManager, face.font, glyph.glyphIndex, key, [&]() -> Path {
            return UnplacedGlyphOutline(textEngine, face.font, glyph, face.scale).outline;
          });
      if (entry == nullptr || entry->outline.empty()) {
        continue;  // No vector outline.
      }

      const Transform2d glyphFromLocal = GlyphPlacementTransform(glyph);
      runGlyphOccurrences.push_back(RunGlyphOccurrence{entry, glyphFromLocal});
      if (glyphModes.placedPaths) {
        runGlyphPaths.push_back(impl_->materializePlacedGlyph(entry->outline, glyphFromLocal));
      }
    }

    const auto drawRunFill = [&]() {
      if (glyphModes.residentFill) {
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
          const Impl::TextShape shape = impl_->textShape(textShapes, placed);
          // Gradient fill on text: resolve the gradient against the text bbox
          // (the gradient *geometry*), fill the glyph outline (the *draw* path).
          impl_->drawPaintedPathAgainst(
              textBoundsPath, placed, fillServer, fillOpacity, FillRule::NonZero, shape.encoded,
              shape.slot, Impl::TextShapeGradientSlot(shape, runFillGradient), runFillGradient);
        } else if (usePatternFill) {
          const Impl::TextShape shape = impl_->textShape(textShapes, placed);
          impl_->releaseGradientRamp(shape.gradientSlot);
          // Fill the glyph outline with the staged pattern tile, same as a
          // pattern-filled `<path>` (see Impl::fillResolved). The glyph path is
          // already in the element's local space and `deviceFromLocalTransform`
          // is set above, so `buildPatternPaint` composes the right sample space.
          impl_->syncTransform();
          impl_->emitPatternFill(placed, FillRule::NonZero,
                                 impl_->buildPatternPaint(*fillTile, fillOpacity), shape.encoded,
                                 shape.slot);
        } else if (hasFill) {
          const Impl::TextShape shape = impl_->textShape(textShapes, placed);
          impl_->releaseGradientRamp(shape.gradientSlot);
          impl_->emitSolidFill(placed, spanFill, FillRule::NonZero, shape.encoded, shape.slot);
        }
      }
    };
    // Paints one stroke shape: a joined stroke outline, its encoding and its residence.
    const auto drawStrokeShape = [&](const Path& stroked, const Impl::TextShape& shape) {
      const FillRule strokeRule = FillRule::NonZero;
      if (strokeIsGradient) {
        // Gradient stroke: resolve against the text bbox (the *original*
        // geometry, per SVG), fill the stroked outline.
        impl_->drawPaintedPathAgainst(
            textBoundsPath, stroked, *strokeServer, strokeOpacity, strokeRule, shape.encoded,
            shape.slot, Impl::TextShapeGradientSlot(shape, runStrokeGradient), runStrokeGradient);
        return;
      }
      impl_->releaseGradientRamp(shape.gradientSlot);
      if (usePatternStroke) {
        impl_->syncTransform();
        impl_->emitPatternFill(stroked, strokeRule,
                               impl_->buildPatternPaint(*strokeTile, strokeOpacity), shape.encoded,
                               shape.slot);
      } else {
        impl_->emitSolidFill(stroked, *strokeColor, strokeRule, shape.encoded, shape.slot);
      }
    };
    const auto drawRunStroke = [&]() {
      if (!hasStrokePaint) {
        return;
      }
      // Each text chunk of the run is stroked as one shape, as resvg strokes the part of a span in
      // one chunk: the stroke pieces share one winding and one draw, so where the strokes of
      // adjacent glyphs overlap, stroke opacity applies once. Each glyph's stroke is expanded on
      // its own (stroking a path strokes its contours independently), with the same device-aware
      // tolerance as `drawPath`'s stroke: glyph outlines are submitted in text-local space and
      // scaled on the GPU, so the flattening must track the draw transform or stroked glyphs facet
      // at high zoom.
      const std::span<const Path> paths(runGlyphPaths);
      for (const auto& [begin, end] : runChunks.ranges(paths.size())) {
        const std::vector<Path> outlines =
            impl_->strokeGlyphOutlines(paths.subspan(begin, end - begin), *strokeStyle);
        impl_->drawTextStrokeOutlines(outlines, textShapes, drawStrokeShape);
        impl_->releaseTextStrokeOutlines(outlines);
      }
    };

    // Text-decoration lines (underline / overline / line-through). Mirrors
    // `RendererTinySkia::drawText`. Per CSS Text Decoration 3 the decoration uses the
    // declaring element's paint + font metrics, and underlines and overlines
    // paint beneath the run's glyphs while line-throughs paint above them.
    const auto drawDecorations = [&](TextDecoration emittedLines) {
      if (spanDecoration == TextDecoration::None || run.glyphs.empty() ||
          runIndex >= text.spans.size()) {
        return;
      }
      const auto& span = text.spans[runIndex];
      components::TextDecorationOrigin fallback;
      fallback.lines = spanDecoration;
      const auto origins = span.decorations
                               ? span.decorations->origins()
                               : std::span<const components::TextDecorationOrigin>(&fallback, 1);
      const Impl::TextRunFillPaint runFill{spanFill, fillIsGradient ? &fillServer : nullptr,
                                           usePatternFill, fillOpacity};
      // Origins with no line in this pass are skipped before their paint is resolved.
      ForEachEmittingOrigin(
          origins, emittedLines, [&](const components::TextDecorationOrigin& decoration) {
            const Impl::TextDecorationPaint decorationPaint = impl_->resolveTextDecorationPaint(
                decoration, span.opacity, span.decorations ? nullptr : &runFill);
            const std::size_t workBefore = decorationWorkRemaining;
            EmitTextDecorationPaths(
                run, decoration, emittedLines, origins.size(), spanFontSizePx, textEngine,
                impl_->deviceFromLocalTransform, decorationWorkRemaining, [&](const Path& path) {
                  impl_->paintTextDecorationPath(path, textBoundsPath, decorationPaint, textShapes);
                });
            impl_->textMaterializationBudget->consumeDecorationWork(workBefore -
                                                                    decorationWorkRemaining);
          });
    };

    drawDecorations(kDecorationLinesBeneathText);

    // Bitmap glyphs paint after the decorations beneath the run and before its outlines, as in
    // `RendererTinySkia::drawText`.
    if (!impl_->drawBitmapGlyphs(textEngine, text, runIndex, run, scale, runBitmapOnly)) {
      return;
    }

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

    drawDecorations(TextDecoration::LineThrough);
  }
  impl_->endTextShapes(textShapes);

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
  renderer->impl_->inheritCounterOwner(*impl_);
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
  renderer->impl_->drawBudget = impl_->drawBudget;
  renderer->impl_->ownsDrawBudget = false;
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
  if (impl_->frameRecordingAbandoned || impl_->frameArenaWriteFailed || !impl_->device ||
      !impl_->ownedTarget.isValid() || impl_->hostTarget.isValid() || impl_->pixelWidth <= 0 ||
      impl_->pixelHeight <= 0) {
    return nullptr;
  }

  const Vector2i dimensions(impl_->pixelWidth, impl_->pixelHeight);
  auto snapshot = RendererGeodeTextureSnapshot::AdoptRuntimeTexture(
      impl_->device, std::move(impl_->ownedTarget), dimensions, impl_->textureFormat,
      AlphaType::Premultiplied);
  if (!snapshot.isValid()) {
    return nullptr;
  }
  impl_->target = gpu::Texture();
  impl_->targetWidth = 0;
  impl_->targetHeight = 0;
  return std::make_shared<RendererGeodeTextureSnapshot>(std::move(snapshot));
}

const RendererTextureSnapshot* RendererGeode::borrowTextureSnapshot() {
  if (impl_->frameRecordingAbandoned || impl_->frameArenaWriteFailed || !impl_->device ||
      !impl_->ownedTarget.isValid() || impl_->hostTarget.isValid() || impl_->pixelWidth <= 0 ||
      impl_->pixelHeight <= 0) {
    impl_->borrowedTargetSnapshot.reset();
    return nullptr;
  }

  impl_->borrowedTargetSnapshot.emplace(RendererGeodeTextureSnapshot::BorrowCurrentFrame(
      impl_->device->runtimeDevice(), impl_->ownedTarget,
      Vector2i(impl_->pixelWidth, impl_->pixelHeight)));
  if (!impl_->borrowedTargetSnapshot->isValid()) {
    impl_->borrowedTargetSnapshot.reset();
    return nullptr;
  }
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
  /// The total capture deadline expired; any pending map was cancelled and its buffer retired.
  /// Contention may consume that budget, so this alone does not declare device loss.
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
ReadbackMapResult MapAndWaitReadback(geode::GeodeDevice& device, gpu::Buffer& buffer,
                                     uint64_t mapSize, const std::function<bool()>& shouldCancel,
                                     std::chrono::steady_clock::time_point deadline,
                                     const std::function<void()>& mapRequested) {
  if (device.isDeviceLost()) {
    // A lost device will never deliver the map. Fail fast so a caller does
    // not spend another full readback deadline against a hung driver; the
    // caller drops the buffer without pooling it.
    return ReadbackMapResult{ReadbackMapStatus::DeviceLost, {}};
  }

  if (std::chrono::steady_clock::now() >= deadline) {
    return ReadbackMapResult{ReadbackMapStatus::TimedOut, {}};
  }

  gpu::Device& runtime = device.runtimeDevice();
  gpu::Result<gpu::BufferMapping> mapping =
      runtime.mapBufferAsync(buffer, gpu::MapMode::Read, 0, mapSize);
  if (mapping.hasError()) {
    // The request never started, so no slice ran and there are no poll statistics to record. A
    // device lost between the check above and here is reported as such rather than as a plain
    // failure, because the caller retries a failure and must not retry a lost device.
    return ReadbackMapResult{
        device.isDeviceLost() ? ReadbackMapStatus::DeviceLost : ReadbackMapStatus::Failed, {}};
  }
  gpu::BufferMapping liveMapping = std::move(mapping).result();
  if (mapRequested) {
    mapRequested();
  }

  int pollIter = 0;
  bool cancelled = false;
  bool timedOut = false;
  gpu::MapWaitOutcome outcome = gpu::MapWaitOutcome::TimedOut;
  // Sticky across the loop: one slice that waited on the map's completion event is what says this
  // readback was not reduced to polling, and a later polled slice does not take that back.
  bool usedCompletionEvent = false;
  const auto readbackWaitStart = std::chrono::steady_clock::now();
  while (true) {
    if (shouldCancel && shouldCancel()) {
      cancelled = true;
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      timedOut = true;
      break;
    }
    const double remainingSeconds =
        std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
    if (remainingSeconds <= 0.0) {
      timedOut = true;
      break;
    }
    const double sliceSeconds = std::min(kReadbackWaitSliceSeconds, remainingSeconds);
    // Counted after the two checks above, so a wait that was cancelled or already past its
    // deadline reports zero slices - the signal a caller uses to tell "gave up immediately"
    // from "waited and then gave up".
    ++pollIter;

    const gpu::Result<gpu::MapWaitReport> slice =
        runtime.waitForMapping(liveMapping, gpu::MapWaitParams{sliceSeconds, sliceSeconds},
                               /*shouldCancel=*/{});
    if (slice.hasError()) {
      outcome = gpu::MapWaitOutcome::Failed;
      break;
    }
    usedCompletionEvent =
        usedCompletionEvent || slice.result().waitKind == gpu::MapWaitKind::CompletionEvent;
    outcome = slice.result().outcome;
    // A slice budget equal to one slice reports TimedOut for "not ready yet"; every other
    // outcome is terminal.
    if (outcome != gpu::MapWaitOutcome::TimedOut) {
      break;
    }
  }

  device.recordReadback(usedCompletionEvent, pollIter);

  if (cancelled || timedOut) {
    if (timedOut) {
      std::fprintf(
          stderr, "[Geode] snapshot map reached the capture deadline after %.3f seconds waiting\n",
          std::chrono::duration<double>(std::chrono::steady_clock::now() - readbackWaitStart)
              .count());
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

/// Convert one mapped row to tightly packed straight-alpha RGBA.
void CopyReadbackRow(uint8_t* destination, const uint8_t* source, uint32_t width, bool sourceIsBgra,
                     AlphaType alphaType) {
  if (!sourceIsBgra && alphaType == AlphaType::Unpremultiplied) {
    std::memcpy(destination, source, static_cast<size_t>(width) * 4u);
    return;
  }
  for (uint32_t x = 0; x < width; ++x) {
    const uint8_t red = source[x * 4 + (sourceIsBgra ? 2 : 0)];
    const uint8_t green = source[x * 4 + 1];
    const uint8_t blue = source[x * 4 + (sourceIsBgra ? 0 : 2)];
    const uint8_t alpha = source[x * 4 + 3];
    uint8_t* pixel = destination + x * 4;
    if (alphaType == AlphaType::Unpremultiplied) {
      pixel[0] = red;
      pixel[1] = green;
      pixel[2] = blue;
    } else if (alpha == 0) {
      pixel[0] = pixel[1] = pixel[2] = 0;
    } else if (alpha == 255) {
      pixel[0] = red;
      pixel[1] = green;
      pixel[2] = blue;
    } else {
      const unsigned half = static_cast<unsigned>(alpha) >> 1u;
      pixel[0] = static_cast<uint8_t>(std::min(255u, (red * 255u + half) / alpha));
      pixel[1] = static_cast<uint8_t>(std::min(255u, (green * 255u + half) / alpha));
      pixel[2] = static_cast<uint8_t>(std::min(255u, (blue * 255u + half) / alpha));
    }
    pixel[3] = alpha;
  }
}

/// Submit unpremultiplication and staging-copy work on the capture context's own queue.
/// @param context Capture context to record and submit on.
/// @param pipeline Unpremultiply compute pipeline of \p context.
/// @param texture Source texture, named in \p context.
/// @param resources Staging texture, its view, and the buffer the result is copied into.
/// @param width Content width in pixels. @param height Content height in pixels.
bool RecordGpuReadback(geode::GeodeDevice& context,
                       const geode::GeodeSnapshotReadbackPipeline& pipeline,
                       const gpu::Texture& texture,
                       const geode::SnapshotReadbackResources& resources, uint32_t width,
                       uint32_t height) {
  gpu::Device& runtime = context.runtimeDevice();
  auto createdView = runtime.createTextureView(
      texture, gpu::TextureViewDescriptor{"RendererGeodeReadbackInputView"});
  if (createdView.hasError()) {
    return false;
  }
  const gpu::TextureView inputView = std::move(createdView).result();
  auto bindGroup = runtime.createBindGroup(gpu::BindGroupDescriptor{
      "RendererGeodeReadbackBG",
      pipeline.bindGroupLayout(),
      {gpu::BindGroupEntry{pipeline.inputBinding(), gpu::TextureViewBinding{inputView}},
       gpu::BindGroupEntry{pipeline.outputBinding(),
                           gpu::TextureViewBinding{resources.stagingView}}}});
  if (bindGroup.hasError()) {
    return false;
  }
  auto createdEncoder = runtime.createCommandEncoder();
  if (createdEncoder.hasError()) {
    return false;
  }
  const std::unique_ptr<gpu::CommandEncoder> encoder = std::move(createdEncoder).result();
  auto createdPass =
      encoder->beginComputePass(gpu::ComputePassDescriptor{"RendererGeodeReadbackPass"});
  if (createdPass.hasError()) {
    return false;
  }
  gpu::ComputePassEncoder* pass = createdPass.result();
  const gpu::WorkgroupSize workgroup = pipeline.workgroupSize();
  if (pass->setPipeline(pipeline.pipeline()).hasError() ||
      pass->setBindGroup(0, bindGroup.result()).hasError() ||
      pass->dispatchWorkgroups((width + workgroup.x - 1) / workgroup.x,
                               (height + workgroup.y - 1) / workgroup.y, 1)
          .hasError() ||
      pass->end().hasError()) {
    return false;
  }
  if (encoder
          ->copyTextureToBuffer(gpu::TexelCopyTextureInfo{resources.staging}, resources.readback,
                                gpu::TexelCopyBufferLayout{0, alignBytesPerRow(width * 4u), height},
                                gpu::Extent2d{width, height})
          .hasError()) {
    return false;
  }
  auto commands = encoder->finish();
  if (commands.hasError()) {
    return false;
  }
  return !runtime.submit(std::move(commands).result()).hasError();
}

/// The descriptor a snapshot readback may run against, or nothing when \p source is not a
/// readable snapshot at \p dimensions. Read from the export, never from the producer's tables.
/// @param source Export of the texture to read.
/// @param dimensions Content extent to read, anchored at the texture origin.
std::optional<gpu::TextureDescriptor> ReadableSnapshotDescriptor(const gpu::TextureExport& source,
                                                                 Vector2i dimensions) {
  if (!source.isValid()) {
    return std::nullopt;
  }
  const gpu::TextureDescriptor& descriptor = source.descriptor();
  if (descriptor.sampleCount != 1 || !IsSnapshotFormat(descriptor.format) ||
      !SnapshotExtentFits(dimensions, SnapshotAllocationExtent(descriptor.size))) {
    return std::nullopt;
  }
  return descriptor;
}

bool CanUnpremultiplySnapshotOnGpu(const gpu::TextureDescriptor& descriptor, AlphaType alphaType) {
  return alphaType == AlphaType::Premultiplied &&
         descriptor.format == gpu::TextureFormat::RGBA8Unorm &&
         gpu::HasAllFlags(descriptor.usage, gpu::TextureUsage::Sampled);
}

bool SnapshotHasReadbackRoute(const gpu::TextureDescriptor& descriptor, AlphaType alphaType) {
  return gpu::HasAllFlags(descriptor.usage, gpu::TextureUsage::CopySrc) ||
         CanUnpremultiplySnapshotOnGpu(descriptor, alphaType);
}
}  // namespace

struct RendererGeodeTextureSnapshot::ReadbackControl {
  const std::function<bool()>& shouldCancel;
  std::chrono::steady_clock::time_point deadline;
  const std::function<void()>& mapRequested;
  ReadbackMapStatus status = ReadbackMapStatus::Success;

  bool stopped() {
    if (status == ReadbackMapStatus::Cancelled || status == ReadbackMapStatus::TimedOut ||
        status == ReadbackMapStatus::DeviceLost) {
      return true;
    }
    if (shouldCancel && shouldCancel()) {
      status = ReadbackMapStatus::Cancelled;
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      status = ReadbackMapStatus::TimedOut;
      return true;
    }
    return false;
  }
};

RendererBitmap RendererGeodeTextureSnapshot::readMappedTexture(
    geode::GeodeDevice& context, gpu::BufferMapping& mapping, uint32_t width, uint32_t height,
    gpu::TextureFormat format, AlphaType alphaType, ReadbackControl& control) {
  auto mappedBytes = context.runtimeDevice().mappedBytes(mapping);
  if (mappedBytes.hasError()) {
    (void)context.runtimeDevice().unmapBuffer(std::move(mapping));
    return {};
  }
  RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(static_cast<int>(width), static_cast<int>(height));
  bitmap.rowBytes = static_cast<size_t>(width) * 4u;
  bitmap.alphaType = AlphaType::Unpremultiplied;
  bitmap.pixels.resize(bitmap.rowBytes * height);
  const uint32_t bytesPerRow = alignBytesPerRow(width * 4u);
  for (uint32_t y = 0; y < height; ++y) {
    if (control.stopped()) {
      (void)context.runtimeDevice().unmapBuffer(std::move(mapping));
      return {};
    }
    CopyReadbackRow(bitmap.pixels.data() + static_cast<size_t>(y) * bitmap.rowBytes,
                    mappedBytes.result().data() + static_cast<size_t>(y) * bytesPerRow, width,
                    IsBgraTextureFormat(format), alphaType);
  }
  (void)context.runtimeDevice().unmapBuffer(std::move(mapping));
  return bitmap;
}

RendererBitmap RendererGeodeTextureSnapshot::readTextureGpu(geode::GeodeDevice& context,
                                                            const gpu::Texture& texture,
                                                            uint32_t width, uint32_t height,
                                                            ReadbackControl& control) {
  if (control.stopped()) {
    return {};
  }
  const geode::GeodeSnapshotReadbackPipeline& pipeline = context.snapshotReadbackPipeline();
  if (!pipeline.valid()) {
    return {};
  }
  if (control.stopped()) {
    return {};
  }
  geode::SnapshotReadbackResources resources =
      context.acquireSnapshotReadbackResources(width, height);
  if (resources.empty()) {
    return {};
  }
  if (control.stopped()) {
    return {};
  }
  if (!RecordGpuReadback(context, pipeline, texture, resources, width, height)) {
    return {};
  }
  const uint64_t mapSize = static_cast<uint64_t>(alignBytesPerRow(width * 4u)) * height;
  ReadbackMapResult mapped =
      MapAndWaitReadback(context, resources.readback, mapSize, control.shouldCancel,
                         control.deadline, control.mapRequested);
  control.status = mapped.status;
  if (mapped.status != ReadbackMapStatus::Success) {
    return {};
  }
  RendererBitmap bitmap =
      readMappedTexture(context, mapped.mapping, width, height, gpu::TextureFormat::RGBA8Unorm,
                        AlphaType::Unpremultiplied, control);
  if (bitmap.empty()) {
    return {};
  }
  context.releaseSnapshotReadbackResources(std::move(resources));
  return bitmap;
}

RendererBitmap RendererGeodeTextureSnapshot::readTextureCpu(
    geode::GeodeDevice& context, const gpu::Texture& texture, uint32_t width, uint32_t height,
    gpu::TextureFormat format, AlphaType alphaType, ReadbackControl& control) {
  if (control.stopped()) {
    return {};
  }
  gpu::Device& runtime = context.runtimeDevice();
  const uint32_t bytesPerRow = alignBytesPerRow(width * 4u);
  const uint64_t mapSize = static_cast<uint64_t>(bytesPerRow) * height;
  auto createdBuffer = runtime.createBuffer(gpu::BufferDescriptor{
      "RendererGeodeReadback", mapSize, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead});
  if (createdBuffer.hasError()) {
    return {};
  }
  gpu::Buffer buffer = std::move(createdBuffer).result();
  if (control.stopped()) {
    return {};
  }
  auto createdEncoder = runtime.createCommandEncoder();
  if (createdEncoder.hasError()) {
    return {};
  }
  const std::unique_ptr<gpu::CommandEncoder> encoder = std::move(createdEncoder).result();
  if (encoder
          ->copyTextureToBuffer(gpu::TexelCopyTextureInfo{texture}, buffer,
                                gpu::TexelCopyBufferLayout{0, bytesPerRow, height},
                                gpu::Extent2d{width, height})
          .hasError()) {
    return {};
  }
  auto commands = encoder->finish();
  if (commands.hasError()) {
    return {};
  }
  if (control.stopped()) {
    return {};
  }
  if (runtime.submit(std::move(commands).result()).hasError()) {
    return {};
  }
  ReadbackMapResult mapped = MapAndWaitReadback(context, buffer, mapSize, control.shouldCancel,
                                                control.deadline, control.mapRequested);
  control.status = mapped.status;
  if (mapped.status != ReadbackMapStatus::Success) {
    return {};
  }
  return readMappedTexture(context, mapped.mapping, width, height, format, alphaType, control);
}

bool RendererGeodeTextureSnapshot::waitForCaptureSource(geode::GeodeDevice& context,
                                                        const gpu::Texture& source,
                                                        ReadbackControl& control) {
  // On a backend whose contexts submit to separate queues and do not order each other's work on
  // the device, nothing recorded here may reach the queue before the producer's work on the
  // texture has completed. Sliced so the capture's own cancellation and deadline still apply.
  while (!context.runtimeDevice().waitForTextureSource(source, kReadbackWaitSliceSeconds)) {
    if (context.isDeviceLost()) {
      control.status = ReadbackMapStatus::DeviceLost;
      return false;
    }
    if (control.stopped()) {
      return false;
    }
  }
  return true;
}

RendererBitmap RendererGeodeTextureSnapshot::readTextureWithContext(
    geode::GeodeDevice& context, const gpu::TextureExport& exported, Vector2i dimensions,
    const gpu::TextureDescriptor& descriptor, AlphaType alphaType, ReadbackControl& control) {
  if (control.stopped()) {
    return {};
  }
  if (context.isDeviceLost()) {
    control.status = ReadbackMapStatus::DeviceLost;
    return {};
  }
  // The capture context is its own runtime device, so the producer's texture has to be
  // registered there before anything recorded here may name it. The registration never owns the
  // allocation and lasts exactly as long as this capture.
  gpu::Result<gpu::Texture> registered = context.registerCaptureSource(exported);
  if (registered.hasError()) {
    return {};
  }
  const gpu::Texture source = std::move(registered).result();
  if (!waitForCaptureSource(context, source, control)) {
    return {};
  }
  const uint32_t width = static_cast<uint32_t>(dimensions.x);
  const uint32_t height = static_cast<uint32_t>(dimensions.y);
  if (CanUnpremultiplySnapshotOnGpu(descriptor, alphaType)) {
    RendererBitmap bitmap = readTextureGpu(context, source, width, height, control);
    if (!bitmap.empty()) {
      return bitmap;
    }
  }
  if (control.stopped()) {
    return {};
  }
  if (context.isDeviceLost()) {
    control.status = ReadbackMapStatus::DeviceLost;
    return {};
  }
  if (!gpu::HasAllFlags(descriptor.usage, gpu::TextureUsage::CopySrc)) {
    return {};
  }
  return readTextureCpu(context, source, width, height, descriptor.format, alphaType, control);
}

RendererBitmap RendererGeodeTextureSnapshot::readTexture(std::shared_ptr<geode::GeodeDevice> device,
                                                         const gpu::TextureExport& exported,
                                                         Vector2i dimensions, AlphaType alphaType,
                                                         const std::function<bool()>& shouldCancel,
                                                         std::shared_ptr<Backing> backing) {
  (void)backing;  // Keep the source allocation leased through context-local resource teardown.
  const auto captureStart = std::chrono::steady_clock::now();
  if (!device) {
    return {};
  }
  const auto deadline =
      captureStart +
      std::chrono::milliseconds(device->snapshotReadbackBudgetMs_.load(std::memory_order_relaxed));
  const std::function<void()> mapRequested = [&device] {
    device->notifySnapshotReadbackPhaseForTesting(
        geode::GeodeDevice::SnapshotReadbackPhase::MapRequested);
  };
  ReadbackControl control{shouldCancel, deadline, mapRequested};
  if (control.stopped()) {
    if (control.status == ReadbackMapStatus::Cancelled) {
      device->readbackCaptureCancellations_.fetch_add(1, std::memory_order_relaxed);
    } else {
      device->recordSnapshotCaptureTimeout();
    }
    return {};
  }
  const std::optional<gpu::TextureDescriptor> descriptor =
      ReadableSnapshotDescriptor(exported, dimensions);
  if (!descriptor || !SnapshotHasReadbackRoute(*descriptor, alphaType)) {
    return {};
  }
  if (device->isDeviceLost()) {
    return {};
  }
  auto capture = device->acquireSnapshotCapture(shouldCancel, deadline);
  if (capture.status != geode::GeodeDevice::SnapshotCaptureStatus::Ready) {
    return {};
  }
  RendererBitmap bitmap = readTextureWithContext(*capture.context, exported, dimensions,
                                                 *descriptor, alphaType, control);
  if (control.status == ReadbackMapStatus::Cancelled) {
    device->readbackCaptureCancellations_.fetch_add(1, std::memory_order_relaxed);
  } else if (control.status == ReadbackMapStatus::TimedOut) {
    device->recordSnapshotCaptureTimeout();
  }
  return bitmap;
}

RendererBitmap RendererGeodeTextureSnapshot::takeSnapshot() const {
  return takeSnapshotInterruptibly(/*shouldCancel=*/{});
}

RendererBitmap RendererGeodeTextureSnapshot::takeSnapshotInterruptibly(
    const std::function<bool()>& shouldCancel) const {
  if (!isValid() || backing_ == nullptr) {
    return {};
  }
  return readTexture(device_, backing_->exported, dimensions_, alphaType_, shouldCancel, backing_);
}

RendererBitmap RendererGeode::takeSnapshotInterruptibly(
    const std::function<bool()>& shouldCancel) const {
  if (impl_->frameRecordingAbandoned || impl_->frameArenaWriteFailed || !impl_->device ||
      !impl_->target.isValid() || impl_->pixelWidth <= 0 || impl_->pixelHeight <= 0) {
    return RendererBitmap{};
  }
  // Exported here, on the renderer's own thread, which is the only thread that may read its
  // tables; the capture registers the export on its own context.
  gpu::Result<gpu::TextureExport> exported =
      impl_->device->runtimeDevice().exportTexture(impl_->target);
  if (exported.hasError()) {
    return RendererBitmap{};
  }
  return RendererGeodeTextureSnapshot::readTexture(impl_->device, exported.result(),
                                                   Vector2i(impl_->pixelWidth, impl_->pixelHeight),
                                                   AlphaType::Premultiplied, shouldCancel);
}

void RendererGeode::setWithinFrameTextureReuseForTesting(bool enabled) {
  impl_->withinFrameReuseEnabled = enabled;
}

void RendererGeode::injectDeviceLossForTesting() {
  if (impl_->device) {
    impl_->device->markDeviceLost("test-injected device loss");
  }
}

void RendererGeode::injectFilterLocalRasterAllocationFailureForTesting() {
  impl_->failNextLocalFilterRasterAllocationForTesting = true;
}

void RendererGeode::injectFilterFrameSuspensionAndRestoreFailureForTesting() {
  impl_->failFilterFrameSuspensionForTesting = true;
  impl_->failFilterFrameRestoreForTesting = true;
}

void RendererGeode::injectActiveArenaWriteFailureForTesting() {
  if (impl_->encoder) {
    impl_->encoder->injectNextArenaWriteFailureForTesting();
  }
}

void RendererGeode::injectTransformedResampleArenaWriteFailureForTesting() {
  impl_->failNextTransformedResampleArenaWriteForTesting = true;
}

void RendererGeode::injectFrameEncoderCloseFailureForTesting(std::size_t successfulCloses) {
  impl_->failFrameEncoderCloseForTesting = successfulCloses;
}

bool RendererGeode::submitFilterBudgetChunkForTesting() {
  impl_->forceFilterBudgetChunkForTesting = true;
  return impl_->submitFilterBudgetChunk();
}

size_t RendererGeode::failedFilterTextureCountForTesting() const {
  return impl_->failedFilterTextures.size();
}

size_t RendererGeode::recoverableArenaTextureCountForTesting() const {
  return impl_->device ? impl_->device->pendingFencedTextureBackings() : 0;
}

bool RendererGeode::hasActiveDrawingEncoderForTesting() const {
  return impl_->encoder != nullptr;
}

bool RendererGeode::filterFrameFailureInjectionPendingForTesting() const {
  return impl_->failFilterFrameSuspensionForTesting || impl_->failFilterFrameRestoreForTesting;
}

bool RendererGeode::deviceLost() const {
  return impl_->device && impl_->device->isDeviceLost();
}

geode::GpuWaitResult RendererGeode::waitForSubmittedWork(std::chrono::milliseconds stallBound) {
  if (!impl_->device) {
    return geode::GpuWaitResult::Complete;
  }
  return impl_->device->waitForQueueIdle(stallBound);
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
    case geode::GpuWaitSite::Present: return GpuWaitTimeoutSite::Present;
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
      .captureCancellations = stats.captureCancellations,
      .captureTimeouts = stats.captureTimeouts,
      .contextCreates = stats.contextCreates,
      .bufferCreates = stats.bufferCreates,
      .textureCreates = stats.textureCreates,
      .bindgroupCreates = stats.bindgroupCreates,
      .submits = stats.submits,
      .poolEntries = stats.poolEntries,
      .poolBytes = stats.poolBytes,
      .sharedTextureTailBytes = stats.sharedTextureTailBytes,
  };
}

}  // namespace donner::svg
