#include "donner/svg/graph/Reference.h"

#include "donner/svg/components/SVGDocumentContext.h"

namespace donner::svg {

namespace {

/// Find the position of the '#' fragment delimiter in an href string.
/// Returns std::string_view::npos if there is no fragment.
size_t findFragmentDelimiter(std::string_view href) {
  // Data URLs may contain '#' in the encoded data, so skip past "data:" prefix.
  if (StringUtils::StartsWith(href, std::string_view("data:"))) {
    return std::string_view::npos;
  }

  return href.find('#');
}

}  // namespace

bool Reference::isExternal() const {
  const std::string_view sv(href);
  if (sv.empty()) {
    return false;
  }

  // A same-document reference starts with '#'.
  if (sv[0] == '#') {
    return false;
  }

  // Data URLs are not external document references.
  if (StringUtils::StartsWith(sv, std::string_view("data:"))) {
    return false;
  }

  return true;
}

std::string_view Reference::documentUrl() const {
  const std::string_view sv(href);
  if (sv.empty() || sv[0] == '#') {
    return {};
  }

  // Data URLs are inline content, not external document references.
  if (StringUtils::StartsWith(sv, std::string_view("data:"))) {
    return {};
  }

  const size_t hashPos = findFragmentDelimiter(sv);
  if (hashPos == std::string_view::npos) {
    return sv;
  }

  return sv.substr(0, hashPos);
}

std::string_view Reference::fragment() const {
  const std::string_view sv(href);
  if (sv.empty()) {
    return {};
  }

  // Same-document reference: "#id" → fragment is "id".
  if (sv[0] == '#') {
    return sv.substr(1);
  }

  // External reference: "file.svg#id" → fragment is "id".
  const size_t hashPos = findFragmentDelimiter(sv);
  if (hashPos == std::string_view::npos || hashPos + 1 >= sv.size()) {
    return {};
  }

  return sv.substr(hashPos + 1);
}

std::optional<ResolvedReference> Reference::resolve(Registry& registry) const {
  if (StringUtils::StartsWith(href, std::string_view("#"))) {
    if (auto entity = registry.ctx().get<const components::SVGDocumentContext>().getEntityById(
            href.substr(1));
        entity != entt::null) {
      return ResolvedReference{EntityHandle(registry, entity)};
    }
  }

  return std::nullopt;
}

std::optional<ResolvedReference> Reference::resolveFragment(Registry& registry) const {
  const std::string_view frag = fragment();
  if (frag.empty()) {
    return std::nullopt;
  }

  if (auto entity =
          registry.ctx().get<const components::SVGDocumentContext>().getEntityById(RcString(frag));
      entity != entt::null) {
    return ResolvedReference{EntityHandle(registry, entity)};
  }

  return std::nullopt;
}

}  // namespace donner::svg
