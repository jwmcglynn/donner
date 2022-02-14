#pragma once

#include "src/base/rc_string.h"
#include "src/svg/components/document_context.h"
#include "src/svg/components/registry.h"

namespace donner::svg {

class ShadowTreeComponent {
public:
  ShadowTreeComponent(RcString href) : href_(href) {}

  RcString href() const { return href_; }

  Entity targetEntity(Registry& registry) {
    // TODO: Full parsing support for URL references.
    if (StringUtils::StartsWith(href_, std::string_view("#"))) {
      if (auto entity = registry.ctx<const DocumentContext>().getEntityById(href_.substr(1));
          entity != entt::null) {
        return entity;
      }
    }

    return entt::null;
  }

private:
  RcString href_;
};

}  // namespace donner::svg
