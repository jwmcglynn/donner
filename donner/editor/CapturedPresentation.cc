#include "donner/editor/CapturedPresentation.h"

#include "donner/editor/SelectionAabb.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {
namespace {

bool HasFixedAncestor(const svg::SVGElement& element) {
  for (auto parent = element.parentElement(); parent.has_value();
       parent = parent->parentElement()) {
    const auto& style = parent->getComputedStyle();
    const auto filters = style.filter.get();
    if (style.clipPath.get().has_value() || style.mask.get().has_value() ||
        (filters.has_value() && !filters->empty())) {
      return true;
    }
  }
  return false;
}

bool PathBoundsCoverFrame(const svg::SVGElement& element) {
  if (!CollectRenderableTextRoots(element).empty()) {
    return false;
  }
  const auto geometry = CollectRenderableGeometry(element);
  if (geometry.empty()) {
    return false;
  }
  for (const auto& leaf : geometry) {
    const auto& style = leaf.getComputedStyle();
    if (!style.stroke.get().value().is<svg::PaintServer::None>() ||
        style.markerStart.get().has_value() || style.markerMid.get().has_value() ||
        style.markerEnd.get().has_value()) {
      return false;
    }
  }
  return true;
}

bool HasUnboundedPaintEffect(const svg::SVGElement& element) {
  const auto& style = element.getComputedStyle();
  const auto filters = style.filter.get();
  return style.clipPath.get().has_value() || style.mask.get().has_value() ||
         (filters.has_value() && !filters->empty()) || style.markerStart.get().has_value() ||
         style.markerMid.get().has_value() || style.markerEnd.get().has_value();
}

bool HasUnmodelledPaint(const svg::SVGElement& element) {
  if (HasUnboundedPaintEffect(element)) {
    return true;
  }
  if (element.isa<svg::SVGGeometryElement>()) {
    return false;
  }
  switch (element.type()) {
    case svg::ElementType::Defs:
    case svg::ElementType::Style:
    case svg::ElementType::Metadata:
    case svg::ElementType::Title:
    case svg::ElementType::Desc: return false;
    case svg::ElementType::G:
    case svg::ElementType::A:
    case svg::ElementType::Text:
    case svg::ElementType::TSpan:
    case svg::ElementType::TextPath: break;
    default: return true;
  }
  for (auto child = element.firstChild(); child.has_value(); child = child->nextSibling()) {
    if (HasUnmodelledPaint(*child)) {
      return true;
    }
  }
  return false;
}

std::vector<Box2d> PaintBounds(const svg::SVGElement& element, bool& bounded) {
  std::vector<Box2d> boxes;
  bounded = !HasUnmodelledPaint(element);
  for (const auto& geometry : CollectRenderableGeometry(element)) {
    bounded &= !HasUnboundedPaintEffect(geometry);
    if (const auto box = GeometryWorldFrameBounds(geometry)) {
      boxes.push_back(*box);
    }
  }
  for (const auto& text : CollectRenderableTextRoots(element)) {
    bounded &= !HasUnboundedPaintEffect(text);
    if (const auto box = TextWorldInkBounds(text)) {
      Box2d coverage = *box;
      if (const auto frame = TextWorldFrameBounds(text)) {
        coverage.addBox(*frame);
      }
      boxes.push_back(coverage);
    }
  }
  return boxes;
}

}  // namespace

std::optional<svg::SVGElement> CapturedPresentation::findAttachedElement(svg::SVGDocument& document,
                                                                         Entity entity) {
  Registry& registry = document.unsafeRegistry();
  if (entity == entt::null || !registry.valid(entity)) {
    return std::nullopt;
  }
  svg::SVGElement element(EntityHandle(registry, entity));
  if (!element.tryType().has_value()) {
    return std::nullopt;
  }
  svg::SVGElement root = element;
  while (const auto parent = root.parentElement()) {
    root = *parent;
  }
  return root == document.svgElement() ? std::optional<svg::SVGElement>(element) : std::nullopt;
}

void CapturedPresentation::capturePose(const svg::SVGElement& element) {
  const Entity entity = element.unsafeEntityHandle().entity();
  if (element.isa<svg::SVGGraphicsElement>()) {
    trackedPoses_[entity] = PresentationPose{
        .entity = entity,
        .documentFromElement = element.cast<svg::SVGGraphicsElement>().elementFromWorld()};
  }
  bool boundedPaint = true;
  trackedPaintBounds_[entity] = PaintBounds(element, boundedPaint);
  if (!boundedPaint || HasFixedAncestor(element)) {
    rasterBound_.insert(entity);
  }
}

std::optional<PresentationPose> CapturedPresentation::pose(Entity entity) const {
  const auto found = trackedPoses_.find(entity);
  return found != trackedPoses_.end() ? std::optional<PresentationPose>(found->second)
                                      : std::nullopt;
}

Entity CapturedPresentation::adornmentOwner(const svg::SVGElement& element) const {
  for (std::optional<svg::SVGElement> ancestor = element; ancestor.has_value();
       ancestor = ancestor->parentElement()) {
    const Entity entity = ancestor->unsafeEntityHandle().entity();
    if (independentlyMovable_.contains(entity)) {
      return entity;
    }
  }
  return entt::null;
}

void CapturedPresentation::captureAdornments(
    std::span<const svg::SVGElement> sourceHover,
    const std::optional<LockedRejectionFlashInput>& lockedFlash) {
  for (const auto& element : sourceHover) {
    const std::array hovered{element};
    adornments_.push_back(Adornment{.subject = element.unsafeEntityHandle().entity(),
                                    .owner = adornmentOwner(element),
                                    .chrome = OverlayRenderer::captureChromeSnapshot(
                                        {}, std::nullopt, Transform2d(), std::nullopt, hovered)});
  }
  if (lockedFlash.has_value()) {
    adornments_.push_back(
        Adornment{.subject = lockedFlash->element.unsafeEntityHandle().entity(),
                  .owner = adornmentOwner(lockedFlash->element),
                  .lockedFlash = true,
                  .chrome = OverlayRenderer::captureChromeSnapshot(
                      {}, std::nullopt, Transform2d(), std::nullopt, {}, std::nullopt,
                      SelectionChromeDetail::Full, Transform2d(), lockedFlash)});
  }
}

bool CapturedPresentation::captureSelected(svg::SVGDocument& document,
                                           std::span<const svg::SVGElement> selection,
                                           const std::optional<svg::SVGElement>& livePath) {
  for (const auto& element : selection) {
    const EntityHandle handle = element.unsafeEntityHandle();
    if (handle.registry() != &document.unsafeRegistry() || !handle.valid()) {
      return false;
    }
    const Entity entity = handle.entity();
    selection_.push_back(entity);
    capturePose(element);
    if (const auto capturedPose = pose(entity)) {
      poses_.push_back(*capturedPose);
    }
    const std::array selected{element};
    objects_.push_back(
        Object{.entity = entity,
               .chrome = OverlayRenderer::captureChromeSnapshot(
                   selected, std::nullopt, Transform2d(), std::nullopt, {}, std::nullopt,
                   SelectionChromeDetail::Complete, Transform2d(), std::nullopt, 1.0,
                   livePath == std::optional(element) ? livePath : std::nullopt),
               .pathBoundsCoverFrame = PathBoundsCoverFrame(element),
               .paintBoundsDoc = paintBounds(entity)});
  }
  for (const auto& element : selection) {
    for (auto parent = element.parentElement(); parent.has_value();
         parent = parent->parentElement()) {
      if (std::ranges::find(selection_, parent->unsafeEntityHandle().entity()) !=
          selection_.end()) {
        affinePreviewAllowed_ = false;
      }
    }
  }
  return true;
}

void CapturedPresentation::captureTracked(svg::SVGDocument& document,
                                          std::span<const Entity> tracked) {
  for (Entity entity : tracked) {
    if (trackedPoses_.contains(entity)) {
      continue;
    }
    const auto element = findAttachedElement(document, entity);
    if (element.has_value()) {
      capturePose(*element);
    } else {
      absent_.insert(entity);
    }
  }
}

void CapturedPresentation::captureValidatedAdornments(
    svg::SVGDocument& document, std::span<const svg::SVGElement> sourceHover,
    const std::optional<LockedRejectionFlashInput>& lockedFlash) {
  std::vector<svg::SVGElement> validHover;
  for (const auto& hovered : sourceHover) {
    const auto handle = hovered.unsafeEntityHandle();
    if (handle.registry() == &document.unsafeRegistry() && handle.valid()) {
      validHover.push_back(hovered);
    }
  }
  auto validFlash = lockedFlash;
  if (validFlash.has_value() &&
      (validFlash->element.unsafeEntityHandle().registry() != &document.unsafeRegistry() ||
       !validFlash->element.unsafeEntityHandle().valid())) {
    validFlash.reset();
  }
  captureAdornments(validHover, validFlash);
}

std::shared_ptr<const CapturedPresentation> CapturedPresentation::Capture(
    svg::SVGDocument& document, PresentationIdentity identity,
    std::span<const svg::SVGElement> selection, std::span<const Entity> tracked,
    std::span<const Entity> independentlyMovable, std::span<const svg::SVGElement> sourceHover,
    const std::optional<LockedRejectionFlashInput>& lockedFlash,
    const std::optional<svg::SVGElement>& livePath, const std::optional<TextEditing>& textEditing) {
  const auto access = document.writeAccess();
  auto snapshot = std::shared_ptr<CapturedPresentation>(new CapturedPresentation());
  snapshot->independentlyMovable_.insert(independentlyMovable.begin(), independentlyMovable.end());
  snapshot->canvasSize_ = document.canvasSize();
  snapshot->documentOrigin_ = document.svgElement().viewBox().value_or(Box2d()).topLeft;
  if (!snapshot->captureSelected(document, selection, livePath)) {
    return nullptr;
  }
  snapshot->captureTracked(document, tracked);
  snapshot->captureValidatedAdornments(document, sourceHover, lockedFlash);
  identity.documentRevision = document.handle()->revision();
  snapshot->identity_ = identity;
  if (textEditing.has_value() && textEditing->identity.sameScene(identity) &&
      textEditing->canvasSize == snapshot->canvasSize_) {
    snapshot->textEditing_ = textEditing;
  }
  return snapshot;
}

}  // namespace donner::editor
