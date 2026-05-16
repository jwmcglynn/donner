#pragma once
/// @file

#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
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
  XMLNode node;           ///< Element node that owns the attribute.
  XMLQualifiedName name;  ///< Attribute name.
  SourceRange location;   ///< Current full source range of the attribute.
};

/// Local XML reparse scope chosen for a source edit.
enum class ReparseScope : std::uint8_t {
  AttributeValue,  ///< Edit was contained inside one quoted attribute value.
  OpeningTag,      ///< Edit touched an element opening tag outside one attribute value.
  TextNode,        ///< Edit was contained inside a text-like node.
  ElementSubtree,  ///< Edit touched one element subtree.
  Document,        ///< Edit requires whole-document fallback.
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
  ReparseScope scope = ReparseScope::Document;  ///< Scope that produced the mutation.
};

/// Result from \ref XMLDocument::applySourceEdit.
struct ApplySourceEditResult {
  bool applied = false;                         ///< True if source bytes were changed.
  ReparseScope scope = ReparseScope::Document;  ///< Reparse scope selected for the edit.
  std::vector<XMLSourceDelta> sourceDeltas;     ///< Source edits applied by this operation.
  std::vector<XMLMutation> mutations;           ///< DOM mutations emitted by this operation.
  std::optional<ParseDiagnostic> diagnostic;    ///< Diagnostic if local reparsing failed.
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

  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  Registry& registry() { return *registry_; }

  /// Gets the registry as a shared pointer, for advanced use.
  std::shared_ptr<Registry> sharedRegistry() const { return registry_; }

  /// Get the root XMLNdoe of the document.
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
  std::string_view source() const;

  /// Return the source version, or 0 for documents without a source store.
  std::uint64_t sourceVersion() const;

  /// Get the mutable source store, or `nullptr` if this document does not own source text.
  XMLSourceStore* sourceStore();

  /// Get the immutable source store, or `nullptr` if this document does not own source text.
  const XMLSourceStore* sourceStore() const;

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
   * This currently resolves the containing node, then reparses that node's current opening tag to
   * locate attributes. It is intentionally XML-owned and source-store aware, but does not yet
   * require long-lived per-attribute anchors.
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
   * The first implementation handles `AttributeValue` edits. Wider scopes keep the source bytes
   * current and return a diagnostic so callers can preserve the last valid semantic projection.
   *
   * @param intent Source edit request.
   */
  ApplySourceEditResult applySourceEdit(const XMLEditIntent& intent);

  /**
   * Install owned source text for this document.
   *
   * @param source XML source text to own.
   */
  void setSource(std::string source);

private:
  /// Owned reference to the registry, which contains all information about the loaded document.
  std::shared_ptr<Registry> registry_;
};

}  // namespace donner::xml
