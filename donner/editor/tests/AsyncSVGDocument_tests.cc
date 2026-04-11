#include "donner/editor/AsyncSVGDocument.h"

#include "donner/svg/SVGGraphicsElement.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r1" x="0" y="0" width="10" height="10" fill="red"/>
       </svg>)";

constexpr std::string_view kReplacementSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <rect id="r2" x="50" y="50" width="20" height="20" fill="blue"/>
       </svg>)";

TEST(AsyncSVGDocumentTest, EmptyByDefault) {
  AsyncSVGDocument doc;
  EXPECT_FALSE(doc.hasDocument());
  EXPECT_EQ(doc.currentFrameVersion(), 0u);
  EXPECT_FALSE(doc.flushFrame());
  EXPECT_EQ(doc.currentFrameVersion(), 0u);
}

TEST(AsyncSVGDocumentTest, LoadFromStringSucceedsAndBumpsVersion) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  EXPECT_TRUE(doc.hasDocument());
  EXPECT_EQ(doc.currentFrameVersion(), 1u);

  auto rect = doc.document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
}

TEST(AsyncSVGDocumentTest, FlushAppliesQueuedSetTransform) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  const auto initialVersion = doc.currentFrameVersion();

  auto rect = doc.document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  const Transform2d translateBy = Transform2d::Translate(Vector2d(7.0, 11.0));
  doc.applyMutation(EditorCommand::SetTransformCommand(*rect, translateBy));

  EXPECT_TRUE(doc.flushFrame());
  EXPECT_GT(doc.currentFrameVersion(), initialVersion);

  auto graphicsElement = rect->cast<svg::SVGGraphicsElement>();
  const Transform2d after = graphicsElement.transform();
  EXPECT_DOUBLE_EQ(after.data[4], 7.0);
  EXPECT_DOUBLE_EQ(after.data[5], 11.0);
}

TEST(AsyncSVGDocumentTest, MultipleSetTransformsCoalesceAtFlush) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));

  auto rect = doc.document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  // Three writes, only the last should apply.
  doc.applyMutation(EditorCommand::SetTransformCommand(*rect, Transform2d::Translate(Vector2d(1.0, 0.0))));
  doc.applyMutation(EditorCommand::SetTransformCommand(*rect, Transform2d::Translate(Vector2d(2.0, 0.0))));
  doc.applyMutation(EditorCommand::SetTransformCommand(*rect, Transform2d::Translate(Vector2d(3.0, 0.0))));
  EXPECT_EQ(doc.queue().size(), 3u);

  EXPECT_TRUE(doc.flushFrame());

  auto graphicsElement = rect->cast<svg::SVGGraphicsElement>();
  EXPECT_DOUBLE_EQ(graphicsElement.transform().data[4], 3.0);
}

TEST(AsyncSVGDocumentTest, ReplaceDocumentSwapsTheTreeAndDropsPriorMutations) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  ASSERT_TRUE(doc.document().querySelector("#r1").has_value());

  auto rect = doc.document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());

  // Queue a SetTransform, then a ReplaceDocument. The SetTransform must
  // be dropped because its target entity belongs to the doomed document.
  doc.applyMutation(EditorCommand::SetTransformCommand(
      *rect, Transform2d::Translate(Vector2d(99.0, 99.0))));
  doc.applyMutation(EditorCommand::ReplaceDocumentCommand(std::string(kReplacementSvg)));

  EXPECT_TRUE(doc.flushFrame());

  EXPECT_FALSE(doc.document().querySelector("#r1").has_value());
  EXPECT_TRUE(doc.document().querySelector("#r2").has_value());
}

TEST(AsyncSVGDocumentTest, FlushIsNoOpWhenQueueIsEmpty) {
  AsyncSVGDocument doc;
  ASSERT_TRUE(doc.loadFromString(kTrivialSvg));
  const auto versionAfterLoad = doc.currentFrameVersion();

  EXPECT_FALSE(doc.flushFrame());
  EXPECT_EQ(doc.currentFrameVersion(), versionAfterLoad);
}

}  // namespace
}  // namespace donner::editor
