#include "donner/svg/SVGTextPositioningElement.h"

#include "donner/svg/components/text/TextPositioningComponent.h"

namespace donner::svg {
namespace {

template <typename T>
const SmallVector<T, 1>& SnapshotTextPositionList(const SmallVector<T, 1>* value) {
  static thread_local SmallVector<T, 1> snapshot;
  if (value) {
    snapshot = *value;
  } else {
    snapshot.clear();
  }
  return snapshot;
}

}  // namespace

SVGTextPositioningElement::SVGTextPositioningElement(EntityHandle handle)
    : SVGTextContentElement(handle) {
  handle_.emplace<components::TextPositioningComponent>();
}

void SVGTextPositioningElement::setX(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  SmallVector<Lengthd, 1>& x = handle_.get_or_emplace<components::TextPositioningComponent>().x;
  if (value) {
    x = {*value};
  } else {
    x.clear();
  }
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

void SVGTextPositioningElement::setXList(SmallVector<Lengthd, 1>&& value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::TextPositioningComponent>().x = std::move(value);
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

std::optional<Lengthd> SVGTextPositioningElement::x() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::TextPositioningComponent>();
  const SmallVector<Lengthd, 1>& x =
      component ? component->x : SnapshotTextPositionList<Lengthd>(nullptr);
  if (x.empty()) {
    return std::nullopt;
  }

  return x[0];
}

const SmallVector<Lengthd, 1>& SVGTextPositioningElement::xList() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::TextPositioningComponent>();
  return SnapshotTextPositionList(component ? &component->x : nullptr);
}

void SVGTextPositioningElement::setY(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  SmallVector<Lengthd, 1>& y = handle_.get_or_emplace<components::TextPositioningComponent>().y;
  if (value) {
    y = {*value};
  } else {
    y.clear();
  }
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

void SVGTextPositioningElement::setYList(SmallVector<Lengthd, 1>&& value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::TextPositioningComponent>().y = std::move(value);
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

std::optional<Lengthd> SVGTextPositioningElement::y() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::TextPositioningComponent>();
  const SmallVector<Lengthd, 1>& y =
      component ? component->y : SnapshotTextPositionList<Lengthd>(nullptr);
  if (y.empty()) {
    return std::nullopt;
  }

  return y[0];
}

const SmallVector<Lengthd, 1>& SVGTextPositioningElement::yList() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::TextPositioningComponent>();
  return SnapshotTextPositionList(component ? &component->y : nullptr);
}

void SVGTextPositioningElement::setDx(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  SmallVector<Lengthd, 1>& dx = handle_.get_or_emplace<components::TextPositioningComponent>().dx;
  if (value) {
    dx = {*value};
  } else {
    dx.clear();
  }
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

void SVGTextPositioningElement::setDxList(SmallVector<Lengthd, 1>&& value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::TextPositioningComponent>().dx = std::move(value);
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

std::optional<Lengthd> SVGTextPositioningElement::dx() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::TextPositioningComponent>();
  const SmallVector<Lengthd, 1>& dx =
      component ? component->dx : SnapshotTextPositionList<Lengthd>(nullptr);
  if (dx.empty()) {
    return std::nullopt;
  }

  return dx[0];
}

const SmallVector<Lengthd, 1>& SVGTextPositioningElement::dxList() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::TextPositioningComponent>();
  return SnapshotTextPositionList(component ? &component->dx : nullptr);
}

void SVGTextPositioningElement::setDy(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  SmallVector<Lengthd, 1>& dy = handle_.get_or_emplace<components::TextPositioningComponent>().dy;
  if (value) {
    dy = {*value};
  } else {
    dy.clear();
  }
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

void SVGTextPositioningElement::setDyList(SmallVector<Lengthd, 1>&& value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::TextPositioningComponent>().dy = std::move(value);
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

std::optional<Lengthd> SVGTextPositioningElement::dy() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::TextPositioningComponent>();
  const SmallVector<Lengthd, 1>& dy =
      component ? component->dy : SnapshotTextPositionList<Lengthd>(nullptr);
  if (dy.empty()) {
    return std::nullopt;
  }

  return dy[0];
}

const SmallVector<Lengthd, 1>& SVGTextPositioningElement::dyList() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::TextPositioningComponent>();
  return SnapshotTextPositionList(component ? &component->dy : nullptr);
}

void SVGTextPositioningElement::setRotate(std::optional<double> degrees) {
  DocumentWriteAccess access = handle_.writeAccess();
  SmallVector<double, 1>& rotateDegrees =
      handle_.get_or_emplace<components::TextPositioningComponent>().rotateDegrees;
  if (degrees) {
    rotateDegrees = {*degrees};
  } else {
    rotateDegrees.clear();
  }
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

void SVGTextPositioningElement::setRotateList(SmallVector<double, 1>&& value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::TextPositioningComponent>().rotateDegrees = std::move(value);
  invalidateTextGeometry();
  access.bumpMutationRevision();
}

std::optional<double> SVGTextPositioningElement::rotate() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::TextPositioningComponent>();
  const SmallVector<double, 1>& rotateDegrees =
      component ? component->rotateDegrees : SnapshotTextPositionList<double>(nullptr);
  if (rotateDegrees.empty()) {
    return std::nullopt;
  }

  return rotateDegrees[0];
}

const SmallVector<double, 1>& SVGTextPositioningElement::rotateList() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::TextPositioningComponent>();
  return SnapshotTextPositionList(component ? &component->rotateDegrees : nullptr);
}

}  // namespace donner::svg
