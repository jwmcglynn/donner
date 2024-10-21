#include "donner/svg/graph/Reference.h"

#include "donner/svg/components/SVGDocumentContext.h"

namespace donner::svg {

std::optional<ResolvedReference> Reference::resolve(Registry& registry) const {
  // TODO: Full parsing support for URL references.
  if (StringUtils::StartsWith(href, std::string_view("#"))) {
    if (auto entity = registry.ctx().get<const components::SVGDocumentContext>().getEntityById(
            href.substr(1));
        entity != entt::null) {
      return ResolvedReference{EntityHandle(registry, entity)};
    }
  }

  return std::nullopt;
}

}  // namespace donner::svg
