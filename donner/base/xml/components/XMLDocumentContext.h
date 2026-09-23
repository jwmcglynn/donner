#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/Utils.h"

namespace donner::xml {

// Forward declarations
class XMLDocument;
class XMLNode;
class XMLSourceStore;

}  // namespace donner::xml

namespace donner::xml::components {

/**
 * Holds the source store of a document for as long as the document exists.
 *
 * \ref XMLDocument::setSource puts its new store in this holder rather than replacing the holder,
 * so anything that keeps the holder sees the replacement on its next read. An SVG document keeps
 * it outside the registry, to read its source without document access.
 */
struct XMLSourceStoreHolder {
  /// The document's current source store, or null for a document without source text.
  std::shared_ptr<XMLSourceStore> store;
};

/**
 * Holds global state of an XML document, such as the root element.
 *
 * One instance of this class is created per XML document.
 *
 * Access the document context via the \c Registry::ctx API:
 * ```
 * XMLDocumentContext& context = registry.ctx().get<XMLDocumentContext>();
 * ```
 */
class XMLDocumentContext {
private:
  friend class donner::xml::XMLDocument;
  friend class donner::xml::XMLNode;

  /// Tag to allow internal construction, used by \ref XMLDocument.
  struct InternalCtorTag {};

public:
  /// Bounded incremental-edit defaults, kept in sync with XMLParser::Options.
  static constexpr std::uint64_t kDefaultMaximumSourceEditTreeNodes = 8'192;
  static constexpr int kDefaultMaximumSourceEditTreeDepth = 256;
  static constexpr std::uint64_t kDefaultMaximumSourceEditTotalAttributes = 100'000;

  /**
   * Internal constructor, creates a context on the given \ref XMLDocument.
   *
   * To use this class, access it via the \c Registry::ctx API.
   * ```
   * XMLDocumentContext& context = registry.ctx().get<XMLDocumentContext>();
   * ```
   *
   * @param ctorTag Internal tag to allow construction.
   */
  explicit XMLDocumentContext(InternalCtorTag ctorTag) {}

  /// Root entity of the document.
  Entity rootEntity = entt::null;

  /// Source store for parsed documents that own their source projection. The holder is created
  /// with the context and never replaced; only the store inside it is.
  const std::shared_ptr<XMLSourceStoreHolder> sourceStoreHolder =
      std::make_shared<XMLSourceStoreHolder>();

  /// Whether the parse that built this document resolved a DOCTYPE internal subset. Set by the
  /// XML parser while it consumes the DOCTYPE, and cleared only when whole new source is
  /// installed, since incremental fragment reparses never see the prolog.
  bool declaredDoctypeInternalSubset = false;

  /// One source span the live tree does not reflect, with the failure that reported it.
  struct UnreparsedSpan {
    /// Start byte offset in current source coordinates (inclusive).
    std::size_t start = 0;
    /// End byte offset in current source coordinates (exclusive).
    std::size_t end = 0;
    /// Failure that marked the span; surfaced verbatim while pending. Its range tracks the
    /// span across later edits.
    ParseDiagnostic diagnostic;
    /// Recency order for surfacing; higher is a newer failure.
    std::uint64_t sequence = 0;
  };

  /**
   * Source spans the live tree does not reflect. Empty means the tree matches the source.
   *
   * Spans are mapped across every source change and removed only when a successful reparse
   * covers them, so an empty set is the sound staleness signal for consumers that slice
   * current source bytes: a later success elsewhere never clears an unrelated broken span.
   */
  std::vector<UnreparsedSpan> unreparsedSpans;

  /// Monotonic recency counter for \ref unreparsedSpans; the newest reason is surfaced.
  std::uint64_t unreparsedSpanSequence = 0;

  /// Maximum live non-document XML nodes admitted by one incremental source edit.
  std::uint64_t maximumSourceEditTreeNodes = kDefaultMaximumSourceEditTreeNodes;

  /// Maximum attached element depth admitted by one incremental source edit.
  int maximumSourceEditTreeDepth = kDefaultMaximumSourceEditTreeDepth;

  /// Maximum live attributes admitted by one incremental source edit.
  std::uint64_t maximumSourceEditTotalAttributes = kDefaultMaximumSourceEditTotalAttributes;
};

}  // namespace donner::xml::components
