#include "donner/editor/SourceStructuralMove.h"

#include <gtest/gtest.h>

#include <string>

#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorCommand.h"

namespace donner::editor {
namespace {

constexpr std::string_view kSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
  <g id="a">
    <rect id="r1" width="10" height="10"/>
    <g id="inner"><rect id="nested" width="2" height="2"/></g>
    <rect id="r2" width="10" height="10"/>
  </g>
  <g id="b"><circle id="c" r="4"/></g>
</svg>)svg";

svg::SVGElement RequiredElement(EditorApp& app, std::string_view selector) {
  std::optional<svg::SVGElement> element = app.document().document().querySelector(selector);
  EXPECT_TRUE(element.has_value()) << selector;
  return *element;
}

TEST(SourceStructuralMove, BuildsRevisionBoundCrossParentPlanAndCommitsThroughDom) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  svg::SVGElement r1 = RequiredElement(app, "#r1");
  svg::SVGElement b = RequiredElement(app, "#b");
  svg::SVGElement c = RequiredElement(app, "#c");

  SourceStructuralMoveEvaluation evaluation = BuildSourceStructuralMovePlan(app, kSvg, r1, b, c);

  ASSERT_EQ(evaluation.status, SourceStructuralMoveStatus::Ready);
  ASSERT_TRUE(evaluation.plan.has_value());
  EXPECT_EQ(kSvg.substr(evaluation.plan->elementRange.start,
                        evaluation.plan->elementRange.end - evaluation.plan->elementRange.start)
                .substr(0, 5),
            "<rect");
  EXPECT_EQ(kSvg.substr(evaluation.plan->insertionOffset, 7), "<circle");

  EXPECT_EQ(CommitSourceStructuralMove(app, *evaluation.plan, kSvg),
            SourceStructuralMoveStatus::Ready);
  ASSERT_TRUE(app.flushFrame());

  r1 = RequiredElement(app, "#r1");
  b = RequiredElement(app, "#b");
  EXPECT_EQ(r1.parentElement(), std::optional<svg::SVGElement>(b));
  EXPECT_EQ(b.firstChild(), std::optional<svg::SVGElement>(r1));
  const std::string source(app.document().document().source());
  EXPECT_LT(source.find("id=\"r1\"", source.find("id=\"b\"")), source.find("id=\"c\""));
}

TEST(SourceStructuralMove, RejectsRootCycleLockedAndNoOpMoves) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  svg::SVGElement root = app.document().document().svgElement();
  svg::SVGElement a = RequiredElement(app, "#a");
  svg::SVGElement inner = RequiredElement(app, "#inner");
  svg::SVGElement r1 = RequiredElement(app, "#r1");
  svg::SVGElement r2 = RequiredElement(app, "#r2");

  EXPECT_EQ(BuildSourceStructuralMovePlan(app, kSvg, root, a, std::nullopt).status,
            SourceStructuralMoveStatus::RootElement);
  EXPECT_EQ(BuildSourceStructuralMovePlan(app, kSvg, a, inner, std::nullopt).status,
            SourceStructuralMoveStatus::Cycle);
  EXPECT_EQ(BuildSourceStructuralMovePlan(app, kSvg, r1, a, inner).status,
            SourceStructuralMoveStatus::NoChange);

  app.setElementLocked(r1, true);
  ASSERT_TRUE(app.flushFrame());
  r1 = RequiredElement(app, "#r1");
  a = RequiredElement(app, "#a");
  r2 = RequiredElement(app, "#r2");
  EXPECT_EQ(
      BuildSourceStructuralMovePlan(app, app.document().document().source(), r1, a, r2).status,
      SourceStructuralMoveStatus::Locked);
}

TEST(SourceStructuralMove, RejectsPlanAfterDocumentAdvances) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kSvg));
  svg::SVGElement r1 = RequiredElement(app, "#r1");
  svg::SVGElement b = RequiredElement(app, "#b");
  SourceStructuralMoveEvaluation evaluation =
      BuildSourceStructuralMovePlan(app, kSvg, r1, b, std::nullopt);
  ASSERT_TRUE(evaluation.plan.has_value());

  app.applyMutation(EditorCommand::SetAttributeCommand(r1, "fill", "red"));
  ASSERT_TRUE(app.flushFrame());

  EXPECT_EQ(CommitSourceStructuralMove(app, *evaluation.plan, kSvg),
            SourceStructuralMoveStatus::StaleRevision);
}

TEST(SourceStructuralMove, AcceptsEditorSourceWithHiddenTerminalNewline) {
  EditorApp app;
  const std::string sourceWithNewline = std::string(kSvg) + "\n";
  ASSERT_TRUE(app.loadFromString(sourceWithNewline));
  svg::SVGElement r1 = RequiredElement(app, "#r1");
  svg::SVGElement b = RequiredElement(app, "#b");

  SourceStructuralMoveEvaluation evaluation =
      BuildSourceStructuralMovePlan(app, kSvg, r1, b, std::nullopt);
  ASSERT_EQ(evaluation.status, SourceStructuralMoveStatus::Ready);
  ASSERT_TRUE(evaluation.plan.has_value());
  EXPECT_EQ(CommitSourceStructuralMove(app, *evaluation.plan, kSvg),
            SourceStructuralMoveStatus::Ready);
}

}  // namespace
}  // namespace donner::editor
