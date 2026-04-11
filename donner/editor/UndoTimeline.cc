#include "donner/editor/UndoTimeline.h"

#include "donner/svg/SVGGraphicsElement.h"

namespace donner::editor {

UndoSnapshot captureTransformSnapshot(const svg::SVGElement& element) {
  auto graphicsElement = element.cast<svg::SVGGraphicsElement>();
  return UndoSnapshot{
      .element = element,
      .transform = graphicsElement.elementFromWorld(),
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

  const UndoEntry& target = entries_[undoCursor_];

  // Apply the "before" snapshot to reverse this entry.
  applySnapshot(target.before);

  // Record the undo as a new timeline entry (appended after chainStart_, so not revisited).
  entries_.push_back(UndoEntry{
      .label = "Undo: " + target.label,
      .before = target.after,
      .after = target.before,
      .undoOf = undoCursor_,
  });

  // Move cursor backward for the next undo in this chain.
  if (undoCursor_ == 0) {
    undoCursor_ = kCursorExhausted;
  } else {
    --undoCursor_;
  }

  return target.before;
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
