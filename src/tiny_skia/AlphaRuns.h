#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tiny_skia {

using AlphaRun = std::optional<std::uint16_t>;
using AlphaU8 = std::uint8_t;
using LengthU32 = std::uint32_t;

class AlphaRuns {
 public:
  explicit AlphaRuns(LengthU32 width);

  static std::uint8_t catchOverflow(std::uint16_t alpha);
  [[nodiscard]] bool empty() const;

  void reset(LengthU32 width);

  std::size_t add(std::uint32_t x, AlphaU8 startAlpha, std::size_t middleCount,
                  AlphaU8 stopAlpha, std::uint8_t maxValue, std::size_t offsetX);

  static void breakRun(std::span<AlphaRun> runs, std::span<std::uint8_t> alpha, std::size_t x,
                       std::size_t count);
  static void breakAt(std::span<std::uint8_t> alpha, std::span<AlphaRun> runs, std::int32_t x);

  std::vector<AlphaRun> runs;
  std::vector<std::uint8_t> alpha;
};

}  // namespace tiny_skia
