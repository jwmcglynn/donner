#pragma once

#include <functional>
#include <string_view>

#include "src/svg/components/tree_component.h"
#include "src/svg/svg_document.h"

namespace donner {

class SVGSVGElement;

class SVGElement {
protected:
  SVGElement(Registry& registry, Entity entity);

public:
  SVGElement(const SVGElement& other);
  SVGElement& operator=(const SVGElement& other);

  ElementType type() const;
  Entity entity() const;

  std::string id() const;
  void setId(std::string_view id);

  std::optional<SVGElement> parentElement();
  std::optional<SVGElement> firstChild();
  std::optional<SVGElement> lastChild();
  std::optional<SVGElement> previousSibling();
  std::optional<SVGElement> nextSibling();

  /**
   * Insert newNode as a child, before referenceNode. If referenceNode is std::nullopt, append the
   * child.
   *
   * If newNode is already in the tree, it is first removed from its parent. However, if inserting
   * the child will create a cycle, the behavior is undefined.
   *
   * @param newNode New node to insert.
   * @param referenceNode A child of this node to insert newNode before, or std::nullopt. Must be
   *                      a child of the current node.
   */
  SVGElement insertBefore(SVGElement newNode, std::optional<SVGElement> referenceNode);

  /**
   * Append child as a child of the current node.
   *
   * If child is already in the tree, it is first removed from its parent. However, if inserting
   * the child will create a cycle, the behavior is undefined.
   *
   * @param child Node to append.
   */
  SVGElement appendChild(SVGElement child);

  /**
   * Replace oldChild with newChild in the tree, removing oldChild and inserting newChild in its
   * place.
   *
   * If newChild is already in the tree, it is first removed from its parent. However, if inserting
   * the child will create a cycle, the behavior is undefined.
   *
   * @param newChild New child to insert.
   * @param oldChild Old child to remove, must be a child of the current node.
   */
  SVGElement replaceChild(SVGElement newChild, SVGElement oldChild);

  /**
   * Remove child from this node.
   *
   * @param child Child to remove, must be a child of the current node.
   */
  SVGElement removeChild(SVGElement child);

  /**
   * Remove this node from its parent, if it has one. Has no effect if this has no parent.
   */
  void remove();

  bool operator==(const SVGElement& other) const { return entity_ == other.entity_; }

private:
  std::reference_wrapper<Registry> registry_;
  Entity entity_;
};

class SVGSVGElement : public SVGElement {
  friend class SVGDocument;

protected:
  SVGSVGElement(Registry& registry, Entity entity) : SVGElement(registry, entity) {}

public:
  static SVGSVGElement Create(SVGDocument& document) {
    Registry& registry = document.registry();
    Entity entity = registry.create();
    registry.emplace<TreeComponent>(entity, ElementType::SVG, entity);
    return SVGSVGElement(registry, entity);
  }
};

class SVGGraphicsElement : public SVGElement {
protected:
  SVGGraphicsElement(Registry& registry, Entity entity) : SVGElement(registry, entity) {}
};

class SVGUnknownElement : public SVGGraphicsElement {
protected:
  SVGUnknownElement(Registry& registry, Entity entity) : SVGGraphicsElement(registry, entity) {}

public:
  static SVGUnknownElement Create(SVGDocument& document) {
    Registry& registry = document.registry();
    Entity entity = registry.create();
    registry.emplace<TreeComponent>(entity, ElementType::Unknown, entity);
    return SVGUnknownElement(registry, entity);
  }
};

class SVGGeometryElement : public SVGGraphicsElement {};

class SVGPathElement : public SVGGraphicsElement {};

}  // namespace donner