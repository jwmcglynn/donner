#pragma once
/// @file

#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/registry/Registry.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {

namespace parser {
class XMLParser;  // Forward declaration, #include "donner/svg/xml/XMLParser.h"
}  // namespace parser

class SVGSVGElement;  // Forward declaration, #include "donner/svg/SVGSVGElement.h"

/**
 * Represents an SVG document, which holds a collection of \ref SVGElement as the document tree.
 *
 * Each \ref SVGElement may only belong to a single document, and each document can have only one
 * root. SVGDocument is responsible for managing the lifetime of all elements in the document, by
 * storing a shared pointer to the internal Registry data-store.
 *
 * Data is stored using the Entity Component System (\ref EcsArchitecture) pattern, which is a
 * data-oriented design optimized for fast data access and cache locality, particularly during
 * rendering.
 *
 * SVGDocument and \ref SVGElement provide a facade over the ECS, and surface a familiar
 * Document Object Model (DOM) API to traverse and manipulate the document tree, which is internally
 * stored within Components in the ECS.  This makes \ref SVGElement a thin wrapper around an \ref
 * Entity, making the object lightweight and usable on the stack.
 *
 * @see \ref SVGElement
 * @see \ref EcsArchitecture
 */
class SVGDocument {
public:
  /// Document settings which configure the document behavior.
  struct Settings {
    /// Resource loader to use for loading external resources.
    std::unique_ptr<ResourceLoaderInterface> resourceLoader;
  };

  /**
   * Constructor to create an empty SVGDocument.
   *
   * To load a document from an SVG file, use \ref donner::svg::parser::XMLParser::ParseSVG.
   *
   * @param settings Settings to configure the document.
   */
  SVGDocument(Settings settings = Settings());

  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  Registry& registry() { return *registry_; }
  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  const Registry& registry() const { return *registry_; }
  /// Get the root ECS Entity of the document, for advanced use.
  Entity rootEntity() const;

  /// Get the root \ref xml_svg element of the document.
  SVGSVGElement svgElement() const;

  /**
   * Set the canvas size to a fixed width and height, in pixels.
   *
   * @param width Width of the canvas, in pixels.
   * @param height Height of the canvas, in pixels.
   */
  void setCanvasSize(int width, int height);

  /**
   * Automatically determine the canvas size based on the size of the root \ref xml_svg element.
   */
  void useAutomaticCanvasSize();

  /**
   * Get the current canvas size, or the default size (512x512) if the canvas size has not been
   * explicitly set.
   */
  Vector2i canvasSize();

  /**
   * Returns true if the two SVGDocument handles reference the same underlying document.
   */
  bool operator==(const SVGDocument& other) const;

  /**
   * Find the first element in the tree that matches the given CSS selector.
   *
   * ```
   * auto element = document.querySelector("#elementId");
   * ```
   *
   * Complex selectors are supported:
   * ```
   * auto element = document.querySelector("svg > g:nth-child(2) > rect");
   * ```
   *
   * @param selector CSS selector to match.
   * @return The first matching element, or `std::nullopt` if no element matches.
   */

  std::optional<SVGElement> querySelector(std::string_view selector);

private:
  /// Owned reference to the registry, which contains all information about the loaded document.
  std::shared_ptr<Registry> registry_;
};

}  // namespace donner::svg
