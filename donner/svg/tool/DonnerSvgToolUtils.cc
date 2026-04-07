#include "donner/svg/tool/DonnerSvgToolUtils.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/StringUtils.h"

namespace donner::svg {
namespace {

std::string TagNameForSelector(const SVGElement& element) {
  return std::string(element.tagName().name);
}

std::string ClassToken(const SVGElement& element) {
  const std::string className = std::string(element.className());
  const auto parts = StringUtils::Split(className, ' ');
  for (const std::string_view part : parts) {
    if (!part.empty()) {
      return std::string(part);
    }
  }
  return std::string();
}

int NthChildIndex(const SVGElement& element) {
  int index = 1;
  auto maybeSibling = element.previousSibling();
  while (maybeSibling) {
    ++index;
    maybeSibling = maybeSibling->previousSibling();
  }
  return index;
}

std::string SelectorSegment(const SVGElement& element) {
  std::ostringstream out;
  out << TagNameForSelector(element);

  const std::string id = std::string(element.id());
  if (!id.empty()) {
    out << '#' << id;
    return out.str();
  }

  const std::string classToken = ClassToken(element);
  if (!classToken.empty()) {
    out << '.' << classToken;
  }

  out << ":nth-child(" << NthChildIndex(element) << ')';
  return out.str();
}

std::vector<SVGElement> ElementChain(const SVGElement& element) {
  std::vector<SVGElement> chain;
  chain.push_back(element);
  auto maybeParent = element.parentElement();
  while (maybeParent) {
    chain.push_back(*maybeParent);
    maybeParent = maybeParent->parentElement();
  }
  std::reverse(chain.begin(), chain.end());
  return chain;
}

}  // namespace

std::string BuildCssSelectorPath(const SVGElement& element) {
  const std::vector<SVGElement> chain = ElementChain(element);

  std::ostringstream out;
  bool first = true;
  for (const SVGElement& chainElement : chain) {
    if (!first) {
      out << " > ";
    }
    out << SelectorSegment(chainElement);
    first = false;
  }
  return out.str();
}

void CompositeAABBRect(RendererBitmap& bitmap, const Box2d& bounds,
                       const SampledImageInfo& imageInfo) {
  const int imgW = bitmap.dimensions.x;
  const int imgH = bitmap.dimensions.y;
  const size_t stride = bitmap.rowBytes / 4;
  const double sx = imageInfo.xScale;
  const double sy = imageInfo.yScale;
  const int totalSubX = imageInfo.columns * 2;
  const int totalSubY = imageInfo.rows * 2;

  // The sampler uses a cell-based mapping for quarter-pixel mode (2x2 per cell):
  //   cell c samples startPixel = int(c * 2 * scale)
  //   sub-pixel s within cell has offset (s % 2)
  //   so sub-pixel s maps to image pixel: int(floor(s/2) * 2 * scale) + (s % 2)

  auto subPixelToImgX = [&](int s) -> int {
    return static_cast<int>(static_cast<double>(s / 2) * 2.0 * sx) + (s % 2);
  };
  auto subPixelToImgY = [&](int s) -> int {
    return static_cast<int>(static_cast<double>(s / 2) * 2.0 * sy) + (s % 2);
  };

  // Find the sub-pixel index containing an image coordinate.
  // We need the inverse: given pixel p, find s such that subPixelToImg(s) == p.
  // For the cell: cell = floor(p / (2 * scale)), but the offset within the cell matters.
  // Simpler: brute-force search within a small range, or compute from the cell.
  auto imgToSubPixelX = [&](double p) -> int {
    const int cell = static_cast<int>(std::floor(p / (2.0 * sx)));
    const int s0 = std::clamp(cell * 2, 0, totalSubX - 1);
    // Check if the coordinate is closer to sub-pixel s0 or s0+1.
    if (s0 + 1 < totalSubX && static_cast<double>(subPixelToImgX(s0 + 1)) <= p) {
      return s0 + 1;
    }
    return s0;
  };
  auto imgToSubPixelY = [&](double p) -> int {
    const int cell = static_cast<int>(std::floor(p / (2.0 * sy)));
    const int s0 = std::clamp(cell * 2, 0, totalSubY - 1);
    if (s0 + 1 < totalSubY && static_cast<double>(subPixelToImgY(s0 + 1)) <= p) {
      return s0 + 1;
    }
    return s0;
  };

  // Sub-pixel indices for the AABB edges. For the max edges, use the last sub-pixel
  // that is strictly inside the AABB (exclusive upper bound).
  const int sLeft = imgToSubPixelX(bounds.topLeft.x);
  const int sTop = imgToSubPixelY(bounds.topLeft.y);
  // Right/bottom: find the sub-pixel containing the last pixel inside the bounds.
  const int sRight = imgToSubPixelX(std::max(0.0, bounds.bottomRight.x - 1.0));
  const int sBottom = imgToSubPixelY(std::max(0.0, bounds.bottomRight.y - 1.0));

  constexpr uint8_t kR = 0x44, kG = 0x88, kB = 0xff, kA = 0xff;

  auto fillPixel = [&](int x, int y) {
    if (x >= 0 && x < imgW && y >= 0 && y < imgH) {
      const size_t off = (static_cast<size_t>(y) * stride + static_cast<size_t>(x)) * 4;
      bitmap.pixels[off + 0] = kR;
      bitmap.pixels[off + 1] = kG;
      bitmap.pixels[off + 2] = kB;
      bitmap.pixels[off + 3] = kA;
    }
  };

  // Horizontal span: image pixels for the full range of sub-pixels from sLeft to sRight.
  const int horzLo = subPixelToImgX(sLeft);
  const int horzHi = subPixelToImgX(sRight) + 1;  // inclusive -> exclusive

  // Top edge: color the single image pixel for each sub-pixel across the span.
  {
    const int py = subPixelToImgY(sTop);
    for (int x = horzLo; x < horzHi; ++x) {
      fillPixel(x, py);
    }
  }

  // Bottom edge.
  {
    const int py = subPixelToImgY(sBottom);
    for (int x = horzLo; x < horzHi; ++x) {
      fillPixel(x, py);
    }
  }

  // Left edge (between top and bottom, exclusive of corners).
  {
    const int px = subPixelToImgX(sLeft);
    for (int s = sTop + 1; s < sBottom; ++s) {
      fillPixel(px, subPixelToImgY(s));
    }
  }

  // Right edge.
  {
    const int px = subPixelToImgX(sRight);
    for (int s = sTop + 1; s < sBottom; ++s) {
      fillPixel(px, subPixelToImgY(s));
    }
  }
}

}  // namespace donner::svg
