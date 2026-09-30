#include "donner/editor/DocumentPresenter.h"

#include <utility>

namespace donner::editor {

DocumentPresenter::DocumentPresenter(FramePresentationSink sink) : sink_(std::move(sink)) {}

bool DocumentPresenter::present(std::shared_ptr<const FramePresentation> frame) {
  if (frame_ != nullptr && frame != nullptr &&
      (frame->frameId() < frame_->frameId() ||
       (frame->frameId() == frame_->frameId() && frame != frame_))) {
    return false;
  }
  frame_ = std::move(frame);
  if (sink_) {
    sink_(frame_);
  }
  return true;
}

}  // namespace donner::editor
