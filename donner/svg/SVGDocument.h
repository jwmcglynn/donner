#pragma once
/// @file

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/SVGDocumentHandle.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/core/ProcessingMode.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {

class SVGElement;     // Forward declaration, #include "donner/svg/SVGElement.h"
class SVGSVGElement;  // Forward declaration, #include "donner/svg/SVGSVGElement.h"
class SVGDocumentMutation;

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
 * SVGDocument defaults to \ref ThreadingMode::SingleThreaded. To access the DOM from multiple
 * threads, opt into \ref ThreadingMode::ConcurrentDom and use the DOM facade APIs or scoped access
 * helpers. Direct ECS access through `registry()` and `entityHandle()` is only conditionally safe
 * while the caller holds an explicit document access guard.
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

  /// Internal constructor used by \ref SVGElement, to rehydrate a SVGDocument from shared state.
  explicit SVGDocument(SVGDocumentHandle documentState)
      : documentState_(std::move(documentState)) {}

  /**
   * Internal constructor used by the main SVGDocument constructor and \ref
   * donner::svg::parser::SVGParser.
   *
   * @param documentState Shared state for the document.
   * @param settings Settings to configure the document.
   * @param ontoEntityHandle Optional handle to an existing entity, used by SVGParser to create the
   * SVG on an existing XML tree.
   */
  explicit SVGDocument(SVGDocumentHandle documentState, Settings settings,
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

  /**
   * Get the underlying ECS Registry, which holds all data for the document.
   *
   * This is an unsafe advanced escape hatch. In \ref ThreadingMode::ConcurrentDom, callers must
   * hold an explicit document access guard while reading or mutating the returned registry.
   */
  Registry& unsafeRegistry() { return documentState_->registry(); }

  /**
   * Get the underlying ECS Registry, which holds all data for the document.
   *
   * This is an unsafe advanced escape hatch. In \ref ThreadingMode::ConcurrentDom, callers must
   * hold an explicit document access guard while reading the returned registry.
   */
  const Registry& unsafeRegistry() const { return documentState_->registry(); }

  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  Registry& registry() {
    assertScopedRegistryAccessAllowed();
    return unsafeRegistry();
  }
  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  const Registry& registry() const {
    assertScopedRegistryAccessAllowed();
    return unsafeRegistry();
  }
  /**
   * Get the internal shared document handle used by this value facade.
   *
   * @return Shared document-state handle backing this \ref SVGDocument.
   */
  SVGDocumentHandle handle() const { return documentState_; }

  /// Current DOM threading policy for this document.
  ThreadingMode threadingMode() const { return documentState_->threadingMode(); }

  /**
   * Set the DOM threading policy for this document.
   *
   * @param mode New threading mode.
   */
  void setThreadingMode(ThreadingMode mode) { documentState_->setThreadingMode(mode); }

  /// Acquire scoped read access to the underlying document state.
  DocumentReadAccess readAccess() const { return documentState_->read(); }

  /// Acquire scoped write access to the underlying document state.
  DocumentWriteAccess writeAccess() const { return documentState_->write(); }

  /**
   * Run a callback with scoped read access to this document.
   *
   * In \ref ThreadingMode::ConcurrentDom, use this to batch repeated reads such as traversal or
   * selector scans under one document read lock.
   *
   * @param callback Callable invoked as `callback(DocumentReadAccess&)`.
   */
  template <typename Callback>
  decltype(auto) withReadAccess(Callback&& callback) const {
    DocumentReadAccess access = readAccess();
    using Result = std::invoke_result_t<Callback, DocumentReadAccess&>;
    if constexpr (std::is_void_v<Result>) {
      std::forward<Callback>(callback)(access);
    } else {
      return std::forward<Callback>(callback)(access);
    }
  }

  /**
   * Run a callback with scoped write access to this document.
   *
   * Nested DOM setters called by the callback reuse this write access and coalesce their mutation
   * revision bumps into one revision increment for the whole callback. Callbacks can accept either
   * \ref DocumentWriteAccess for raw ECS work or \ref SVGDocumentMutation for typed DOM mutation
   * helpers.
   *
   * @param callback Callable invoked as `callback(DocumentWriteAccess&)` or
   * `callback(SVGDocumentMutation&)`.
   */
  template <typename Callback>
  decltype(auto) withWriteAccess(Callback&& callback) const;

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
   * Return true when the document carries render invalidation that the renderer has not yet
   * consumed: per-entity dirty flags, or a queued full render-tree rebuild / style recompute.
   * Always false before the render tree has been built for the first time.
   *
   * UI surfaces that prepare the document for auxiliary rendering (for example the editor's
   * layer-thumbnail refresh) use this to avoid consuming pending invalidation ahead of the
   * canvas renderer.
   */
  bool hasPendingRenderInvalidation() const;

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

  /**
   * Replace an element's text content through this document and update owned source text.
   *
   * This is the DOM-side structured editing entry point for text-content writes: it removes the
   * element's existing text-like XML child nodes and (for non-empty @p text) inserts a single data
   * node holding @p text, applying every change through \ref xml::XMLSourceStore so source deltas
   * are emitted. Element children (e.g. `<tspan>`) are preserved. Callers remain responsible for
   * updating any component-level text mirror (e.g. `SVGTextContentElement::setTextContent`).
   *
   * @param element Element whose text content to replace.
   * @param text New text content (raw, unescaped).
   */
  xml::ApplySourceEditResult setElementTextContent(const SVGElement& element,
                                                   std::string_view text);

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
   * Set the current document time for animations, in seconds from the document start.
   *
   * Advancing the document time causes the animation system to update animated attribute values
   * on the next render. This invalidates the render tree.
   *
   * @param seconds Document time in seconds.
   */
  void setTime(double seconds);

  /**
   * Get the current document time in seconds.
   */
  double currentTime() const;

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
   * a viewBox-space point yields a canvas-space point - i.e. it is
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
   * This method performs its own scoped read. For repeated DOM reads in
   * \ref ThreadingMode::ConcurrentDom, wrap the whole scan in \ref withReadAccess so nested reads
   * reuse the same document access.
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

  void assertScopedRegistryAccessAllowed() const {
    UTILS_RELEASE_ASSERT_MSG(
        documentState_->threadingMode() != ThreadingMode::ConcurrentDom ||
            documentState_->currentThreadHasAccess(),
        "SVGDocument::registry() requires withReadAccess() or withWriteAccess() in "
        "ConcurrentDom; use unsafeRegistry() for intentionally unguarded ECS access");
  }

  /// Shared state containing the loaded document registry and mutation bookkeeping.
  SVGDocumentHandle documentState_;
};

/// Typed DOM mutation helper used by \ref SVGDocument::withWriteAccess.
class SVGDocumentMutation {
public:
  /**
   * Create a mutation helper over an active document write access.
   *
   * @param document Document facade being mutated.
   * @param access Active write access for the document.
   */
  explicit SVGDocumentMutation(SVGDocument document, DocumentWriteAccess& access);

  /// Copying mutation helpers is not allowed.
  SVGDocumentMutation(const SVGDocumentMutation& other) = delete;

  /// Moving mutation helpers is not allowed.
  SVGDocumentMutation(SVGDocumentMutation&& other) noexcept = delete;

  /// Copying mutation helpers is not allowed.
  SVGDocumentMutation& operator=(const SVGDocumentMutation& other) = delete;

  /// Moving mutation helpers is not allowed.
  SVGDocumentMutation& operator=(SVGDocumentMutation&& other) noexcept = delete;

  /// Get the raw write access for advanced ECS mutation.
  DocumentWriteAccess& access() const;

  /**
   * Set the canvas size.
   *
   * @param width Width of the canvas, in pixels.
   * @param height Height of the canvas, in pixels.
   */
  void setCanvasSize(int width, int height);

  /// Automatically determine the canvas size from the root `<svg>` element.
  void useAutomaticCanvasSize();

  /**
   * Set an element attribute.
   *
   * @param element Element to mutate.
   * @param name Attribute name.
   * @param value Attribute value.
   */
  void setAttribute(SVGElement element, const xml::XMLQualifiedNameRef& name,
                    std::string_view value);

  /**
   * Remove an element attribute.
   *
   * @param element Element to mutate.
   * @param name Attribute name.
   */
  void removeAttribute(SVGElement element, const xml::XMLQualifiedNameRef& name);

  /**
   * Insert \p newNode into \p parent before \p referenceNode.
   *
   * @param parent Parent element to mutate.
   * @param newNode Node to insert.
   * @param referenceNode Existing child to insert before, or \c std::nullopt.
   */
  void insertBefore(SVGElement parent, const SVGElement& newNode,
                    std::optional<SVGElement> referenceNode);

  /**
   * Append \p child to \p parent.
   *
   * @param parent Parent element to mutate.
   * @param child Child to append.
   */
  void appendChild(SVGElement parent, const SVGElement& child);

  /**
   * Replace \p oldChild with \p newChild under \p parent.
   *
   * @param parent Parent element to mutate.
   * @param newChild Child to insert.
   * @param oldChild Existing child to remove.
   */
  void replaceChild(SVGElement parent, const SVGElement& newChild, const SVGElement& oldChild);

  /**
   * Remove \p child from \p parent.
   *
   * @param parent Parent element to mutate.
   * @param child Child to remove.
   */
  void removeChild(SVGElement parent, const SVGElement& child);

  /**
   * Remove \p element from its parent, if it has one.
   *
   * @param element Element to remove.
   */
  void remove(SVGElement element);

private:
  SVGDocument document_;
  DocumentWriteAccess* access_;
};

template <typename Callback>
decltype(auto) SVGDocument::withWriteAccess(Callback&& callback) const {
  DocumentMutationBatch batch(*documentState_);
  if constexpr (std::is_invocable_v<Callback, DocumentWriteAccess&>) {
    using Result = std::invoke_result_t<Callback, DocumentWriteAccess&>;
    if constexpr (std::is_void_v<Result>) {
      std::forward<Callback>(callback)(batch.access());
    } else {
      return std::forward<Callback>(callback)(batch.access());
    }
  } else {
    SVGDocumentMutation mutation(SVGDocument::CreateFromHandle(documentState_), batch.access());
    using Result = std::invoke_result_t<Callback, SVGDocumentMutation&>;
    if constexpr (std::is_void_v<Result>) {
      std::forward<Callback>(callback)(mutation);
    } else {
      return std::forward<Callback>(callback)(mutation);
    }
  }
}

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
