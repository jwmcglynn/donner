#include "donner/svg/components/style/SelectorContent.h"

#include <optional>
#include <string_view>

#include "donner/base/xml/XMLNodeTypeComponent.h"
#include "donner/base/xml/components/XMLValueComponent.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/css/selectors/SelectorMatchOptions.h"
#include "donner/svg/components/DescriptiveTextComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/text/TextComponent.h"

namespace donner::svg::components {
namespace {

bool IsIgnorableText(const RcString& value, css::SelectorTraversalBudget* budget) {
  const std::string_view text(value.data(), value.size());
  // Charge bounded chunks so repeated whitespace-only text inspection stays bounded too.
  constexpr std::size_t kTextBytesPerStep = 64;
  for (std::size_t offset = 0; offset < text.size(); offset += kTextBytesPerStep) {
    if (budget != nullptr && !budget->consume()) {
      return false;
    }
    if (text.substr(offset, kTextBytesPerStep).find_first_not_of(" \t\r\n") !=
        std::string_view::npos) {
      return false;
    }
  }
  return true;
}

std::optional<RcString> RetainedText(entt::basic_handle<const Registry> element) {
  if (const auto* text = element.try_get<TextComponent>()) {
    return text->text;
  }
  if (const auto* text = element.try_get<DescriptiveTextComponent>()) {
    return text->text;
  }
  if (const auto* style = element.try_get<StylesheetComponent>();
      style && element.get<ElementTypeComponent>().type() == ElementType::Style) {
    return style->text;
  }
  return std::nullopt;
}

bool IsTextNode(const xml::XMLNode& node) {
  return node.type() == xml::XMLNode::Type::Data || node.type() == xml::XMLNode::Type::CData;
}

bool RawNodeHasContent(const Registry& registry, Entity child, bool inspectText,
                       css::SelectorTraversalBudget* budget) {
  const auto* kind = registry.try_get<xml::components::XMLNodeTypeComponent>(child);
  if (!kind) {
    return false;
  }
  if (kind->type() == xml::XMLNode::Type::Element) {
    return true;
  }
  if (!inspectText || (kind->type() != xml::XMLNode::Type::Data &&
                       kind->type() != xml::XMLNode::Type::CData)) {
    return false;
  }
  const auto* value = registry.try_get<xml::components::XMLValueComponent>(child);
  return value && !IsIgnorableText(value->value, budget);
}

bool ChildrenAreEmpty(const Registry& registry, Entity treeEntity, bool inspectText,
                      css::SelectorTraversalBudget* budget) {
  if (treeEntity == entt::null) {
    return true;
  }
  const auto& tree = registry.get<donner::components::TreeComponent>(treeEntity);
  for (Entity child = tree.firstChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    if (budget != nullptr && !budget->consume()) {
      return false;
    }
    if (registry.all_of<ShadowEntityComponent>(child)) {
      continue;
    }
    if (registry.all_of<ElementTypeComponent>(child) ||
        RawNodeHasContent(registry, child, inspectText, budget)) {
      return false;
    }
  }
  return true;
}

bool HasRawTextChildren(EntityHandle parent) {
  const auto node = xml::XMLNode::TryCast(parent);
  if (node) {
    for (auto child = node->firstChild(); child; child = child->nextSibling()) {
      if (IsTextNode(*child)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

void RejectSelectorContentProjection(EntityHandle parent) {
  if (RetainedText(parent)) {
    parent.get_or_emplace<SelectorRejectedTextTag>();
  }
}

void AcceptSelectorTextValueProjection(EntityHandle parent) {
  parent.remove<SelectorRejectedTextTag, ProjectedNonWhitespaceTextTag>();
}

void CompleteSelectorContentProjection(EntityHandle parent, bool contentsReparsed) {
  if (contentsReparsed) {
    parent.remove<ProjectedNonWhitespaceTextTag, SelectorRawTextAuthorityTag,
                  SelectorRejectedTextTag>();
    if (RetainedText(parent) && HasRawTextChildren(parent)) {
      parent.get_or_emplace<SelectorRawTextAuthorityTag>();
    }
  }
}

void ResetSelectorTextContent(EntityHandle parent) {
  parent.remove<ProjectedNonWhitespaceTextTag, SelectorRawTextAuthorityTag,
                SelectorRejectedTextTag>();
  const auto node = xml::XMLNode::TryCast(parent);
  if (!node) {
    return;
  }
  for (auto child = node->firstChild(); child;) {
    const auto next = child->nextSibling();
    if (IsTextNode(*child)) {
      child->remove();
    }
    child = next;
  }
}

void RecordAppendedSelectorText(EntityHandle parent, std::string_view text) {
  if (parent.all_of<SelectorRawTextAuthorityTag>() && !IsIgnorableText(RcString(text), nullptr)) {
    parent.get_or_emplace<ProjectedNonWhitespaceTextTag>();
  }
}

void RecordDiscardedSelectorText(EntityHandle parent, const xml::XMLNode& node) {
  // A retained string is authoritative. Preserve a summary only for otherwise lost content.
  if (const auto text = RetainedText(parent); text && !text->empty()) {
    return;
  }
  for (auto child = node.firstChild(); child; child = child->nextSibling()) {
    if (IsTextNode(*child) && !IsIgnorableText(child->value().value_or(RcString("")), nullptr)) {
      parent.get_or_emplace<ProjectedNonWhitespaceTextTag>();
      return;
    }
  }
}

bool IsSelectorEmpty(const Registry& registry, Entity dataEntity, css::SelectorTraversalBudget* budget) {
  const entt::basic_handle<const Registry> data(registry, dataEntity);
  if (data.all_of<ProjectedNonWhitespaceTextTag>()) {
    return false;
  }
  const bool rejectedText = data.all_of<SelectorRejectedTextTag>();
  if (rejectedText || !data.all_of<SelectorRawTextAuthorityTag>()) {
    if (const auto text = RetainedText(data); text && !IsIgnorableText(*text, budget)) {
      return false;
    }
  }
  return ChildrenAreEmpty(registry, dataEntity, !rejectedText, budget);
}

}  // namespace donner::svg::components
