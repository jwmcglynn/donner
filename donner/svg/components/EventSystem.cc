#include "donner/svg/components/EventSystem.h"

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/EventListenersComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"

namespace donner::svg::components {

void EventSystem::buildAncestorPath(Registry& registry, Entity entity,
                                    std::vector<Entity>& outPath) {
  outPath.clear();

  // If this is a rendering instance, resolve to the DOM entity.
  Entity domEntity = entity;
  if (const auto* instance = registry.try_get<RenderingInstanceComponent>(entity)) {
    if (instance->dataEntity != entt::null) {
      domEntity = instance->dataEntity;
    }
  }

  // Walk up the tree to collect ancestors.
  SmallVector<Entity, 16> ancestors;
  Entity current = domEntity;
  while (current != entt::null) {
    ancestors.push_back(current);
    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      break;
    }
    current = tree->parent();
  }

  // Reverse so path is [root, ..., parent, target].
  outPath.reserve(ancestors.size());
  for (size_t i = ancestors.size(); i > 0; --i) {
    outPath.push_back(ancestors[i - 1]);
  }
}

void EventSystem::fireListeners(Registry& registry, Entity entity, Event& event, bool capture) {
  auto* listeners = registry.try_get<EventListenersComponent>(entity);
  if (!listeners) {
    return;
  }

  // Iterate a snapshot of listeners to avoid issues if a callback modifies the list.
  const auto snapshot = listeners->listeners;
  for (const auto& entry : snapshot) {
    if (event.propagationStopped) {
      break;
    }

    if (entry.type != event.type) {
      continue;
    }

    // During capture phase, only capture listeners fire.
    // During bubble phase, only non-capture listeners fire.
    // During target phase, all listeners fire (called once with capture=true).
    if (event.phase == Event::Phase::Target || entry.useCapture == capture) {
      entry.callback(event);
    }
  }
}

void EventSystem::dispatch(Registry& registry, Event& event) {
  if (event.target == entt::null) {
    return;
  }

  // Build ancestor path: [root, ..., parent, target].
  std::vector<Entity> path;
  buildAncestorPath(registry, event.target, path);

  if (path.empty()) {
    return;
  }

  // Resolve the target to the DOM entity for consistent comparison.
  Entity domTarget = path.back();

  // Capture phase: root to target (exclusive).
  event.phase = Event::Phase::Capture;
  for (size_t i = 0; i + 1 < path.size() && !event.propagationStopped; ++i) {
    event.currentTarget = path[i];
    fireListeners(registry, path[i], event, /*capture=*/true);
  }

  // Target phase: fire all listeners (both capture and bubble) once.
  if (!event.propagationStopped) {
    event.phase = Event::Phase::Target;
    event.currentTarget = domTarget;
    fireListeners(registry, domTarget, event, /*capture=*/true);
  }

  // Bubble phase: target parent to root.
  if (!event.propagationStopped && eventBubbles(event.type)) {
    event.phase = Event::Phase::Bubble;
    for (int i = static_cast<int>(path.size()) - 2; i >= 0 && !event.propagationStopped; --i) {
      event.currentTarget = path[i];
      fireListeners(registry, path[i], event, /*capture=*/false);
    }
  }
}

void EventSystem::updateHover(Registry& registry, Entity newHoveredEntity,
                              const Vector2d& documentPosition) {
  auto& pointerState = registry.ctx().emplace<PointerStateComponent>();
  const Entity oldHoveredEntity = pointerState.hoveredEntity;

  pointerState.lastPosition = documentPosition;

  if (oldHoveredEntity == newHoveredEntity) {
    return;
  }

  pointerState.hoveredEntity = newHoveredEntity;

  // Fire leave events on the old entity.
  if (oldHoveredEntity != entt::null) {
    Event leaveEvent;
    leaveEvent.type = EventType::MouseLeave;
    leaveEvent.documentPosition = documentPosition;
    leaveEvent.target = oldHoveredEntity;
    // MouseLeave does not bubble — fire directly on the target only.
    leaveEvent.phase = Event::Phase::Target;
    leaveEvent.currentTarget = oldHoveredEntity;
    fireListeners(registry, oldHoveredEntity, leaveEvent, /*capture=*/true);

    // MouseOut bubbles.
    Event outEvent;
    outEvent.type = EventType::MouseOut;
    outEvent.documentPosition = documentPosition;
    outEvent.target = oldHoveredEntity;
    dispatch(registry, outEvent);
  }

  // Fire enter events on the new entity.
  if (newHoveredEntity != entt::null) {
    Event enterEvent;
    enterEvent.type = EventType::MouseEnter;
    enterEvent.documentPosition = documentPosition;
    enterEvent.target = newHoveredEntity;
    // MouseEnter does not bubble — fire directly on the target only.
    enterEvent.phase = Event::Phase::Target;
    enterEvent.currentTarget = newHoveredEntity;
    fireListeners(registry, newHoveredEntity, enterEvent, /*capture=*/true);

    // MouseOver bubbles.
    Event overEvent;
    overEvent.type = EventType::MouseOver;
    overEvent.documentPosition = documentPosition;
    overEvent.target = newHoveredEntity;
    dispatch(registry, overEvent);
  }
}

}  // namespace donner::svg::components
