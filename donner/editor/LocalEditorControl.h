#pragma once
/// @file

#include <functional>
#include <iosfwd>
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
/// Run the stdio MCP adapter for an already-running native editor.
/// @param socketPath Private local endpoint selected by the user.
/// @param input Newline-delimited MCP input.
/// @param output Newline-delimited MCP responses, without diagnostic text.
/// @param errors Diagnostic output.
/// @return Zero on clean input EOF, or nonzero for malformed input.
int RunEditorControlStdio(const std::string& socketPath, std::istream& input, std::ostream& output,
                          std::ostream& errors);

}  // namespace donner::editor
