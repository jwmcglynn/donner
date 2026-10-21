#pragma once
/// @file

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/RcString.h"
#include "donner/base/Utils.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLSourceStore.h"

namespace donner::xml {

/// Result of locating an attribute at a source offset.
struct XMLAttributeAtSourceOffset {
  XMLNode node;               ///< Element node that owns the attribute.
  XMLQualifiedName name;      ///< Attribute name.
  SourceRange location;       ///< Current full source range of the attribute.
  SourceRange valueLocation;  ///< Current unquoted value source range.
  char quote = '"';           ///< Quote delimiter used for the value.
};

/// Local XML reparse scope chosen for a source edit.
enum class ReparseScope : std::uint8_t {
  AttributeValue,  ///< Edit was contained inside one quoted attribute value.
  OpeningTag,      ///< Edit touched an element opening tag outside one attribute value.
  TextNode,        ///< Edit was contained inside a text-like node.
  ElementSubtree,  ///< Edit touched one element subtree.
  /// Edit matched no incremental scope. The bytes changed but the tree was left untouched;
  /// callers that need the tree to match the source must reparse the document fresh, since
  /// later incremental edits may never cover the unreconciled span.
  Document,
};

/// Print a \ref ReparseScope.
std::ostream& operator<<(std::ostream& os, ReparseScope scope);

/// Source edit request from an editor/source view.
struct XMLEditIntent {
  SourceRange range;                ///< Source byte range to replace.
  std::string_view replacement;     ///< Replacement source bytes.
  std::uint64_t sourceVersion = 0;  ///< Source version observed by the caller.
};

/// XML DOM mutation emitted by an incremental source edit.
struct XMLMutation {
  /// Mutation kind.
  enum class Kind : std::uint8_t {
    AttributeSet,             ///< Attribute value was set.
    AttributeRemoved,         ///< Attribute was removed.
    NodeValueChanged,         ///< Text-like node value changed.
    NodeInserted,             ///< Node was inserted.
    NodeRemoved,              ///< Node was removed.
    SubtreeReplaced,          ///< A subtree was replaced.
    SourceDiagnosticChanged,  ///< A source diagnostic changed.
  };

  /// Print an \ref XMLMutation::Kind.
  friend std::ostream& operator<<(std::ostream& os, Kind kind) {
    switch (kind) {
      case Kind::AttributeSet: return os << "AttributeSet";
      case Kind::AttributeRemoved: return os << "AttributeRemoved";
      case Kind::NodeValueChanged: return os << "NodeValueChanged";
      case Kind::NodeInserted: return os << "NodeInserted";
      case Kind::NodeRemoved: return os << "NodeRemoved";
      case Kind::SubtreeReplaced: return os << "SubtreeReplaced";
      case Kind::SourceDiagnosticChanged: return os << "SourceDiagnosticChanged";
    }

    UTILS_UNREACHABLE();
  }

  Kind kind;                                    ///< Mutation kind.
  XMLNode node;                                 ///< Mutated node.
  XMLQualifiedName attributeName;               ///< Attribute name for attribute mutations.
  std::optional<RcString> value;                ///< New value when relevant.
  std::optional<ParseDiagnostic> diagnostic;    ///< Source diagnostic for diagnostic mutations.
  ReparseScope scope = ReparseScope::Document;  ///< Scope that produced the mutation.
};

/// Result from \ref XMLDocument::applySourceEdit.
struct ApplySourceEditResult {
  /// True if the operation fully applied: source bytes changed and the tree was updated
  /// to match. A multi-step operation that fails partway still commits its completed
  /// source replacements, so check `sourceDeltas` for the exact record of committed changes
  /// rather than assuming applied false means untouched bytes.
  bool applied = false;
  ReparseScope scope = ReparseScope::Document;  ///< Reparse scope selected for the edit.
  /// Source edits committed by this operation, in order. Non-empty whenever source bytes
  /// changed, even if the operation did not fully apply.
  std::vector<XMLSourceDelta> sourceDeltas;
  std::vector<XMLMutation> mutations;         ///< DOM mutations emitted by this operation.
  std::optional<ParseDiagnostic> diagnostic;  ///< Diagnostic if local reparsing failed.
};

/**
 * Represents an XML document, which holds a collection of \ref XMLNode as the document tree.
 *
 * Each \ref XMLNode may only belong to a single document, and each document can have only one
 * root. XMLDocument is responsible for managing the lifetime of all elements in the document, by
 * storing a shared pointer to the internal Registry data-store.
 *
 * Data is stored using the Entity Component System (\ref EcsArchitecture) pattern, which is a
 * data-oriented design optimized for fast data access and cache locality, particularly during
 * rendering.
 *
 * XMLDocument and \ref XMLNode provide a facade over the ECS, and surface a familiar Document
 * Object Model (DOM) API to traverse and manipulate the document tree, which is internally stored
 * within Components in the ECS.  This makes \ref XMLNode a thin wrapper around an \ref Entity,
 * making the object lightweight and usable on the stack.
 *
 * @see \ref XMLNode
 * @see \ref EcsArchitecture
 */
class XMLDocument {
public:
  /**
   * Constructor to create an empty XMLDocument.
   *
   * To load a document from an XML file, use \ref donner::xml::XMLParser.
   */
  XMLDocument();

  /**
   * Rehydrate an \ref XMLDocument facade from an internal shared registry.
   *
   * @param registry Shared XML document registry to wrap.
   */
  static XMLDocument CreateFromRegistry(std::shared_ptr<Registry> registry);

  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  Registry& registry() { return *registry_; }

  /// Gets the registry as a shared pointer, for advanced use.
  std::shared_ptr<Registry> sharedRegistry() const { return registry_; }

  /// Get the root XMLNode of the document.
  XMLNode root() const;

  /// Get the root ECS Entity of the document, for advanced use.
  EntityHandle rootEntityHandle() const;

  /// Return true if this document has an owned source store.
  bool hasSourceStore() const;

  /**
   * Return the current XML source text.
   *
   * Documents created programmatically may not have source text; in that case this returns an
   * empty view.
   */
  std::string_view source() const UTILS_LIFETIME_BOUND;

  /// Return the source version, or 0 for documents without a source store.
  std::uint64_t sourceVersion() const;

  /**
   * Whether the parse that built this document resolved a DOCTYPE internal subset, i.e. a
   * `<!DOCTYPE root [ ... ]>` whose bracketed declarations the parser consumed.
   *
   * The parser expands entity references and does not keep the declarations in the tree, so a
   * consumer that reproduces the document from the tree (the viewport export, for example)
   * cannot reproduce them and must refuse rather than emit a body carrying entity references
   * that nothing declares. This reports what the parser resolved, not a scan of the source
   * text: a `<!DOCTYPE` inside a comment or a processing instruction is not a DOCTYPE.
   *
   * \ref setSource clears it, since installing whole new source hands responsibility for the
   * tree back to a full reparse. Incremental fragment reparses leave it alone; they never see
   * the prolog, so they cannot observe that the subset is gone.
   */
  bool declaresDoctypeInternalSubset() const;

  /**
   * Return the pending source diagnostic, if any.
   *
   * Incremental source edits commit source bytes before reparsing; when the reparse fails, the
   * tree keeps its last-valid state and the broken span is recorded until a later edit
   * successfully reparses a fragment covering it. A set diagnostic therefore means the tree
   * is stale relative to \ref source, and consumers that need current-source accuracy must
   * fail closed instead of using stale ranges. A later success elsewhere never clears an
   * unrelated broken span.
   *
   * Documents without a source store never carry a diagnostic.
   */
  std::optional<ParseDiagnostic> sourceDiagnostic() const;

  /// Get the mutable source store, or `nullptr` if this document does not own source text.
  XMLSourceStore* sourceStore();

  /// Get the immutable source store, or `nullptr` if this document does not own source text.
  const XMLSourceStore* sourceStore() const;

  /// Owning read-only handle to the current source store, for identity-bound recovery.
  std::shared_ptr<const XMLSourceStore> sharedSourceStore() const;

  /**
   * Return the deepest parsed XML node whose current source range contains \p offset.
   *
   * This is the first source-position lookup primitive used by incremental editing. It resolves
   * node source anchors before comparing offsets, so source edits made through
   * \ref XMLSourceStore are reflected without reparsing the full document.
   *
   * @param offset Byte offset in the current XML source.
   */
  std::optional<XMLNode> nodeAtSourceOffset(std::size_t offset) const;

  /**
   * Return the attribute whose current source range contains \p offset, if any.
   *
   * Resolves the deepest node containing \p offset, then checks each of its attributes, using the
   * attribute's tracked source range when it has one and otherwise reparsing the node's current
   * opening tag.
   *
   * @param offset Byte offset in the current XML source.
   */
  std::optional<XMLAttributeAtSourceOffset> attributeAtSourceOffset(std::size_t offset) const;

  /**
   * Apply a source edit through this XML document.
   *
   * This is the incremental editing entry point. It validates the caller's source version, applies
   * the source change through \ref XMLSourceStore, chooses the smallest implemented reparse scope,
   * and emits XML mutations for scopes that can update the live tree.
   *
   * Implemented local scopes keep source bytes current and either update the live tree or return a
   * scoped diagnostic so callers can preserve the last valid semantic projection while the edited
   * XML is temporarily malformed. Documents parsed with an internal DTD subset or disabled entity
   * translation require Document-scope recovery: draft bytes change, but decoded live values stay
   * untouched. A caller must reparse with the original XMLParser::SourceOptions policy, or keep the
   * diagnostic and last valid tree rather than reinterpret it with broader defaults.
   *
   * @param intent Source edit request.
   */
  ApplySourceEditResult applySourceEdit(const XMLEditIntent& intent);

  /**
   * Set document-wide tree limits enforced transactionally by incremental source edits.
   *
   * @param maximumTreeNodes Maximum live non-document XML nodes after an edit.
   * @param maximumTreeDepth Maximum attached element depth after an edit.
   */
  void setSourceEditTreeLimits(std::size_t maximumTreeNodes, std::size_t maximumTreeDepth);

  /// Number of additional non-document XML nodes the structural-edit budget allows.
  /// Detached XML nodes already owned by this document count against the budget.
  [[nodiscard]] std::uint64_t remainingSourceEditTreeNodeBudget() const;

  /**
   * Set an XML attribute through this document and update owned source text.
   *
   * This is the DOM-side structured editing entry point for source-backed attribute writes. It
   * preserves the attribute's existing quote style, escapes \p value for XML, applies the source
   * change through \ref XMLSourceStore, updates the live DOM attribute, and emits an
   * \ref XMLMutation::Kind::AttributeSet mutation.
   *
   * @param node Element node that owns the attribute.
   * @param name Attribute name to set.
   * @param value Raw unescaped attribute value.
   */
  ApplySourceEditResult setAttribute(XMLNode node, const XMLQualifiedNameRef& name,
                                     std::string_view value);

  /**
   * Remove an XML attribute through this document and update owned source text.
   *
   * This is the DOM-side structured editing entry point for source-backed attribute removals. It
   * removes the serialized attribute through \ref XMLSourceStore, updates the live DOM attribute
   * set, and emits an \ref XMLMutation::Kind::AttributeRemoved mutation.
   *
   * @param node Element node that owns the attribute.
   * @param name Attribute name to remove.
   */
  ApplySourceEditResult removeAttribute(XMLNode node, const XMLQualifiedNameRef& name);

  /**
   * Insert an XML node under a source-backed element and update owned source text.
   *
   * A source-less, unparented \p node is serialized and inserted, emitting an
   * \ref XMLMutation::Kind::NodeInserted mutation. A source-backed element that is already in the
   * tree is moved instead: its existing source bytes are relocated, and a
   * \ref XMLMutation::Kind::NodeRemoved mutation precedes the NodeInserted one. If
   * \p referenceNode is set, it must be an existing child of \p parent and the node is inserted
   * immediately before it; otherwise the node is appended before \p parent 's closing tag, and a
   * self-closing \p parent is expanded into an opening and closing tag pair.
   *
   * @param parent Element node to receive the inserted child.
   * @param node Source-less node to insert, or source-backed element to move.
   * @param referenceNode Optional existing child to insert before.
   */
  ApplySourceEditResult insertNode(XMLNode parent, XMLNode node,
                                   std::optional<XMLNode> referenceNode = std::nullopt);

  /**
   * Remove a source-backed XML node through this document.
   *
   * This applies the source removal through \ref XMLSourceStore, detaches \p node from the live XML
   * tree, and emits an \ref XMLMutation::Kind::NodeRemoved mutation.
   *
   * @param node Node to remove.
   */
  ApplySourceEditResult removeNode(XMLNode node);

  /// One completed source-less node insertion and the source delta \ref insertNode returned.
  struct NodeInsertion {
    XMLNode node;              //!< Node that was inserted.
    XMLSourceDelta insertion;  //!< The pure-insertion source delta that inserted the node.
  };

  /**
   * Revert node insertions that are the most recent edits to this document's source.
   *
   * \p insertions lists source-less node insertions in the order they were applied. Their source
   * deltas must be pure insertions (no removed bytes) whose versions run consecutively up to the
   * current source version, and each node's source range must lie within the bytes its delta
   * inserted. Newest first, each delta's bytes are removed and its node is detached with an
   * \ref XMLMutation::Kind::NodeRemoved mutation, so the source returns byte for byte to its
   * state before the first insertion, including the indentation the insertions added. When the
   * list does not meet these conditions, nothing changes and the result carries a diagnostic. If
   * the source store refuses a removal part way, the newer insertions stay reverted and the result
   * is applied with a diagnostic.
   *
   * @param insertions Insertions to revert, oldest first.
   */
  ApplySourceEditResult revertNodeInsertions(std::span<const NodeInsertion> insertions);

  /**
   * Replace an element's text content through this document and update owned source text.
   *
   * Elements without element children replace their entire authored content, including CDATA,
   * and detach obsolete non-element nodes. A self-closing element expands when needed.
   * With element children, only the tracked text run is replaced; element children are untouched.
   * Emits a parent \ref XMLMutation::Kind::NodeValueChanged receipt on success. The input may alias
   * the document source or a child value; it is copied before mutation.
   *
   * @param element Element whose text content to replace.
   * @param text New raw (unescaped) text content; empty clears the content.
   */
  ApplySourceEditResult setElementText(XMLNode element, std::string_view text);

  /**
   * Install owned source text for this document, replacing its source store if it has one.
   *
   * Pending unreparsed spans are cleared: the caller takes responsibility for rebuilding
   * the tree against the new bytes (as the XML parser does), after which the tree matches
   * the source again.
   *
   * Every reader sees the new source on its next call: \ref source, \ref sourceVersion and
   * \ref hasSourceStore here, and the same accessors of an SVG document built on this tree. The
   * replaced source is released, so a view from \ref source or a pointer from \ref sourceStore
   * taken before this call must not be used after it, just as a view must not be kept across a
   * source edit. Replacing a source store advances its version past the previous version, so
   * edits recorded against the old source are rejected. Existing nodes lose their source
   * locations until the caller rebuilds the tree against the new bytes.
   *
   * The XML parser calls this before any other code can reach the document. On a document other
   * code can reach, call it as a source edit is made: for an SVG document, under its write access
   * and on the thread that edits its source, the one thread that may read the source without
   * document access.
   *
   * @param source XML source text to own.
   * @param maximumSourceSize Maximum source size retained after later structured edits.
   */
  void setSource(std::string source, std::size_t maximumSourceSize = 16 * 1024 * 1024);

private:
  /// Internal constructor used to rehydrate an XMLDocument from an existing Registry.
  explicit XMLDocument(std::shared_ptr<Registry> registry);

  /// Owned reference to the registry, which contains all information about the loaded document.
  std::shared_ptr<Registry> registry_;
};

}  // namespace donner::xml
