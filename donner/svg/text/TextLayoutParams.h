#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/base/SmallVector.h"
#include "donner/svg/core/LengthAdjust.h"
#include "donner/svg/core/TextAnchor.h"
#include "donner/svg/core/WritingMode.h"

namespace donner::svg {

/**
 * Layout-only parameters consumed by TextEngine.
 *
 * Baseline alignment (`dominant-baseline` / `alignment-baseline`) is resolved per span into
 * \ref donner::svg::components::ComputedTextComponent::TextSpan::alignmentBaseline rather than
 * carried here, since each span may resolve a different baseline.
 */
struct TextLayoutParams {
  SmallVector<RcString, 1> fontFamilies;
  Lengthd fontSize;
  Box2d viewBox;
  FontMetrics fontMetrics;
  TextAnchor textAnchor = TextAnchor::Start;
  WritingMode writingMode = WritingMode::HorizontalTb;
  double letterSpacingPx = 0.0;
  double wordSpacingPx = 0.0;
  std::optional<Lengthd> textLength;
  LengthAdjust lengthAdjust = LengthAdjust::Default;
};

}  // namespace donner::svg
