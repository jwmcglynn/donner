#include "donner/svg/compositor/CompositorLayer.h"

namespace donner::svg::compositor {

CompositorLayer::CompositorLayer(uint32_t id, Entity entity, Entity firstEntity, Entity lastEntity)
    : id_(id), entity_(entity), firstEntity_(firstEntity), lastEntity_(lastEntity) {}

}  // namespace donner::svg::compositor
