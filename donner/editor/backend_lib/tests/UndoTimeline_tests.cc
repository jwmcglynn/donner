#include "donner/editor/backend_lib/UndoTimeline.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
         <rect id="r1" x="10" y="10" width="20" height="20" fill="red"/>
       </svg>)";

svg::SVGDocument ParseOrDie() {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto result = svg::parser::SVGParser::ParseSVG(kTrivialSvg, sink);
  EXPECT_FALSE(result.hasError());
  return std::move(result.result());
}

svg::SVGElement FirstRect(svg::SVGDocument& doc) {
  auto element = doc.querySelector("#r1");
  EXPECT_TRUE(element.has_value());
  return *element;
}

TEST(UndoTimelineTest, EmptyByDefault) {
  UndoTimeline timeline;
  EXPECT_FALSE(timeline.canUndo());
  EXPECT_EQ(timeline.entryCount(), 0u);
  EXPECT_FALSE(timeline.undo().has_value());
}

TEST(UndoTimelineTest, CaptureSnapshotRoundTripsThroughApply) {
  auto doc = ParseOrDie();
  auto rect = FirstRect(doc);
  auto graphics = rect.cast<svg::SVGGraphicsElement>();

  // Initial transform is identity; capture it.
  const UndoSnapshot initial = captureTransformSnapshot(rect);

  // Move the element.
  const Transform2d moved = Transform2d::Translate(Vector2d(50.0, 70.0));
  graphics.setTransform(moved);
  EXPECT_DOUBLE_EQ(graphics.transform().data[4], 50.0);
  EXPECT_DOUBLE_EQ(graphics.transform().data[5], 70.0);

  // Applying the initial snapshot must round-trip us back.
  applySnapshot(initial);
  EXPECT_DOUBLE_EQ(graphics.transform().data[4], 0.0);
  EXPECT_DOUBLE_EQ(graphics.transform().data[5], 0.0);
}

TEST(UndoTimelineTest, RecordAppendsAnEntry) {
  auto doc = ParseOrDie();
  auto rect = FirstRect(doc);

  UndoTimeline timeline;

  UndoSnapshot before{.element = rect, .transform = Transform2d()};
  UndoSnapshot after{.element = rect, .transform = Transform2d::Translate(Vector2d(10, 0))};

  timeline.record("Move", before, after);

  EXPECT_EQ(timeline.entryCount(), 1u);
  EXPECT_TRUE(timeline.canUndo());
  ASSERT_TRUE(timeline.nextUndoLabel().has_value());
  EXPECT_EQ(*timeline.nextUndoLabel(), "Move");
}

TEST(UndoTimelineTest, UndoRestoresBeforeStateAndAppendsUndoEntry) {
  auto doc = ParseOrDie();
  auto rect = FirstRect(doc);
  auto graphics = rect.cast<svg::SVGGraphicsElement>();

  UndoTimeline timeline;

  const Transform2d startXform = graphics.transform();
  const Transform2d movedXform = Transform2d::Translate(Vector2d(25, 40));

  // Move the element and record the drag.
  graphics.setTransform(movedXform);
  timeline.record("Move", UndoSnapshot{.element = rect, .transform = startXform},
                  UndoSnapshot{.element = rect, .transform = movedXform});

  // Undo should restore the start state and leave the timeline with two
  // entries (original + undo).
  auto restored = timeline.undo();
  ASSERT_TRUE(restored.has_value());
  applySnapshot(*restored);

  EXPECT_DOUBLE_EQ(graphics.transform().data[4], startXform.data[4]);
  EXPECT_DOUBLE_EQ(graphics.transform().data[5], startXform.data[5]);
  EXPECT_EQ(timeline.entryCount(), 2u);
}

TEST(UndoTimelineTest, UndoOfUndoReappliesTheOriginalMove) {
  auto doc = ParseOrDie();
  auto rect = FirstRect(doc);
  auto graphics = rect.cast<svg::SVGGraphicsElement>();

  UndoTimeline timeline;

  const Transform2d a = graphics.transform();
  const Transform2d b = Transform2d::Translate(Vector2d(30, 50));

  graphics.setTransform(b);
  timeline.record("Move", UndoSnapshot{.element = rect, .transform = a},
                  UndoSnapshot{.element = rect, .transform = b});

  // Undo: back to `a`.
  {
    auto restored = timeline.undo();
    ASSERT_TRUE(restored.has_value());
    applySnapshot(*restored);
    EXPECT_DOUBLE_EQ(graphics.transform().data[4], a.data[4]);
  }

  // Break the chain and start a new one: the new chain walks back through
  // the undo entry we just appended, and undoing it re-applies `b`.
  timeline.breakUndoChain();
  {
    auto restored = timeline.undo();
    ASSERT_TRUE(restored.has_value());
    applySnapshot(*restored);
    EXPECT_DOUBLE_EQ(graphics.transform().data[4], b.data[4]);
    EXPECT_DOUBLE_EQ(graphics.transform().data[5], b.data[5]);
  }
}

TEST(UndoTimelineTest, TransactionBeginCommitAppendsEntry) {
  auto doc = ParseOrDie();
  auto rect = FirstRect(doc);

  UndoTimeline timeline;

  EXPECT_FALSE(timeline.inTransaction());
  timeline.beginTransaction("Drag",
                            UndoSnapshot{.element = rect, .transform = Transform2d()});
  EXPECT_TRUE(timeline.inTransaction());

  timeline.commitTransaction(
      UndoSnapshot{.element = rect, .transform = Transform2d::Translate(Vector2d(10, 0))});

  EXPECT_FALSE(timeline.inTransaction());
  EXPECT_EQ(timeline.entryCount(), 1u);
}

TEST(UndoTimelineTest, AbortTransactionLeavesNoEntry) {
  auto doc = ParseOrDie();
  auto rect = FirstRect(doc);

  UndoTimeline timeline;

  timeline.beginTransaction("Drag",
                            UndoSnapshot{.element = rect, .transform = Transform2d()});
  timeline.abortTransaction();

  EXPECT_FALSE(timeline.inTransaction());
  EXPECT_EQ(timeline.entryCount(), 0u);
  EXPECT_FALSE(timeline.canUndo());
}

TEST(UndoTimelineTest, ClearResetsEverything) {
  auto doc = ParseOrDie();
  auto rect = FirstRect(doc);

  UndoTimeline timeline;
  timeline.record("A", UndoSnapshot{.element = rect, .transform = Transform2d()},
                  UndoSnapshot{.element = rect, .transform = Transform2d()});
  timeline.record("B", UndoSnapshot{.element = rect, .transform = Transform2d()},
                  UndoSnapshot{.element = rect, .transform = Transform2d()});
  EXPECT_EQ(timeline.entryCount(), 2u);

  timeline.clear();
  EXPECT_EQ(timeline.entryCount(), 0u);
  EXPECT_FALSE(timeline.canUndo());
}

}  // namespace
}  // namespace donner::editor
