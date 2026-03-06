#include "tiny_skia/scan/Path.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "tiny_skia/FixedPoint.h"

namespace tiny_skia {

namespace {

constexpr float kConservativeRoundBias = 0.5f + 1.5f / static_cast<float>(fdot6::one);

std::int32_t saturatingToInt32(double value) {
  constexpr auto kMax = static_cast<double>(std::numeric_limits<std::int32_t>::max());
  constexpr auto kMin = static_cast<double>(std::numeric_limits<std::int32_t>::min());

  if (value > kMax) {
    return std::numeric_limits<std::int32_t>::max();
  }
  if (value < kMin) {
    return std::numeric_limits<std::int32_t>::min();
  }
  return static_cast<std::int32_t>(value);
}

std::int32_t roundDownToInt(float x) {
  const auto adjusted = static_cast<double>(x) - kConservativeRoundBias;
  return saturatingToInt32(std::ceil(adjusted));
}

std::int32_t roundUpToInt(float x) {
  const auto adjusted = static_cast<double>(x) + kConservativeRoundBias;
  return saturatingToInt32(std::floor(adjusted));
}

std::optional<IntRect> conservativeRoundToInt(const Rect& rect) {
  const auto left = roundDownToInt(rect.left());
  const auto top = roundDownToInt(rect.top());
  const auto right = roundUpToInt(rect.right());
  const auto bottom = roundUpToInt(rect.bottom());

  const auto width = static_cast<std::int64_t>(right) - left;
  const auto height = static_cast<std::int64_t>(bottom) - top;
  if (width <= 0 || height <= 0) {
    return std::nullopt;
  }
  if (width > std::numeric_limits<std::uint32_t>::max() ||
      height > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }

  return IntRect::fromXYWH(left, top, static_cast<std::uint32_t>(width),
                           static_cast<std::uint32_t>(height));
}

void removeEdge(std::size_t currIdx, std::vector<Edge>& edges) {
  const auto prev = edges[currIdx].asLine().prev.value();
  const auto next = edges[currIdx].asLine().next.value();

  edges[prev].asLine().next = next;
  edges[next].asLine().prev = prev;
}

void insertEdgeAfter(std::size_t currIdx, std::size_t afterIdx, std::vector<Edge>& edges) {
  edges[currIdx].asLine().prev = static_cast<std::uint32_t>(afterIdx);
  edges[currIdx].asLine().next = edges[afterIdx].asLine().next;

  const auto afterNext = static_cast<std::size_t>(edges[afterIdx].asLine().next.value());
  edges[afterNext].asLine().prev = static_cast<std::uint32_t>(currIdx);
  edges[afterIdx].asLine().next = static_cast<std::uint32_t>(currIdx);
}

void backwardInsertEdgeBasedOnX(std::size_t currIdx, std::vector<Edge>& edges) {
  const auto x = edges[currIdx].asLine().x;
  auto prevIdx = static_cast<std::size_t>(edges[currIdx].asLine().prev.value());

  while (prevIdx != 0) {
    if (edges[prevIdx].asLine().x > x) {
      prevIdx = static_cast<std::size_t>(edges[prevIdx].asLine().prev.value());
    } else {
      break;
    }
  }

  const auto nextIdx = static_cast<std::size_t>(edges[prevIdx].asLine().next.value());
  if (nextIdx != currIdx) {
    removeEdge(currIdx, edges);
    insertEdgeAfter(currIdx, prevIdx, edges);
  }
}

std::size_t backwardInsertStart(std::size_t prevIdx, FDot16 x, const std::vector<Edge>& edges) {
  auto idx = prevIdx;
  while (const auto prev = edges[idx].asLine().prev) {
    idx = static_cast<std::size_t>(prev.value());
    if (edges[idx].asLine().x <= x) {
      break;
    }
  }

  return idx;
}

void insertNewEdges(std::size_t newIdx, std::int32_t currY, std::vector<Edge>& edges) {
  if (edges[newIdx].asLine().firstY != currY) {
    return;
  }

  const auto prevIdx = static_cast<std::size_t>(edges[newIdx].asLine().prev.value());
  if (edges[prevIdx].asLine().x <= edges[newIdx].asLine().x) {
    return;
  }

  auto startIdx = backwardInsertStart(prevIdx, edges[newIdx].asLine().x, edges);
  do {
    const auto nextIdx = static_cast<std::size_t>(edges[newIdx].asLine().next.value());
    bool keepEdge = false;
    while (true) {
      const auto afterIdx = static_cast<std::size_t>(edges[startIdx].asLine().next.value());
      if (afterIdx == newIdx) {
        keepEdge = true;
        break;
      }
      if (edges[afterIdx].asLine().x >= edges[newIdx].asLine().x) {
        break;
      }

      startIdx = afterIdx;
    }

    if (!keepEdge) {
      removeEdge(newIdx, edges);
      insertEdgeAfter(newIdx, startIdx, edges);
    }

    startIdx = newIdx;
    newIdx = nextIdx;
  } while (edges[newIdx].asLine().firstY == currY);
}

void walkEdges(FillRule fillRule, std::uint32_t startY, std::uint32_t stopY,
               std::uint32_t rightClip, std::vector<Edge>& edges, Blitter& blitter) {
  auto currY = startY;
  const auto windingMask = fillRule == FillRule::EvenOdd ? 1 : -1;
  while (true) {
    auto winding = 0;
    auto left = 0u;
    auto prevX = edges[0].asLine().x;
    auto currIdx = static_cast<std::size_t>(edges[0].asLine().next.value());

    while (edges[currIdx].asLine().firstY <= static_cast<std::int32_t>(currY)) {
      const auto x = static_cast<std::uint32_t>(fdot16::roundToI32(edges[currIdx].asLine().x));

      if ((winding & windingMask) == 0) {
        left = x;
      }

      winding += edges[currIdx].asLine().winding;

      if ((winding & windingMask) == 0) {
        if (x > left) {
          blitter.blitH(left, currY, static_cast<LengthU32>(x - left));
        }
      }

      const auto nextIdx = static_cast<std::size_t>(edges[currIdx].asLine().next.value());
      if (edges[currIdx].asLine().lastY == static_cast<std::int32_t>(currY)) {
        if (edges[currIdx].isLine()) {
          removeEdge(currIdx, edges);
        } else if (edges[currIdx].isQuadratic()) {
          auto& quad = edges[currIdx].asQuadratic();
          if (quad.curveCount > 0 && quad.update()) {
            const auto newX = quad.line.x;
            if (newX < prevX) {
              backwardInsertEdgeBasedOnX(currIdx, edges);
            } else {
              prevX = newX;
            }
          } else {
            removeEdge(currIdx, edges);
          }
        } else {
          auto& cubic = edges[currIdx].asCubic();
          if (cubic.curveCount < 0 && cubic.update()) {
            const auto newX = cubic.line.x;
            if (newX < prevX) {
              backwardInsertEdgeBasedOnX(currIdx, edges);
            } else {
              prevX = newX;
            }
          } else {
            removeEdge(currIdx, edges);
          }
        }
      } else {
        auto& line = edges[currIdx].asLine();
        const auto newX = line.x + line.dx;
        line.x = newX;

        if (newX < prevX) {
          backwardInsertEdgeBasedOnX(currIdx, edges);
        } else {
          prevX = newX;
        }
      }

      currIdx = nextIdx;
    }

    if ((winding & windingMask) != 0) {
      if (rightClip > left) {
        blitter.blitH(left, currY, static_cast<LengthU32>(rightClip - left));
      }
    }

    ++currY;
    if (currY >= stopY) {
      break;
    }

    insertNewEdges(currIdx, static_cast<std::int32_t>(currY), edges);
  }
}

}  // namespace

namespace scan {

void fillPath(const Path& path, FillRule fillRule, const ScreenIntRect& clip, Blitter& blitter) {
  const auto ir = conservativeRoundToInt(path.bounds());
  if (!ir.has_value()) {
    return;
  }

  const auto pathContainedInClip = [&]() {
    const auto bounds = ir->toScreenIntRect();
    return bounds.has_value() && clip.contains(bounds.value());
  }();

  fillPathImpl(path, fillRule, clip, ir->y(), ir->y() + static_cast<std::int32_t>(ir->height()), 0,
               pathContainedInClip, blitter);
}

void fillPathImpl(const Path& path, FillRule fillRule, const ScreenIntRect& clipRect,
                  std::int32_t startY, std::int32_t stopY, std::int32_t shiftEdgesUp,
                  bool pathContainedInClip, Blitter& blitter) {
  const auto shiftedClipOpt = ShiftedIntRect::create(clipRect, shiftEdgesUp);
  if (!shiftedClipOpt.has_value()) {
    return;
  }
  const auto& shiftedClip = shiftedClipOpt.value();

  const auto* clip = pathContainedInClip ? nullptr : &shiftedClip;
  auto edgesOpt = BasicEdgeBuilder::buildEdges(path, clip, shiftEdgesUp);
  if (!edgesOpt.has_value()) {
    return;
  }
  auto builtEdges = std::move(edgesOpt.value());

  std::sort(builtEdges.begin(), builtEdges.end(), [](const Edge& lhs, const Edge& rhs) {
    auto valueL = lhs.asLine().firstY;
    auto valueR = rhs.asLine().firstY;
    if (valueL == valueR) {
      valueL = lhs.asLine().x;
      valueR = rhs.asLine().x;
    }

    return valueL < valueR;
  });

  // Build final edge list with head/tail sentinels at indices 0 and n+1.
  // Pre-allocate to avoid O(n) insert at the beginning.
  constexpr auto kEdgeHeadY = std::numeric_limits<std::int32_t>::min();
  constexpr auto kEdgeTailY = std::numeric_limits<std::int32_t>::max();

  std::vector<Edge> edges;
  edges.reserve(builtEdges.size() + 2);

  LineEdge headLine;
  headLine.next = 1;
  headLine.x = std::numeric_limits<FDot16>::min();
  headLine.firstY = kEdgeHeadY;
  edges.push_back(Edge(headLine));

  for (std::size_t i = 0; i < builtEdges.size(); ++i) {
    builtEdges[i].asLine().prev = static_cast<std::uint32_t>(i);
    builtEdges[i].asLine().next = static_cast<std::uint32_t>(i + 2);
    edges.push_back(std::move(builtEdges[i]));
  }

  LineEdge tailLine;
  tailLine.prev = static_cast<std::uint32_t>(edges.size() - 1);
  tailLine.firstY = kEdgeTailY;
  edges.push_back(Edge(tailLine));

  auto shiftedStartY = static_cast<std::int64_t>(startY) << shiftEdgesUp;
  auto shiftedStopY = static_cast<std::int64_t>(stopY) << shiftEdgesUp;
  const auto top = static_cast<std::int64_t>(shiftedClip.shifted().y());
  if (!pathContainedInClip && shiftedStartY < top) {
    shiftedStartY = top;
  }
  const auto bottom = static_cast<std::int64_t>(shiftedClip.shifted().bottom());
  if (!pathContainedInClip && shiftedStopY > bottom) {
    shiftedStopY = bottom;
  }

  if (shiftedStartY < 0 || shiftedStopY < 0 || shiftedStartY >= shiftedStopY) {
    return;
  }
  if (shiftedStartY > std::numeric_limits<std::uint32_t>::max() ||
      shiftedStopY > std::numeric_limits<std::uint32_t>::max()) {
    return;
  }

  walkEdges(fillRule, static_cast<std::uint32_t>(shiftedStartY),
            static_cast<std::uint32_t>(shiftedStopY), shiftedClip.shifted().right(), edges,
            blitter);
}

}  // namespace scan

}  // namespace tiny_skia
