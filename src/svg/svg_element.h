#pragma once

#include <functional>
#include <string_view>

#include "src/base/transform.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/svg_document.h"

namespace donner {

class SVGSVGElement;
struct ParseError;

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

  std::string className() const;
  void setClassName(std::string_view name);

  Transformd transform() const;
  void setTransform(Transformd transform);

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

protected:
  std::reference_wrapper<Registry> registry_;
  Entity entity_;
};

class SVGSVGElement : public SVGElement {
  friend class SVGDocument;

protected:
  SVGSVGElement(Registry& registry, Entity entity) : SVGElement(registry, entity) {}

public:
  static constexpr ElementType Type = ElementType::SVG;
  static constexpr std::string_view Tag = "svg";

  static SVGSVGElement Create(SVGDocument& document) {
    Registry& registry = document.registry();
    Entity entity = registry.create();
    registry.emplace<TreeComponent>(entity, Type, entity);
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
  static constexpr ElementType Type = ElementType::Unknown;

  static SVGUnknownElement Create(SVGDocument& document) {
    Registry& registry = document.registry();
    Entity entity = registry.create();
    registry.emplace<TreeComponent>(entity, Type, entity);
    return SVGUnknownElement(registry, entity);
  }
};

class SVGGeometryElement : public SVGGraphicsElement {};

class SVGPathElement : public SVGGraphicsElement {
protected:
  SVGPathElement(Registry& registry, Entity entity) : SVGGraphicsElement(registry, entity) {}

public:
  static constexpr ElementType Type = ElementType::Path;
  static constexpr std::string_view Tag = "path";

  static SVGPathElement Create(SVGDocument& document) {
    Registry& registry = document.registry();
    Entity entity = registry.create();
    registry.emplace<TreeComponent>(entity, Type, entity);
    return SVGPathElement(registry, entity);
  }

  std::string_view d() const;
  std::optional<ParseError> setD(std::string_view d);
};

}  // namespace donner