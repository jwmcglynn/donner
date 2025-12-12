#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/base/SmallVector.h"
#include "donner/svg/core/Overflow.h"

namespace donner::svg::components {

/**
 * Alignment options for flowed text within a region.
 */
enum class FlowAlignment : uint8_t {
  Start,
  Center,
  End,
  Justify,
};

/**
 * Region definition for flowed text layout. Regions are provided by child flow elements under a
 * text node and will later bound auto-flow layout boxes.
 */
struct FlowRegion {
  Lengthd x{0.0, Lengthd::Unit::None};
  Lengthd y{0.0, Lengthd::Unit::None};
  Lengthd width{0.0, Lengthd::Unit::None};
  Lengthd height{0.0, Lengthd::Unit::None};
  Overflow overflow{Overflow::Visible};
};

/**
 * Captures flow-region metadata for text elements that opt into auto-flow layout.
 */
struct TextFlowComponent {
  SmallVector<FlowRegion, 1> regions;
  std::optional<FlowAlignment> alignment;
  std::optional<Overflow> overflow;

  bool hasFlow() const { return !regions.empty(); }
};

}  // namespace donner::svg::components

