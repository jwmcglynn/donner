#pragma once
/// @file

#include "donner/base/SmallVector.h"
#include "donner/svg/core/Event.h"

namespace donner::svg::components {

/**
 * ECS component storing event listeners registered on an element.
 *
 * Each entry records the event type, callback, capture/bubble preference, and a unique ID
 * for removal via \ref ListenerHandle.
 */
struct EventListenersComponent {
  /// A single registered event listener.
  struct Entry {
    EventType type;          //!< Which event type this listener responds to.
    EventCallback callback;  //!< The callback to invoke.
    bool useCapture;         //!< If true, fires during capture phase; otherwise during bubble.
    uint32_t id;             //!< Unique ID for removal.
  };

  SmallVector<Entry, 2> listeners;  //!< All registered listeners on this element.
  uint32_t nextId = 1;              //!< Next unique ID to assign.

  /**
   * Add a listener and return its handle.
   *
   * @param type Event type to listen for.
   * @param callback Callback to invoke when the event fires.
   * @param useCapture If true, listen during capture phase.
   * @return Handle that can be used to remove the listener.
   */
  ListenerHandle addListener(EventType type, EventCallback callback, bool useCapture) {
    const uint32_t id = nextId++;
    listeners.push_back(Entry{type, std::move(callback), useCapture, id});
    return ListenerHandle{id};
  }

  /**
   * Remove a listener by its handle.
   *
   * @param handle The handle returned by \ref addListener.
   * @return true if the listener was found and removed.
   */
  bool removeListener(ListenerHandle handle) {
    for (size_t i = 0; i < listeners.size(); ++i) {
      if (listeners[i].id == handle.id) {
        // Shift remaining elements down.
        for (size_t j = i; j + 1 < listeners.size(); ++j) {
          listeners[j] = std::move(listeners[j + 1]);
        }
        listeners.pop_back();
        return true;
      }
    }
    return false;
  }
};

/**
 * Singleton component tracking the currently hovered entity for mouseenter/mouseleave dispatch.
 */
struct PointerStateComponent {
  Entity hoveredEntity = entt::null;  //!< The entity currently under the pointer.
  Vector2d lastPosition;              //!< The last known pointer position in document coordinates.
};

}  // namespace donner::svg::components
