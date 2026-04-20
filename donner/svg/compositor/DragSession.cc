#include "donner/svg/compositor/DragSession.h"

namespace donner::svg::compositor {

std::optional<DragSession> DragSession::begin(CompositorController& compositor, Entity target) {
  if (!compositor.promoteEntity(target)) {
    return std::nullopt;
  }
  return DragSession(compositor, target);
}

DragSession::DragSession(CompositorController& compositor, Entity target)
    : compositor_(&compositor), target_(target) {}

DragSession::~DragSession() {
  end();
}

DragSession::DragSession(DragSession&& other) noexcept
    : compositor_(other.compositor_), target_(other.target_) {
  other.compositor_ = nullptr;
  other.target_ = entt::null;
}

DragSession& DragSession::operator=(DragSession&& other) noexcept {
  if (this != &other) {
    end();
    compositor_ = other.compositor_;
    target_ = other.target_;
    other.compositor_ = nullptr;
    other.target_ = entt::null;
  }
  return *this;
}

void DragSession::end() {
  if (compositor_) {
    compositor_->demoteEntity(target_);
    compositor_ = nullptr;
    target_ = entt::null;
  }
}

}  // namespace donner::svg::compositor
