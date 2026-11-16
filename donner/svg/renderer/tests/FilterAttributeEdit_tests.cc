/// @file
/// Attribute edits on filter elements of an already rendered document reach the next frame.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGFEOffsetElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

/// A 2x2 solid red PNG as a data URI.
constexpr std::string_view kRedImageDataUri =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8AARAwQCgAf7gP9i18U1AAAAABJRU5E"
    "rkJggg==";

/// A 16x16 document whose rect is filtered by \p filter, a complete `<filter id="f">` element.
SVGDocument FilteredDocument(std::string_view filter) {
  return instantiateSubtree(
      "<defs>" + std::string(filter) +
          "</defs><rect x='4' y='4' width='8' height='8' fill='#804020' filter='url(#f)'/>",
      parser::SVGParser::Options(), Vector2i(16, 16));
}

/// The usual filter element around \p primitives.
std::string FilterOf(std::string_view primitives) {
  return "<filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='16' height='16' "
         "color-interpolation-filters='sRGB'>" +
         std::string(primitives) + "</filter>";
}

/// Filter `f`, which takes its region and primitives from \p href: `b` holds \p primitives and
/// `c` floods blue.
std::string HrefChainOf(std::string_view primitives, std::string_view href) {
  constexpr std::string_view kRegion =
      "filterUnits='userSpaceOnUse' x='0' y='0' width='16' height='16' "
      "color-interpolation-filters='sRGB'";
  return "<filter id='b' " + std::string(kRegion) + ">" + std::string(primitives) +
         "</filter><filter id='c' " + std::string(kRegion) +
         "><feFlood flood-color='blue'/></filter><filter id='f' href='" + std::string(href) + "'/>";
}

/// One edit: render \c base, apply the edit to \c selector, and expect the next render to match a
/// document authored as \c expected.
struct EditCase {
  const char* name;             //!< Label for the comparison outputs.
  std::string base;             //!< Complete filter element before the edit.
  const char* attribute;        //!< Edited attribute on `#p`.
  const char* value;            //!< New value, or nullptr to remove the attribute.
  std::string expected;         //!< Complete filter element as authored with the edit.
  const char* selector = "#p";  //!< The edited element.
};

void PrintTo(const EditCase& value, std::ostream* os) {
  *os << value.name;
}

class FilterAttributeEditTest : public testing::TestWithParam<EditCase> {};

TEST_P(FilterAttributeEditTest, StaticEditReachesTheNextFrame) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  const EditCase& item = GetParam();
  SVGDocument document = FilteredDocument(item.base);
  const RendererBitmap before = RenderDocumentWithActiveBackend(document);

  std::optional<SVGElement> target = document.querySelector(item.selector);
  ASSERT_THAT(target, testing::Optional(testing::_));
  if (item.value != nullptr) {
    target->setAttribute(xml::XMLQualifiedNameRef(item.attribute), item.value);
  } else {
    target->removeAttribute(xml::XMLQualifiedNameRef(item.attribute));
  }

  SVGDocument expectedDocument = FilteredDocument(item.expected);
  const RendererBitmap expected = RenderDocumentWithActiveBackend(expectedDocument);
  ASSERT_NE(before.pixels, expected.pixels)
      << "the edit must change the rendering for this case to detect a stale filter";
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithActiveBackend(document), expected,
                                       std::string(item.name) + "-static-edit",
                                       editor::tests::PixelmatchIdentityParams());
}

std::vector<EditCase> EditCases() {
  const std::string floods =
      "<feFlood flood-color='red' result='red'/><feFlood flood-color='blue' result='blue'/>";
  const auto filter = [&](std::string_view primitives) {
    return FilterOf(floods + std::string(primitives));
  };
  return {
      {"input", filter("<feOffset id='p' in='red'/>"), "in", "blue",
       filter("<feOffset in='blue'/>")},
      {"input2", filter("<feBlend id='p' in='SourceGraphic' in2='red' mode='multiply'/>"), "in2",
       "blue", filter("<feBlend in='SourceGraphic' in2='blue' mode='multiply'/>")},
      {"mode", filter("<feBlend id='p' in='SourceGraphic' in2='blue'/>"), "mode", "screen",
       filter("<feBlend in='SourceGraphic' in2='blue' mode='screen'/>")},
      {"offset", filter("<feOffset id='p' in='SourceGraphic'/>"), "dx", "3",
       filter("<feOffset in='SourceGraphic' dx='3'/>")},
      {"subregion", filter("<feFlood id='p' flood-color='blue'/>"), "x", "8",
       filter("<feFlood flood-color='blue' x='8'/>")},
      {"blur", filter("<feGaussianBlur id='p' in='SourceGraphic'/>"), "stdDeviation", "1.5",
       filter("<feGaussianBlur in='SourceGraphic' stdDeviation='1.5'/>")},
      {"matrix", filter("<feColorMatrix id='p' in='SourceGraphic' type='saturate'/>"), "values",
       ".3", filter("<feColorMatrix in='SourceGraphic' type='saturate' values='.3'/>")},
      {"merge-node",
       filter("<feMerge><feMergeNode in='SourceGraphic'/><feMergeNode id='p' in='red'/></feMerge>"),
       "in", "blue",
       filter("<feMerge><feMergeNode in='SourceGraphic'/><feMergeNode in='blue'/></feMerge>")},
      {"transfer",
       filter("<feComponentTransfer in='SourceGraphic'><feFuncR id='p' "
              "type='linear'/></feComponentTransfer>"),
       "slope", ".25",
       filter("<feComponentTransfer in='SourceGraphic'><feFuncR type='linear' "
              "slope='.25'/></feComponentTransfer>")},
      {"light",
       filter("<feDiffuseLighting in='SourceGraphic'><feDistantLight id='p' "
              "elevation='90'/></feDiffuseLighting>"),
       "elevation", "30",
       filter("<feDiffuseLighting in='SourceGraphic'><feDistantLight "
              "elevation='30'/></feDiffuseLighting>")},
      {"drop-shadow",
       filter("<feDropShadow id='p' in='SourceGraphic' stdDeviation='0' flood-color='blue'/>"),
       "dx", "-3",
       filter("<feDropShadow in='SourceGraphic' dx='-3' stdDeviation='0' flood-color='blue'/>")},
      {"image", FilterOf("<feImage id='p' href='" + std::string(kRedImageDataUri) + "'/>"), "href",
       "#missing", FilterOf("<feImage href='#missing'/>")},
      {"filter-region", FilterOf("<feFlood flood-color='blue'/>"), "width", "8",
       "<filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='8' height='16' "
       "color-interpolation-filters='sRGB'><feFlood flood-color='blue'/></filter>",
       "#f"},
      {"remove-input", filter("<feOffset id='p' in='red'/>"), "in", nullptr, filter("<feOffset/>")},
      {"remove-offset", filter("<feOffset id='p' in='SourceGraphic' dx='3'/>"), "dx", nullptr,
       filter("<feOffset in='SourceGraphic'/>")},
      {"invalid-offset", filter("<feOffset id='p' in='SourceGraphic' dx='3'/>"), "dx", "wide",
       filter("<feOffset in='SourceGraphic'/>")},
      {"point-light",
       filter("<feDiffuseLighting in='SourceGraphic'><fePointLight id='p' x='8' y='8' "
              "z='2'/></feDiffuseLighting>"),
       "z", "40",
       filter("<feDiffuseLighting in='SourceGraphic'><fePointLight x='8' y='8' "
              "z='40'/></feDiffuseLighting>")},
      {"spot-light",
       filter("<feSpecularLighting in='SourceGraphic' specularExponent='2'><feSpotLight id='p' "
              "x='8' y='8' z='12' pointsAtX='8' pointsAtY='8'/></feSpecularLighting>"),
       "limitingConeAngle", "5",
       filter("<feSpecularLighting in='SourceGraphic' specularExponent='2'><feSpotLight x='8' "
              "y='8' z='12' pointsAtX='8' pointsAtY='8' "
              "limitingConeAngle='5'/></feSpecularLighting>")},
      // An edit inside a filter that another filter references through href reaches the
      // referencing filter, and so does an edit of that href.
      {"href-chain-primitive", HrefChainOf("<feOffset id='p' in='SourceGraphic'/>", "#b"), "dx",
       "3", HrefChainOf("<feOffset in='SourceGraphic' dx='3'/>", "#b")},
      {"href-chain-link", HrefChainOf("<feOffset in='SourceGraphic' dx='3'/>", "#b"), "href", "#c",
       HrefChainOf("<feOffset in='SourceGraphic' dx='3'/>", "#c"), "#f"},
  };
}

INSTANTIATE_TEST_SUITE_P(Edits, FilterAttributeEditTest, testing::ValuesIn(EditCases()),
                         [](const testing::TestParamInfo<EditCase>& info) {
                           std::string name = info.param.name;
                           std::erase(name, '-');
                           return name;
                         });

// An attribute the filter parser does not interpret, such as a data attribute, is only stored: it
// changes no filter state, so it must not discard the rendered tree.
TEST(FilterAttributeEditInvalidation, GenericAttributeKeepsTheRenderTree) {
  if (!ActiveRendererSupportsFeature(RendererBackendFeature::FilterEffects)) {
    GTEST_SKIP() << "Filters are disabled in this configuration";
  }
  SVGDocument document = FilteredDocument(FilterOf("<feOffset id='p' dx='3'/>"));
  (void)RenderDocumentWithActiveBackend(document);
  auto& renderState = document.registry().ctx().get<components::RenderTreeState>();
  ASSERT_THAT(renderState.needsFullRebuild, testing::IsFalse());

  std::optional<SVGElement> offset = document.querySelector("#p");
  ASSERT_THAT(offset, testing::Optional(testing::_));
  offset->setAttribute("data-note", "kept");
  EXPECT_THAT(renderState.needsFullRebuild, testing::IsFalse());

  offset->setAttribute("dx", "4");
  EXPECT_THAT(renderState.needsFullRebuild, testing::IsTrue());
}

// A document assembled without the SVG parser has no way to interpret a filter element's own
// attributes, so an edit reports that the value was only stored instead of silently ignoring it.
TEST(FilterAttributeEditInvalidation, DocumentWithoutParserReportsUninterpretedEdit) {
  SVGDocument document;
  SVGFEOffsetElement offset = SVGFEOffsetElement::Create(document);
  const xml::ApplySourceEditResult result =
      document.setElementAttribute(offset, xml::XMLQualifiedNameRef("dx"), "3");
  EXPECT_THAT(result.diagnostic,
              testing::Optional(testing::Field(
                  &ParseDiagnostic::reason,
                  testing::Property(&RcString::str,
                                    testing::HasSubstr("filter attributes are interpreted only "
                                                       "in documents built by the SVG parser")))));
}

}  // namespace
}  // namespace donner::svg
