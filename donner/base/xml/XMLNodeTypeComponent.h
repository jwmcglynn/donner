#pragma once
/// @file

#include "donner/base/xml/XMLNode.h"

namespace donner::xml::components {

/// Authored XML node kind, including retained raw text nodes.
struct XMLNodeTypeComponent {
  explicit XMLNodeTypeComponent(XMLNode::Type type) : type_(type) {}

  XMLNode::Type type() const { return type_; }

private:
  XMLNode::Type type_;
};

}  // namespace donner::xml::components
