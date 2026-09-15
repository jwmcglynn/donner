#pragma once
/// @file

#include <chrono>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace donner::editor {

/// Opt-in same-user local socket; I/O runs separately and document work stays on the UI thread.
class LocalEditorControl {
public:
  /// Transport-owned identity established by the MCP initialization handshake.
  struct AgentSession {
    std::uint64_t connectionId = 0;
    std::string name;
    std::string version;
  };
  /// UI dispatch receives the identity of the connection that supplied the request.
  using Handler =
      std::function<std::optional<nlohmann::json>(const nlohmann::json&, const AgentSession&)>;

  /// @param idleTimeout Maximum silent connection lease, positive and at most sixty seconds.
  explicit LocalEditorControl(std::chrono::milliseconds idleTimeout = std::chrono::seconds(60));
  ~LocalEditorControl();
  LocalEditorControl(const LocalEditorControl&) = delete;
  LocalEditorControl& operator=(const LocalEditorControl&) = delete;

  /// Bind a new socket in an existing private directory and wake the editor on incoming work.
  bool start(std::string socketPath, std::function<void()> wake, std::string* error);
  /// Dispatch each queued request once on the UI thread. Deferred requests do not block peers.
  /// Call only at an idle renderer frame boundary.
  bool process(const Handler& handler);
  /// Stop receiving commands and join the I/O worker. Undispatched requests are cancelled.
  /// Disconnect cannot undo an operation already executing on the UI thread.
  void stop();
  /// True when the I/O worker has a request waiting for the UI thread.
  bool hasPending() const;
  /// Snapshot of initialized, connected agents; listening alone returns an empty list.
  std::vector<AgentSession> connectedAgents() const;
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
/// Timing policy for the native adapter's bounded connection liveness checks.
struct EditorControlStdioOptions {
  std::chrono::milliseconds heartbeatInterval{15000};
  std::chrono::milliseconds requestTimeout{35000};
};
/// Run a persistent, multiplexed stdio MCP adapter for an already-running editor.
/// @param socketPath Private local endpoint selected by the user.
/// @param inputFd Exclusively owned input descriptor, retained by the caller.
/// @param outputFd Exclusively owned output descriptor, retained by the caller.
/// @param errors Diagnostic output, separate from protocol frames.
/// @param options Bounded liveness timings; no automatic reconnect or request replay.
/// @return Zero after EOF and pending replies drain, 1 for transport failure, 2 for invalid input.
int RunEditorControlStdio(const std::string& socketPath, int inputFd, int outputFd,
                          std::ostream& errors, EditorControlStdioOptions options = {});

}  // namespace donner::editor
