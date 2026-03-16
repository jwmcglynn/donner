/**
 * @file Tests for \ref donner::svg::EventType enum, its ostream output operator, and
 * \ref donner::svg::eventBubbles.
 */

#include "donner/svg/core/Event.h"

#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"

namespace donner::svg {

/// @test Ostream output \c operator<< for all \ref EventType values.
TEST(EventTypeTest, OstreamOutput) {
  EXPECT_THAT(EventType::Click, ToStringIs("click"));
  EXPECT_THAT(EventType::DblClick, ToStringIs("dblclick"));
  EXPECT_THAT(EventType::MouseDown, ToStringIs("mousedown"));
  EXPECT_THAT(EventType::MouseUp, ToStringIs("mouseup"));
  EXPECT_THAT(EventType::MouseMove, ToStringIs("mousemove"));
  EXPECT_THAT(EventType::MouseEnter, ToStringIs("mouseenter"));
  EXPECT_THAT(EventType::MouseLeave, ToStringIs("mouseleave"));
  EXPECT_THAT(EventType::MouseOver, ToStringIs("mouseover"));
  EXPECT_THAT(EventType::MouseOut, ToStringIs("mouseout"));
  EXPECT_THAT(EventType::Wheel, ToStringIs("wheel"));
}

/// @test \ref eventBubbles returns correct results.
TEST(EventTypeTest, EventBubbles) {
  // MouseEnter and MouseLeave do not bubble.
  EXPECT_FALSE(eventBubbles(EventType::MouseEnter));
  EXPECT_FALSE(eventBubbles(EventType::MouseLeave));

  // All other events bubble.
  EXPECT_TRUE(eventBubbles(EventType::Click));
  EXPECT_TRUE(eventBubbles(EventType::DblClick));
  EXPECT_TRUE(eventBubbles(EventType::MouseDown));
  EXPECT_TRUE(eventBubbles(EventType::MouseUp));
  EXPECT_TRUE(eventBubbles(EventType::MouseMove));
  EXPECT_TRUE(eventBubbles(EventType::MouseOver));
  EXPECT_TRUE(eventBubbles(EventType::MouseOut));
  EXPECT_TRUE(eventBubbles(EventType::Wheel));
}

/// @test Event::stopPropagation and Event::preventDefault work correctly.
TEST(EventTest, StopPropagationAndPreventDefault) {
  Event event;
  event.type = EventType::Click;

  EXPECT_FALSE(event.propagationStopped);
  EXPECT_FALSE(event.defaultPrevented);

  event.stopPropagation();
  EXPECT_TRUE(event.propagationStopped);
  EXPECT_FALSE(event.defaultPrevented);

  event.preventDefault();
  EXPECT_TRUE(event.defaultPrevented);
}

}  // namespace donner::svg
