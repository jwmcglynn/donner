#pragma once
/// @file

#include "src/svg/registry/registry.h"
#include "src/svg/svg_svg_element.h"

namespace donner::svg {

class XMLParser;
class SVGSVGElement;

/**
 * Represents an SVG document, which holds a collection of \ref SVGElement representing the document
 * tree.
 *
 * Each \ref SVGElement may only belong to a single document, and each document can have only one
 * root. SVGDocument is responsible for managing the lifetime of all elements in the document, by
 * storing a shared pointer to the internal Registry data-store.
 *
 * Data is stored using the Entity Component System (\ref ECS) pattern, which is a data-oriented
 * design optimized for fast data access and cache locality, particularly during rendering.
 *
 * SVGDocument and \ref SVGElement provide a facade over the \ref ECS, and surface a familiar
 * Document Object Model (DOM) API to traverse and manipulate the document tree, which is internally
 * stored within Components in the ECS.  This makes \ref SVGElement a thin wrapper around an \ref
 * Entity, making the object lightweight and usable on the stack.
 *
 * \see \ref SVGElement
 * \see \ref ECS
 * \see \ref Component
 */
class SVGDocument {
public:
  SVGDocument();

  Registry& registry() { return *registry_; }
  const Registry& registry() const { return *registry_; }
  Entity rootEntity() const;

  SVGSVGElement svgElement();

  void setCanvasSize(int width, int height);
  void useAutomaticCanvasSize();

  bool operator==(const SVGDocument& other) const;

private:
  std::shared_ptr<Registry> registry_;
};

}  // namespace donner::svg
