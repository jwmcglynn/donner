#pragma once
/// @file
///
/// Host-side driver for the sandbox child process. See
/// docs/design_docs/0023-editor_sandbox.md (S1 for the process model, S2 for the
/// wire format, S3 for the host-side replay path).
///
/// `SandboxHost` spawns `donner_parser_child` as a subprocess, pipes SVG bytes
/// to its stdin, reads a `RendererInterface` wire stream from its stdout, and
/// either replays the stream into a caller-provided backend
/// (`renderToBackend`) or runs the full host-side rasterizer + PNG encode
/// (`render`) as a convenience wrapper.
///
/// The host process never crashes on adversarial SVG input — the worst failure
/// mode is a `SandboxStatus::kCrashed` result with the previous document left
/// intact on the caller side.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::sandbox {

/// Outcome of a single render invocation on `SandboxHost`.
enum class SandboxStatus {
  kOk,            ///< Child exited 0 and the replay consumed the stream cleanly.
  kSpawnFailed,   ///< `posix_spawn` or pipe setup failed before the child ran.
  kWriteFailed,   ///< Host could not deliver the full SVG payload to the child.
  kReadFailed,    ///< Host could not read stdout/stderr from the child.
  kParseError,    ///< Child returned `kExitParseError` — malformed SVG.
  kUsageError,    ///< Child returned `kExitUsageError` — bad argv/dimensions.
  kRenderError,   ///< Child returned `kExitRenderError` — encoder bailed out.
  kCrashed,       ///< Child died via signal (SIGSEGV, SIGABRT, ...).
  kUnknownExit,   ///< Child exited with an unrecognized non-zero code.
  kWireMalformed, ///< Child exited 0 but its wire stream failed to decode.
};

/// Result payload for a render call. Fields are populated according to
/// `status` — see each field comment for when it's valid.
struct RenderResult {
  SandboxStatus status = SandboxStatus::kOk;
  /// Raw exit code (0 on success), or `-signal` when the child died on a signal.
  int exitCode = 0;
  /// Child's stderr captured verbatim. Always available regardless of status.
  std::string diagnostics;
  /// PNG-encoded image bytes. Populated only by `render()` on `kOk`.
  std::vector<uint8_t> png;
  /// Raw wire-format bytes read from the child's stdout. Always populated
  /// when the child reached `kEndFrame`, regardless of whether the host-side
  /// replay succeeded. Useful for debugging and for piping into a file
  /// recorder (see S4 in the design doc).
  std::vector<uint8_t> wire;
  /// Count of `kUnsupported` messages the replayer observed, if any. A
  /// non-zero value means the output is lossy compared to an in-process
  /// render (gradients, filters, masks, etc.).
  uint32_t unsupportedCount = 0;
};

/// Spawns and communicates with the sandbox child binary. Not thread-safe:
/// construct one instance per thread that needs to render.
class SandboxHost {
public:
  /// @param childBinaryPath Absolute path to the `donner_parser_child` executable.
  ///   Callers in tests should resolve this via `donner::Runfiles`.
  explicit SandboxHost(std::string childBinaryPath);

  /// Renders `svgBytes` into a caller-provided `RendererInterface` via the
  /// sandbox child + host-side `ReplayingRenderer`. The child's wire stream
  /// is decoded into `target` — after this call returns with `kOk`, the
  /// target has received `beginFrame()`, a sequence of drawing calls, and
  /// `endFrame()`, same as if the driver had run in-process.
  ///
  /// @param svgBytes Raw SVG source (may contain any bytes; not NUL-terminated).
  /// @param width Canvas width in CSS pixels.
  /// @param height Canvas height in CSS pixels.
  /// @param target Destination renderer. Must outlive this call.
  RenderResult renderToBackend(std::string_view svgBytes, int width, int height,
                               svg::RendererInterface& target);

  /// Convenience wrapper: renders via `renderToBackend` into a host-side
  /// `RendererTinySkia` and encodes the snapshot to PNG. Preserves the S1
  /// public contract so existing callers (tests, CLIs) don't have to change.
  RenderResult render(std::string_view svgBytes, int width, int height);

private:
  struct RawExitInfo {
    std::string diagnostics;
    int exitCode = 0;
    SandboxStatus status = SandboxStatus::kOk;
  };

  /// Spawns the child, pipes `svgBytes` into it, collects stdout + stderr,
  /// and classifies the exit. Does no decoding — callers decide whether to
  /// replay the stdout bytes into a `RendererInterface`.
  RawExitInfo spawnAndCollect(std::string_view svgBytes, int width, int height,
                              std::vector<uint8_t>& outStdout);

  std::string childBinaryPath_;
};

}  // namespace donner::editor::sandbox
