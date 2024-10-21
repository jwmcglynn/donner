#pragma once
/// @file

#include <algorithm>
#include <cassert>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/base/element/ElementLike.h"  // IWYU pragma: keep, for ElementLike
#include "donner/base/xml/XMLQualifiedName.h"

namespace donner {

/**
 * A test fake for a type that satisfies the \ref ElementLike concept. This is used for testing
 * purposes, and has simple implementations for each API.
 */
class FakeElement {
public:
  /**
   * Construct a fake element with the given tag name.
   *
   * @param tagName The XML tag name for this element.
   */
  explicit FakeElement(const xml::XMLQualifiedNameRef& tagName = "unknown")
      : data_(std::make_shared<ElementData>()) {
    // NOTE: This copies the string, but that's okay since this is a test helper.
    data_->tagName = xml::XMLQualifiedName(tagName.namespacePrefix, tagName.name);
  }

  /// @name Core API that satisfies the \ref ElementLike concept
  /// @{

  /// Equality operator.
  bool operator==(const FakeElement& other) const { return data_ == other.data_; }

  /// Get the XML tag name string for this element.
  xml::XMLQualifiedNameRef tagName() const { return data_->tagName; }

  /// Returns true if this is a known element type. For FakeElement, returns false for "unknown"
  /// elements.
  bool isKnownType() const { return tagName() != xml::XMLQualifiedNameRef("unknown"); }

  /// Gets the element id, the value of the "id" attribute.
  RcString id() const { return data_->id; }

  /// Gets the element class name, the value of the "class" attribute.
  RcString className() const { return data_->className; }

  /**
   * Get the value of an attribute, if it exists.
   *
   * @param name Name of the attribute to get.
   * @return The value of the attribute, or \c std::nullopt if the attribute does not exist.
   */
  std::optional<RcString> getAttribute(const xml::XMLQualifiedNameRef& name) const {
    auto it = data_->attributes.find(xml::XMLQualifiedName(name.namespacePrefix, name.name));
    if (it != data_->attributes.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  /**
   * Find attributes matching the given name matcher.
   *
   * @param matcher Matcher to use to find attributes. If \ref XMLQualifiedNameRef::namespacePrefix
   * is "*", the matcher will match any namespace with the given attribute name.
   * @return A vector of attributes matching the given name matcher.
   */
  SmallVector<xml::XMLQualifiedNameRef, 1> findMatchingAttributes(
      const xml::XMLQualifiedNameRef& matcher) const {
    SmallVector<xml::XMLQualifiedNameRef, 1> result;
    const xml::XMLQualifiedNameRef attributeNameOnly(matcher.name);

    for (const auto& [name, value] : data_->attributes) {
      if (matcher.namespacePrefix == "*") {
        if (StringUtils::Equals<StringComparison::IgnoreCase>(name.name, attributeNameOnly.name)) {
          result.push_back(name);
        }
      } else {
        if (StringUtils::Equals<StringComparison::IgnoreCase>(name.namespacePrefix,
                                                              matcher.namespacePrefix) &&
            StringUtils::Equals<StringComparison::IgnoreCase>(name.name, matcher.name)) {
          result.push_back(name);
        }
      }
    }

    return result;
  }

  /**
   * Get this element's parent, if it exists. If the parent is not set, this document is either the
   * root element or has not been inserted into the document tree.
   *
   * @return The parent element, or \c std::nullopt if the parent is not set.
   */
  std::optional<FakeElement> parentElement() const {
    if (data_->parent) {
      if (auto parent = data_->parent->lock()) {
        return FakeElement(parent);
      }
    }
    return std::nullopt;
  }

  /**
   * Get the first child of this element, if it exists.
   *
   * @return The first child element, or \c std::nullopt if the element has no children.
   */
  std::optional<FakeElement> firstChild() const {
    return data_->children.empty() ? std::nullopt : std::make_optional(data_->children.front());
  }

  /**
   * Get the last child of this element, if it exists.
   *
   * @return The last child element, or \c std::nullopt if the element has no children.
   */
  std::optional<FakeElement> lastChild() const {
    return data_->children.empty() ? std::nullopt : std::make_optional(data_->children.back());
  }

  /**
   * Get the previous sibling of this element, if it exists.
   *
   * @return The previous sibling element, or \c std::nullopt if the element has no previous
   * sibling.
   */
  std::optional<FakeElement> previousSibling() const {
    auto parent = parentElement();
    if (!parent) return std::nullopt;

    // Find where this element is in the parent, and return the previous if non-zero.
    auto it = std::find(parent->data_->children.begin(), parent->data_->children.end(), *this);
    assert(it != parent->data_->children.end());
    if (it != parent->data_->children.begin()) {
      return std::make_optional(*(it - 1));
    }

    return std::nullopt;
  }

  /**
   * Get the next sibling of this element, if it exists.
   *
   * @return The next sibling element, or \c std::nullopt if the element has no next sibling.
   */
  std::optional<FakeElement> nextSibling() const {
    auto parent = parentElement();
    if (!parent) return std::nullopt;

    // Find where this element is in the parent, and return the next if we're not the last.
    auto it = std::find(parent->data_->children.begin(), parent->data_->children.end(), *this);
    assert(it != parent->data_->children.end());
    if (++it != parent->data_->children.end()) {
      return std::make_optional(*it);
    }

    return std::nullopt;
  }

  /// @}

  /// @name Mutator methods
  /// @{

  /// Sets the element id, the value of the "id" attribute.
  void setId(const RcString& id) { data_->id = id; }

  /// Sets the element class name, the value of the "class" attribute.
  void setClassName(const RcString& className) { data_->className = className; }

  /// Sets the value of an attribute.
  void setAttribute(const xml::XMLQualifiedNameRef& name, const RcString& value) {
    // NOTE: This copies the string, but that's okay since this is a test helper.
    data_->attributes[xml::XMLQualifiedName(name.namespacePrefix, name.name)] = value;
  }

  /// Appends a new child to this element's child list.
  void appendChild(const FakeElement& child) {
    assert(!child.data_->parent && "Child element cannot already have a parent");
    child.data_->parent = std::weak_ptr<ElementData>(data_);
    data_->children.push_back(child);
  }

  /// @}

  /// Ostream output operator, which outputs the element's tag name, id, class name, and attributes
  /// like a CSS selector.
  friend std::ostream& operator<<(std::ostream& os, const FakeElement& elem) {
    os << "FakeElement: " << elem.data_->tagName;
    if (!elem.data_->id.empty()) {
      os << "#" << elem.data_->id;
    }
    if (!elem.data_->className.empty()) {
      os << "." << elem.data_->className;
    }
    if (!elem.data_->attributes.empty()) {
      for (const auto& [key, value] : elem.data_->attributes) {
        os << "[";
        os << key.toString() << "=" << value;
        os << "]";
      }
    }
    return os << ", numChildren=" << elem.data_->children.size();
  }

  /**
   * Helper class to change how this element is printed as a tree.
   *
   * Example usage:
   * ```
   * FakeElement element;
   * std::cout << element.printAsTree() << "\n";
   * ```
   *
   * Where \ref FakeElement::printAsTree returns a DeferredPrinter as its result.
   */
  struct DeferredPrinter {
    /// The element to print.
    const FakeElement& element;  // NOLINT

    /// Ostream output operator, which prints the element's and all children.
    friend std::ostream& operator<<(std::ostream& os, const DeferredPrinter& printer) {
      return printer.element.printTreeImpl(os);
    }
  };

  /**
   * When used in an ostream output stream, prints the element's and all children instead of just
   * the element.
   *
   * Example usage:
   * ```
   * FakeElement element;
   * std::cout << element.printAsTree() << "\n";
   * ```
   */
  DeferredPrinter printAsTree() const { return DeferredPrinter{*this}; }

  /// Comparison operator, uses pointer comparison for simplicity.
  bool operator<(const FakeElement& other) const {
    // Use pointer comparison for simplicity.
    return data_.get() < other.data_.get();
  }

private:
  /// Element data storage.
  struct ElementData {
    /// Element id, the value of the "id" attribute.
    RcString id;
    /// Element class name, the value of the "class" attribute.
    RcString className;
    /// Element tag name, the value of the "tag" attribute.
    xml::XMLQualifiedName tagName;
    /// Element attributes.
    std::map<xml::XMLQualifiedName, RcString> attributes;
    /// Element children.
    std::vector<FakeElement> children;
    /// Element parent.
    std::optional<std::weak_ptr<ElementData>> parent;

    /// Default constructor.
    ElementData() : tagName("unknown") {}
    /// Destructor.
    ~ElementData() = default;

    // No copy.
    ElementData(const ElementData&) = delete;
    ElementData& operator=(const ElementData&) = delete;
  };

  /**
   * Internal constructor to FakeElement from existing \ref ElementData.
   *
   * \param data The element data.
   */
  explicit FakeElement(std::shared_ptr<ElementData> data) : data_(std::move(data)) {}

  /// Prints the element and all children to the given ostream.
  std::ostream& printTreeImpl(std::ostream& os, int depth = 0) const {
    for (int i = depth; i > 0; --i) {
      os << "  ";
      if (i == 1) {
        os << "- ";
      }
    }
    os << *this << "\n";
    for (const auto& child : data_->children) {
      child.printTreeImpl(os, depth + 1);
    }

    return os;
  }

  /// The element data.
  std::shared_ptr<ElementData> data_;
};

static_assert(ElementLike<FakeElement>, "FakeElement must satisfy ElementLike");

}  // namespace donner
