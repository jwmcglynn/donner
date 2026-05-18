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

  const auto& tree = element.get<donner::components::TreeComponent>();
  Registry& registry = *element.registry();
  for (Entity child = tree.lastChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).previousSibling()) {
    stack.push_back(child);
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
