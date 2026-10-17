#include "harness/FeatureSupport.h"

#include <algorithm>

namespace donner::bakeoff {

namespace {

constexpr std::array<std::string_view, kFeatureCount> kFeatureNames = {
    "transform",     "solid_paint",    "linear_gradient",  "radial_gradient",
    "pattern_paint", "even_odd_fill",  "stroke",           "stroke_dash",
    "clip_rect",     "clip_path",      "clip_path_nested", "isolated_layer",
    "blend_mode",    "mask_luminance", "mask_alpha",       "filter",
    "image",         "text",
};

}  // namespace

std::string_view FeatureName(Feature feature) {
  return kFeatureNames[static_cast<std::size_t>(feature)];
}

std::string_view SupportName(Support support) {
  switch (support) {
    case Support::Native: return "native";
    case Support::Emulated: return "emulated";
    case Support::Unsupported: return "unsupported";
  }
  return "unsupported";
}

CapabilityTable CapabilityTable::All(Support level) {
  CapabilityTable table;
  table.support.fill(level);
  return table;
}

CapabilityTable& CapabilityTable::set(Feature feature, Support level, std::string_view note) {
  support[static_cast<std::size_t>(feature)] = level;
  notes[static_cast<std::size_t>(feature)] = note;
  return *this;
}

bool FeatureUsage::rendersFully() const {
  return std::all_of(dropped_.begin(), dropped_.end(), [](std::uint64_t n) { return n == 0; });
}

void FeatureUsage::reset() {
  uses_.fill(0);
  dropped_.fill(0);
}

}  // namespace donner::bakeoff
