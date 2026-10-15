#include "harness/AdapterBase.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "donner/svg/renderer/RendererDriver.h"

namespace donner::bakeoff {

namespace {

int PixelDimension(double logical, double devicePixelRatio) {
  const double pixels = std::ceil(logical * devicePixelRatio);
  if (!(pixels > 0.0) || pixels > 16384.0) {
    return 0;
  }
  return static_cast<int>(pixels);
}

bool HasNestedClipLayers(const svg::ResolvedClip& clip) {
  const int firstLayer = clip.clipPaths.front().layer;
  return std::any_of(
      clip.clipPaths.begin(), clip.clipPaths.end(),
      [firstLayer](const svg::ClipPathShape& shape) { return shape.layer != firstLayer; });
}

ClipDesc MakeClipDesc(const svg::ResolvedClip& clip, const Transform2d& deviceFromLocal) {
  ClipDesc desc;
  desc.rect = clip.clipRect;
  desc.deviceFromRect = deviceFromLocal;
  for (const svg::ClipPathShape& shape : clip.clipPaths) {
    // Same composition order as Donner's software clip: units, then the clip child's own
    // transform, then the current device transform.
    desc.shapes.push_back({shape.path, shape.fillRule,
                           clip.clipPathUnitsTransform * shape.parentFromEntity * deviceFromLocal,
                           shape.layer});
  }
  return desc;
}

}  // namespace

AdapterBase::AdapterBase(CapabilityTable capabilities) : capabilities_(capabilities) {}

AdapterBase::~AdapterBase() = default;

void AdapterBase::draw(svg::SVGDocument& document) {
  svg::RendererDriver driver(*this);
  driver.draw(document);
}

bool AdapterBase::admitAll(const std::vector<Feature>& features) {
  bool all = true;
  for (const Feature feature : features) {
    all = admit(feature) && all;
  }
  return all;
}

bool AdapterBase::admit(Feature feature) {
  usage_.recordUse(feature);
  if (capabilities_.get(feature) == Support::Unsupported) {
    usage_.recordDropped(feature);
    return false;
  }
  return true;
}

void AdapterBase::beginFrame(const svg::RenderViewport& viewport) {
  pixelWidth_ = PixelDimension(viewport.size.x, viewport.devicePixelRatio);
  pixelHeight_ = PixelDimension(viewport.size.y, viewport.devicePixelRatio);
  deviceFromLocal_ = Transform2d();
  transformStack_.clear();
  pushStack_.clear();
  patternStates_.assign(1, PatternState{});
  tileDepth_ = 0;
  maskSuppressing_.clear();
  suppressDepth_ = 0;
  onBeginFrame(pixelWidth_, pixelHeight_);
}

void AdapterBase::endFrame() {
  // Unwind anything the traversal left open so the backend sees balanced composition.
  while (!pushStack_.empty()) {
    popRecord(pushStack_.back().kind);
  }
  onEndFrame();
}

void AdapterBase::setTransform(const Transform2d& deviceFromLocal) {
  if (!deviceFromLocal.isIdentity()) {
    admit(Feature::Transform);
  }
  deviceFromLocal_ = deviceFromLocal;
}

void AdapterBase::pushTransform(const Transform2d& parentFromChild) {
  if (!parentFromChild.isIdentity()) {
    admit(Feature::Transform);
  }
  transformStack_.push_back(deviceFromLocal_);
  deviceFromLocal_ = parentFromChild * deviceFromLocal_;
}

void AdapterBase::popTransform() {
  if (transformStack_.empty()) {
    return;
  }
  deviceFromLocal_ = transformStack_.back();
  transformStack_.pop_back();
}

void AdapterBase::pushClip(const svg::ResolvedClip& clip) {
  if (clip.empty() || (!clip.clipRect.has_value() && clip.clipPaths.empty())) {
    pushStack_.push_back({PushKind::Clip, false});
    return;
  }

  std::vector<Feature> features;
  if (clip.clipRect.has_value()) {
    features.push_back(Feature::ClipRect);
  }
  if (!clip.clipPaths.empty()) {
    features.push_back(HasNestedClipLayers(clip) ? Feature::ClipPathNested : Feature::ClipPath);
  }

  const bool supported = admitAll(features);
  bool forward = supported && !suppressed();
  if (forward && !onPushClip(MakeClipDesc(clip, deviceFromLocal_))) {
    // The adapter supports these features only for some geometry; this clip is dropped.
    for (const Feature feature : features) {
      usage_.recordDropped(feature);
    }
    forward = false;
  }
  pushStack_.push_back({PushKind::Clip, forward});
}

void AdapterBase::popClip() {
  popRecord(PushKind::Clip);
}

void AdapterBase::pushIsolatedLayer(double opacity, svg::MixBlendMode blendMode) {
  std::vector<Feature> features = {Feature::IsolatedLayer};
  if (blendMode != svg::MixBlendMode::Normal) {
    features.push_back(Feature::BlendMode);
  }
  bool forward = admitAll(features) && !suppressed();
  if (forward && !onPushLayer(opacity, blendMode)) {
    // The adapter supports only some blend modes; this group is dropped.
    usage_.recordDropped(features.back());
    forward = false;
  }
  pushStack_.push_back({PushKind::Layer, forward});
}

void AdapterBase::popIsolatedLayer() {
  popRecord(PushKind::Layer);
}

void AdapterBase::pushFilterLayer(const svg::components::FilterGraph& filterGraph,
                                  const std::optional<Box2d>& /*filterRegion*/) {
  bool forward = admit(Feature::Filter) && !suppressed();
  if (forward && !onPushFilter(filterGraph, deviceFromLocal_)) {
    // The adapter can express only some filter graphs; this one is dropped.
    usage_.recordDropped(Feature::Filter);
    forward = false;
  }
  pushStack_.push_back({PushKind::Filter, forward});
}

void AdapterBase::popFilterLayer() {
  popRecord(PushKind::Filter);
}

void AdapterBase::pushMask(const std::optional<Box2d>& maskBounds) {
  pushMask(maskBounds, svg::MaskType::Luminance, svg::ColorInterpolation::Auto);
}

void AdapterBase::pushMask(const std::optional<Box2d>& maskBounds, svg::MaskType maskType) {
  pushMask(maskBounds, maskType, svg::ColorInterpolation::Auto);
}

void AdapterBase::pushMask(const std::optional<Box2d>& maskBounds, svg::MaskType maskType,
                           svg::ColorInterpolation /*colorInterpolation*/) {
  const Feature feature =
      maskType == svg::MaskType::Alpha ? Feature::MaskAlpha : Feature::MaskLuminance;
  bool forward = admit(feature) && !suppressed();
  if (forward && !onPushMask(maskType, deviceFromLocal_, maskBounds)) {
    usage_.recordDropped(feature);
    forward = false;
  }
  // Mask definition content of a dropped mask must not paint onto the frame.
  const bool suppressing = !forward;
  if (suppressing) {
    ++suppressDepth_;
  }
  maskSuppressing_.push_back(suppressing);
  pushStack_.push_back({PushKind::Mask, forward});
}

void AdapterBase::transitionMaskToContent() {
  if (maskSuppressing_.empty()) {
    return;
  }
  if (maskSuppressing_.back()) {
    --suppressDepth_;
    maskSuppressing_.back() = false;
  }
  if (!pushStack_.empty() && pushStack_.back().kind == PushKind::Mask &&
      pushStack_.back().forwarded) {
    onMaskContent();
  }
}

void AdapterBase::popMask() {
  popRecord(PushKind::Mask);
}

void AdapterBase::popRecord(PushKind kind) {
  if (pushStack_.empty() || pushStack_.back().kind != kind) {
    return;
  }
  const PushRecord record = pushStack_.back();
  pushStack_.pop_back();
  if (kind == PushKind::Mask && !maskSuppressing_.empty()) {
    if (maskSuppressing_.back()) {
      --suppressDepth_;
    }
    maskSuppressing_.pop_back();
  }
  if (!record.forwarded) {
    return;
  }
  switch (kind) {
    case PushKind::Clip: onPopClip(); break;
    case PushKind::Layer: onPopLayer(); break;
    case PushKind::Filter: onPopFilter(); break;
    case PushKind::Mask: onPopMask(); break;
  }
}

bool AdapterBase::beginPatternTile(const Box2d& tileRect, const Transform2d& targetFromPattern) {
  // Rejecting a tile keeps its content off the frame.
  if (!admit(Feature::PatternPaint) || suppressed()) {
    return false;
  }
  if (!onBeginPatternTile(tileRect, targetFromPattern)) {
    usage_.recordDropped(Feature::PatternPaint);
    return false;
  }
  // Tile content draws one level deeper, so it never sees the enclosing element's patterns.
  ++tileDepth_;
  if (patternStates_.size() <= tileDepth_) {
    patternStates_.resize(tileDepth_ + 1);
  }
  patternStates_[tileDepth_] = PatternState{};
  return true;
}

void AdapterBase::endPatternTile(bool forStroke) {
  // The driver ends only tiles whose begin returned true.
  if (tileDepth_ == 0) {
    return;
  }
  --tileDepth_;
  PatternState& state = patternStates_[tileDepth_];
  (forStroke ? state.pendingStroke : state.pendingFill) = true;
  onEndPatternTile(forStroke);
}

void AdapterBase::setPaint(const svg::PaintParams& paint) {
  paint_ = paint;
  PatternState& state = patternStates_[tileDepth_];
  state.activeFill = state.pendingFill;
  state.activeStroke = state.pendingStroke;
  state.pendingFill = false;
  state.pendingStroke = false;
}

bool AdapterBase::admitPaint(const PaintDesc& paint) {
  if (paint.kind == PaintDesc::Kind::Solid) {
    return admit(Feature::SolidPaint);
  }
  if (paint.kind == PaintDesc::Kind::Gradient) {
    return admit(paint.gradient.type == GradientDesc::Type::Linear ? Feature::LinearGradient
                                                                   : Feature::RadialGradient);
  }
  // A pattern paint exists only after an accepted tile, which already counted its use.
  return paint.kind == PaintDesc::Kind::Pattern;
}

void AdapterBase::drawFill(const Path& path, FillRule fillRule, const Box2d& bounds) {
  PaintDesc fill;
  if (patternStates_[tileDepth_].activeFill) {
    // The accepted tile is the paint; no fallback color is substituted for it.
    fill.kind = PaintDesc::Kind::Pattern;
  } else {
    fill = ResolvePaint(paint_.fill, paint_.fillOpacity, paint_.currentColor.rgba(), bounds,
                        paint_.viewBox);
  }
  if (fill.kind == PaintDesc::Kind::None) {
    return;
  }
  bool ok = admitPaint(fill);
  if (fillRule == FillRule::EvenOdd) {
    ok = admit(Feature::EvenOddFill) && ok;
  }
  if (ok) {
    onFillPath(path, fillRule, fill, deviceFromLocal_);
  }
}

void AdapterBase::drawStroke(const Path& path, const svg::StrokeParams& stroke,
                             const Box2d& bounds) {
  PaintDesc paint;
  if (patternStates_[tileDepth_].activeStroke) {
    paint.kind = PaintDesc::Kind::Pattern;
  } else {
    paint = ResolvePaint(paint_.stroke, paint_.strokeOpacity, paint_.currentColor.rgba(), bounds,
                         paint_.viewBox);
  }
  if (paint.kind == PaintDesc::Kind::None) {
    return;
  }
  bool ok = admitPaint(paint);
  ok = admit(Feature::Stroke) && ok;
  if (!stroke.dashArray.empty()) {
    ok = admit(Feature::StrokeDash) && ok;
  }
  if (ok) {
    onStrokePath(path, stroke, paint, deviceFromLocal_);
  }
}

void AdapterBase::drawShape(const Path& path, FillRule fillRule, const svg::StrokeParams& stroke) {
  if (suppressed() || path.empty()) {
    return;
  }
  const Box2d bounds = path.bounds();
  if (paint_.drawFillComponent && svg::components::HasPaint(paint_.fill)) {
    drawFill(path, fillRule, bounds);
  }
  if (paint_.drawStrokeComponent && svg::components::HasPaint(paint_.stroke) &&
      stroke.strokeWidth > 0.0) {
    drawStroke(path, stroke, bounds);
  }
}

void AdapterBase::drawPath(const svg::PathShape& path, const svg::StrokeParams& stroke) {
  drawShape(path.pathOrEmpty(), path.fillRule, stroke);
}

void AdapterBase::drawRect(const Box2d& rect, const svg::StrokeParams& stroke) {
  drawShape(PathBuilder().addRect(rect).build(), FillRule::NonZero, stroke);
}

void AdapterBase::drawEllipse(const Box2d& bounds, const svg::StrokeParams& stroke) {
  drawShape(PathBuilder().addEllipse(bounds).build(), FillRule::NonZero, stroke);
}

void AdapterBase::drawImage(const svg::ImageResource& image, const svg::ImageParams& params) {
  if (suppressed() || image.data.empty() || image.width <= 0 || image.height <= 0) {
    return;
  }
  if (admit(Feature::Image)) {
    const bool pixelated = params.imageRenderingPixelated ||
                           params.imageRendering == svg::ImageRendering::Pixelated ||
                           params.imageRendering == svg::ImageRendering::CrispEdges ||
                           params.imageRendering == svg::ImageRendering::OptimizeSpeed;
    if (!onDrawImage(image, params.targetRect, deviceFromLocal_, pixelated)) {
      usage_.recordDropped(Feature::Image);
    }
  }
}

void AdapterBase::drawText(Registry& registry, const svg::components::ComputedTextComponent& text,
                           const svg::TextParams& params) {
  if (suppressed() || !admit(Feature::Text)) {
    return;
  }
  if (!onDrawText(registry, text, params)) {
    usage_.recordDropped(Feature::Text);
  }
}

bool AdapterBase::onPushClip(const ClipDesc& /*clip*/) {
  return false;
}
void AdapterBase::onPopClip() {}
bool AdapterBase::onPushLayer(double /*opacity*/, svg::MixBlendMode /*blendMode*/) {
  return false;
}
void AdapterBase::onPopLayer() {}
bool AdapterBase::onPushMask(svg::MaskType /*maskType*/, const Transform2d& /*deviceFromLocal*/,
                             const std::optional<Box2d>& /*bounds*/) {
  return false;
}
void AdapterBase::onMaskContent() {}
void AdapterBase::onPopMask() {}
bool AdapterBase::onPushFilter(const svg::components::FilterGraph& /*graph*/,
                               const Transform2d& /*deviceFromLocal*/) {
  return false;
}
void AdapterBase::onPopFilter() {}
bool AdapterBase::onBeginPatternTile(const Box2d& /*tileRect*/,
                                     const Transform2d& /*targetFromPattern*/) {
  return false;
}
void AdapterBase::onEndPatternTile(bool /*forStroke*/) {}
bool AdapterBase::onDrawImage(const svg::ImageResource& /*image*/, const Box2d& /*targetRect*/,
                              const Transform2d& /*deviceFromLocal*/, bool /*pixelated*/) {
  return false;
}

bool AdapterBase::onDrawText(Registry& /*registry*/,
                             const svg::components::ComputedTextComponent& /*text*/,
                             const svg::TextParams& /*params*/) {
  return false;
}

}  // namespace donner::bakeoff
