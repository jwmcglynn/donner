#pragma once
/// @file

#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/svg/SVGDocumentHandle.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/core/ProcessingMode.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {

class SVGElement;     // Forward declaration, #include "donner/svg/SVGElement.h"
class SVGSVGElement;  // Forward declaration, #include "donner/svg/SVGSVGElement.h"

/**
 * Represents a parsed SVG document containing a tree of \ref SVGElement nodes.
 *
 * To create a document, parse SVG content with \ref donner::svg::parser::SVGParser::ParseSVG, or
 * construct an empty document and build the tree programmatically. Access the root `<svg>` element
 * with svgElement(), find elements with querySelector(), and render with \ref
 * donner::svg::Renderer.
 *
 * SVGDocument and \ref SVGElement expose a familiar DOM API for traversal and manipulation (e.g.,
 * `firstChild()`, `appendChild()`, `querySelector()`). Elements are lightweight value types that
 * can be copied and passed on the stack.
 *
 * SVGDocument is **not thread-safe** — do not access the same document from multiple threads
 * concurrently.
 *
 * @note Internally, data is stored using an Entity Component System (ECS) for cache-friendly
 * access during rendering. The `registry()` and `entityHandle()` accessors expose this for
 * advanced use cases, but most users can ignore the ECS layer entirely.
 *
 * @see \ref SVGElement
 */
class SVGDocument {
public:
  struct Settings;
  /// Callback used to parse external SVG content referenced from within a document.
  using SvgParseCallback = std::function<std::optional<SVGDocumentHandle>(
      const std::vector<uint8_t>& svgContent, ParseWarningSink& warningSink)>;

private:
  friend class SVGElement;
  friend class parser::SVGParserImpl;

  /// Internal constructor used by \ref SVGElement, to rehydrate a SVGDocument from the Registry.
  explicit SVGDocument(std::shared_ptr<Registry> registry) : registry_(std::move(registry)) {}

  /**
   * Internal constructor used by the main SVGDocument constructor and \ref
   * donner::svg::parser::SVGParser.
   *
   * @param registry Underlying registry for the document.
   * @param settings Settings to configure the document.
   * @param ontoEntityHandle Optional handle to an existing entity, used by SVGParser to create the
   * SVG on an existing XML tree.
   */
  explicit SVGDocument(std::shared_ptr<Registry> registry, Settings settings,
                       EntityHandle ontoEntityHandle);

public:
  /// Constructor to create an empty SVGDocument with default settings.
  SVGDocument();

  /**
   * Constructor to create an empty SVGDocument.
   *
   * To load a document from an SVG file, use \ref donner::svg::parser::SVGParser::ParseSVG.
   *
   * @param settings Settings to configure the document.
   */
  explicit SVGDocument(Settings settings);

  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  Registry& registry() { return *registry_; }
  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  const Registry& registry() const { return *registry_; }
  /**
   * Get the internal shared document handle used by this value facade.
   *
   * @return Shared document-state handle backing this \ref SVGDocument.
   */
  SVGDocumentHandle handle() const { return registry_; }

  /**
   * Rehydrate an \ref SVGDocument facade from an internal shared document handle.
   *
   * @param handle Shared document-state handle to wrap.
   * @return A new \ref SVGDocument facade referencing the same underlying document state.
   */
  static SVGDocument CreateFromHandle(SVGDocumentHandle handle) {
    return SVGDocument(std::move(handle));
  }

  /// Return true if the parsed SVG document has owned XML source text.
  bool hasSourceStore() const;

  /**
   * Return the current XML source text owned by this parsed SVG document.
   *
   * Programmatically-created documents may not have source text; in that case this returns an
   * empty view.
   */
  std::string_view source() const;

  /// Return the current XML source version, or 0 for documents without source text.
  std::uint64_t sourceVersion() const;

  /**
   * Apply an incremental source edit through the underlying XML document and update the SVG
   * semantic projection from emitted XML mutations.
   *
   * @param intent Source edit request.
   */
  xml::ApplySourceEditResult applySourceEdit(const xml::XMLEditIntent& intent);

  /**
   * Set an element attribute and return any XML-owned source edit result.
   *
   * Source-backed documents mutate through the underlying XML document, then project emitted XML
   * mutations back into SVG semantics. Programmatic documents without source text still mutate the
   * SVG element and return an unapplied result.
   *
   * @param element Element whose attribute should be set.
   * @param name Attribute name to set.
   * @param value Raw unescaped attribute value.
   */
  xml::ApplySourceEditResult setElementAttribute(const SVGElement& element,
                                                 const xml::XMLQualifiedNameRef& name,
                                                 std::string_view value);

  /**
   * Remove an element attribute and return any XML-owned source edit result.
   *
   * @param element Element whose attribute should be removed.
   * @param name Attribute name to remove.
   */
  xml::ApplySourceEditResult removeElementAttribute(const SVGElement& element,
                                                    const xml::XMLQualifiedNameRef& name);

  /**
   * Insert an element into the document tree and return any XML-owned source edit result.
   *
   * @param parent Element that receives \p element as a child.
   * @param element Element to insert.
   * @param referenceElement Optional existing child to insert before.
   */
  xml::ApplySourceEditResult insertElement(
      const SVGElement& parent, const SVGElement& element,
      std::optional<SVGElement> referenceElement = std::nullopt);

  /**
   * Remove an element from the document tree and return any XML-owned source edit result.
   *
   * @param element Element to remove.
   */
  xml::ApplySourceEditResult removeElement(const SVGElement& element);

  /// Get the root ECS Entity of the document, for advanced use.
  EntityHandle rootEntityHandle() const;

  /// Get the root \ref xml_svg element of the document.
  SVGSVGElement svgElement() const;

  /**
   * Set the canvas (output image) size to a fixed width and height, in pixels.
   *
   * This controls the rendered output dimensions and may differ from the SVG's viewBox. If not
   * set, defaults to 512x512 or the size specified by the root `<svg>` element's `width`/`height`
   * attributes (when using useAutomaticCanvasSize()).
   *
   * @param width Width of the canvas, in pixels.
   * @param height Height of the canvas, in pixels.
   */
  void setCanvasSize(int width, int height);

  /**
   * Automatically determine the canvas size from the root `<svg>` element's `width` and `height`
   * attributes. If those attributes are not set, falls back to the default size (512x512).
   */
  void useAutomaticCanvasSize();

  /**
   * Get the current canvas size, or the default size (512x512) if the canvas size has not been
   * explicitly set.
   */
  Vector2i canvasSize() const;

  /**
   * Get the width of the SVG document, in pixels.
   *
   * This is the width of the canvas, which may be different from the width of the SVG content.
   */
  int width() const { return canvasSize().x; }

  /**
   * Get the height of the SVG document, in pixels.
   *
   * This is the height of the canvas, which may be different from the height of the SVG content.
   */
  int height() const { return canvasSize().y; }

  /**
   * Returns the transform that maps points from the SVG document's viewBox
   * coordinate space into the canvas-scaled output space. This bakes in the
   * `preserveAspectRatio` fit (letterbox offset + uniform scale) between the
   * viewBox and the current canvas size.
   *
   * Naming: per the `destFromSource` convention, applying this transform to
   * a viewBox-space point yields a canvas-space point — i.e. it is
   * `canvasFromDocument`, despite an earlier misnomer. Callers that need the
   * opposite direction (canvas pixel → document viewBox coordinate, e.g.
   * click math in editors/viewers) should invert it.
   */
  Transform2d canvasFromDocumentTransform() const;

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
  /// Rehydrate the underlying XML document facade from this SVG document's shared registry.
  xml::XMLDocument xmlDocument() const;

  /**
   * Apply a single XML mutation to this SVG document's semantic projection.
   *
   * @param mutation XML mutation emitted by the source-edit layer.
   */
  std::optional<ParseDiagnostic> applyXMLMutation(const xml::XMLMutation& mutation);

  /**
   * Project an XML element subtree into SVG semantic components.
   *
   * Used after XML-owned element-subtree reparsing has already updated the DOM tree in place.
   * Existing SVG entities keep their identity; newly cloned XML element nodes receive SVG
   * components and attributes.
   *
   * @param node XML element subtree root.
   */
  std::optional<ParseDiagnostic> projectXMLSubtree(const xml::XMLNode& node);

  /// Owned reference to the registry, which contains all information about the loaded document.
  std::shared_ptr<Registry> registry_;
};

/// Document settings which configure the document behavior.
struct SVGDocument::Settings {
  /// Resource loader to use for loading external resources.
  std::unique_ptr<ResourceLoaderInterface> resourceLoader;

  /// Processing mode for this document. Defaults to \ref
  /// donner::svg::ProcessingMode::DynamicInteractive.
  ProcessingMode processingMode = ProcessingMode::DynamicInteractive;

  /// Callback to parse SVG content into sub-documents.
  SvgParseCallback svgParseCallback;
};

}  // namespace donner::svg
