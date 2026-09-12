#include "donner/gpu/metal/tests/MetalValidationProfile.h"

#include <ostream>

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

}  // namespace donner::gpu::metal::tests
