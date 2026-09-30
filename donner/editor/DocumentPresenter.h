#pragma once
/// @file
/// Installs one immutable presentation for all document and chrome passes.

#include <functional>
#include <memory>

#include "donner/editor/FramePresentation.h"

namespace donner::editor {

/// Sink that installs or clears both framebuffer passes together.
using FramePresentationSink = std::function<void(std::shared_ptr<const FramePresentation>)>;

/// Owns the installed frame; raster and chrome callbacks receive the same immutable object.
class DocumentPresenter {
public:
  /// @param sink Installs every draw pass for the frame, or clears them on null.
  explicit DocumentPresenter(FramePresentationSink sink);

  /// Install a sealed frame. An older frame cannot supersede a newer installed frame.
  /// @param frame Complete presentation, or null to clear the draw callbacks.
  /// @return False when a stale or conflicting frame identity was refused.
  bool present(std::shared_ptr<const FramePresentation> frame);

  /// Frame retained by both presentation passes.
  [[nodiscard]] std::shared_ptr<const FramePresentation> currentFrame() const { return frame_; }

private:
  FramePresentationSink sink_;
  std::shared_ptr<const FramePresentation> frame_;
};

}  // namespace donner::editor
