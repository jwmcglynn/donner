#include "donner/editor/UndoTimeline.h"

#include "donner/svg/SVGGraphicsElement.h"

namespace donner::editor {

UndoSnapshot captureTransformSnapshot(const svg::SVGElement& element) {
  // The snapshot must capture the local (parent-from-entity) transform so
  // that `applySnapshot` can restore it via `setTransform`. The prototype
  // used `elementFromWorld()` here, but that's the world-space cumulative
  // transform and does not round-trip through `setTransform`.
  auto graphicsElement = element.cast<svg::SVGGraphicsElement>();
  return UndoSnapshot{
      .element = element,
      .transform = graphicsElement.transform(),
  };
}

void applySnapshot(const UndoSnapshot& snapshot) {
  auto graphicsElement = snapshot.element.cast<svg::SVGGraphicsElement>();
  graphicsElement.setTransform(snapshot.transform);
}

void UndoTimeline::beginTransaction(std::string_view label, UndoSnapshot before) {
  if (pendingTransaction_.has_value()) {
    return;  // Nested — outermost wins.
  }
  pendingTransaction_ = PendingTransaction{std::string(label), std::move(before)};
}

void UndoTimeline::commitTransaction(UndoSnapshot after) {
  if (!pendingTransaction_.has_value()) {
    return;
  }

  breakUndoChain();
  entries_.push_back(UndoEntry{
      .label = std::move(pendingTransaction_->label),
      .before = std::move(pendingTransaction_->before),
      .after = std::move(after),
  });
  pendingTransaction_ = std::nullopt;
}

void UndoTimeline::abortTransaction() {
  pendingTransaction_ = std::nullopt;
}

void UndoTimeline::record(std::string_view label, UndoSnapshot before, UndoSnapshot after) {
  breakUndoChain();
  entries_.push_back(UndoEntry{
      .label = std::string(label),
      .before = std::move(before),
      .after = std::move(after),
  });
}

std::optional<UndoSnapshot> UndoTimeline::undo() {
  if (!canUndo()) {
    return std::nullopt;
  }

  if (!inUndoChain_) {
    // Start a new undo chain. The cursor walks backward through entries that existed before the
    // chain started. New undo entries appended during the chain are not revisited.
    inUndoChain_ = true;
    chainStart_ = entries_.size();
    undoCursor_ = chainStart_ - 1;
  }

  // Copy the target out of the vector before appending — `push_back` can
  // reallocate, invalidating any reference into `entries_`.
  const size_t cursorAtEntry = undoCursor_;
  UndoEntry targetCopy = entries_[cursorAtEntry];

  // Apply the "before" snapshot to reverse this entry.
  applySnapshot(targetCopy.before);

  // Record the undo as a new timeline entry (appended after chainStart_, so not revisited).
  entries_.push_back(UndoEntry{
      .label = "Undo: " + targetCopy.label,
      .before = targetCopy.after,
      .after = targetCopy.before,
      .undoOf = cursorAtEntry,
  });

  // Move cursor backward for the next undo in this chain.
  if (undoCursor_ == 0) {
    undoCursor_ = kCursorExhausted;
  } else {
    --undoCursor_;
  }

  return targetCopy.before;
}

bool UndoTimeline::canUndo() const {
  if (inUndoChain_) {
    return undoCursor_ != kCursorExhausted;
  }

  return !entries_.empty();
}

std::optional<std::string_view> UndoTimeline::nextUndoLabel() const {
  if (!canUndo()) {
    return std::nullopt;
  }

  if (inUndoChain_) {
    return entries_[undoCursor_].label;
  }

  return entries_.back().label;
}

void UndoTimeline::clear() {
  entries_.clear();
  pendingTransaction_ = std::nullopt;
  inUndoChain_ = false;
  undoCursor_ = 0;
  chainStart_ = 0;
}

void UndoTimeline::breakUndoChain() {
  inUndoChain_ = false;
}

}  // namespace donner::editor
