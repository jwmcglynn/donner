#pragma once
/// @file

#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>

namespace donner::editor {

/// Half-open byte range in the current source buffer.
struct SourceByteRange {
  std::size_t start = 0;  ///< Inclusive byte offset.
  std::size_t end = 0;    ///< Exclusive byte offset.

  /// Equality operator.
  bool operator==(const SourceByteRange& other) const = default;
};

/// Visible flash decoration for the current frame.
struct ActiveFlash {
  SourceByteRange byteRange;  ///< Source span to highlight.
  float intensity = 0.0f;     ///< Fade intensity in [0, 1].
};

/// Per-view transient source-change highlights.
class FlashDecorations {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  /// Maximum number of simultaneous flashes retained by the view.
  static constexpr std::size_t kMaxFlashes = 64;
  /// Default fade duration for a source flash.
  static constexpr float kDurationSeconds = 0.4f;

  /**
   * Add a flash for \p byteRange.
   *
   * @param byteRange Source range to highlight.
   * @param now Start time for the flash.
   * @param bufferSize Current source buffer size for clamping.
   */
  void flash(SourceByteRange byteRange, TimePoint now, std::size_t bufferSize);

  /**
   * Shift or drop existing flashes after a source edit.
   *
   * @param offset Edit start offset in the old buffer.
   * @param removedLength Removed byte count in the old buffer.
   * @param insertedLength Inserted byte count in the new buffer.
   * @param newBufferSize Source buffer size after the edit.
   */
  void applySourceEdit(std::size_t offset, std::size_t removedLength, std::size_t insertedLength,
                       std::size_t newBufferSize);

  /**
   * Remove expired flashes.
   *
   * @param now Current time.
   */
  void tick(TimePoint now);

  /**
   * Return visible flashes for the current frame.
   *
   * @param now Current time.
   */
  [[nodiscard]] std::vector<ActiveFlash> activeBackgrounds(TimePoint now) const;

  /**
   * Return seconds until the next fade frame should be drawn.
   *
   * @param now Current time.
   */
  [[nodiscard]] std::optional<float> nextWakeSeconds(TimePoint now) const;

private:
  struct Flash {
    SourceByteRange byteRange;
    TimePoint startTime;
  };

  [[nodiscard]] static float AgeSeconds(TimePoint now, TimePoint startTime);

  std::vector<Flash> flashes_;
};

}  // namespace donner::editor
