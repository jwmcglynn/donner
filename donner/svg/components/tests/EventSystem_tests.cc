#include "donner/svg/components/EventSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/EventListenersComponent.h"

using testing::ElementsAre;

namespace donner::svg::components {
namespace {

class EventSystemTest : public ::testing::Test {
protected:
  Registry registry;
  EventSystem eventSystem;

  /// Create a DOM entity with a TreeComponent (needed for ancestor path walking).
  Entity createDomEntity(const char* name) {
    Entity e = registry.create();
    registry.emplace<donner::components::TreeComponent>(
        e, xml::XMLQualifiedNameRef("", name));
    return e;
  }

  /// Make `child` a child of `parent` in the DOM tree.
  void setParent(Entity child, Entity parent) {
    auto& parentTree = registry.get<donner::components::TreeComponent>(parent);
    parentTree.insertBefore(registry, child, entt::null);
  }

  /// Register an event listener on an entity, returning the handle.
  ListenerHandle addListener(Entity entity, EventType type, EventCallback callback,
                             bool capture = false) {
    auto& listeners = registry.get_or_emplace<EventListenersComponent>(entity);
    return listeners.addListener(type, std::move(callback), capture);
  }

  /// Create a basic click event at the origin.
  Event makeClickEvent(Entity target) {
    Event event;
    event.type = EventType::Click;
    event.documentPosition = Vector2d(0.0, 0.0);
    event.target = target;
    return event;
  }
};

TEST_F(EventSystemTest, DispatchToTarget) {
  Entity root = createDomEntity("svg");
  Entity rect = createDomEntity("rect");
  setParent(rect, root);

  bool called = false;
  addListener(rect, EventType::Click, [&](Event& event) {
    called = true;
    EXPECT_EQ(event.phase, Event::Phase::Target);
    EXPECT_EQ(event.currentTarget, rect);
    EXPECT_EQ(event.target, rect);
  });

  Event event = makeClickEvent(rect);
  eventSystem.dispatch(registry, event);

  EXPECT_TRUE(called);
}

TEST_F(EventSystemTest, CapturePhase) {
  Entity root = createDomEntity("svg");
  Entity group = createDomEntity("g");
  Entity rect = createDomEntity("rect");
  setParent(group, root);
  setParent(rect, group);

  std::vector<std::string> callOrder;

  // Capture listener on root.
  addListener(root, EventType::Click,
              [&](Event&) { callOrder.push_back("root-capture"); }, /*capture=*/true);

  // Capture listener on group.
  addListener(group, EventType::Click,
              [&](Event&) { callOrder.push_back("group-capture"); }, /*capture=*/true);

  // Bubble listener on target.
  addListener(rect, EventType::Click, [&](Event&) { callOrder.push_back("rect-target"); });

  Event event = makeClickEvent(rect);
  eventSystem.dispatch(registry, event);

  EXPECT_THAT(callOrder, ElementsAre("root-capture", "group-capture", "rect-target"));
}

TEST_F(EventSystemTest, BubblePhase) {
  Entity root = createDomEntity("svg");
  Entity rect = createDomEntity("rect");
  setParent(rect, root);

  std::vector<std::string> callOrder;

  addListener(rect, EventType::Click, [&](Event&) { callOrder.push_back("rect"); });
  addListener(root, EventType::Click, [&](Event&) { callOrder.push_back("root-bubble"); });

  Event event = makeClickEvent(rect);
  eventSystem.dispatch(registry, event);

  EXPECT_THAT(callOrder, ElementsAre("rect", "root-bubble"));
}

TEST_F(EventSystemTest, StopPropagation) {
  Entity root = createDomEntity("svg");
  Entity rect = createDomEntity("rect");
  setParent(rect, root);

  bool rootCalled = false;

  addListener(rect, EventType::Click, [&](Event& event) { event.stopPropagation(); });
  addListener(root, EventType::Click, [&](Event&) { rootCalled = true; });

  Event event = makeClickEvent(rect);
  eventSystem.dispatch(registry, event);

  EXPECT_FALSE(rootCalled);
  EXPECT_TRUE(event.propagationStopped);
}

TEST_F(EventSystemTest, StopPropagationInCapture) {
  Entity root = createDomEntity("svg");
  Entity rect = createDomEntity("rect");
  setParent(rect, root);

  bool targetCalled = false;

  // Capture listener stops propagation.
  addListener(root, EventType::Click,
              [&](Event& event) { event.stopPropagation(); }, /*capture=*/true);
  addListener(rect, EventType::Click, [&](Event&) { targetCalled = true; });

  Event event = makeClickEvent(rect);
  eventSystem.dispatch(registry, event);

  EXPECT_FALSE(targetCalled);
}

TEST_F(EventSystemTest, PreventDefault) {
  Entity root = createDomEntity("svg");
  Entity rect = createDomEntity("rect");
  setParent(rect, root);

  addListener(rect, EventType::Click, [&](Event& event) { event.preventDefault(); });

  Event event = makeClickEvent(rect);
  eventSystem.dispatch(registry, event);

  EXPECT_TRUE(event.defaultPrevented);
}

TEST_F(EventSystemTest, NonBubblingEventDoesNotBubble) {
  Entity root = createDomEntity("svg");
  Entity rect = createDomEntity("rect");
  setParent(rect, root);

  bool rootCalled = false;

  // MouseEnter does not bubble.
  addListener(root, EventType::MouseEnter, [&](Event&) { rootCalled = true; });

  Event event;
  event.type = EventType::MouseEnter;
  event.target = rect;
  eventSystem.dispatch(registry, event);

  // Root bubble listener should NOT be called because MouseEnter doesn't bubble.
  EXPECT_FALSE(rootCalled);
}

TEST_F(EventSystemTest, WrongEventTypeNotFired) {
  Entity rect = createDomEntity("rect");

  bool called = false;
  addListener(rect, EventType::MouseDown, [&](Event&) { called = true; });

  Event event = makeClickEvent(rect);
  eventSystem.dispatch(registry, event);

  EXPECT_FALSE(called);
}

TEST_F(EventSystemTest, RemoveListener) {
  Entity rect = createDomEntity("rect");

  int callCount = 0;
  ListenerHandle handle =
      addListener(rect, EventType::Click, [&](Event&) { callCount++; });

  // First dispatch: listener fires.
  Event event1 = makeClickEvent(rect);
  eventSystem.dispatch(registry, event1);
  EXPECT_EQ(callCount, 1);

  // Remove the listener.
  auto& listeners = registry.get<EventListenersComponent>(rect);
  EXPECT_TRUE(listeners.removeListener(handle));

  // Second dispatch: listener does not fire.
  Event event2 = makeClickEvent(rect);
  eventSystem.dispatch(registry, event2);
  EXPECT_EQ(callCount, 1);
}

TEST_F(EventSystemTest, HoverUpdateFiresEnterLeave) {
  Entity root = createDomEntity("svg");
  Entity rect1 = createDomEntity("rect1");
  Entity rect2 = createDomEntity("rect2");
  setParent(rect1, root);
  setParent(rect2, root);

  std::vector<std::string> events;

  addListener(rect1, EventType::MouseEnter, [&](Event&) { events.push_back("rect1-enter"); });
  addListener(rect1, EventType::MouseLeave, [&](Event&) { events.push_back("rect1-leave"); });
  addListener(rect2, EventType::MouseEnter, [&](Event&) { events.push_back("rect2-enter"); });
  addListener(rect2, EventType::MouseLeave, [&](Event&) { events.push_back("rect2-leave"); });

  // Move pointer over rect1.
  eventSystem.updateHover(registry, rect1, Vector2d(10.0, 10.0));
  EXPECT_THAT(events, ElementsAre("rect1-enter"));
  events.clear();

  // Move pointer to rect2.
  eventSystem.updateHover(registry, rect2, Vector2d(50.0, 50.0));
  EXPECT_THAT(events, ElementsAre("rect1-leave", "rect2-enter"));
  events.clear();

  // Move pointer off everything.
  eventSystem.updateHover(registry, entt::null, Vector2d(100.0, 100.0));
  EXPECT_THAT(events, ElementsAre("rect2-leave"));
}

TEST_F(EventSystemTest, HoverNoChangeNoEvents) {
  Entity rect = createDomEntity("rect");

  int enterCount = 0;
  addListener(rect, EventType::MouseEnter, [&](Event&) { enterCount++; });

  eventSystem.updateHover(registry, rect, Vector2d(10.0, 10.0));
  EXPECT_EQ(enterCount, 1);

  // Same entity again — no new events.
  eventSystem.updateHover(registry, rect, Vector2d(11.0, 11.0));
  EXPECT_EQ(enterCount, 1);
}

TEST_F(EventSystemTest, DispatchNullTargetIsNoop) {
  Event event;
  event.type = EventType::Click;
  event.target = entt::null;
  eventSystem.dispatch(registry, event);
  // Should not crash.
}

TEST_F(EventSystemTest, FullCaptureTargetBubblePath) {
  Entity root = createDomEntity("svg");
  Entity group = createDomEntity("g");
  Entity rect = createDomEntity("rect");
  setParent(group, root);
  setParent(rect, group);

  std::vector<std::string> callOrder;

  addListener(root, EventType::Click,
              [&](Event&) { callOrder.push_back("root-capture"); }, /*capture=*/true);
  addListener(group, EventType::Click,
              [&](Event&) { callOrder.push_back("group-capture"); }, /*capture=*/true);
  addListener(rect, EventType::Click,
              [&](Event&) { callOrder.push_back("rect-target"); });
  addListener(group, EventType::Click,
              [&](Event&) { callOrder.push_back("group-bubble"); });
  addListener(root, EventType::Click,
              [&](Event&) { callOrder.push_back("root-bubble"); });

  Event event = makeClickEvent(rect);
  eventSystem.dispatch(registry, event);

  EXPECT_THAT(callOrder, ElementsAre("root-capture", "group-capture", "rect-target",
                                      "group-bubble", "root-bubble"));
}

}  // namespace
}  // namespace donner::svg::components
