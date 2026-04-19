#pragma once
/// @file
///
/// **PipelinedRenderer** — a multi-threaded, in-process renderer that reuses
/// the sandbox wire format as a producer/consumer boundary between the main
/// thread (parser + driver + serializer) and a worker thread (deserializer +
/// real backend).
///
/// Motivation: the same observation that drives the sandbox design —
/// `RendererInterface` is a self-contained command stream — means the same
/// serialize/replay pipeline works across any boundary:
///
/// | Boundary           | Producer → Consumer         | Transport         |
/// |--------------------|----------------------------|-------------------|
/// | Process (sandbox)  | child → host               | pipe              |
/// | Thread (this file) | main → render worker       | `std::vector` buf |
/// | Time (frame inspect)| last frame → inspector pane| in-memory replay  |
/// | File (record/replay)| now → later                 | `.rnr` file       |
///
/// `PipelinedRenderer` targets the threading case. The main thread calls
/// `submit(document)`, which runs the parser/driver inline (fast, mostly
/// cache-local) and serializes the `RendererInterface` calls into a byte
/// buffer. The buffer is handed off to a worker thread that decodes it and
/// invokes a real backend (`Renderer`, resolving to the build-selected
/// backend). The main thread
/// returns as soon as serialization completes, freeing it to mutate the
/// document for frame N+1 while frame N rasterizes in parallel.
///
/// This is "newest wins": if `submit()` is called while a previous frame is
/// still rasterizing, the old frame is abandoned on the worker side. We're
/// building an interactive editor — the user cares about the current frame,
/// not the complete history.
///
/// **Why not just pass `SVGDocument` across threads?** Two reasons:
/// 1. The ECS registry inside `SVGDocument` has mutable caches (layout,
///    computed style) that the driver writes during traversal. Passing the
///    same document to a worker while the main thread is mutating it races.
///    Serializing freezes the snapshot.
/// 2. The wire buffer is a natural sync point. Once `submit()` returns, the
///    caller can mutate the document freely — the worker is operating on
///    bytes, not references.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {
class SVGDocument;
}  // namespace donner::svg

namespace donner::editor::sandbox {

/// One frame's worth of rendered pixels and metadata handed back to the main
/// thread after `PipelinedRenderer` completes a submission.
struct PipelinedFrame {
  /// Monotonically increasing frame sequence number. Frame N == submission N.
  uint64_t frameId = 0;
  /// RGBA8 snapshot at viewport resolution. Empty on failure.
  svg::RendererBitmap bitmap;
  /// Number of `kUnsupported` messages the replayer saw on this frame.
  uint32_t unsupportedCount = 0;
  /// True iff the wire stream decoded and rasterized cleanly.
  bool ok = false;
};

/// Factory for the real backend the worker thread should rasterize into.
/// Called exactly once, on the worker thread, before the first replay. The
/// worker owns the returned renderer for its whole lifetime.
using RendererFactory = std::unique_ptr<svg::RendererInterface> (*)();

/// Default factory that returns a `Renderer` backed by whichever renderer
/// backend was selected at build time (tiny-skia, Skia, or Geode).
std::unique_ptr<svg::RendererInterface> MakeDefaultRenderer();

/// A pipelined renderer that moves rasterization off the caller's thread.
///
/// Thread-safety: `submit()` and `acquireLatestFrame()` are main-thread-only.
/// The worker thread is internal and never exposed.
class PipelinedRenderer {
public:
  /// Construct the renderer and spawn the worker thread.
  ///
  /// @param factory Called on the worker thread to build the real backend
  ///   the first time a frame is rasterized. Defaults to
  ///   `MakeDefaultRenderer`.
  explicit PipelinedRenderer(RendererFactory factory = &MakeDefaultRenderer);

  /// Stop the worker thread and tear down state. Blocks until the worker
  /// observes the shutdown flag and exits.
  ~PipelinedRenderer();

  PipelinedRenderer(const PipelinedRenderer&) = delete;
  PipelinedRenderer& operator=(const PipelinedRenderer&) = delete;

  /// Serializes `document` on the calling thread and hands the resulting
  /// byte stream to the worker. Returns as soon as serialization completes —
  /// typically a handful of milliseconds.
  ///
  /// Overwrites any previously-queued frame that the worker hasn't started
  /// yet. If the worker is already rasterizing, its in-flight frame
  /// continues; the *next* frame it picks up is this one. This is the
  /// "newest wins" policy — see the file comment for rationale.
  ///
  /// @param document The document to render. The caller may mutate or free
  ///   it immediately after this call returns.
  /// @param width Viewport width in CSS pixels.
  /// @param height Viewport height in CSS pixels.
  /// @returns Frame id assigned to this submission. Pair with the id on the
  ///   returned `PipelinedFrame` to detect whether a call to
  ///   `acquireLatestFrame()` has produced the frame you care about yet.
  uint64_t submit(svg::SVGDocument& document, int width, int height);

  /// Returns the most recently completed frame, if any. The frame is moved
  /// out of the renderer; subsequent calls return `std::nullopt` until the
  /// worker completes another frame.
  [[nodiscard]] std::optional<PipelinedFrame> acquireLatestFrame();

  /// Blocks until a frame with `frameId >= target` is available and
  /// returns it. Intended for tests and synchronous callers that don't want
  /// to spin. Returns `std::nullopt` on shutdown.
  [[nodiscard]] std::optional<PipelinedFrame> waitForFrame(uint64_t target);

private:
  struct PendingFrame {
    uint64_t frameId = 0;
    std::vector<uint8_t> wire;
    int width = 0;
    int height = 0;
  };

  void workerMain();

  RendererFactory factory_;
  std::thread worker_;

  // Signals the worker that a new frame is queued. Guards `pending_` and
  // `shutdown_`.
  mutable std::mutex inboxMutex_;
  std::condition_variable inboxCv_;
  std::optional<PendingFrame> pending_;
  std::atomic<bool> shutdown_{false};

  // Guards `latest_` and wakes `waitForFrame()` callers. Distinct mutex so
  // the worker can publish a completed frame without blocking the main
  // thread that may be concurrently submitting the next one.
  mutable std::mutex outboxMutex_;
  std::condition_variable outboxCv_;
  std::optional<PipelinedFrame> latest_;

  std::atomic<uint64_t> nextFrameId_{1};
};

}  // namespace donner::editor::sandbox
