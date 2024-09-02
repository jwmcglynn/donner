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
  explicit FakeElement(const XMLQualifiedNameRef& tagName = "unknown")
      : data_(std::make_shared<ElementData>()) {
    // NOTE: This copies the string, but that's okay since this is a test helper.
    data_->tagName = XMLQualifiedName(tagName.namespacePrefix, tagName.name);
  }

  /// @name Core API that satisfies the \ref ElementLike concept
  /// @{

  bool operator==(const FakeElement& other) const { return data_ == other.data_; }

  XMLQualifiedNameRef tagName() const { return data_->tagName; }

  bool isKnownType() const { return tagName() != XMLQualifiedNameRef("unknown"); }

  RcString id() const { return data_->id; }

  RcString className() const { return data_->className; }

  std::optional<RcString> getAttribute(const XMLQualifiedNameRef& name) const {
    auto it = data_->attributes.find(XMLQualifiedName(name.namespacePrefix, name.name));
    if (it != data_->attributes.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  SmallVector<XMLQualifiedNameRef, 1> findMatchingAttributes(
      const XMLQualifiedNameRef& matcher) const {
    SmallVector<XMLQualifiedNameRef, 1> result;
    const XMLQualifiedNameRef attributeNameOnly(matcher.name);

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

  std::optional<FakeElement> parentElement() const {
    if (data_->parent) {
      if (auto parent = data_->parent->lock()) {
        return FakeElement(parent);
      }
    }
    return std::nullopt;
  }

  std::optional<FakeElement> firstChild() const {
    return data_->children.empty() ? std::nullopt : std::make_optional(data_->children.front());
  }

  std::optional<FakeElement> lastChild() const {
    return data_->children.empty() ? std::nullopt : std::make_optional(data_->children.back());
  }

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

  void setId(const RcString& id) { data_->id = id; }

  void setClassName(const RcString& className) { data_->className = className; }

  void setAttribute(const XMLQualifiedNameRef& name, const RcString& value) {
    // NOTE: This copies the string, but that's okay since this is a test helper.
    data_->attributes[XMLQualifiedName(name.namespacePrefix, name.name)] = value;
  }

  void appendChild(const FakeElement& child) {
    assert(!child.data_->parent && "Child element cannot already have a parent");
    child.data_->parent = std::weak_ptr<ElementData>(data_);
    data_->children.push_back(child);
  }

  /// @}

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

  struct DeferredPrinter {
    const FakeElement& element;

    friend std::ostream& operator<<(std::ostream& os, const DeferredPrinter& printer) {
      return printer.element.printTreeImpl(os);
    }
  };

  DeferredPrinter printAsTree() const { return DeferredPrinter{*this}; }

  bool operator<(const FakeElement& other) const {
    // Use pointer comparison for simplicity.
    return data_.get() < other.data_.get();
  }

private:
  struct ElementData {
    RcString id;
    RcString className;
    XMLQualifiedName tagName;
    std::map<XMLQualifiedName, RcString> attributes;
    std::vector<FakeElement> children;
    std::optional<std::weak_ptr<ElementData>> parent;

    ElementData() : tagName("unknown") {}
    ~ElementData() = default;

    // No copy.
    ElementData(const ElementData&) = delete;
    ElementData& operator=(const ElementData&) = delete;
  };

  explicit FakeElement(std::shared_ptr<ElementData> data) : data_(std::move(data)) {}

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

  std::shared_ptr<ElementData> data_;
};

static_assert(ElementLike<FakeElement>, "FakeElement must satisfy ElementLike");

}  // namespace donner
