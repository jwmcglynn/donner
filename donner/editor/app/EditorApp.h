#pragma once
/// @file
///
/// **RenderSession** — the headless render-session core used by REPL and tooling. Owns the
/// current document state (URI, raw bytes, rendered bitmap) and exposes a narrow
/// state-transition API that higher layers can call into.
///
/// This is deliberately UI-free: no ImGui, no GLFW, no GL, no stdin/stdout.
/// The UI layer takes an `EditorApp&` and drives it — tests can do the
/// same.
/// right now the only mutation the MVP supports is "navigate to a new URI
/// and re-render."
///
/// Rendering is performed in-process through `svg::Renderer`.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::app {

/// High-level state the editor can be in. The REPL uses this to decide
/// what status chip / prompt decoration to show the user.
enum class RenderSessionStatus {
  kEmpty,        ///< No document loaded yet.
  kLoading,      ///< Navigation in flight (rare — rendering is synchronous today).
  kRendered,     ///< A bitmap is available and was rendered cleanly.
  kFetchError,   ///< Source loading failed. Previous bitmap is retained.
  kParseError,   ///< Parser returned an error. Previous bitmap is retained.
  kRenderError,  ///< Rasterization failed post-parse. Previous bitmap is retained.
};

/// Configuration knobs for loading SVG source bytes.
struct SourceLoadOptions {
  /// Directory used to resolve relative paths. Defaults to the current working directory.
  std::filesystem::path baseDirectory = std::filesystem::current_path();
  /// Maximum number of bytes the loader will read from a single file.
  std::size_t maxFileBytes = 100u * 1024u * 1024u;
};

struct RenderSessionOptions {
  /// Default viewport used for navigations that don't carry an explicit
  /// width/height. Matches typical desktop editor previews.
  int defaultWidth = 512;
  int defaultHeight = 384;

  /// Options forwarded to the internal source loader. Most tests override
  /// `baseDirectory` to isolate fixture files from the developer's CWD.
  SourceLoadOptions sourceOptions;
};

/// Snapshot of the most recent navigation result. All fields are read-only
/// after `EditorApp::navigate` returns — subsequent navigations produce a
/// new snapshot.
struct RenderSessionSnapshot {
  RenderSessionStatus status = RenderSessionStatus::kEmpty;
  /// Resolved URI the snapshot corresponds to, or empty for `kEmpty`.
  std::string uri;
  /// RGBA snapshot of the rendered frame. Empty if no render has succeeded.
  svg::RendererBitmap bitmap;
  /// Human-readable status line suitable for display in a UI chip or a
  /// terminal prompt.
  std::string message;
};

class RenderSession {
public:
  explicit RenderSession(RenderSessionOptions options = {});
  ~RenderSession();

  RenderSession(const RenderSession&) = delete;
  RenderSession& operator=(const RenderSession&) = delete;

  /// Fetches `uri`, drives the renderer at the configured viewport, and
  /// blocks until a new frame is available on the worker. Returns the
  /// updated snapshot. On any error, the previous successful frame's
  /// bitmap is retained (accessible via `lastGoodBitmap()`) and the new
  /// snapshot's status reflects the failure.
  const RenderSessionSnapshot& navigate(std::string_view uri);

  /// Fetches the current URI again with the same viewport, useful for
  /// "reload the file after editing it in an external editor" workflows.
  /// No-op when no URI is loaded yet.
  const RenderSessionSnapshot& reload();

  /// Re-renders the currently-loaded document at a new viewport. Cheaper
  /// than a full navigate because no fetch happens.
  const RenderSessionSnapshot& resize(int width, int height);

  [[nodiscard]] const RenderSessionSnapshot& current() const { return current_; }
  [[nodiscard]] int width() const { return width_; }
  [[nodiscard]] int height() const { return height_; }

  /// The most recent successful bitmap, regardless of the current status.
  /// Allows UIs to keep showing the last good render while an error chip
  /// covers the new failure — matches the address bar's "keep previous
  /// document on screen" behavior from the design doc.
  [[nodiscard]] const svg::RendererBitmap& lastGoodBitmap() const { return lastGoodBitmap_; }

  /// Enable or disable filesystem watch mode. When enabled,
  /// `pollForChanges()` checks whether the loaded file's mtime has changed
  /// and auto-reloads if so.
  void setWatchEnabled(bool v) { watchEnabled_ = v; }
  [[nodiscard]] bool watchEnabled() const { return watchEnabled_; }

  /// Checks if the currently-loaded file's modification time has changed
  /// since the last fetch. If yes (and watch is enabled), calls `reload()`
  /// internally and returns true. Returns false when watch is disabled, no
  /// file is loaded, or the file hasn't changed.
  bool pollForChanges();

private:
  /// Parses and renders the cached raw SVG bytes, then updates
  /// `current_` + `lastGood*_` according to the outcome. Common helper for
  /// navigate/reload/resize.
  const RenderSessionSnapshot& renderCachedBytes(std::string_view uri);

  RenderSessionOptions options_;

  int width_;
  int height_;

  RenderSessionSnapshot current_;
  std::vector<uint8_t> rawBytes_;  ///< Last successfully-fetched bytes, for reload/resize.
  svg::RendererBitmap lastGoodBitmap_;

  bool watchEnabled_ = false;
  /// Path of the currently-loaded file, used for mtime polling.
  std::filesystem::path loadedPath_;
  /// The file's mtime at the time of the last successful fetch.
  std::filesystem::file_time_type lastModTime_{};
};

}  // namespace donner::editor::app
