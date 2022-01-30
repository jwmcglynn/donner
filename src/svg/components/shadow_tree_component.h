#pragma once

#include "src/base/rc_string.h"
#include "src/svg/components/document_context.h"
#include "src/svg/components/registry.h"

namespace donner {

class ShadowTreeComponent {
public:
  ShadowTreeComponent(RcString href) : href_(href) {}

  RcString href() const { return href_; }

  Entity targetEntity(Registry& registry) {
    // TODO: Full parsing support for URL references.
    if (StringUtils::StartsWith(href_, std::string_view("#"))) {
      auto& idToEntity = registry.ctx<DocumentContext>().idToEntity;
      if (auto it = idToEntity.find(href_.substr(1)); it != idToEntity.end()) {
        return it->second;
      }
    }

    return entt::null;
  }

private:
  RcString href_;
};

}  // namespace donner
