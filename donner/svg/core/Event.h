#pragma once
/// @file

#include <cstdint>
#include <functional>
#include <ostream>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"

namespace donner::svg {

/**
 * Types of DOM events that can be dispatched through the SVG event system.
 *
 * @see https://www.w3.org/TR/uievents/
 */
enum class EventType : uint8_t {
  Click,       //!< A click (mousedown + mouseup on same element).
  DblClick,    //!< A double click.
  MouseDown,   //!< A mouse button was pressed.
  MouseUp,     //!< A mouse button was released.
  MouseMove,   //!< The mouse pointer moved.
  MouseEnter,  //!< The mouse entered the element (does not bubble).
  MouseLeave,  //!< The mouse left the element (does not bubble).
  MouseOver,   //!< The mouse is over the element (bubbles).
  MouseOut,    //!< The mouse moved out of the element (bubbles).
  Wheel,       //!< A wheel event (scroll).
};

/// Output stream operator for \ref EventType.
inline std::ostream& operator<<(std::ostream& os, EventType type) {
  switch (type) {
    case EventType::Click: return os << "click";
    case EventType::DblClick: return os << "dblclick";
    case EventType::MouseDown: return os << "mousedown";
    case EventType::MouseUp: return os << "mouseup";
    case EventType::MouseMove: return os << "mousemove";
    case EventType::MouseEnter: return os << "mouseenter";
    case EventType::MouseLeave: return os << "mouseleave";
    case EventType::MouseOver: return os << "mouseover";
    case EventType::MouseOut: return os << "mouseout";
    case EventType::Wheel: return os << "wheel";
  }
  return os << "unknown";
}

/// Returns true if the event type bubbles up the DOM tree.
inline bool eventBubbles(EventType type) {
  switch (type) {
    case EventType::MouseEnter:
    case EventType::MouseLeave: return false;
    default: return true;
  }
}

/**
 * A DOM event object, carrying information about a user interaction.
 */
struct Event {
  /// The phase of event propagation.
  enum class Phase : uint8_t {
    None,     //!< Not yet dispatched.
    Capture,  //!< Capture phase: root to target.
    Target,   //!< Target phase: at the target element.
    Bubble,   //!< Bubble phase: target parent to root.
  };

  EventType type;                     //!< The type of event.
  Vector2d clientPosition;            //!< Position in canvas coordinates.
  Vector2d documentPosition;          //!< Position in document coordinates.
  int button = 0;                     //!< Mouse button (0=left, 1=middle, 2=right).
  int buttons = 0;                    //!< Bitmask of currently pressed buttons.
  bool ctrlKey = false;               //!< Whether the Ctrl key was held.
  bool shiftKey = false;              //!< Whether the Shift key was held.
  bool altKey = false;                //!< Whether the Alt key was held.
  bool metaKey = false;               //!< Whether the Meta/Cmd key was held.
  double deltaX = 0.0;                //!< Horizontal scroll delta.
  double deltaY = 0.0;                //!< Vertical scroll delta.
  Entity target = entt::null;         //!< The original event target.
  Entity currentTarget = entt::null;  //!< The element currently handling the event.
  Phase phase = Phase::None;          //!< Current propagation phase.
  bool propagationStopped = false;    //!< Whether stopPropagation() was called.
  bool defaultPrevented = false;      //!< Whether preventDefault() was called.

  /// Stop the event from propagating further.
  void stopPropagation() { propagationStopped = true; }

  /// Prevent the default action associated with this event.
  void preventDefault() { defaultPrevented = true; }
};

/// Handle for removing an event listener. Returned by addEventListener.
struct ListenerHandle {
  uint32_t id = 0;  //!< Unique identifier for the listener.

  bool operator==(const ListenerHandle& other) const { return id == other.id; }
  bool operator!=(const ListenerHandle& other) const { return id != other.id; }
};

/// Callback type for event listeners.
using EventCallback = std::function<void(Event&)>;

}  // namespace donner::svg
