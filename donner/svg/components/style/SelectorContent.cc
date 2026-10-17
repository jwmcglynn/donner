#include "donner/svg/components/style/SelectorContent.h"

#include <optional>
#include <string_view>

#include "donner/base/xml/XMLNode.h"
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

std::optional<RcString> RetainedText(EntityHandle element) {
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

bool RawNodeHasContent(EntityHandle child, bool inspectText, css::SelectorTraversalBudget* budget) {
  const auto node = xml::XMLNode::TryCast(child);
  if (!node) {
    return false;
  }
  if (node->type() == xml::XMLNode::Type::Element) {
    return true;
  }
  return inspectText && IsTextNode(*node) &&
         !IsIgnorableText(node->value().value_or(RcString("")), budget);
}

bool ChildrenAreEmpty(Registry& registry, Entity treeEntity, bool inspectText,
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
    const auto* shadow = registry.try_get<ShadowEntityComponent>(child);
    const Entity data = shadow ? shadow->lightEntity : child;
    if (registry.all_of<ElementTypeComponent>(data) ||
        RawNodeHasContent(EntityHandle(registry, data), inspectText, budget)) {
      return false;
    }
  }
  return true;
}

}  // namespace

void BeginSelectorContentProjection(EntityHandle parent, bool contentsReparsed) {
  if (contentsReparsed) {
    parent.remove<ProjectedNonWhitespaceTextTag>();
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

bool IsSelectorEmpty(Registry& registry, Entity treeEntity, Entity dataEntity,
                     css::SelectorTraversalBudget* budget) {
  const EntityHandle data(registry, dataEntity);
  if (data.all_of<ProjectedNonWhitespaceTextTag>()) {
    return false;
  }
  const auto text = RetainedText(data);
  if (text && !IsIgnorableText(*text, budget)) {
    return false;
  }
  return ChildrenAreEmpty(registry, treeEntity, !text.has_value(), budget);
}

}  // namespace donner::svg::components
