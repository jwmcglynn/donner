#pragma once
/// @file
/// Bounds how many of a renderer's frames the GPU may be behind by.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>

#include "donner/gpu/Descriptors.h"

namespace donner::geode {

/**
 * Frames-in-flight limit for one renderer.
 *
 * Every frame a renderer submits retains what its GPU work reads until that work completes:
 * staged uploads, command buffers, and the transient surfaces it named. A renderer that submits
 * faster than the GPU retires therefore grows without bound, and a backend that limits its queue
 * instead of the renderer makes the CPU wait in the middle of a frame, at whichever submission
 * happens to find the queue full. The pacer moves that wait to the frame boundary and bounds it:
 * before a frame begins, it waits until fewer than \ref maxFramesInFlight earlier frames are
 * still executing, so at most that many frames are ever in flight.
 *
 * A frame's GPU work is represented by the serial of the last submission made while it was open,
 * which completes only after everything submitted before it.
 *
 * The pacer only tracks serials; the renderer supplies how completion is observed and waited on
 * through \ref Backend, so the policy is testable without a GPU. A backend that cannot block the
 * calling thread (a browser's main thread, where completions only arrive by returning to the
 * event loop) supplies no wait; frames are then admitted without waiting and the embedder paces
 * them, normally through `requestAnimationFrame`, reading \ref framesInFlight to skip a frame while
 * the GPU is behind.
 *
 * Not thread-safe; owned by one renderer.
 */
class GeodeFramePacer {
public:
  /// Frames in flight a renderer allows unless configured otherwise: one frame executing on the
  /// GPU while the next is recorded.
  static constexpr uint32_t kDefaultMaxFramesInFlight = 2;

  /// Largest accepted limit. A deeper queue only adds latency and retained memory.
  static constexpr uint32_t kMaxFramesInFlightLimit = 8;

  /// How the pacer observes and waits for GPU completion.
  struct Backend {
    /// Highest submission serial whose work has completed.
    std::function<uint64_t()> completedSerial;
    /// Blocks until \p serial completes or the GPU stops making progress. Empty when the calling
    /// thread may not block, in which case frames are admitted without waiting.
    std::function<gpu::SerialWaitResult(uint64_t serial)> waitForSerial;
  };

  /// Outcome of admitting one frame.
  struct Admission {
    uint32_t waits = 0;    //!< Earlier frames this admission waited for.
    bool stalled = false;  //!< A wait gave up because the GPU stopped making progress.
    bool failed = false;   //!< A wait ended because the device can no longer complete work.
    std::chrono::milliseconds waited{0};  //!< Time spent waiting.
  };

  /// Sets the limit. Zero disables pacing; values above \ref kMaxFramesInFlightLimit clamp to it.
  /// @param frames Most frames that may be in flight once a frame is submitted.
  void setMaxFramesInFlight(uint32_t frames) {
    maxFramesInFlight_ = std::min(frames, kMaxFramesInFlightLimit);
  }

  /// The limit; zero when pacing is disabled.
  [[nodiscard]] uint32_t maxFramesInFlight() const { return maxFramesInFlight_; }

  /**
   * Waits, before a frame begins, until it can be submitted without exceeding the limit.
   *
   * After a stall or failure the pending frames are forgotten: nothing they wait for will
   * complete, and the caller declares the device lost.
   *
   * @param backend Completion source and wait.
   */
  Admission admitFrame(const Backend& backend) {
    Admission admission;
    retireCompleted(backend);
    if (maxFramesInFlight_ == 0 || !backend.waitForSerial) {
      return admission;
    }
    while (pending_.size() >= maxFramesInFlight_) {
      const gpu::SerialWaitResult wait = backend.waitForSerial(pending_.front());
      admission.waited += wait.waited;
      if (wait.end != gpu::SerialWaitEnd::Completed) {
        admission.stalled = wait.end == gpu::SerialWaitEnd::Stalled;
        admission.failed = wait.end == gpu::SerialWaitEnd::Failed;
        pending_.clear();
        return admission;
      }
      ++admission.waits;
      pending_.pop_front();
      retireCompleted(backend);
    }
    return admission;
  }

  /// Records that a frame was submitted. @param serial Serial of the last submission made while
  /// the frame was open; a frame that submitted nothing new adds no entry.
  void frameSubmitted(uint64_t serial) {
    if (serial != 0 && (pending_.empty() || serial > pending_.back())) {
      pending_.push_back(serial);
    }
  }

  /// Submitted frames whose work has not completed, observed without blocking.
  /// @param backend Completion source.
  [[nodiscard]] uint32_t framesInFlight(const Backend& backend) {
    retireCompleted(backend);
    return static_cast<uint32_t>(pending_.size());
  }

  /// Forgets every pending frame, for a device that will complete none of them.
  void clear() { pending_.clear(); }

private:
  void retireCompleted(const Backend& backend) {
    if (pending_.empty() || !backend.completedSerial) {
      return;
    }
    const uint64_t completed = backend.completedSerial();
    while (!pending_.empty() && pending_.front() <= completed) {
      pending_.pop_front();
    }
  }

  uint32_t maxFramesInFlight_ = kDefaultMaxFramesInFlight;
  std::deque<uint64_t> pending_;  //!< Submission serials of frames in flight, oldest first.
};

}  // namespace donner::geode
