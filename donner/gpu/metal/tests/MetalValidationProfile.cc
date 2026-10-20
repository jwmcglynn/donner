#include "donner/gpu/metal/tests/MetalValidationProfile.h"

#include <ostream>
#include <sstream>

namespace donner::gpu::metal::tests {

std::ostream& operator<<(std::ostream& os, MetalValidationProfile profile) {
  switch (profile) {
    case MetalValidationProfile::Unsupported: return os << "unsupported";
    case MetalValidationProfile::Full: return os << "full";
    case MetalValidationProfile::ParavirtualTextureUsageOff:
      return os << "paravirtual-texture-usage-off";
  }
  return os << "unsupported";
}

MetalValidationProfile ClassifyMetalValidationProfile(
    const MetalValidationObservation& observation) {
  if (observation.shaderValidationState == 1) {
    return MetalValidationProfile::Full;
  }
  if (observation.shaderValidationState == 2 && observation.gpuName == "Apple Paravirtual device" &&
      observation.osMajor == 26 && observation.osMinor == 6 && observation.osPatch == 2 &&
      observation.osBuild == "25G83") {
    return MetalValidationProfile::ParavirtualTextureUsageOff;
  }
  return MetalValidationProfile::Unsupported;
}

std::string DescribeMetalValidationObservation(const MetalValidationObservation& observation) {
  std::string name;
  for (const char c : observation.gpuName) {
    if (c >= ' ' && c <= '~' && c != '"' && c != '\\') {
      name.push_back(c);
    }
  }
  std::string build;
  for (const char c : observation.osBuild) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      build.push_back(c);
    }
  }
  std::ostringstream os;
  os << "gpu=\"" << name << "\" os=" << observation.osMajor << '.' << observation.osMinor << '.'
     << observation.osPatch << " os_build=" << build
     << " validation_state=" << observation.shaderValidationState;
  return os.str();
}

}  // namespace donner::gpu::metal::tests
