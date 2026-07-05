#include "donner/svg/components/text/TextPositioningPresentation.h"

#include <string_view>

#include "donner/base/parser/LengthParser.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/parser/ListParser.h"

namespace donner::svg::components {

namespace {

/// Parse a whitespace/comma separated list of lengths (units optional - bare numbers are user
/// units), matching the XML parse path's `ParseLengthAttribute` + `ListParser` semantics.
std::optional<SmallVector<Lengthd, 1>> ParseLengthList(std::string_view value) {
  using donner::parser::LengthParser;

  SmallVector<Lengthd, 1> list;
  bool ok = true;
  if (auto error = svg::parser::ListParser::Parse(value, [&](std::string_view token) {
        LengthParser::Options options;
        options.unitOptional = true;
        auto maybeLength = LengthParser::Parse(token, options);
        if (maybeLength.hasError() || maybeLength.result().consumedChars != token.size()) {
          ok = false;
          return;
        }
        list.push_back(maybeLength.result().length);
      })) {
    return std::nullopt;
  }
  if (!ok) {
    return std::nullopt;
  }
  return list;
}

/// Parse a whitespace/comma separated list of rotations in degrees.
std::optional<SmallVector<double, 1>> ParseRotateList(std::string_view value) {
  using donner::parser::NumberParser;

  SmallVector<double, 1> list;
  bool ok = true;
  if (auto error = svg::parser::ListParser::Parse(value, [&](std::string_view token) {
        auto maybeNumber = NumberParser::Parse(token);
        if (maybeNumber.hasError() || maybeNumber.result().consumedChars != token.size()) {
          ok = false;
          return;
        }
        list.push_back(maybeNumber.result().number);
      })) {
    return std::nullopt;
  }
  if (!ok) {
    return std::nullopt;
  }
  return list;
}

/// Invalidate the enclosing text root's cached layout, mirroring
/// `SVGTextContentElement::invalidateTextGeometry()`.
void InvalidateTextRootLayout(EntityHandle handle) {
  Registry& registry = *handle.registry();
  Entity current = handle.entity();
  while (current != entt::null) {
    if (registry.any_of<TextRootComponent>(current)) {
      registry.remove<ComputedTextGeometryComponent>(current);
      registry.remove<ComputedTextComponent>(current);
      registry.get_or_emplace<DirtyFlagsComponent>(current).mark(
          DirtyFlagsComponent::TextGeometry | DirtyFlagsComponent::RenderInstance);
      return;
    }
    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      break;
    }
    current = tree->parent();
  }
}

}  // namespace

ParseResult<bool> ParseTextPositioningPresentationAttribute(
    EntityHandle handle, std::string_view name, const parser::PropertyParseFnParams& params) {
  const bool isX = name == "x";
  const bool isY = name == "y";
  const bool isDx = name == "dx";
  const bool isDy = name == "dy";
  const bool isRotate = name == "rotate";
  if (!isX && !isY && !isDx && !isDy && !isRotate) {
    return false;
  }

  // These are attribute-only inputs (not CSS properties), so only the raw
  // attribute-value form is handled.
  const std::string_view* rawValue = std::get_if<std::string_view>(&params.valueOrComponents);
  if (rawValue == nullptr) {
    return false;
  }

  auto& positioning = handle.get_or_emplace<TextPositioningComponent>();
  if (isRotate) {
    std::optional<SmallVector<double, 1>> list = ParseRotateList(*rawValue);
    if (!list.has_value()) {
      ParseDiagnostic err;
      err.reason = "Invalid " + std::string(name) + " list '" + std::string(*rawValue) + "'";
      return err;
    }
    positioning.rotateDegrees = std::move(*list);
  } else {
    std::optional<SmallVector<Lengthd, 1>> list = ParseLengthList(*rawValue);
    if (!list.has_value()) {
      ParseDiagnostic err;
      err.reason = "Invalid " + std::string(name) + " list '" + std::string(*rawValue) + "'";
      return err;
    }
    if (isX) {
      positioning.x = std::move(*list);
    } else if (isY) {
      positioning.y = std::move(*list);
    } else if (isDx) {
      positioning.dx = std::move(*list);
    } else {
      positioning.dy = std::move(*list);
    }
  }

  InvalidateTextRootLayout(handle);
  return true;
}

}  // namespace donner::svg::components
