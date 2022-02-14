#include "src/svg/components/path_component.h"

#include "src/svg/components/computed_path_component.h"
#include "src/svg/parser/path_parser.h"
#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

namespace {

ParseResult<RcString> ParseD(std::span<const css::ComponentValue> components) {
  if (auto maybeIdent = TryGetSingleIdent(components);
      maybeIdent && maybeIdent->equalsLowercase("none")) {
    return RcString();
  } else if (components.size() == 1) {
    if (const auto* str = components[0].tryGetToken<css::Token::String>()) {
      return str->value;
    }
  }

  ParseError err;
  err.reason = "Expected string or 'none'";
  err.offset = !components.empty() ? components.front().sourceOffset() : 0;
  return err;
}

std::optional<ParseError> ParseDFromAttributes(PathComponent& properties,
                                               const PropertyParseFnParams& params) {
  if (const std::string_view* str = std::get_if<std::string_view>(&params.valueOrComponents)) {
    properties.d.set(RcString(*str), params.specificity);
  } else {
    auto maybeError = Parse(
        params, [](const PropertyParseFnParams& params) { return ParseD(params.components()); },
        &properties.d);
    if (maybeError) {
      return std::move(maybeError.value());
    }
  }

  return std::nullopt;
}

}  // namespace

std::optional<ParseError> PathComponent::computePathWithPrecomputedStyle(
    EntityHandle handle, const ComputedStyleComponent& style) {
  Property<RcString> actualD = d;
  const auto& properties = style.properties().unparsedProperties;
  if (auto it = properties.find("d"); it != properties.end()) {
    auto maybeError = Parse(
        CreateParseFnParams(it->second.declaration, it->second.specificity),
        [](const PropertyParseFnParams& params) { return ParseD(params.components()); }, &actualD);
    if (maybeError) {
      return std::move(maybeError.value());
    }
  }

  if (actualD.hasValue()) {
    auto maybePath = PathParser::Parse(actualD.get().value());
    if (maybePath.hasError()) {
      handle.remove<ComputedPathComponent>();
      return std::move(maybePath.error());
    }

    if (maybePath.hasResult()) {
      auto& computedPath = handle.get_or_emplace<ComputedPathComponent>();
      if (!maybePath.result().empty()) {
        computedPath.spline = std::move(maybePath.result());
      } else {
        computedPath.spline = std::nullopt;
      }
      computedPath.userPathLength = userPathLength;
    }
  } else {
    handle.remove<ComputedPathComponent>();
  }

  return std::nullopt;
}

std::optional<ParseError> PathComponent::computePath(EntityHandle handle) {
  ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
  style.computeProperties(handle);
  return computePathWithPrecomputedStyle(handle, style);
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Path>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  if (name == "d") {
    auto maybeError = ParseDFromAttributes(handle.get_or_emplace<PathComponent>(), params);
    if (maybeError) {
      return std::move(maybeError).value();
    } else {
      // Property found and parsed successfully.
      return true;
    }
  }
  return false;
}

void InstantiateComputedPathComponents(Registry& registry, std::vector<ParseError>* outWarnings) {
  for (auto view = registry.view<PathComponent, ComputedStyleComponent>(); auto entity : view) {
    auto [path, style] = view.get(entity);
    auto maybeError = path.computePathWithPrecomputedStyle(EntityHandle(registry, entity), style);
    if (maybeError && outWarnings) {
      outWarnings->emplace_back(std::move(maybeError.value()));
    }
  }
}

}  // namespace donner::svg
