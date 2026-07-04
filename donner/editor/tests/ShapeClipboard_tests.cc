/// @file
/// Unit tests for the shape clipboard helpers.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/ShapeClipboardCommands.h"
#include "donner/editor/ShapeClipboardPayload.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {

namespace {

using ::testing::HasSubstr;
using ::testing::Not;

/// Parse \p svg into a source-backed SVGDocument, asserting success.
svg::SVGDocument Parse(std::string_view svg) {
  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(svg, sink);
  EXPECT_FALSE(result.hasError()) << "parse failed for: " << svg;
  return result.result();
}

/// Collect the selected elements named by \p ids from \p document.
std::vector<svg::SVGElement> Select(svg::SVGDocument& document,
                                    const std::vector<std::string>& ids) {
  std::vector<svg::SVGElement> selection;
  for (const std::string& id : ids) {
    auto element = document.querySelector("#" + id);
    EXPECT_TRUE(element.has_value()) << "missing #" << id;
    if (element.has_value()) {
      selection.push_back(*element);
    }
  }
  return selection;
}

constexpr std::string_view kTwoRects =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect id="a" x="0" y="0" width="10" height="10" fill="red"/>
<rect id="b" x="20" y="20" width="10" height="10" fill="blue"/>
</svg>)";

}  // namespace

TEST(ShapeClipboardPayload, HeaderedRoundTrip) {
  ShapeClipboardPayload payload;
  payload.svgFragment = R"(<rect id="a" x="0" y="0" width="10" height="10"/>)";
  payload.sourceElementIds = {"a", ""};
  payload.wasGroupSelection = false;
  payload.documentBounds = Box2d(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0));

  const std::string text = payload.toClipboardText();
  EXPECT_THAT(text, HasSubstr(std::string(ShapeClipboardPayload::kHeader)));

  std::optional<ShapeClipboardPayload> parsed = ShapeClipboardPayload::parse(text);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->svgFragment, payload.svgFragment);
  EXPECT_EQ(parsed->sourceElementIds, payload.sourceElementIds);
  EXPECT_FALSE(parsed->wasGroupSelection);
  ASSERT_TRUE(parsed->documentBounds.has_value());
  EXPECT_DOUBLE_EQ(parsed->documentBounds->topLeft.x, 1.0);
  EXPECT_DOUBLE_EQ(parsed->documentBounds->bottomRight.y, 4.0);
}

TEST(ShapeClipboardPayload, HeaderlessTextIsBestEffortFragment) {
  const std::string raw = R"(<rect id="z" x="0" y="0" width="5" height="5"/>)";
  std::optional<ShapeClipboardPayload> parsed = ShapeClipboardPayload::parse(raw);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->svgFragment, raw);
  EXPECT_TRUE(parsed->sourceElementIds.empty());
  EXPECT_FALSE(parsed->documentBounds.has_value());
}

TEST(ShapeClipboardPayload, EmptyTextRejected) {
  EXPECT_FALSE(ShapeClipboardPayload::parse("").has_value());
  EXPECT_FALSE(ShapeClipboardPayload::parse("   \n\t ").has_value());
}

TEST(ShapeClipboardCommands, CopySerializesStableFragment) {
  svg::SVGDocument document = Parse(kTwoRects);
  std::vector<svg::SVGElement> selection = Select(document, {"a"});

  std::optional<ShapeClipboardPayload> payload = copySelectionToPayload(document, selection);
  ASSERT_TRUE(payload.has_value());
  // The fragment is the source-store substring, so the id and geometry survive
  // verbatim.
  EXPECT_THAT(payload->svgFragment, HasSubstr("id=\"a\""));
  EXPECT_THAT(payload->svgFragment, HasSubstr("width=\"10\""));
  ASSERT_EQ(payload->sourceElementIds.size(), 1u);
  EXPECT_EQ(payload->sourceElementIds[0], "a");

  // Copy is deterministic: copying the same selection twice yields identical
  // fragment bytes.
  std::optional<ShapeClipboardPayload> payload2 = copySelectionToPayload(document, selection);
  ASSERT_TRUE(payload2.has_value());
  EXPECT_EQ(payload2->svgFragment, payload->svgFragment);
}

TEST(ShapeClipboardCommands, CopyEmptySelectionReturnsNullopt) {
  svg::SVGDocument document = Parse(kTwoRects);
  EXPECT_FALSE(copySelectionToPayload(document, {}).has_value());
}

TEST(ShapeClipboardCommands, CopyGroupSelectionMarksPayloadAsGroupSelection) {
  constexpr std::string_view kGrouped =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<g id="group"><rect id="inside" x="0" y="0" width="10" height="10"/></g>
</svg>)";
  svg::SVGDocument document = Parse(kGrouped);
  std::optional<ShapeClipboardPayload> payload =
      copySelectionToPayload(document, Select(document, {"group"}));

  ASSERT_TRUE(payload.has_value());
  EXPECT_TRUE(payload->wasGroupSelection);
  EXPECT_THAT(payload->svgFragment, HasSubstr("<g id=\"group\">"));
  EXPECT_THAT(payload->sourceElementIds, ::testing::ElementsAre("group"));
  EXPECT_FALSE(payload->documentBounds.has_value())
      << "group-only selections do not have geometry bounds of their own";
}

TEST(ShapeClipboardCommands, PasteInsertsInsideRootAndOffsets) {
  svg::SVGDocument document = Parse(kTwoRects);
  std::optional<ShapeClipboardPayload> payload =
      copySelectionToPayload(document, Select(document, {"a"}));
  ASSERT_TRUE(payload.has_value());

  PreparePasteResult result = preparePaste(document, *payload, PastePlacement::EndOfRootOffset);
  ASSERT_TRUE(result.ok) << result.error;

  // Inserted before the root </svg>, not after it.
  const std::size_t close = result.mergedSource.rfind("</svg>");
  ASSERT_NE(close, std::string::npos);
  EXPECT_THAT(result.mergedSource.substr(0, close), HasSubstr("translate(20,20)"));
  EXPECT_EQ(result.mergedSource.find("</svg>", close + 1), std::string::npos)
      << "paste must not add content after the root close tag";

  // Colliding id "a" is renamed deterministically.
  EXPECT_THAT(result.mergedSource, HasSubstr("id=\"a_pasted\""));
  ASSERT_EQ(result.pastedElementIds.size(), 1u);
  EXPECT_EQ(result.pastedElementIds[0], "a_pasted");

  // The merged source still parses.
  svg::SVGDocument pasted = Parse(result.mergedSource);
  EXPECT_TRUE(pasted.querySelector("#a").has_value());
  EXPECT_TRUE(pasted.querySelector("#a_pasted").has_value());
}

TEST(ShapeClipboardCommands, PasteWithoutIdSelectsGeneratedWrapper) {
  svg::SVGDocument document = Parse(kTwoRects);
  ShapeClipboardPayload payload;
  payload.svgFragment = R"(<circle cx="5" cy="6" r="4"/>)";

  PreparePasteResult result = preparePaste(document, payload, PastePlacement::EndOfRootOffset);
  ASSERT_TRUE(result.ok) << result.error;

  ASSERT_EQ(result.pastedElementIds.size(), 1u);
  EXPECT_EQ(result.pastedElementIds[0], "donner-paste_pasted");
  EXPECT_THAT(result.mergedSource, HasSubstr("id=\"donner-paste_pasted\""));
}

TEST(ShapeClipboardCommands, PasteRegeneratesIdsDeterministically) {
  svg::SVGDocument document = Parse(kTwoRects);
  std::optional<ShapeClipboardPayload> payload =
      copySelectionToPayload(document, Select(document, {"a", "b"}));
  ASSERT_TRUE(payload.has_value());

  PreparePasteResult first = preparePaste(document, *payload, PastePlacement::EndOfRootOffset);
  PreparePasteResult second = preparePaste(document, *payload, PastePlacement::EndOfRootOffset);
  ASSERT_TRUE(first.ok);
  ASSERT_TRUE(second.ok);
  // Same inputs → identical id assignment and merged output.
  EXPECT_EQ(first.pastedElementIds, second.pastedElementIds);
  EXPECT_EQ(first.mergedSource, second.mergedSource);
  EXPECT_THAT(first.pastedElementIds, ::testing::ElementsAre("a_pasted", "b_pasted"));
}

TEST(ShapeClipboardCommands, PasteUniqueIdsKeepsFragmentIdsAndIgnoresIdSubstrings) {
  svg::SVGDocument document = Parse(kTwoRects);
  ShapeClipboardPayload payload;
  payload.svgFragment =
      R"(<rect gradientid="ignored" id='fresh' x="1" y="2" width="3" height="4"/>)";
  payload.sourceElementIds = {"fresh"};

  PreparePasteResult result = preparePaste(document, payload, PastePlacement::EndOfRootOffset);
  ASSERT_TRUE(result.ok) << result.error;

  EXPECT_THAT(result.pastedElementIds, ::testing::ElementsAre("fresh"));
  EXPECT_THAT(result.mergedSource, HasSubstr("gradientid=\"ignored\""));
  EXPECT_THAT(result.mergedSource, HasSubstr("id='fresh'"));
  EXPECT_THAT(result.mergedSource, Not(HasSubstr("fresh_pasted")));
}

TEST(ShapeClipboardCommands, PasteRepairsUrlAndHrefReferencesToRenamedIds) {
  constexpr std::string_view kDestination =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<defs><linearGradient id="grad"/></defs>
<rect id="shape" x="0" y="0" width="10" height="10"/>
</svg>)";
  svg::SVGDocument document = Parse(kDestination);
  ShapeClipboardPayload payload;
  payload.svgFragment =
      R"SVG(<defs><linearGradient id='grad'><stop offset="0" stop-color="red"/></linearGradient></defs>
<rect id="shape" x="1" y="1" width="10" height="10" fill="url(#grad)"/>
<use xlink:href='#shape'/>)SVG";
  payload.sourceElementIds = {"shape"};

  PreparePasteResult result = preparePaste(document, payload, PastePlacement::EndOfRootOffset);
  ASSERT_TRUE(result.ok) << result.error;

  EXPECT_THAT(result.pastedElementIds, ::testing::ElementsAre("shape_pasted"));
  EXPECT_THAT(result.mergedSource, HasSubstr("id='grad_pasted'"));
  EXPECT_THAT(result.mergedSource, HasSubstr("fill=\"url(#grad_pasted)\""));
  EXPECT_THAT(result.mergedSource, HasSubstr("xlink:href='#shape_pasted'"));
}

TEST(ShapeClipboardCommands, PasteIdRepairSkipsAlreadyTakenGeneratedIds) {
  constexpr std::string_view kDestination =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect id="shape" x="0" y="0" width="10" height="10"/>
<rect id="shape_pasted" x="20" y="0" width="10" height="10"/>
</svg>)";
  svg::SVGDocument document = Parse(kDestination);
  ShapeClipboardPayload payload;
  payload.svgFragment = R"(<rect id="shape" x="1" y="1" width="10" height="10"/>)";
  payload.sourceElementIds = {"shape"};

  PreparePasteResult result = preparePaste(document, payload, PastePlacement::EndOfRootOffset);
  ASSERT_TRUE(result.ok) << result.error;

  EXPECT_THAT(result.pastedElementIds, ::testing::ElementsAre("shape_pasted2"));
  EXPECT_THAT(result.mergedSource, HasSubstr("id=\"shape_pasted2\""));
}

TEST(ShapeClipboardCommands, PasteInFrontHasNoOffsetAndPaintsAfterSource) {
  svg::SVGDocument document = Parse(kTwoRects);
  std::optional<ShapeClipboardPayload> payload =
      copySelectionToPayload(document, Select(document, {"a"}));
  ASSERT_TRUE(payload.has_value());

  PreparePasteResult result = preparePaste(document, *payload, PastePlacement::InFrontNoOffset);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_THAT(result.mergedSource, Not(HasSubstr("translate(20,20)")));

  // The pasted copy is the last child of the root → painted in front. Its id
  // appears after the original #a in source order.
  const std::size_t origPos = result.mergedSource.find("id=\"a\"");
  const std::size_t pastedPos = result.mergedSource.find("id=\"a_pasted\"");
  ASSERT_NE(origPos, std::string::npos);
  ASSERT_NE(pastedPos, std::string::npos);
  EXPECT_LT(origPos, pastedPos);
}

TEST(ShapeClipboardCommands, PasteIntoSelectedGroupLandsInsideGroup) {
  constexpr std::string_view kGrouped =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<g id="grp"><rect id="inside" x="0" y="0" width="4" height="4"/></g>
<rect id="loose" x="50" y="50" width="4" height="4"/>
</svg>)";
  svg::SVGDocument document = Parse(kGrouped);
  std::optional<ShapeClipboardPayload> payload =
      copySelectionToPayload(document, Select(document, {"loose"}));
  ASSERT_TRUE(payload.has_value());

  std::optional<svg::SVGElement> group = document.querySelector("#grp");
  ASSERT_TRUE(group.has_value());
  PreparePasteResult result =
      preparePaste(document, *payload, PastePlacement::EndOfRootOffset, group);
  ASSERT_TRUE(result.ok) << result.error;

  // The pasted wrapper id must appear before the group's closing </g>.
  const std::size_t wrapperPos = result.mergedSource.find("loose_pasted");
  const std::size_t groupClose = result.mergedSource.find("</g>");
  ASSERT_NE(wrapperPos, std::string::npos);
  ASSERT_NE(groupClose, std::string::npos);
  EXPECT_LT(wrapperPos, groupClose);
}

TEST(ShapeClipboardCommands, PasteWithNonGroupSelectedElementFallsBackToRoot) {
  svg::SVGDocument document = Parse(kTwoRects);
  std::optional<ShapeClipboardPayload> payload =
      copySelectionToPayload(document, Select(document, {"b"}));
  ASSERT_TRUE(payload.has_value());

  std::optional<svg::SVGElement> rect = document.querySelector("#a");
  ASSERT_TRUE(rect.has_value());
  PreparePasteResult result =
      preparePaste(document, *payload, PastePlacement::EndOfRootOffset, rect);
  ASSERT_TRUE(result.ok) << result.error;

  const std::size_t rectClose =
      result.mergedSource.find("/>", result.mergedSource.find("id=\"a\""));
  const std::size_t wrapperPos = result.mergedSource.find("b_pasted");
  const std::size_t rootClose = result.mergedSource.rfind("</svg>");
  ASSERT_NE(rectClose, std::string::npos);
  ASSERT_NE(wrapperPos, std::string::npos);
  ASSERT_NE(rootClose, std::string::npos);
  EXPECT_GT(wrapperPos, rectClose);
  EXPECT_LT(wrapperPos, rootClose);
}

TEST(ShapeClipboardCommands, FailedPasteLeavesDocumentUnchanged) {
  svg::SVGDocument document = Parse(kTwoRects);

  // Fragment references an id that exists neither in the fragment nor the
  // destination → unrepairable → must fail without producing merged source.
  ShapeClipboardPayload payload;
  payload.svgFragment =
      R"SVG(<rect id="needs-ref" x="0" y="0" width="5" height="5" fill="url(#missing-grad)"/>)SVG";
  payload.sourceElementIds = {"needs-ref"};

  const std::string sourceBefore(document.source());
  PreparePasteResult result = preparePaste(document, payload, PastePlacement::EndOfRootOffset);
  EXPECT_FALSE(result.ok);
  EXPECT_THAT(result.error, HasSubstr("missing-grad"));
  EXPECT_TRUE(result.mergedSource.empty());
  // The destination document source is untouched (preparePaste never mutates).
  EXPECT_EQ(std::string(document.source()), sourceBefore);
}

TEST(ShapeClipboardCommands, FailedPasteOnMalformedFragment) {
  svg::SVGDocument document = Parse(kTwoRects);
  ShapeClipboardPayload payload;
  payload.svgFragment = "<rect <<< not valid";
  PreparePasteResult result = preparePaste(document, payload, PastePlacement::EndOfRootOffset);
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.mergedSource.empty());
}

TEST(ShapeClipboardCommands, CopyModifyPasteRoundTripPreservesGeometryAtOffset) {
  // Copy → (independently) modify the doc → paste the original payload back.
  // The pasted geometry reflects the copied bytes, offset by translate(20,20),
  // regardless of intervening edits.
  svg::SVGDocument document = Parse(kTwoRects);
  std::optional<ShapeClipboardPayload> payload =
      copySelectionToPayload(document, Select(document, {"a"}));
  ASSERT_TRUE(payload.has_value());
  const std::string copiedFragment = payload->svgFragment;

  // Simulate a later edit by pasting into a different document with the same id
  // present; the payload's geometry must be preserved verbatim in the output.
  PreparePasteResult result = preparePaste(document, *payload, PastePlacement::EndOfRootOffset);
  ASSERT_TRUE(result.ok) << result.error;

  svg::SVGDocument pasted = Parse(result.mergedSource);
  std::optional<svg::SVGElement> pastedRect = pasted.querySelector("#a_pasted");
  ASSERT_TRUE(pastedRect.has_value());
  // The geometry attributes are unchanged from the original copy (offset is
  // carried by the wrapper transform, not by mutating the rect).
  EXPECT_THAT(copiedFragment, HasSubstr("width=\"10\""));
  EXPECT_THAT(result.mergedSource, HasSubstr("translate(20,20)"));
}

}  // namespace donner::editor
