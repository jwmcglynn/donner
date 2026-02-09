#pragma once
/// @file

#include <string_view>

#include "donner/base/EcsRegistry.h"
#include "donner/base/FileOffset.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/base/Utils.h"
#include "donner/base/xml/XMLQualifiedName.h"

namespace donner::xml {

struct EditOperation {
  enum class Type { ReplaceValue };

  static EditOperation ReplaceValue(FileOffsetRange range, const RcStringOrRef& replacement) {
    return EditOperation{Type::ReplaceValue, range, RcString(replacement)};
  }

  Type type;
  FileOffsetRange targetRange;
  RcString replacement;
};

// Forward declaration, #include "donner/base/xml/XMLDocument.h"
class XMLDocument;

/**
 * Represents an XML element belonging to an \ref XMLDocument.
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
 * @see \ref XMLDocument
 * @see \ref EcsArchitecture
 */
class XMLNode {
protected:
  friend class XMLDocument;

  /**
   * Internal constructor to create an XMLNode from an \ref EntityHandle.
   *
   * @param handle EntityHandle to wrap.
   */
  explicit XMLNode(EntityHandle handle);

  /**
   * Create an XMLNode for the root node of a document. This is called internally by \ref
   * XMLDocument.
   */
  static XMLNode CreateDocumentNode(XMLDocument& document);

public:
  /// Node type, use \ref type() to query the value. To create nodes of different types, use the
  /// relevant static method constructor, such as \ref CreateElementNode and \ref CreateCommentNode.
  enum class Type : uint8_t {
    /// Document node, which is the root of the document tree. This is created automatically by \ref
    /// XMLDocument. \ref tagName() and \ref value() are empty.
    Document,
    /// Element node, representing a regular XML tag, such as `<svg>`. \ref tagName() is the tag
    /// name, and \ref value() contains the text of the first data node.
    /// @see https://www.w3.org/TR/xml/#dt-element
    Element,
    /// Data node, containing verbatim text. \ref value() contains the data, and \ref tagName() is
    /// empty.
    Data,
    /// CDATA node, such as `<![CDATA[ ... ]]>` . \ref value() contains the data, and \ref tagName()
    /// is empty.
    /// @see https://www.w3.org/TR/xml/#sec-cdata-sect
    CData,
    /// Comment node, such as `<!-- ... -->`. \ref value() contains the comment text, and \ref
    /// tagName() is empty.
    /// @see https://www.w3.org/TR/xml/#sec-comments
    Comment,
    /// Document Type Declaration (DTD) node, such as `<!DOCTYPE ...>`. \ref tagName() is empty, and
    /// \ref value() contains the contents of the node.
    /// @see https://www.w3.org/TR/xml/#dtd
    DocType,
    /// Processing Instruction (PI) node, such as `<?php ... ?>`. \ref tagName() is the PI
    /// target, e.g. "php". \ref value() contains the remaining content.
    /// @see https://www.w3.org/TR/xml/#sec-pi
    ProcessingInstruction,
    /// XML Declaration node, such as `<?xml ... ?>`, which is a special case of \ref
    /// ProcessingInstruction when the type is "xml". Contents are parsed as attributes. \ref
    /// tagName() is "xml" and \ref value() is empty.
    /// @see https://www.w3.org/TR/xml/#sec-prolog-dtd
    XMLDeclaration,
  };

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const Type& type) {
    switch (type) {
      case Type::Document: return os << "Document";
      case Type::Element: return os << "Element";
      case Type::Data: return os << "Data";
      case Type::CData: return os << "CData";
      case Type::Comment: return os << "Comment";
      case Type::DocType: return os << "DocType";
      case Type::ProcessingInstruction: return os << "ProcessingInstruction";
      case Type::XMLDeclaration: return os << "XMLDeclaration";
    }

    UTILS_UNREACHABLE();
  }

  /**
   * Create a new XML node for an element bound to \p document. Note that this does not insert the
   * node into the document tree, to do so call \ref appendChild.
   *
   * @param document Containing document.
   * @param tagName Node tag name, such as "xml" or "svg".
   */
  static XMLNode CreateElementNode(XMLDocument& document, const XMLQualifiedNameRef& tagName);

  /**
   * Create a new XML node for an element bound to \p document, with a given \p value. Note that
   * this does not insert the node into the document tree, to do so call \ref appendChild.
   *
   * @param document Containing document.
   * @param value Node value, such as "Hello, world!".
   */
  static XMLNode CreateDataNode(XMLDocument& document, const RcStringOrRef& value);

  /**
   * Create a new XML node for a CDATA section bound to \p document, with a given \p value. Note
   * that this does not insert the node into the document tree, to do so call \ref appendChild.
   *
   * This is represented as `<![CDATA[ ... ]]>` in XML.
   *
   * @param document Containing document.
   * @param value Node value, such as "Hello, world!".
   */
  static XMLNode CreateCDataNode(XMLDocument& document, const RcStringOrRef& value);

  /**
   * Create a new XML node for a comment bound to \p document, with a given \p value. Note that this
   * does not insert the node into the document tree, to do so call \ref appendChild.
   *
   * This is represented as `<!-- ... -->` in XML.
   *
   * @param document Containing document.
   * @param value Node value, such as "Hello, world!".
   */
  static XMLNode CreateCommentNode(XMLDocument& document, const RcStringOrRef& value);

  /**
   * Create a new XML node for a document type declaration bound to \p document, with a given \p
   * value. Note that this does not insert the node into the document tree, to do so call \ref
   * appendChild.
   *
   * This is represented as `<!DOCTYPE ...>` in XML.
   *
   * @param document Containing document.
   * @param value Node value, such as `svg`.
   */
  static XMLNode CreateDocTypeNode(XMLDocument& document, const RcStringOrRef& value);

  /**
   * Create a new XML node for a processing instruction bound to \p document, with a given \p
   * target and \p value. Note that this does not insert the node into the document tree, to do so
   * call \ref appendChild.
   *
   * This is represented as `<?php ... ?>` in XML.
   *
   * @param document Containing document.
   * @param target Processing instruction target, such as "php".
   * @param value Processing instruction value, such as "echo 'Hello, world!';".
   */
  static XMLNode CreateProcessingInstructionNode(XMLDocument& document, const RcStringOrRef& target,
                                                 const RcStringOrRef& value);

  /**
   * Create a new XML node for an XML declaration bound to \p document. Note that this does not
   * insert the node into the document tree, to do so call \ref appendChild.
   *
   * This is represented as `<?xml ... ?>` in XML.
   *
   * Contents of the declaration are accessible through the attribute getters/setters.
   *
   * @param document Containing document.
   */
  static XMLNode CreateXMLDeclarationNode(XMLDocument& document);

  /**
   * Try to cast to an XMLNode from a raw \ref Entity. This is a checked cast, and will return \c
   * std::nullopt if the entity is not an XML node.
   *
   * @param entity Entity to cast.
   */
  static std::optional<XMLNode> TryCast(EntityHandle entity);

  /// Create another reference to the same XMLNode.
  XMLNode(const XMLNode& other);

  /// Move constructor.
  XMLNode(XMLNode&& other) noexcept;

  /// Destructor.
  ~XMLNode() noexcept = default;

  /// Create another reference to the same XMLNode.
  XMLNode& operator=(const XMLNode& other);

  /// Move assignment operator.
  XMLNode& operator=(XMLNode&& other) noexcept;

  /// Get the type of this node.
  Type type() const;

  /// Get the XML tag name string for this node.
  XMLQualifiedNameRef tagName() const;

  /// Get the underlying \ref EntityHandle, for advanced use-cases that require direct access to the
  /// ECS.
  EntityHandle entityHandle() const { return handle_; }

  /**
   * Get the value of this node, which depends on the node type. For nodes without a value, this
   * will return \c std::nullopt.
   */
  std::optional<RcString> value() const;

  /**
   * Set the value of this node.
   */
  void setValue(const RcStringOrRef& value);

  /**
   * Set the value of this node and emit an EditOperation if a source span is available.
   */
  std::optional<EditOperation> setValuePreserveSource(const RcStringOrRef& value);

  /**
   * Returns true if the element has an attribute with the given name.
   *
   * @param name Name of the attribute to check.
   * @return true if the attribute exists, false otherwise.
   */
  bool hasAttribute(const XMLQualifiedNameRef& name) const;

  /**
   * Get the value of an attribute, if it exists.
   *
   * @param name Name of the attribute to get.
   * @return The value of the attribute, or \c std::nullopt if the attribute does not exist.
   */
  std::optional<RcString> getAttribute(const XMLQualifiedNameRef& name) const;

  /**
   * Get the location of this node in the input string.
   *
   * For example, for the following XML:
   * ```xml
   * <root>
   *   <child>Hello, world!</child>
   * </root>
   * ```
   *
   * The FileOffsetRange for the `child` element should contain the substring
   * `<child>Hello, world!</child>`
   *
   * @return Start and end offsets of the node in the input string, or \c std::nullptr if source
   * locations are not available.
   */
  std::optional<FileOffsetRange> getNodeLocation() const;

  /**
   * Get the location of an attribute in the input string.
   *
   * For example, for the following XML:
   * ```xml
   * <root>
   *   <child attr="Hello, world!">
   * </root>
   * ```
   *
   * The FileOffsetRange for the `attr` attribute should contain the substring `attr="Hello,
   * world!"`
   *
   * @param xmlInput Input string to get the attribute location in.
   * @param name Name of the attribute to get the location for.
   * @return Offset of the attribute in the input string, or \c std::nullopt if the attribute does
   * not exist.
   */
  std::optional<FileOffsetRange> getAttributeLocation(std::string_view xmlInput,
                                                      const XMLQualifiedNameRef& name) const;

  /**
   * Get the source span for this node's value content, if tracked.
   */
  std::optional<FileOffsetRange> getValueLocation() const;

  /**
   * Get the source span of an attribute's value, if tracked.
   */
  std::optional<FileOffsetRange> getAttributeValueLocation(const XMLQualifiedNameRef& name) const;

  /**
   * Record the source range of an attribute during parsing.
   *
   * @param name Name of the attribute.
   * @param location Byte range covering the full attribute text, including name, equals, quotes,
   * and value.
   */
  void addAttributeLocation(const XMLQualifiedNameRef& name, FileOffsetRange location,
                            FileOffsetRange valueRange);

  /// Get the list of attributes for this element.
  SmallVector<XMLQualifiedNameRef, 10> attributes() const;

  /**
   * Get the namespace URI of an attribute prefix, if it exists.
   *
   * @param prefix Prefix of the attribute to get the namespace URI for.
   * @return The URI of the prefix, or \c std::nullopt if the prefix does not exist.
   */
  std::optional<RcString> getNamespaceUri(const RcString& prefix) const;

  /**
   * Set the value of a generic XML attribute, which may be either a presentation attribute or
   * custom user-provided attribute.
   *
   * This API supports a superset of \ref trySetPresentationAttribute, however its parse errors are
   * ignored. If the attribute is not a presentation attribute, or there are parse errors the
   * attribute will be stored as a custom attribute instead.
   *
   * @param name Name of the attribute to set.
   * @param value New value to set.
   */
  void setAttribute(const XMLQualifiedNameRef& name, std::string_view value);

  /**
   * Set the attribute value and emit an EditOperation when the attribute spans are known.
   */
  std::optional<EditOperation> setAttributePreserveSource(const XMLQualifiedNameRef& name,
                                                          std::string_view value);

  /**
   * Remove an attribute, which may be either a presentation attribute or custom user-provided
   * attribute.
   *
   * If this is a presentation attribute, the presentation attributes value will be removed
   * (internally by setting the value to 'inherit').
   *
   * @param name Name of the attribute to remove.
   */
  void removeAttribute(const XMLQualifiedNameRef& name);

  /**
   * Get this element's parent, if it exists. If the parent is not set, this document is either the
   * root element or has not been inserted into the document tree.
   *
   * @return The parent element, or \c std::nullopt if the parent is not set.
   */
  std::optional<XMLNode> parentElement() const;

  /**
   * Get the first child of this element, if it exists.
   *
   * @return The first child element, or \c std::nullopt if the element has no children.
   */
  std::optional<XMLNode> firstChild() const;

  /**
   * Get the last child of this element, if it exists.
   *
   * @return The last child element, or \c std::nullopt if the element has no children.
   */
  std::optional<XMLNode> lastChild() const;

  /**
   * Get the previous sibling of this element, if it exists.
   *
   * @return The previous sibling element, or \c std::nullopt if the element has no previous
   * sibling.
   */
  std::optional<XMLNode> previousSibling() const;

  /**
   * Get the next sibling of this element, if it exists.
   *
   * @return The next sibling element, or \c std::nullopt if the element has no next sibling.
   */
  std::optional<XMLNode> nextSibling() const;

  /**
   * Insert \p newNode as a child, before \p referenceNode. If \p referenceNode is std::nullopt,
   * append the child.
   *
   * If \p newNode is already in the tree, it is first removed from its parent. However, if
   * inserting the child will create a cycle, the behavior is undefined.
   *
   * @param newNode New node to insert.
   * @param referenceNode A child of this node to insert \p newNode before, or \c std::nullopt. Must
   * be a child of the current node.
   */
  void insertBefore(const XMLNode& newNode, std::optional<XMLNode> referenceNode);

  /**
   * Append \p child as a child of the current node.
   *
   * If child is already in the tree, it is first removed from its parent. However, if inserting
   * the child will create a cycle, the behavior is undefined.
   *
   * @param child Node to append.
   */
  void appendChild(const XMLNode& child);

  /**
   * Replace \p oldChild with \p newChild in the tree, removing \p oldChild and inserting \p
   * newChild in its place.
   *
   * If \p newChild is already in the tree, it is first removed from its parent. However, if
   * inserting the child will create a cycle, the behavior is undefined.
   *
   * @param newChild New child to insert.
   * @param oldChild Old child to remove, must be a child of the current node.
   */
  void replaceChild(const XMLNode& newChild, const XMLNode& oldChild);

  /**
   * Remove \p child from this node.
   *
   * @param child Child to remove, must be a child of the current node.
   */
  void removeChild(const XMLNode& child);

  /**
   * Remove this node from its parent, if it has one. Has no effect if this has no parent.
   */
  void remove();

  /**
   * Get the source offset of where this node starts in the XML document source (if this node was
   * instantiated by \ref XMLParser).
   */
  std::optional<FileOffset> sourceStartOffset() const;

  /**
   * Set the source offset of where this node starts in the XML document source.
   *
   * @param offset Offset in the source document, in characters from the start.
   */
  void setSourceStartOffset(FileOffset offset);

  /**
   * Get the source offset of where this node ends in the XML document source (if this node was
   * instantiated by \ref XMLParser).
   */
  std::optional<FileOffset> sourceEndOffset() const;

  /**
   * Set the source offset of where this node ends in the XML document source.
   *
   * @param offset Offset in the source document, in characters from the start.
   */
  void setSourceEndOffset(FileOffset offset);

  /**
   * Set the source offsets that cover just the node's value payload (excluding delimiters).
   */
  void setValueSourceRange(FileOffsetRange range);

  /**
   * Returns true if the two XMLNode handles reference the same underlying document.
   */
  bool operator==(const XMLNode& other) const { return handle_ == other.handle_; }

  /**
   * Returns true if the two XMLNode handles reference the same underlying document.
   */
  bool operator!=(const XMLNode& other) const { return handle_ != other.handle_; }

protected:
  /**
   * Create an Entity for a node of \p type.
   *
   * @param registry Registry to create the node on.
   * @param type Type of node to create.
   * @param tagName Tag name of the node to create. This is only used for some types, for other
   * nodes pass an empty string.
   */
  static Entity CreateEntity(Registry& registry, Type type,
                             const XMLQualifiedNameRef& tagName = "");

  /// Get the underlying ECS Registry, which holds all data for the document, for advanced use.
  Registry& registry() const { return *handle_.registry(); }

  /**
   * Convert an Entity to an EntityHandle, for advanced use.
   *
   * @param entity Entity to convert.
   */
  EntityHandle toHandle(Entity entity) const { return EntityHandle(registry(), entity); }

  /// The underlying ECS Entity for this element, which holds all data.
  EntityHandle handle_;
};

}  // namespace donner::xml
