#include "donner/svg/SVGQuerySelector.h"

#include "donner/base/SmallVector.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/selectors/SelectorMatchOptions.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"

namespace donner::svg::details {
namespace {

class TraversalElement : public SVGElement {
public:
  explicit TraversalElement(EntityHandle handle) : SVGElement(handle) {}
};

void PushTraversalChildrenReverse(EntityHandle element, SmallVector<Entity, 16>& stack) {
  if (element.all_of<components::ShadowTreeComponent>()) {
    return;
  }

  // querySelector is a public API that may run on documents in transient/edited
  // states (e.g. the editor mid-sync). A node without a TreeComponent, or a
  // dangling child/sibling link, must not abort the entt sparse-set assertion —
  // skip rather than crash.
  const auto* tree = element.try_get<donner::components::TreeComponent>();
  if (tree == nullptr) {
    return;
  }
  Registry& registry = *element.registry();
  for (Entity child = tree->lastChild(); child != entt::null;) {
    stack.push_back(child);
    const auto* childTree = registry.try_get<donner::components::TreeComponent>(child);
    child = childTree != nullptr ? childTree->previousSibling() : entt::null;
  }
}

}  // namespace

std::optional<SVGElement> QuerySelectorSearch(const css::Selector& selector, EntityHandle root) {
  css::SelectorMatchOptions<SVGElement> options;
  TraversalElement scope(root);
  options.scopeElement = &scope;

  Registry& registry = *root.registry();
  SmallVector<Entity, 16> stack;
  PushTraversalChildrenReverse(root, stack);
  while (!stack.empty()) {
    EntityHandle childHandle(registry, stack[stack.size() - 1]);
    stack.pop_back();

    TraversalElement childElement(childHandle);
    const SVGElement& childElementBase = childElement;
    if (selector.matches(childElementBase, options).matched) {
      return childElement;
    }

    PushTraversalChildrenReverse(childHandle, stack);
  }

  return std::nullopt;
}

}  // namespace donner::svg::details
