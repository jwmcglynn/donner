#include "src/svg/graph/reference.h"

#include "src/svg/components/document_context.h"

namespace donner::svg {

std::optional<ResolvedReference> Reference::resolve(Registry& registry) const {
  // TODO: Full parsing support for URL references.
  if (StringUtils::StartsWith(href, std::string_view("#"))) {
    if (auto entity =
            registry.ctx().get<const components::DocumentContext>().getEntityById(href.substr(1));
        entity != entt::null) {
      return ResolvedReference{EntityHandle(registry, entity)};
    }
  }

  return std::nullopt;
}

}  // namespace donner::svg
