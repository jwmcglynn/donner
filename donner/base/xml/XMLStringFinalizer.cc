#include "donner/base/xml/XMLStringFinalizer.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcString.h"
#include "donner/base/Utils.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/base/xml/components/EntityDeclarationsContext.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/base/xml/components/XMLValueComponent.h"

namespace donner::xml {

namespace {

class StringInterner {
public:
  void reserve(std::size_t size) { originals_.reserve(size); }

  void add(const RcString& value) { originals_.push_back(value); }

  void finalize() {
    std::sort(originals_.begin(), originals_.end(), [](const RcString& lhs, const RcString& rhs) {
      return std::string_view(lhs) < std::string_view(rhs);
    });
    const auto uniqueEnd = std::unique(originals_.begin(), originals_.end());
    originals_.erase(uniqueEnd, originals_.end());

    std::size_t totalBytes = 0;
    for (const RcString& original : originals_) {
      UTILS_RELEASE_ASSERT(original.size() < std::numeric_limits<std::size_t>::max() - 1);
      UTILS_RELEASE_ASSERT(totalBytes <=
                           std::numeric_limits<std::size_t>::max() - original.size() - 1);
      totalBytes += original.size() + 1;
    }

    std::vector<char> bytes;
    bytes.reserve(totalBytes);
    for (const RcString& original : originals_) {
      bytes.insert(bytes.end(), original.begin(), original.end());
      bytes.push_back('\0');
    }
    const RcString page = RcString::fromVector(std::move(bytes));
    canonicals_.reserve(originals_.size());
    std::size_t offset = 0;
    for (const RcString& original : originals_) {
      canonicals_.push_back(page.substr(offset, original.size()));
      offset += original.size() + 1;
    }
  }

  RcString intern(const RcString& value) {
    const auto existing =
        std::lower_bound(originals_.begin(), originals_.end(), std::string_view(value),
                         [](const RcString& original, std::string_view candidate) {
                           return std::string_view(original) < candidate;
                         });
    UTILS_RELEASE_ASSERT(existing != originals_.end() && *existing == value);
    return canonicals_[static_cast<std::size_t>(existing - originals_.begin())];
  }

private:
  std::vector<RcString> originals_;
  std::vector<RcString> canonicals_;
};

}  // namespace

void FinalizeXMLDocumentStrings(XMLDocument& document) {
  Registry& registry = document.registry();
  StringInterner interner;
  std::size_t stringCount = 0;
  const auto count = [&stringCount](const RcString& value) {
    if (!value.fitsInlineStorage()) {
      ++stringCount;
    }
  };
  for (const Entity entity : registry.view<donner::components::TreeComponent>()) {
    registry.get<donner::components::TreeComponent>(entity).visitStrings(count);
  }
  for (const Entity entity : registry.view<donner::components::AttributesComponent>()) {
    registry.get<donner::components::AttributesComponent>(entity).visitStrings(count);
  }
  for (const Entity entity : registry.view<components::XMLValueComponent>()) {
    count(registry.get<components::XMLValueComponent>(entity).value);
  }
  if (registry.ctx().contains<components::EntityDeclarationsContext>()) {
    registry.ctx().get<components::EntityDeclarationsContext>().visitStrings(count);
  }
  registry.ctx().get<components::XMLNamespaceContext>().visitStrings(count);
  interner.reserve(stringCount);

  const auto collect = [&interner](const RcString& value) {
    if (!value.fitsInlineStorage()) {
      interner.add(value);
    }
  };

  for (const Entity entity : registry.view<donner::components::TreeComponent>()) {
    registry.get<donner::components::TreeComponent>(entity).visitStrings(collect);
  }
  for (const Entity entity : registry.view<donner::components::AttributesComponent>()) {
    registry.get<donner::components::AttributesComponent>(entity).visitStrings(collect);
  }
  for (const Entity entity : registry.view<components::XMLValueComponent>()) {
    collect(registry.get<components::XMLValueComponent>(entity).value);
  }
  if (registry.ctx().contains<components::EntityDeclarationsContext>()) {
    registry.ctx().get<components::EntityDeclarationsContext>().visitStrings(collect);
  }
  registry.ctx().get<components::XMLNamespaceContext>().visitStrings(collect);
  interner.finalize();

  const auto remap = [&interner](const RcString& value) {
    return value.fitsInlineStorage() ? RcString(std::string_view(value)) : interner.intern(value);
  };

  for (const Entity entity : registry.view<donner::components::TreeComponent>()) {
    registry.get<donner::components::TreeComponent>(entity).remapStrings(remap);
  }
  for (const Entity entity : registry.view<donner::components::AttributesComponent>()) {
    registry.get<donner::components::AttributesComponent>(entity).remapStrings(remap);
  }
  for (const Entity entity : registry.view<components::XMLValueComponent>()) {
    auto& value = registry.get<components::XMLValueComponent>(entity).value;
    value = remap(value);
  }

  if (registry.ctx().contains<components::EntityDeclarationsContext>()) {
    registry.ctx().get<components::EntityDeclarationsContext>().remapStrings(remap);
  }
  registry.ctx().get<components::XMLNamespaceContext>().remapStrings(remap);
}

}  // namespace donner::xml
