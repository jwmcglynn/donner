#pragma once
/// @file
///
/// `AsyncRenderer` runs `svg::Renderer::draw()` + `takeSnapshot()` on a
/// dedicated worker thread so heavy renders don't block the UI thread.
///
/// ## Threading model
///
/// The worker thread owns the Renderer, the SVGDocument (exclusively),
/// and the EditorApp reference (for overlay chrome) **only** during an
/// active render. The UI thread must not mutate the document or touch
/// the Renderer while a render is in flight.
///
/// UI thread flow per frame:
/// 1. `pollResult()` — if a render just finished, pick up the bitmap.
/// 2. If NOT busy: process mutations via `flushFrame()`.
/// 3. If NOT busy AND a new render is needed: `requestRender()`.
/// 4. If busy: skip flushFrame, leave pending mutations in the queue.
///    They apply on the next idle frame. Input (drags, typing) still
///    gets processed and queued — just not dispatched to the ECS.
///
/// The safety invariant: between `requestRender()` and a non-`nullopt`
/// return from `pollResult()`, the UI thread must not call any method
/// on the `Renderer`, must not mutate the `SVGDocument`, and must not
/// touch state the overlay renderer reads (selection, etc. — those are
/// snapshotted at request time, see `RenderRequest`).

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include "donner/svg/SVGElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {
class SVGDocument;
}

namespace donner::editor {

/// Per-request handoff data captured at render-request time so the
/// worker has everything it needs without touching live UI state.
struct RenderRequest {
  svg::Renderer* renderer = nullptr;
  svg::SVGDocument* document = nullptr;
  /// Snapshot of the selection at request time (used for overlay chrome).
  /// The worker holds this optional by value, so if the UI thread clears
  /// the selection mid-render the worker still draws the pre-render chrome.
  std::optional<svg::SVGElement> selection;
};

class AsyncRenderer {
public:
  AsyncRenderer();
  ~AsyncRenderer();

  AsyncRenderer(const AsyncRenderer&) = delete;
  AsyncRenderer& operator=(const AsyncRenderer&) = delete;
  AsyncRenderer(AsyncRenderer&&) = delete;
  AsyncRenderer& operator=(AsyncRenderer&&) = delete;

  /// Returns true while a render is in flight on the worker thread.
  /// The UI thread must not touch the `Renderer` or mutate the
  /// `SVGDocument` while this returns true.
  [[nodiscard]] bool isBusy() const { return busy_.load(std::memory_order_acquire); }

  /// Post a render request to the worker. Non-blocking. Transitions
  /// the worker from idle → busy. Caller must have checked `!isBusy()`
  /// first; calling while busy is a programmer error.
  void requestRender(const RenderRequest& request);

  /// If a render has completed since the last call, returns the
  /// resulting bitmap and transitions the worker back to idle. Returns
  /// `std::nullopt` if no render is pending-ready (either still busy
  /// or idle with nothing to hand off).
  std::optional<svg::RendererBitmap> pollResult();

private:
  void workerLoop();

  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  enum class State : std::uint8_t {
    Idle,    ///< No work. Worker is blocked on `cv_`.
    Busy,    ///< Render in progress on the worker.
    Done,    ///< Render finished; bitmap available in `resultBitmap_`.
    Shutdown ///< Destructor requested shutdown; worker exits.
  };

  State state_ = State::Idle;
  std::atomic<bool> busy_{false};

  RenderRequest pendingRequest_;
  svg::RendererBitmap resultBitmap_;
};

}  // namespace donner::editor
