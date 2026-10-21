#pragma once
/// @file

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/RcString.h"
#include "donner/base/Utils.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/SVGDocumentHandle.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/core/Event.h"
#include "donner/svg/core/ProcessingMode.h"
#include "donner/svg/resources/FontCatalogTypes.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {

namespace components {
class DocumentResourceFamilyBudget;
}

class SVGElement;     // Forward declaration, #include "donner/svg/SVGElement.h"
class SVGSVGElement;  // Forward declaration, #include "donner/svg/SVGSVGElement.h"
class SVGDocumentMutation;

/// A declarative animation boundary observed over a time interval.
struct SVGAnimationEvent {
  /// Boundary kind.
  enum class Type {
    Begin,   ///< The animation's active interval began.
    Repeat,  ///< The animation started a new repeat iteration.
    End,     ///< The animation's active interval ended.
  };
  SVGElement animation;      ///< Animation element producing the boundary.
  Type type;                 ///< Boundary kind.
  double time;               ///< Document time in seconds.
  uint64_t repeatIndex = 0;  ///< One-based repeat index, zero for begin/end.
};

/// Bounded lifecycle query result. An incomplete query returns no partial events.
struct SVGAnimationEventBatch {
  std::vector<SVGAnimationEvent> events;  ///< Boundaries sorted by document time.
  bool complete = true;                   ///< False for invalid bounds or budget exhaustion.
};

/// Readiness of catalog-dependent geometry or pixels for an explicit element target.
struct FontResourcePreflight {
  /// Readiness or refusal category from font-resource preflight.
  enum class Status {
    Ready,          ///< Every known catalog dependency is resolved.
    PendingFonts,   ///< Bytes or decode admission are still pending.
    Unavailable,    ///< A known catalog face failed permanently for its current content.
    NeedsRender,    ///< Render preparation or lazy referenced content must run before retrying.
    InvalidTarget,  ///< The target is foreign, stale, or no longer attached to this document.
    ResourceLimit,  ///< A bounded dependency traversal or font resource limit was exceeded.
  };

  Status status = Status::NeedsRender;  //!< Current readiness of the target's font resources.
  std::vector<FontFaceDependency> dependencies;  ///< Full face identities and current states.
};

/// Write the font-preflight status type and numeric value for diagnostics.
/// @param os Output stream.
/// @param status Status value to format.
/// @return The output stream.
inline std::ostream& operator<<(std::ostream& os, FontResourcePreflight::Status status) {
  return os << "FontResourcePreflight::Status(" << static_cast<int>(status) << ")";
}

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
 * SVGDocument defaults to \ref donner::svg::ThreadingMode::SingleThreaded. To access the DOM from
 * multiple threads, opt into \ref donner::svg::ThreadingMode::ConcurrentDom and use the DOM facade
 * APIs or scoped access helpers. Direct ECS access through `registry()` and `entityHandle()` is
 * only conditionally safe while the caller holds an explicit document access guard.
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

  /// Current loader, callback and resource-envelope identity for full-reparse admission.
  struct ResourcePolicySnapshot {
    std::uint64_t generation = 0;  ///< Loader, processing-mode and callback revision.
    std::shared_ptr<const components::DocumentResourceFamilyBudget> familyBudget;  ///< Shared cap.
    std::size_t maximumPayloadBytes = 0;  ///< Local parsed-payload cap.
    bool operator==(const ResourcePolicySnapshot&) const = default;
  };

  /// Return a policy snapshot, or nullopt if resource envelopes are missing or inconsistent.
  std::optional<ResourcePolicySnapshot> resourcePolicySnapshot() const;
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
   * This is an unsafe advanced escape hatch. In \ref donner::svg::ThreadingMode::ConcurrentDom,
   * callers must hold an explicit document access guard while reading or mutating the returned
   * registry.
   */
  Registry& unsafeRegistry() { return documentState_->registry(); }

  /**
   * Get the underlying ECS Registry, which holds all data for the document.
   *
   * This is an unsafe advanced escape hatch. In \ref donner::svg::ThreadingMode::ConcurrentDom,
   * callers must hold an explicit document access guard while reading the returned registry.
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

  /// Attempt to acquire scoped read access without waiting for a concurrent writer.
  std::optional<DocumentReadAccess> tryReadAccess() const {
    return DocumentReadAccess::tryAcquire(*documentState_);
  }

  /// Acquire scoped write access to the underlying document state.
  DocumentWriteAccess writeAccess() const { return documentState_->write(); }

  /// Try to acquire scoped write access without waiting.
  std::optional<DocumentWriteAccess> tryWriteAccess() const { return documentState_->tryWrite(); }

  /**
   * Run a callback with scoped read access to this document.
   *
   * In \ref donner::svg::ThreadingMode::ConcurrentDom, use this to batch repeated reads such as
   * traversal or selector scans under one document read lock.
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
   * \ref donner::svg::DocumentWriteAccess for raw ECS work or
   * \ref donner::svg::SVGDocumentMutation for typed DOM mutation
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

  /// Adopt newly available fonts and invalidate affected geometry/paint without changing source.
  /// Call only at a frame boundary, before rendering begins. Returns whether rendering changed.
  bool refreshFontResources();

  /// Resolution revision owned by this document's font manager, independent of XML source edits.
  uint64_t fontResourceRevision() const;

  /// True when font dependency tracking exceeded the document's configured resource budget.
  /// Output operations must fail instead of treating an incomplete dependency list as ready.
  bool fontResourcesExceeded() const;
  /// Cheap readiness query for frame scheduling; does not traverse the render graph.
  bool hasUnresolvedFontResources() const;

  /// Current font dependencies used by the prepared render tree. Unresolved metrics cannot prove
  /// a root is outside the viewport, so these represent conservative visible-document demand.
  /// Unused definitions and catalog enumeration do not create dependencies here.
  FontResourcePreflight renderedFontResources() const;

  /// Dependency-only compatibility view; an empty result does not establish readiness.
  std::vector<FontFaceDependency> renderedFontDependencies() const;

  /// Current dependency values for an element and its prepared rendering references.
  /// An empty list does not establish output readiness; use preflightFontResourcesForElement.
  std::vector<FontFaceDependency> fontDependenciesForElement(const SVGElement& element) const;

  /// Prepare a target's text and follow its actual rendering references without changing source.
  /// Pending canvas invalidation is preserved and returns NeedsRender; lazy fragment images also
  /// require a render before readiness can be established. Does not request network bytes.
  FontResourcePreflight preflightFontResourcesForElement(const SVGElement& element);

  /// Return the number of SVG element entities currently owned by the document.
  std::size_t elementCount() const;

  /**
   * Return the current XML source text owned by this parsed SVG document.
   *
   * Programmatically-created documents may not have source text; in that case this returns an
   * empty view.
   *
   * This, \ref sourceVersion and \ref hasSourceStore report the document's current source store,
   * including one that \ref donner::xml::XMLDocument::setSource installed through \ref xmlDocument
   * after the document was built. They do not touch the registry, so the thread that edits the
   * source may call them without document access while another thread holds write access; any other
   * thread needs read access. The view is valid until the source next changes, through a source
   * edit or \ref donner::xml::XMLDocument::setSource; copy it to keep it longer.
   */
  std::string_view source() const;

  /// Return the current XML source version, or 0 for documents without source text.
  std::uint64_t sourceVersion() const;

  /**
   * Rehydrate the underlying XML document facade over this SVG document's shared registry.
   *
   * The returned facade exposes the parsed XML tree, including source locations for
   * source-backed documents. When \ref donner::xml::XMLDocument::sourceDiagnostic reports a pending
   * diagnostic, the tree reflects the last valid parse and is stale relative to \ref source;
   * consumers that slice current source bytes must refuse rather than use stale ranges.
   */
  xml::XMLDocument xmlDocument() const;

  /**
   * Apply an incremental source edit through the underlying XML document and update the SVG
   * semantic projection from emitted XML mutations.
   *
   * Documents without XML source text are not modified, and the result carries a diagnostic.
   *
   * Elements that the edit inserts, or moves into the SVG namespace by changing an `xmlns`
   * declaration, are built through the same element factory and attribute parsing as the parser,
   * so they get the element type a fresh parse of the new source would give them, at the edit's
   * incremental scope.
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
   * This only edits source-backed documents. If the document has no source text, or \p parent has
   * no XML source identity, the tree is not modified and the result is unapplied; use
   * \ref SVGElement::insertBefore or \ref SVGElement::appendChild instead.
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
   * The root \ref svgElement cannot be removed: the result is unapplied and carries a diagnostic,
   * and the document and its source are unchanged.
   *
   * @param element Element to remove.
   */
  xml::ApplySourceEditResult removeElement(const SVGElement& element);

  /**
   * Revert element insertions that are the most recent edits to this document's source.
   *
   * Each element must have been inserted by \ref insertElement as a new, unparented element,
   * oldest first, and \p insertions holds the single source delta each insertion returned. The
   * source returns byte for byte to its state before the first insertion and each element is
   * detached from its parent. See \ref donner::xml::XMLDocument::revertNodeInsertions for the
   * conditions; when they do not hold, nothing changes and the result carries a diagnostic.
   *
   * @param elements Inserted elements, oldest first.
   * @param insertions Source delta for each element, in the same order.
   */
  xml::ApplySourceEditResult revertElementInsertions(
      std::span<const SVGElement> elements, std::span<const xml::XMLSourceDelta> insertions);

  /**
   * Replace an element's text content through this document and update owned source text.
   *
   * This is the DOM-side structured editing entry point for text-content writes: it replaces the
   * element's text through \ref donner::xml::XMLSourceStore so source deltas are emitted, then
   * updates the parsed text of text-content and `<style>` elements to match. Only text-only
   * content is edited: if the element has child elements (e.g. `<tspan>`), or the document has no
   * source text, nothing changes and the result is unapplied without a diagnostic.
   *
   * @param element Element whose text content to replace.
   * @param text New text content (raw, unescaped). An empty string clears the content.
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
   * This controls the rendered output dimensions and may differ from the SVG's viewBox. The
   * document is scaled uniformly to fit within this size, so \ref canvasSize may be smaller than
   * the requested size in one dimension. If not set (or after \ref useAutomaticCanvasSize), the
   * size comes from the root `<svg>` element's `width`, `height`, and `viewBox`, falling back to
   * 512x512.
   *
   * @param width Width of the canvas, in pixels. Must be positive.
   * @param height Height of the canvas, in pixels. Must be positive.
   */
  void setCanvasSize(int width, int height);

  /**
   * Set the current document time for animations, in seconds from the document start.
   *
   * Advancing the document time causes the animation system to update animated attribute values
   * on the next render. This invalidates the render tree. Non-finite values are ignored.
   *
   * Donner has no animation clock of its own: the host supplies every sample time, typically from
   * its frame clock. Any finite time is accepted, including an earlier one; seeking backward
   * samples the intervals that already exist at that time rather than restarting animations. Use
   * \ref restartAnimations to replay from time zero. Documents parsed with
   * \ref donner::svg::ProcessingMode::SecureStatic keep their clock but never apply animated
   * values.
   *
   * @param seconds Document time in seconds.
   */
  void setTime(double seconds);

  /**
   * Get the current document time in seconds.
   */
  double currentTime() const;

  /**
   * Start a fresh animation session at time zero, including animations with restart="never".
   * Preserves the authored document and element identity. The next render evaluates time zero.
   */
  void restartAnimations();

  /**
   * Deliver a mouse event to declarative SVG animation begin/end triggers.
   * Supports event names such as `click` and `mouseover`, optionally prefixed by an element ID
   * (`button.click`). Bubbling events also reach ancestors. Does not execute scripts or edit
   * source. Secure documents ignore interactive events.
   * @param type Event being delivered.
   * @param target Element under the pointer, belonging to this document.
   * @return True if an animation interval changed; false for ignored events or invalid targets.
   */
  bool dispatchMouseEvent(EventType type, const SVGElement& target);

  /** Deliver a named host event to declarative timing, without executing scripts.
   * @param name Event name, at most 128 bytes.
   * @param target Attached target belonging to this document.
   * @param bubbles Whether the event also reaches ancestors.
   * @return Whether a runtime timing instance was retained; secure documents reject events.
   */
  bool dispatchAnimationEvent(std::string_view name, const SVGElement& target, bool bubbles = true);

  /** Schedule a begin instance for an attached animation element.
   * @param animation Animation element in this document.
   * @param offset Seconds relative to the current document time, including negative offsets.
   * @return Whether a runtime instance was retained. Secure documents reject host control.
   */
  bool beginAnimation(const SVGElement& animation, double offset = 0);

  /** Schedule an end instance for an active animation.
   * @param animation Animation element in this document.
   * @param offset Seconds relative to the current document time.
   * @return Whether a runtime instance was retained. Secure documents reject host control.
   */
  bool endAnimation(const SVGElement& animation, double offset = 0);

  /** Query animation lifecycle boundaries in the half-open interval (from, to].
   * Events are returned as values; no callback runs under document access. The query does not
   * change the document clock. Queries exceeding 4096 boundaries return an incomplete empty batch.
   * @param from Exclusive start time, finite.
   * @param to Inclusive end time, finite and not less than from.
   * @return Chronological lifecycle boundaries, or an incomplete batch when bounded out.
   */
  SVGAnimationEventBatch animationEvents(double from, double to);

  /**
   * Automatically determine the canvas size from the root `<svg>` element's `width`, `height`,
   * and `viewBox`. If those do not define a size, falls back to the default size (512x512).
   */
  void useAutomaticCanvasSize();

  /**
   * Get the size of the rendered output, in pixels.
   *
   * Without an explicit canvas size, this is the document size from the root `<svg>` element's
   * `width`, `height`, and `viewBox`, or 512x512 if those do not define a size. With a size from
   * \ref setCanvasSize, this is the document size scaled uniformly to fit within it.
   */
  Vector2i canvasSize() const;

  /**
   * Return the exact bounds requested by setCanvasSize(), or nullopt for automatic sizing.
   * Unlike canvasSize(), this value is not reduced to fit the document's aspect ratio.
   */
  std::optional<Vector2i> canvasSizeOverride() const;

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
   * `preserveAspectRatio` fit (scale plus alignment offset) between the
   * viewBox and the current canvas size.
   *
   * Naming: per the `destFromSource` convention, applying this transform to
   * a viewBox-space point yields a canvas-space point - i.e. it is
   * `canvasFromDocument`. Callers that need the opposite direction (canvas
   * pixel to document viewBox coordinate, e.g. click math in editors and
   * viewers) should invert it.
   */
  Transform2d canvasFromDocumentTransform() const;

  /**
   * Set the user's preferred languages, in priority order, used to evaluate the `systemLanguage`
   * conditional processing attribute (on \ref xml_switch children and on renderable elements).
   *
   * Each entry is a BCP 47 language tag such as `"en"` or `"en-US"`. A `systemLanguage` value
   * matches when it equals (case-insensitive) one of these languages, or begins with one of these
   * languages followed by `-` (e.g. user language `"en"` matches `systemLanguage="en-GB"`).
   * `<switch>` selects the child matching the earliest entry in this list; other elements render
   * when any entry matches.
   *
   * Defaults to `{"en"}` when not set. Passing an empty list disables all `systemLanguage` matches.
   *
   * @param languages User's preferred languages, in priority order.
   */
  void setUserLanguages(std::vector<RcString> languages);

  /**
   * Get the user's preferred languages used to evaluate the `systemLanguage` conditional processing
   * attribute. Defaults to `{"en"}`.
   */
  std::vector<RcString> userLanguages() const;

  /**
   * Returns true if the two SVGDocument handles reference the same underlying document.
   */
  bool operator==(const SVGDocument& other) const;

  /**
   * Find the first descendant of the root `<svg>` element, in document order, that matches the
   * given CSS selector. The root element itself is not matched; use \ref svgElement for it.
   *
   * This method performs its own scoped read. For repeated DOM reads in
   * \ref donner::svg::ThreadingMode::ConcurrentDom, wrap the whole scan in \ref withReadAccess so
   * nested reads reuse the same document access.
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
   * @return The first matching element, or `std::nullopt` if no element matches or the selector is
   * invalid.
   */

  std::optional<SVGElement> querySelector(std::string_view selector);

private:
  bool scheduleAnimationBoundary(const SVGElement& animation, double offset, bool begin);
  /**
   * Apply every XML mutation in \p result to this SVG document's semantic projection, keeping the
   * first diagnostic.
   *
   * @param result XML edit result whose mutations to apply.
   */
  void applyXMLMutations(xml::ApplySourceEditResult& result);

  /**
   * Apply a single XML mutation to this SVG document's semantic projection.
   *
   * @param mutation XML mutation emitted by the source-edit layer.
   */
  std::optional<ParseDiagnostic> applyXMLMutation(const xml::XMLMutation& mutation);

  /**
   * Apply a text or CDATA value change to the SVG projection: the new content of a text or style
   * element, or the new text of a `<title>`, `<desc>` or `<metadata>` element, which the parser
   * that built this document parses again.
   *
   * @param mutation NodeValueChanged mutation emitted by the source-edit layer.
   */
  std::optional<ParseDiagnostic> applyNodeValueXMLMutation(const xml::XMLMutation& mutation);

  /**
   * Parse the text of a `<title>`, `<desc>` or `<metadata>` element again, through the parser that
   * built this document, so its current text replaces the old. Does nothing for other elements.
   *
   * @param node XML element whose text changed.
   * @return An error, with the old text kept, if the new text exceeds the document's
   *   content-projection budget.
   */
  std::optional<ParseDiagnostic> reparseDescriptiveText(const xml::XMLNode& node);

  /**
   * Apply an attribute set or removal to the SVG projection. A namespace declaration change
   * reprojects the element's subtree, since it can change which descendants are SVG elements.
   *
   * @param mutation Attribute mutation emitted by the source-edit layer.
   * @param handle Entity of the mutated element.
   */
  std::optional<ParseDiagnostic> applyAttributeXMLMutation(const xml::XMLMutation& mutation,
                                                           EntityHandle handle);

  /**
   * Apply \p node's attributes to its projected SVG element.
   *
   * @param node XML element whose attributes to apply.
   * @param builtTypedElement Whether the projection just built the element's type, so it also needs
   *   the parser's type-specific attribute and content parsing.
   */
  std::optional<ParseDiagnostic> projectXMLAttributes(const xml::XMLNode& node,
                                                      bool builtTypedElement);

  /**
   * Project an XML element subtree into SVG semantic components.
   *
   * Used after XML-owned element-subtree reparsing has already updated the DOM tree in place.
   * Existing SVG entities keep their identity. Newly cloned XML element nodes, and unknown elements
   * whose tag now names an SVG element type, are built as the parser builds them.
   *
   * @param node XML element subtree root.
   * @param contentsReparsed Whether a subtree reparse just synced the subtree from source, so an
   *   existing `<title>`, `<desc>` or `<metadata>` element in it parses its text again.
   */
  std::optional<ParseDiagnostic> projectXMLSubtree(const xml::XMLNode& node, bool contentsReparsed);

  /// Project the subtree while the caller handles selector authority on a rejected projection.
  /// @param node XML element subtree root.
  /// @param contentsReparsed Whether authored contents were replaced from source.
  std::optional<ParseDiagnostic> projectXMLSubtreeContents(const xml::XMLNode& node,
                                                           bool contentsReparsed);

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

  /// Shared live-memory envelope for this document and parsed external SVG subdocuments.
  std::shared_ptr<components::DocumentResourceFamilyBudget> resourceFamilyBudget;
};

}  // namespace donner::svg
