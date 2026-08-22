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

bool PushTraversalChildrenReverse(EntityHandle element, SmallVector<Entity, 16>& stack,
                                  css::SelectorTraversalBudget& traversalBudget) {
  if (element.all_of<components::ShadowTreeComponent>()) {
    return true;
  }

  const auto& tree = element.get<donner::components::TreeComponent>();
  Registry& registry = *element.registry();
  for (Entity child = tree.lastChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).previousSibling()) {
    if (!traversalBudget.consume()) {
      return false;
    }
    stack.push_back(child);
  }
  return true;
}

}  // namespace

std::optional<SVGElement> QuerySelectorSearch(const css::Selector& selector, EntityHandle root) {
  css::SelectorTraversalBudget traversalBudget;
  return QuerySelectorSearch(selector, root, traversalBudget);
}

std::optional<SVGElement> QuerySelectorSearch(const css::Selector& selector, EntityHandle root,
                                              css::SelectorTraversalBudget& traversalBudget) {
  css::SelectorMatchOptions<SVGElement> options;
  TraversalElement scope(root);
  options.scopeElement = &scope;
  options.traversalBudget = &traversalBudget;

  Registry& registry = *root.registry();
  SmallVector<Entity, 16> stack;
  if (!PushTraversalChildrenReverse(root, stack, traversalBudget)) {
    return std::nullopt;
  }
  while (!stack.empty()) {
    EntityHandle childHandle(registry, stack[stack.size() - 1]);
    stack.pop_back();

    TraversalElement childElement(childHandle);
    const SVGElement& childElementBase = childElement;
    const css::SelectorMatchResult result = selector.matches(childElementBase, options);
    if (traversalBudget.rejected()) {
      return std::nullopt;
    }
    if (result.matched) {
      return childElement;
    }

    if (!PushTraversalChildrenReverse(childHandle, stack, traversalBudget)) {
      return std::nullopt;
    }
  }

  return std::nullopt;
}

}  // namespace donner::svg::details
