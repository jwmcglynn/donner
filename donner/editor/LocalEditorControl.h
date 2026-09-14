#pragma once
/// @file

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "nlohmann/json.hpp"

namespace donner::editor {

/// Opt-in same-user local socket; I/O runs separately and document work stays on the UI thread.
class LocalEditorControl {
public:
  LocalEditorControl();
  ~LocalEditorControl();
  LocalEditorControl(const LocalEditorControl&) = delete;
  LocalEditorControl& operator=(const LocalEditorControl&) = delete;

  /// Bind a new socket in an existing private directory and wake the editor on incoming work.
  bool start(std::string socketPath, std::function<void()> wake, std::string* error);
  /// Dispatch pending work on the UI thread. Call only at an idle renderer frame boundary.
  bool process(const std::function<std::optional<nlohmann::json>(const nlohmann::json&)>& handler);
  /// Stop receiving commands and join the I/O worker. Pending requests are cancelled.
  void stop();
  /// True when the I/O worker has a request waiting for the UI thread.
  bool hasPending() const;
  /// Connected endpoint path, or an empty string while inactive.
  std::string socketPath() const;
  /// Read a bounded private feedback checkpoint beside the socket; never follow a symlink.
  std::optional<nlohmann::json> readFeedback() const;
  /// Atomically save private feedback with mode 0600, outside the SVG and repository.
  bool writeFeedback(const nlohmann::json& data, std::string* error) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace donner::editor
