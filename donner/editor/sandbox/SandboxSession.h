#pragma once
/// @file
///
/// **SandboxSession** — long-lived host/child IPC owner.
///
/// Replaces the per-render `posix_spawn` model of `SandboxHost` (see S7 in
/// docs/design_docs/0023-editor_sandbox.md) with a persistent child process
/// that services many requests over the same stdin/stdout pipes. One session
/// per editor instance; the full editor opens it at startup and keeps it
/// alive for the editor's lifetime.
///
/// The session multiplexes requests through an internal writer thread
/// (drains an SPSC inbox, frames bytes onto the child's stdin) and a reader
/// thread (decodes framed responses from stdout, fulfills the matching
/// `std::promise`). Requests are serviced FIFO; callers may have several in
/// flight but the wire is linearized.
///
/// **Crash policy.** If the child exits unexpectedly (SIGSEGV, SIGABRT,
/// malicious SVG that trips a parser bug, etc.) the session:
///   1. Fulfills every in-flight future with `SandboxStatus::kCrashed` and
///      the captured stderr tail.
///   2. Rebuilds the pipes and re-spawns the child (same hardening profile).
///   3. Returns to accepting new requests — callers observe a one-off error,
///      not a permanent broken state.
///
/// **Clean shutdown.** The destructor closes the child's stdin (which it
/// reads as EOF → exit cleanly), joins the reader/writer threads, and
/// `SIGKILL`s the child if it hasn't exited within 100 ms.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/editor/sandbox/SandboxHost.h"  // SandboxStatus, for result reuse.

namespace donner::editor::sandbox {

/// Snapshot of an abnormal child exit, retained by the session so the editor
/// can surface a "sandbox crashed N seconds ago" chip without polling.
struct ExitInfo {
  /// Raw exit code (0 on clean exit), or `-signal` when the child died on a
  /// signal.
  int exitCode = 0;
  /// Classification matching `SandboxStatus` so callers already branching on
  /// that enum can reuse their code paths.
  SandboxStatus status = SandboxStatus::kOk;
  /// Tail of the child's stderr (truncated to a few KB). Useful to show the
  /// user alongside the chip.
  std::string diagnostics;
};

/// Opaque request/response payloads. The session doesn't interpret the
/// bytes — callers (S8 onward) encode the session-level opcode + payload via
/// `SessionProtocol.h`, and decode the response body the same way. Keeping
/// the session dumb lets the protocol evolve without touching this class.
struct WireRequest {
  std::vector<uint8_t> bytes;
};

struct WireResponse {
  SandboxStatus status = SandboxStatus::kOk;
  /// Raw response payload. Empty when `status != kOk`.
  std::vector<uint8_t> bytes;
  /// Stderr captured between this request and the previous one. Rarely
  /// useful, but preserved for diagnostics.
  std::string diagnostics;
};

struct SandboxSessionOptions {
  /// Absolute path to `donner_parser_child`. Typically resolved via
  /// `donner::Runfiles` at caller startup.
  std::string childBinaryPath;

  /// If true, an abnormal exit triggers an automatic respawn before the next
  /// `submit`. Default on; tests may disable to observe the crashed state.
  bool autoRespawn = true;

  /// Maximum stderr tail retained per exit (bytes). Truncated from the head
  /// so the most recent output survives.
  std::size_t maxStderrTailBytes = 16u * 1024u;
};

/// Thread-safe long-lived session wrapping one `donner_parser_child`.
class SandboxSession {
public:
  explicit SandboxSession(SandboxSessionOptions options);

  /// Destructor sends the shutdown sequence and joins internal threads. Not
  /// movable — the reader/writer threads reference `this`.
  ~SandboxSession();

  SandboxSession(const SandboxSession&) = delete;
  SandboxSession& operator=(const SandboxSession&) = delete;
  SandboxSession(SandboxSession&&) = delete;
  SandboxSession& operator=(SandboxSession&&) = delete;

  /// Enqueues a request and returns a future that resolves when the child
  /// produces a response (or crashes before doing so).
  ///
  /// Safe to call from any thread. Requests are dispatched FIFO.
  [[nodiscard]] std::future<WireResponse> submit(WireRequest request);

  /// True iff the child is currently alive and accepting stdin.
  [[nodiscard]] bool childAlive() const;

  /// Info about the most recent abnormal exit, if any. Cleared after a
  /// successful respawn produces a clean response.
  [[nodiscard]] std::optional<ExitInfo> lastExit() const;

  /// Installs (or replaces) a callback that fires on the reader thread each
  /// time the session observes a `kDiagnostic` message from the child. The
  /// callback should not block — the editor's implementation typically
  /// appends to a ring buffer that the UI thread drains.
  using DiagnosticCallback = std::function<void(std::string_view)>;
  void setDiagnosticCallback(DiagnosticCallback cb);

private:
  struct Pending {
    uint64_t requestId = 0;
    std::promise<WireResponse> promise;
    std::vector<uint8_t> bytes;  // Moved into the writer thread's frame buffer.
  };

  class ChildProcess;  // PImpl over the POSIX plumbing. Owns pipes + pid.

  // Writer thread: drains pendingQueue_, frames each request onto stdin.
  void writerMain();
  // Reader thread: parses framing from stdout, resolves promises, surfaces
  // diagnostics. Also observes EOF/crash and triggers respawn.
  void readerMain();

  // Spins a fresh ChildProcess. Called from both the ctor and the respawn
  // path. Returns true on success; false leaves the session in a dead state
  // whose only legal operation is destruction.
  bool spawnChild();

  // Fulfills every in-flight promise with kCrashed + the captured tail.
  // Called from readerMain when EOF is observed, before respawn.
  void failInFlight(SandboxStatus status, const std::string& diagnostics);

  SandboxSessionOptions options_;
  std::unique_ptr<ChildProcess> child_;

  // Requests waiting to be written to the child.
  mutable std::mutex inboxMutex_;
  std::condition_variable inboxCv_;
  std::deque<Pending> inbox_;
  bool shutdown_ = false;

  // Requests written to the child and awaiting a response. Keyed by
  // requestId to survive out-of-order completion if the protocol ever
  // allows it (today it's strictly FIFO so this is a safety net).
  mutable std::mutex awaitingMutex_;
  std::deque<Pending> awaiting_;

  // Diagnostic sink + last-exit bookkeeping.
  mutable std::mutex diagnosticsMutex_;
  DiagnosticCallback diagnosticCallback_;
  std::optional<ExitInfo> lastExit_;

  std::thread writerThread_;
  std::thread readerThread_;
  std::atomic<uint64_t> nextRequestId_{1};
  std::atomic<bool> childAlive_{false};
};

}  // namespace donner::editor::sandbox
