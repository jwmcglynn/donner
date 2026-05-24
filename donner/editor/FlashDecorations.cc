#include "donner/editor/FlashDecorations.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace donner::editor {

void FlashDecorations::flash(SourceByteRange byteRange, TimePoint now, std::size_t bufferSize) {
  byteRange.start = std::min(byteRange.start, bufferSize);
  byteRange.end = std::min(byteRange.end, bufferSize);
  if (byteRange.end <= byteRange.start) {
    return;
  }

  if (flashes_.size() >= kMaxFlashes) {
    flashes_.erase(flashes_.begin(), flashes_.begin() + (flashes_.size() - kMaxFlashes + 1));
  }
  flashes_.push_back(Flash{.byteRange = byteRange, .startTime = now});
}

void FlashDecorations::applySourceEdit(std::size_t offset, std::size_t removedLength,
                                       std::size_t insertedLength, std::size_t newBufferSize) {
  const std::size_t oldEditEnd = offset + removedLength;
  const long long delta =
      static_cast<long long>(insertedLength) - static_cast<long long>(removedLength);

  const auto shiftOffset = [delta, newBufferSize](std::size_t value) {
    const long long shifted = static_cast<long long>(value) + delta;
    if (shifted <= 0) {
      return std::size_t{0};
    }
    return std::min(static_cast<std::size_t>(shifted), newBufferSize);
  };

  std::vector<Flash> adjusted;
  adjusted.reserve(flashes_.size());
  for (Flash flash : flashes_) {
    if (flash.byteRange.end <= offset) {
      adjusted.push_back(flash);
      continue;
    }

    if (flash.byteRange.start >= oldEditEnd) {
      flash.byteRange.start = shiftOffset(flash.byteRange.start);
      flash.byteRange.end = shiftOffset(flash.byteRange.end);
      if (flash.byteRange.end > flash.byteRange.start) {
        adjusted.push_back(flash);
      }
    }
  }
  flashes_ = std::move(adjusted);
}

void FlashDecorations::tick(TimePoint now) {
  std::erase_if(flashes_, [now](const Flash& flash) {
    return AgeSeconds(now, flash.startTime) >= kDurationSeconds;
  });
}

std::vector<ActiveFlash> FlashDecorations::activeBackgrounds(TimePoint now) const {
  std::vector<ActiveFlash> result;
  result.reserve(flashes_.size());
  for (const Flash& flash : flashes_) {
    const float age = AgeSeconds(now, flash.startTime);
    if (age < kDurationSeconds) {
      result.push_back(ActiveFlash{
          .byteRange = flash.byteRange,
          .intensity = std::clamp(1.0f - age / kDurationSeconds, 0.0f, 1.0f),
      });
    }
  }
  return result;
}

std::optional<float> FlashDecorations::nextWakeSeconds(TimePoint now) const {
  for (const Flash& flash : flashes_) {
    if (AgeSeconds(now, flash.startTime) < kDurationSeconds) {
      return 1.0f / 60.0f;
    }
  }
  return std::nullopt;
}

float FlashDecorations::AgeSeconds(TimePoint now, TimePoint startTime) {
  return std::chrono::duration<float>(now - startTime).count();
}

}  // namespace donner::editor
