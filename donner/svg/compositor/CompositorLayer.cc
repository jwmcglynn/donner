#include "donner/svg/compositor/CompositorLayer.h"

namespace donner::svg::compositor {

CompositorLayer::CompositorLayer(uint32_t id, Entity entity, Entity firstEntity, Entity lastEntity)
    : id_(id), entity_(entity), firstEntity_(firstEntity), lastEntity_(lastEntity) {}

std::string FallbackReasonToString(FallbackReason reasons) {
  if (reasons == FallbackReason::None) {
    return "None";
  }

  std::string out;
  const auto append = [&](const char* name) {
    if (!out.empty()) {
      out.append(" | ");
    }
    out.append(name);
  };

  if ((reasons & FallbackReason::BlendMode) != FallbackReason::None) {
    append("BlendMode");
  }
  if ((reasons & FallbackReason::Filter) != FallbackReason::None) {
    append("Filter");
  }
  if ((reasons & FallbackReason::ClipPath) != FallbackReason::None) {
    append("ClipPath");
  }
  if ((reasons & FallbackReason::Mask) != FallbackReason::None) {
    append("Mask");
  }
  if ((reasons & FallbackReason::Markers) != FallbackReason::None) {
    append("Markers");
  }
  if ((reasons & FallbackReason::ExternalPaint) != FallbackReason::None) {
    append("ExternalPaint");
  }
  if ((reasons & FallbackReason::IsolatedLayer) != FallbackReason::None) {
    append("IsolatedLayer");
  }
  return out;
}

}  // namespace donner::svg::compositor
