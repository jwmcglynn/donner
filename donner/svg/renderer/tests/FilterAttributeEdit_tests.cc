/// @file
/// Attribute edits on filter elements of an already rendered document reach the next frame.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGFEOffsetElement.h"
#include "donner/svg/SVGFilterElement.h"
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

/// The usual filter element around a red flood with result `red`, a blue flood with result `blue`,
/// and then \p primitives.
std::string FloodsFilterOf(std::string_view primitives) {
  return FilterOf(
      "<feFlood flood-color='red' result='red'/><feFlood flood-color='blue' result='blue'/>" +
      std::string(primitives));
}

/// Filter `f`, which takes its region and primitives from \p href, or has no href when it is
/// empty: `b` holds \p primitives and `c` floods blue.
std::string HrefChainOf(std::string_view primitives, std::string_view href) {
  constexpr std::string_view kRegion =
      "filterUnits='userSpaceOnUse' x='0' y='0' width='16' height='16' "
      "color-interpolation-filters='sRGB'";
  return "<filter id='b' " + std::string(kRegion) + ">" + std::string(primitives) +
         "</filter><filter id='c' " + std::string(kRegion) +
         "><feFlood flood-color='blue'/></filter><filter id='f'" +
         (href.empty() ? std::string() : " href='" + std::string(href) + "'") + "/>";
}

/// Filter `f`, which takes its region and primitives from \p href through `xlink:href`, or has no
/// link when it is empty: `b` offsets the source graphic.
std::string XlinkChainOf(std::string_view href) {
  return "<filter id='b' filterUnits='userSpaceOnUse' x='0' y='0' width='16' height='16'>"
         "<feOffset in='SourceGraphic' dx='3'/></filter><filter id='f'" +
         (href.empty() ? std::string() : " xlink:href='" + std::string(href) + "'") + "/>";
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

/// The attribute name \p attribute, split at its namespace prefix such as `xlink:` when present.
xml::XMLQualifiedNameRef AttributeName(std::string_view attribute) {
  const std::size_t colon = attribute.find(':');
  return colon == std::string_view::npos
             ? xml::XMLQualifiedNameRef(attribute)
             : xml::XMLQualifiedNameRef(attribute.substr(0, colon), attribute.substr(colon + 1));
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
    target->setAttribute(AttributeName(item.attribute), item.value);
  } else {
    target->removeAttribute(AttributeName(item.attribute));
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
  const auto filter = FloodsFilterOf;
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
      {"invalid-subregion", filter("<feFlood id='p' flood-color='blue' x='8'/>"), "x", "wide",
       filter("<feFlood flood-color='blue'/>")},
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
      {"href-chain-subregion", HrefChainOf("<feFlood id='p' flood-color='blue'/>", "#b"), "x", "8",
       HrefChainOf("<feFlood flood-color='blue' x='8'/>", "#b")},
      {"href-chain-link", HrefChainOf("<feOffset in='SourceGraphic' dx='3'/>", "#b"), "href", "#c",
       HrefChainOf("<feOffset in='SourceGraphic' dx='3'/>", "#c"), "#f"},
  };
}

/// Names and values of an element's attributes, in authored order.
using Attributes = std::vector<std::pair<std::string_view, std::string_view>>;

/**
 * A removal of \p removed from the element \p selector, authored as \p tag with \p attributes and
 * \p children, whose expected document is the same element authored without that attribute.
 *
 * @param name Label for the comparison outputs.
 * @param wrap Places the element in a complete filter element.
 * @param tag The element's tag name.
 * @param attributes The element's authored attributes, which include \p removed.
 * @param removed The attribute to remove.
 * @param children The element's children.
 * @param selector `#` followed by the element's id.
 */
EditCase Removal(const char* name, const std::function<std::string(std::string_view)>& wrap,
                 std::string_view tag, const Attributes& attributes, const char* removed,
                 std::string_view children = {}, const char* selector = "#p") {
  const auto element = [&](bool withRemoved) {
    std::string markup =
        "<" + std::string(tag) + " id='" + std::string(std::string_view(selector).substr(1)) + "'";
    for (const auto& [key, value] : attributes) {
      if (withRemoved || key != removed) {
        markup += " " + std::string(key) + "='" + std::string(value) + "'";
      }
    }
    return markup + (children.empty()
                         ? std::string("/>")
                         : ">" + std::string(children) + "</" + std::string(tag) + ">");
  };
  return {name, wrap(element(true)), removed, nullptr, wrap(element(false)), selector};
}

/// Removing a filter attribute gives the state of a document that never had it, for each attribute
/// of every filter element family.
std::vector<EditCase> RemovalCases() {
  const auto filter = FloodsFilterOf;
  // Moves the source graphic to the region's top-left corner, so edge modes change the result.
  const auto corner = [](std::string_view primitive) {
    return FloodsFilterOf("<feOffset in='SourceGraphic' dx='-4' dy='-4'/>" +
                          std::string(primitive));
  };
  const auto transfer = [](std::string_view function) {
    return FloodsFilterOf("<feComponentTransfer in='SourceGraphic'>" + std::string(function) +
                          "</feComponentTransfer>");
  };
  const auto diffuse = [](std::string_view light) {
    return FloodsFilterOf("<feDiffuseLighting in='SourceGraphic' surfaceScale='5'>" +
                          std::string(light) + "</feDiffuseLighting>");
  };
  const auto merge = [](std::string_view node) {
    return FloodsFilterOf("<feMerge><feMergeNode in='SourceGraphic'/>" + std::string(node) +
                          "</feMerge>");
  };
  const auto self = [](std::string_view filterElement) { return std::string(filterElement); };
  const std::string distantLight = "<feDistantLight azimuth='45' elevation='45'/>";
  const std::string redImage = std::string(kRedImageDataUri);

  const Attributes blend = {{"in", "SourceGraphic"}, {"in2", "red"}, {"mode", "multiply"}};
  const Attributes arithmetic = {{"in", "SourceGraphic"},
                                 {"in2", "blue"},
                                 {"operator", "arithmetic"},
                                 {"k1", ".5"},
                                 {"k2", ".5"},
                                 {"k3", ".5"},
                                 {"k4", ".25"}};
  const Attributes divided = {
      {"in", "SourceGraphic"}, {"kernelMatrix", "1 1 1 1 1 1 1 1 1"}, {"divisor", "18"}};
  const Attributes targeted = {{"in", "SourceGraphic"},
                               {"kernelMatrix", "1 0 0 0 0 0 0 0 0"},
                               {"targetX", "0"},
                               {"targetY", "0"}};
  const Attributes diffuseLighting = {
      {"in", "SourceGraphic"}, {"surfaceScale", "5"}, {"diffuseConstant", ".5"}};
  const Attributes specularLighting = {{"in", "SourceGraphic"},
                                       {"surfaceScale", "5"},
                                       {"specularConstant", ".5"},
                                       {"specularExponent", "8"}};
  const Attributes distant = {{"azimuth", "90"}, {"elevation", "30"}};
  const Attributes point = {{"x", "8"}, {"y", "8"}, {"z", "2"}};
  const Attributes spot = {{"x", "8"},
                           {"y", "8"},
                           {"z", "12"},
                           {"pointsAtX", "4"},
                           {"pointsAtY", "8"},
                           {"pointsAtZ", "-8"},
                           {"specularExponent", "4"},
                           {"limitingConeAngle", "30"}};
  const Attributes displacement = {{"in", "SourceGraphic"},
                                   {"in2", "red"},
                                   {"scale", "8"},
                                   {"xChannelSelector", "B"},
                                   {"yChannelSelector", "B"}};
  const Attributes shadow = {
      {"in", "SourceGraphic"}, {"dx", "-3"},           {"dy", "-3"}, {"stdDeviation", "0"},
      {"flood-color", "blue"}, {"flood-opacity", ".5"}};
  const Attributes morphology = {{"in", "SourceGraphic"}, {"operator", "dilate"}, {"radius", "1"}};
  const Attributes turbulence = {{"baseFrequency", ".3"},
                                 {"numOctaves", "2"},
                                 {"seed", "3"},
                                 {"stitchTiles", "stitch"},
                                 {"type", "fractalNoise"}};
  const Attributes subregion = {
      {"flood-color", "blue"}, {"x", "8"}, {"y", "8"}, {"width", "4"}, {"height", "4"}};
  const Attributes region = {{"filterUnits", "userSpaceOnUse"},
                             {"primitiveUnits", "objectBoundingBox"},
                             {"x", "4"},
                             {"y", "4"},
                             {"width", "8"},
                             {"height", "8"},
                             {"color-interpolation-filters", "sRGB"}};
  const std::string regionChildren = "<feFlood flood-color='blue' x='.5'/>";
  // An even mix of the red and blue floods, whose color depends on the interpolation color space.
  constexpr std::string_view kMixedFloods =
      "<feComposite in='red' in2='blue' operator='arithmetic' k2='.5' k3='.5'/>";

  return {
      Removal("remove-blend-input2", filter, "feBlend", blend, "in2"),
      Removal("remove-blend-mode", filter, "feBlend", blend, "mode"),
      Removal("remove-matrix-type", filter, "feColorMatrix",
              {{"in", "SourceGraphic"}, {"type", "hueRotate"}, {"values", "90"}}, "type"),
      Removal("remove-matrix-values", filter, "feColorMatrix",
              {{"in", "SourceGraphic"}, {"type", "saturate"}, {"values", ".3"}}, "values"),
      Removal("remove-matrix-value-list", filter, "feColorMatrix",
              {{"in", "SourceGraphic"}, {"values", "0 0 1 0 0 0 1 0 0 0 1 0 0 0 0 0 0 0 1 0"}},
              "values"),
      Removal("remove-transfer-type", transfer, "feFuncA", {{"type", "linear"}, {"slope", ".5"}},
              "type"),
      Removal("remove-table-values", transfer, "feFuncR",
              {{"type", "table"}, {"tableValues", "0 0"}}, "tableValues"),
      Removal("remove-discrete-values", transfer, "feFuncG",
              {{"type", "discrete"}, {"tableValues", "1"}}, "tableValues"),
      Removal("remove-slope", transfer, "feFuncR", {{"type", "linear"}, {"slope", ".25"}}, "slope"),
      Removal("remove-intercept", transfer, "feFuncB", {{"type", "linear"}, {"intercept", ".5"}},
              "intercept"),
      Removal("remove-amplitude", transfer, "feFuncR",
              {{"type", "gamma"}, {"amplitude", ".5"}, {"exponent", "2"}}, "amplitude"),
      Removal("remove-exponent", transfer, "feFuncR", {{"type", "gamma"}, {"exponent", "3"}},
              "exponent"),
      Removal("remove-transfer-offset", transfer, "feFuncG", {{"type", "gamma"}, {"offset", ".5"}},
              "offset"),
      Removal("remove-composite-operator", filter, "feComposite",
              {{"in", "SourceGraphic"}, {"in2", "blue"}, {"operator", "xor"}}, "operator"),
      Removal("remove-composite-input2", filter, "feComposite",
              {{"in", "SourceGraphic"}, {"in2", "red"}, {"operator", "xor"}}, "in2"),
      Removal("remove-composite-k1", filter, "feComposite", arithmetic, "k1"),
      Removal("remove-composite-k2", filter, "feComposite", arithmetic, "k2"),
      Removal("remove-composite-k3", filter, "feComposite", arithmetic, "k3"),
      Removal("remove-composite-k4", filter, "feComposite", arithmetic, "k4"),
      Removal("remove-kernel-matrix", filter, "feConvolveMatrix",
              {{"in", "SourceGraphic"}, {"order", "3"}, {"kernelMatrix", "0 0 0 0 0 0 0 0 1"}},
              "kernelMatrix"),
      Removal("remove-order", filter, "feConvolveMatrix",
              {{"in", "SourceGraphic"}, {"order", "1"}, {"kernelMatrix", "2"}}, "order"),
      Removal("remove-divisor", filter, "feConvolveMatrix", divided, "divisor"),
      Removal("remove-bias", filter, "feConvolveMatrix",
              {{"in", "SourceGraphic"}, {"kernelMatrix", "0 0 0 0 1 0 0 0 0"}, {"bias", ".5"}},
              "bias"),
      Removal("remove-target-x", filter, "feConvolveMatrix", targeted, "targetX"),
      Removal("remove-target-y", filter, "feConvolveMatrix", targeted, "targetY"),
      Removal("remove-convolve-edge-mode", corner, "feConvolveMatrix",
              {{"kernelMatrix", "0 0 0 0 0 0 0 0 1"}, {"edgeMode", "none"}}, "edgeMode"),
      Removal("remove-preserve-alpha", filter, "feConvolveMatrix",
              {{"in", "SourceGraphic"},
               {"kernelMatrix", "0 0 0 0 .5 0 0 0 0"},
               {"divisor", "1"},
               {"preserveAlpha", "true"}},
              "preserveAlpha"),
      Removal("remove-diffuse-surface-scale", filter, "feDiffuseLighting", diffuseLighting,
              "surfaceScale", distantLight),
      Removal("remove-diffuse-constant", filter, "feDiffuseLighting", diffuseLighting,
              "diffuseConstant", distantLight),
      Removal("remove-specular-surface-scale", filter, "feSpecularLighting", specularLighting,
              "surfaceScale", distantLight),
      Removal("remove-specular-constant", filter, "feSpecularLighting", specularLighting,
              "specularConstant", distantLight),
      Removal("remove-specular-exponent", filter, "feSpecularLighting", specularLighting,
              "specularExponent", distantLight),
      Removal("remove-diffuse-lighting-color", filter, "feDiffuseLighting",
              {{"in", "SourceGraphic"}, {"lighting-color", "red"}}, "lighting-color", distantLight),
      Removal("remove-specular-lighting-color", filter, "feSpecularLighting",
              {{"in", "SourceGraphic"}, {"lighting-color", "red"}}, "lighting-color", distantLight),
      Removal("remove-azimuth", diffuse, "feDistantLight", distant, "azimuth"),
      Removal("remove-elevation", diffuse, "feDistantLight", distant, "elevation"),
      Removal("remove-point-x", diffuse, "fePointLight", point, "x"),
      Removal("remove-point-y", diffuse, "fePointLight", point, "y"),
      Removal("remove-point-z", diffuse, "fePointLight", point, "z"),
      Removal("remove-spot-x", diffuse, "feSpotLight", spot, "x"),
      Removal("remove-spot-y", diffuse, "feSpotLight", spot, "y"),
      Removal("remove-spot-z", diffuse, "feSpotLight", spot, "z"),
      Removal("remove-points-at-x", diffuse, "feSpotLight", spot, "pointsAtX"),
      Removal("remove-points-at-y", diffuse, "feSpotLight", spot, "pointsAtY"),
      Removal("remove-points-at-z", diffuse, "feSpotLight", spot, "pointsAtZ"),
      Removal("remove-spot-exponent", diffuse, "feSpotLight", spot, "specularExponent"),
      Removal("remove-cone-angle", diffuse, "feSpotLight", spot, "limitingConeAngle"),
      Removal("remove-displacement-scale", filter, "feDisplacementMap", displacement, "scale"),
      Removal("remove-displacement-input2", filter, "feDisplacementMap", displacement, "in2"),
      Removal("remove-x-channel", filter, "feDisplacementMap", displacement, "xChannelSelector"),
      Removal("remove-y-channel", filter, "feDisplacementMap", displacement, "yChannelSelector"),
      Removal("remove-shadow-dx", filter, "feDropShadow", shadow, "dx"),
      Removal("remove-shadow-dy", filter, "feDropShadow", shadow, "dy"),
      Removal("remove-shadow-deviation", filter, "feDropShadow", shadow, "stdDeviation"),
      Removal("remove-shadow-color", filter, "feDropShadow", shadow, "flood-color"),
      Removal("remove-shadow-opacity", filter, "feDropShadow", shadow, "flood-opacity"),
      Removal("remove-blur-deviation", filter, "feGaussianBlur",
              {{"in", "SourceGraphic"}, {"stdDeviation", "5"}}, "stdDeviation"),
      Removal("remove-blur-deviation-pair", filter, "feGaussianBlur",
              {{"in", "SourceGraphic"}, {"stdDeviation", "2 0"}}, "stdDeviation"),
      Removal("remove-blur-edge-mode", corner, "feGaussianBlur",
              {{"stdDeviation", "2"}, {"edgeMode", "wrap"}}, "edgeMode"),
      Removal("remove-image-href", FilterOf, "feImage", {{"href", redImage}}, "href"),
      Removal("remove-image-xlink-href", FilterOf, "feImage", {{"xlink:href", redImage}},
              "xlink:href"),
      Removal(
          "remove-image-aspect", FilterOf, "feImage",
          {{"href", redImage}, {"width", "16"}, {"height", "8"}, {"preserveAspectRatio", "none"}},
          "preserveAspectRatio"),
      Removal("remove-merge-input", merge, "feMergeNode", {{"in", "red"}}, "in"),
      Removal("remove-morphology-operator", filter, "feMorphology", morphology, "operator"),
      Removal("remove-morphology-radius", filter, "feMorphology", morphology, "radius"),
      Removal("remove-offset-y", filter, "feOffset", {{"in", "SourceGraphic"}, {"dy", "3"}}, "dy"),
      Removal("remove-tile-input", filter, "feTile", {{"in", "red"}}, "in"),
      Removal("remove-base-frequency", filter, "feTurbulence", turbulence, "baseFrequency"),
      Removal("remove-octaves", filter, "feTurbulence", turbulence, "numOctaves"),
      Removal("remove-seed", filter, "feTurbulence", turbulence, "seed"),
      Removal("remove-stitch-tiles", filter, "feTurbulence", turbulence, "stitchTiles"),
      Removal("remove-turbulence-type", filter, "feTurbulence", turbulence, "type"),
      Removal("remove-flood-color", FilterOf, "feFlood",
              {{"flood-color", "blue"}, {"flood-opacity", ".5"}}, "flood-color"),
      Removal("remove-flood-opacity", FilterOf, "feFlood",
              {{"flood-color", "blue"}, {"flood-opacity", ".5"}}, "flood-opacity"),
      Removal("remove-subregion-x", FilterOf, "feFlood", subregion, "x"),
      Removal("remove-subregion-y", FilterOf, "feFlood", subregion, "y"),
      Removal("remove-subregion-width", FilterOf, "feFlood", subregion, "width"),
      Removal("remove-subregion-height", FilterOf, "feFlood", subregion, "height"),
      {"remove-result",
       filter("<feOffset id='p' in='SourceGraphic' dx='3' result='red'/><feMerge><feMergeNode "
              "in='red'/></feMerge>"),
       "result", nullptr,
       filter("<feOffset id='p' in='SourceGraphic' dx='3'/><feMerge><feMergeNode "
              "in='red'/></feMerge>")},
      Removal("remove-filter-units", self, "filter", region, "filterUnits", regionChildren, "#f"),
      Removal("remove-primitive-units", self, "filter", region, "primitiveUnits", regionChildren,
              "#f"),
      Removal("remove-filter-x", self, "filter", region, "x", regionChildren, "#f"),
      Removal("remove-filter-width", self, "filter", region, "width", regionChildren, "#f"),
      Removal("remove-filter-y", self, "filter", region, "y", regionChildren, "#f"),
      Removal("remove-filter-height", self, "filter", region, "height", regionChildren, "#f"),
      Removal("remove-filter-interpolation", self, "filter", region, "color-interpolation-filters",
              "<feFlood flood-color='red' result='red'/><feFlood flood-color='blue' "
              "result='blue'/>" +
                  std::string(kMixedFloods),
              "#f"),
      Removal("remove-primitive-interpolation", filter, "feComposite",
              {{"in", "red"},
               {"in2", "blue"},
               {"operator", "arithmetic"},
               {"k2", ".5"},
               {"k3", ".5"},
               {"color-interpolation-filters", "linearRGB"}},
              "color-interpolation-filters"),
      {"remove-filter-href", HrefChainOf("<feOffset in='SourceGraphic' dx='3'/>", "#b"), "href",
       nullptr, HrefChainOf("<feOffset in='SourceGraphic' dx='3'/>", ""), "#f"},
      {"remove-filter-xlink-href", XlinkChainOf("#b"), "xlink:href", nullptr, XlinkChainOf(""),
       "#f"},
  };
}

/// Test name for \p info: the case name without dashes.
std::string EditCaseName(const testing::TestParamInfo<EditCase>& info) {
  std::string name = info.param.name;
  std::erase(name, '-');
  return name;
}

INSTANTIATE_TEST_SUITE_P(Edits, FilterAttributeEditTest, testing::ValuesIn(EditCases()),
                         EditCaseName);
INSTANTIATE_TEST_SUITE_P(Removals, FilterAttributeEditTest, testing::ValuesIn(RemovalCases()),
                         EditCaseName);

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

// The typed setters and attribute edits are both supported ways to edit a filter. An attribute
// edit changes only that attribute's state, so a value set through a setter survives an edit of
// another attribute.
TEST(FilterAttributeEditKeepsSetterState, OffsetSetterSurvivesAnotherOffsetEdit) {
  SVGDocument document =
      FilteredDocument(FilterOf("<feOffset id='p' in='SourceGraphic' dx='1' dy='2'/>"));
  std::optional<SVGElement> target = document.querySelector("#p");
  ASSERT_THAT(target, testing::Optional(testing::_));
  SVGFEOffsetElement offset = target->cast<SVGFEOffsetElement>();
  offset.setOffset(3, 4);
  offset.setAttribute("dx", "5");
  EXPECT_EQ(offset.dx(), 5.0);
  EXPECT_EQ(offset.dy(), 4.0);
  offset.removeAttribute("dx");
  EXPECT_EQ(offset.dx(), 0.0);
  EXPECT_EQ(offset.dy(), 4.0);
}

TEST(FilterAttributeEditKeepsSetterState, SubregionSetterSurvivesAPrimitiveEdit) {
  SVGDocument document =
      FilteredDocument(FilterOf("<feOffset id='p' in='SourceGraphic' x='1' dx='1'/>"));
  std::optional<SVGElement> target = document.querySelector("#p");
  ASSERT_THAT(target, testing::Optional(testing::_));
  SVGFEOffsetElement offset = target->cast<SVGFEOffsetElement>();
  offset.setX(Lengthd(6, Lengthd::Unit::None));
  offset.setAttribute("dx", "2");
  EXPECT_EQ(offset.x(), Lengthd(6, Lengthd::Unit::None));
  EXPECT_EQ(offset.dx(), 2.0);
}

TEST(FilterAttributeEditKeepsSetterState, FilterRegionSetterSurvivesARegionEdit) {
  SVGDocument document = FilteredDocument(FilterOf("<feFlood flood-color='blue'/>"));
  std::optional<SVGElement> target = document.querySelector("#f");
  ASSERT_THAT(target, testing::Optional(testing::_));
  SVGFilterElement filter = target->cast<SVGFilterElement>();
  filter.setX(Lengthd(4, Lengthd::Unit::None));
  filter.setAttribute("width", "8");
  EXPECT_EQ(filter.x(), Lengthd(4, Lengthd::Unit::None));
  EXPECT_EQ(filter.width(), Lengthd(8, Lengthd::Unit::None));
}

// A value the parser rejects with a warning keeps the attribute's default, as a fresh parse does,
// and the edit reports that warning.
TEST(FilterAttributeEditInvalidation, RejectedTypedValueReportsAWarning) {
  SVGDocument document = FilteredDocument(FilterOf("<feFlood id='p' flood-color='blue' x='8'/>"));
  std::optional<SVGElement> flood = document.querySelector("#p");
  ASSERT_THAT(flood, testing::Optional(testing::_));
  const xml::ApplySourceEditResult result =
      document.setElementAttribute(*flood, xml::XMLQualifiedNameRef("x"), "wide");
  EXPECT_THAT(
      result.diagnostic,
      testing::Optional(testing::AllOf(
          testing::Field(&ParseDiagnostic::severity, DiagnosticSeverity::Warning),
          testing::Field(
              &ParseDiagnostic::reason,
              testing::Property(&RcString::str, testing::HasSubstr("Failed to parse number"))))));
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
