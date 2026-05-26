#include "donner/editor/StyleSourceAnnotations.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/FileOffset.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/css/CSS.h"
#include "donner/css/Declaration.h"
#include "donner/css/Specificity.h"
#include "donner/editor/ReferenceFanout.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/SVGStyleQuery.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {
namespace {

constexpr css::Specificity kPresentationSpecificity = css::Specificity::FromABC(0, 0, 0);
struct ContributionKey {
  StyleContributionKind kind = StyleContributionKind::StylesheetDeclaration;
  Entity elementEntity = entt::null;
  Entity stylesheetEntity = entt::null;
  std::size_t ruleIndex = 0;
  std::size_t declarationIndex = 0;
  std::string propertyName;

  bool operator==(const ContributionKey& other) const = default;
};

struct ContributionKeyHash {
  std::size_t operator()(const ContributionKey& key) const {
    std::size_t hash = std::hash<int>()(static_cast<int>(key.kind));
    hash ^= std::hash<std::uint32_t>()(static_cast<std::uint32_t>(key.elementEntity)) +
            0x9e3779b9u + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<std::uint32_t>()(static_cast<std::uint32_t>(key.stylesheetEntity)) +
            0x9e3779b9u + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<std::size_t>()(key.ruleIndex) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
    hash ^=
        std::hash<std::size_t>()(key.declarationIndex) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
    hash ^= std::hash<std::string>()(key.propertyName) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
    return hash;
  }
};

struct CascadeEntry {
  std::size_t contributionIndex = 0;
  css::Specificity specificity;
  std::optional<css::Declaration> declaration;
  std::string presentationValue;
};

struct Winner {
  css::Specificity specificity;
  std::size_t order = 0;
  std::size_t contributionIndex = 0;
};

struct FragmentReference {
  std::string fragmentId;
  std::optional<svg::SVGElement> referencingElement;
};

[[nodiscard]] std::optional<SourceByteRange> ToByteRange(const SourceRange& range,
                                                         std::size_t sourceSize) {
  if (!range.start.offset.has_value() || !range.end.offset.has_value()) {
    return std::nullopt;
  }

  const std::size_t start = *range.start.offset;
  const std::size_t end = *range.end.offset;
  if (start > end || start > sourceSize) {
    return std::nullopt;
  }

  return SourceByteRange{start, std::min(end, sourceSize)};
}

[[nodiscard]] bool IsAsciiSpace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

[[nodiscard]] std::string TagNameForElement(const svg::SVGElement& element) {
  if (std::optional<xml::XMLQualifiedNameRef> tagName = element.tryTagName()) {
    return std::string(tagName->name);
  }

  return "element";
}

[[nodiscard]] bool HasAncestorNamed(const svg::SVGElement& element, std::string_view tagName) {
  for (std::optional<svg::SVGElement> ancestor = element.parentElement(); ancestor.has_value();
       ancestor = ancestor->parentElement()) {
    if (std::optional<xml::XMLQualifiedNameRef> ancestorTagName = ancestor->tryTagName();
        ancestorTagName.has_value() && std::string_view(ancestorTagName->name) == tagName) {
      return true;
    }
  }

  return false;
}

[[nodiscard]] bool IsReferenceResourceElement(const svg::SVGElement& element) {
  const std::optional<svg::ElementType> type = element.tryType();
  if (!type.has_value() || !xml::XMLNode::TryCast(element.entityHandle()).has_value()) {
    return false;
  }

  switch (*type) {
    case svg::ElementType::ClipPath:
    case svg::ElementType::Filter:
    case svg::ElementType::LinearGradient:
    case svg::ElementType::Marker:
    case svg::ElementType::Mask:
    case svg::ElementType::Pattern:
    case svg::ElementType::RadialGradient:
    case svg::ElementType::Symbol: return true;
    case svg::ElementType::Defs:
    case svg::ElementType::Stop:
    case svg::ElementType::Style: return false;
    default: break;
  }

  return HasAncestorNamed(element, "defs");
}

void AppendFragmentReference(std::vector<FragmentReference>* references,
                             std::string_view fragmentId,
                             std::optional<svg::SVGElement> referencingElement) {
  if (fragmentId.empty()) {
    return;
  }

  references->push_back(FragmentReference{
      .fragmentId = std::string(fragmentId),
      .referencingElement = referencingElement,
  });
}

void AppendUrlFragmentReferences(std::string_view value,
                                 std::optional<svg::SVGElement> referencingElement,
                                 std::vector<FragmentReference>* references) {
  std::size_t searchPos = 0;
  while (searchPos < value.size()) {
    const std::size_t urlPos = value.find("url(", searchPos);
    if (urlPos == std::string_view::npos) {
      return;
    }

    std::size_t index = urlPos + 4;
    while (index < value.size() && IsAsciiSpace(value[index])) {
      ++index;
    }

    char quote = '\0';
    if (index < value.size() && (value[index] == '\'' || value[index] == '"')) {
      quote = value[index];
      ++index;
      while (index < value.size() && IsAsciiSpace(value[index])) {
        ++index;
      }
    }

    if (index >= value.size() || value[index] != '#') {
      searchPos = index;
      continue;
    }

    const std::size_t fragmentStart = index + 1;
    std::size_t fragmentEnd = fragmentStart;
    while (fragmentEnd < value.size()) {
      const char ch = value[fragmentEnd];
      if ((quote != '\0' && ch == quote) || (quote == '\0' && (ch == ')' || IsAsciiSpace(ch)))) {
        break;
      }
      ++fragmentEnd;
    }

    AppendFragmentReference(references, value.substr(fragmentStart, fragmentEnd - fragmentStart),
                            referencingElement);
    searchPos = fragmentEnd;
  }
}

void AppendHrefFragmentReference(std::string_view value,
                                 std::optional<svg::SVGElement> referencingElement,
                                 std::vector<FragmentReference>* references) {
  std::size_t start = 0;
  while (start < value.size() && IsAsciiSpace(value[start])) {
    ++start;
  }

  if (start >= value.size() || value[start] != '#') {
    return;
  }

  std::size_t end = value.size();
  while (end > start + 1 && IsAsciiSpace(value[end - 1])) {
    --end;
  }

  AppendFragmentReference(references, value.substr(start + 1, end - start - 1), referencingElement);
}

[[nodiscard]] SourceByteRange ExpandCssDeclarationRange(std::string_view source,
                                                        SourceByteRange range,
                                                        std::optional<std::size_t> hardEnd) {
  const std::size_t limit = std::min(hardEnd.value_or(source.size()), source.size());
  range.start = std::min(range.start, limit);
  range.end = std::min(range.end, limit);

  std::size_t end = range.end;
  while (end < limit && source[end] != ';' && source[end] != '}') {
    ++end;
  }
  if (end < limit && source[end] == ';') {
    ++end;
  }

  range.end = end;
  while (range.end > range.start &&
         std::isspace(static_cast<unsigned char>(source[range.end - 1])) != 0) {
    --range.end;
  }
  return range;
}

[[nodiscard]] std::optional<SourceByteRange> DeclarationDocumentRange(
    const svg::SVGStylesheetDeclaration& declaration, std::string_view source) {
  if (!declaration.declarationSourceRange.has_value()) {
    return std::nullopt;
  }

  std::optional<SourceByteRange> byteRange =
      ToByteRange(*declaration.declarationSourceRange, source.size());
  if (!byteRange.has_value()) {
    return std::nullopt;
  }

  return ExpandCssDeclarationRange(source, *byteRange, std::nullopt);
}

[[nodiscard]] std::optional<SourceByteRange> SelectorDocumentRange(
    const svg::SVGStylesheetRule& rule, std::string_view source) {
  if (!rule.selectorSourceRange.has_value()) {
    return std::nullopt;
  }

  return ToByteRange(*rule.selectorSourceRange, source.size());
}

[[nodiscard]] SourceByteRange InlineDeclarationDocumentRange(
    const css::Declaration& declaration, const xml::XMLAttributeSourceLocation& styleLocation,
    std::string_view source) {
  const std::size_t valueStart = styleLocation.valueRange.start.offset.value_or(0);
  const std::size_t valueEnd = styleLocation.valueRange.end.offset.value_or(valueStart);
  const std::size_t localStart = declaration.sourceRange.start.offset.value_or(0);
  const std::size_t localEnd = declaration.sourceRange.end.offset.value_or(localStart);
  SourceByteRange range{
      std::min(valueStart + localStart, valueEnd),
      std::min(valueStart + localEnd, valueEnd),
  };
  return ExpandCssDeclarationRange(source, range, valueEnd);
}

[[nodiscard]] css::Specificity SpecificityForDeclaration(const css::Declaration& declaration,
                                                         css::Specificity baseSpecificity) {
  return declaration.important ? css::Specificity::Important() : baseSpecificity;
}

[[nodiscard]] bool IsPropertyDeclarationValid(const css::Declaration& declaration,
                                              css::Specificity specificity) {
  svg::PropertyRegistry registry;
  return !registry.parseProperty(declaration, specificity).has_value();
}

[[nodiscard]] bool IsPresentationAttributeProperty(std::string_view name) {
  if (!svg::PropertyRegistry::isPresentationAttributeName(name)) {
    return false;
  }

  const std::span<const std::string_view> propertyNames = svg::PropertyRegistry::propertyNames();
  return std::find(propertyNames.begin(), propertyNames.end(), name) != propertyNames.end();
}

[[nodiscard]] bool IsPresentationAttributeValid(std::string_view name, std::string_view value,
                                                EntityHandle handle) {
  svg::PropertyRegistry registry;
  ParseResult<bool> result = registry.parsePresentationAttribute(name, value, handle);
  return result.hasResult() && result.result();
}

void CollectElements(svg::SVGElement root, std::vector<svg::SVGElement>* elements) {
  elements->push_back(root);
  for (std::optional<svg::SVGElement> child = root.firstChild(); child.has_value();
       child = child->nextSibling()) {
    CollectElements(*child, elements);
  }
}

void AddUniqueMatchedElement(StyleSourceContribution* contribution,
                             const svg::SVGElement& element) {
  if (std::find(contribution->matchedElements.begin(), contribution->matchedElements.end(),
                element) == contribution->matchedElements.end()) {
    contribution->matchedElements.push_back(element);
  }
}

void AddMatchedElement(StyleSourceContribution* contribution, const svg::SVGElement& element) {
  AddUniqueMatchedElement(contribution, element);
  contribution->matchedElementCount = static_cast<int>(contribution->matchedElements.size());
}

void ApplyReverseReferenceFanoutMarker(StyleSourceContribution* contribution) {
  if (!contribution->showChip ||
      !IsLargeReverseReferenceFanout(static_cast<std::size_t>(contribution->matchedElementCount))) {
    return;
  }

  contribution->showOverflowMarker = true;
  contribution->overflowTooltip = std::string(kReverseReferenceOverflowTooltip);
}

void AddContribution(
    StyleSourceContribution contribution, const ContributionKey& key,
    StyleSourceAnnotations* annotations,
    std::unordered_map<ContributionKey, std::size_t, ContributionKeyHash>* contributionIndexByKey) {
  contribution.id = annotations->contributions.size() + 1u;
  const std::size_t index = annotations->contributions.size();
  annotations->contributions.push_back(std::move(contribution));
  contributionIndexByKey->emplace(key, index);
}

void AddStylesheetContributions(
    std::span<const svg::SVGStylesheetRule> styleRules, std::string_view source,
    StyleSourceAnnotations* annotations,
    std::unordered_map<ContributionKey, std::size_t, ContributionKeyHash>* contributionIndexByKey) {
  for (const svg::SVGStylesheetRule& rule : styleRules) {
    const std::optional<SourceByteRange> selectorRange = SelectorDocumentRange(rule, source);
    bool chipAssignedForRule = false;
    for (const svg::SVGStylesheetDeclaration& stylesheetDeclaration : rule.declarations) {
      const css::Declaration& declaration = stylesheetDeclaration.declaration;
      std::optional<SourceByteRange> sourceRange =
          DeclarationDocumentRange(stylesheetDeclaration, source);
      if (!sourceRange.has_value()) {
        continue;
      }

      StyleSourceContribution contribution;
      contribution.sourceRange = *sourceRange;
      contribution.chipRange = selectorRange.value_or(*sourceRange);
      contribution.propertyName = std::string(declaration.name);
      contribution.kind = StyleContributionKind::StylesheetDeclaration;
      contribution.showChip = !chipAssignedForRule;
      contribution.tooltip = std::string(declaration.name) +
                             " is overridden by a later or higher-specificity CSS declaration";
      contribution.chipTooltip = "Selector matches 0 elements";
      chipAssignedForRule = true;

      AddContribution(std::move(contribution),
                      ContributionKey{
                          .kind = StyleContributionKind::StylesheetDeclaration,
                          .stylesheetEntity = rule.stylesheetEntity,
                          .ruleIndex = rule.ruleIndex,
                          .declarationIndex = stylesheetDeclaration.declarationIndex,
                          .propertyName = std::string(declaration.name),
                      },
                      annotations, contributionIndexByKey);
    }
  }
}

void AppendAttributeFragmentReferences(const svg::SVGElement& element,
                                       std::vector<FragmentReference>* references) {
  std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return;
  }

  for (const xml::XMLQualifiedNameRef& attrName : xmlNode->attributes()) {
    std::optional<RcString> attrValue = xmlNode->getAttribute(attrName);
    if (!attrValue.has_value()) {
      continue;
    }

    const std::string_view value = *attrValue;
    AppendUrlFragmentReferences(value, element, references);
    if (attrName.name == "href") {
      AppendHrefFragmentReference(value, element, references);
    }
  }
}

void AppendStylesheetFragmentReferences(std::span<const svg::SVGStylesheetRule> styleRules,
                                        std::string_view source,
                                        std::vector<FragmentReference>* references) {
  for (const svg::SVGStylesheetRule& rule : styleRules) {
    if (!rule.ruleSourceRange.has_value()) {
      continue;
    }

    std::optional<SourceByteRange> ruleRange = ToByteRange(*rule.ruleSourceRange, source.size());
    if (!ruleRange.has_value()) {
      continue;
    }

    AppendUrlFragmentReferences(source.substr(ruleRange->start, ruleRange->end - ruleRange->start),
                                std::nullopt, references);
  }
}

void AddReferenceResourceContributions(
    std::string_view source, std::span<const svg::SVGStylesheetRule> styleRules,
    const std::vector<svg::SVGElement>& elements, StyleSourceAnnotations* annotations,
    std::unordered_map<ContributionKey, std::size_t, ContributionKeyHash>* contributionIndexByKey) {
  std::vector<FragmentReference> references;
  for (const svg::SVGElement& element : elements) {
    AppendAttributeFragmentReferences(element, &references);
  }
  AppendStylesheetFragmentReferences(styleRules, source, &references);

  std::unordered_map<std::string, std::size_t> referenceCountById;
  std::unordered_map<std::string, std::vector<svg::SVGElement>> referencingElementsById;
  for (const FragmentReference& reference : references) {
    ++referenceCountById[reference.fragmentId];
    if (reference.referencingElement.has_value()) {
      std::vector<svg::SVGElement>& referencingElements =
          referencingElementsById[reference.fragmentId];
      if (std::find(referencingElements.begin(), referencingElements.end(),
                    *reference.referencingElement) == referencingElements.end()) {
        referencingElements.push_back(*reference.referencingElement);
      }
    }
  }

  for (const svg::SVGElement& element : elements) {
    if (!IsReferenceResourceElement(element)) {
      continue;
    }

    const RcString id = element.id();
    if (id.empty()) {
      continue;
    }

    std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
    if (!xmlNode.has_value()) {
      continue;
    }

    std::optional<SourceRange> nodeLocation = xmlNode->getNodeLocation();
    if (!nodeLocation.has_value()) {
      continue;
    }

    std::optional<SourceByteRange> sourceRange = ToByteRange(*nodeLocation, source.size());
    if (!sourceRange.has_value()) {
      continue;
    }

    std::optional<SourceByteRange> chipRange;
    if (std::optional<SourceRange> openingTagRange = xmlNode->getOpeningTagLocation()) {
      chipRange = ToByteRange(*openingTagRange, source.size());
    }

    const std::string idString{std::string_view(id)};
    const auto referenceCountIter = referenceCountById.find(idString);
    const int referenceCount = referenceCountIter == referenceCountById.end()
                                   ? 0
                                   : static_cast<int>(referenceCountIter->second);
    const std::string tagName = TagNameForElement(element);

    StyleSourceContribution contribution;
    contribution.sourceRange = *sourceRange;
    contribution.chipRange = chipRange.value_or(*sourceRange);
    contribution.propertyName = tagName;
    contribution.kind = StyleContributionKind::ReferenceResourceElement;
    contribution.effective = referenceCount > 0;
    contribution.showChip = true;
    contribution.matchedElementCount = referenceCount;
    ApplyReverseReferenceFanoutMarker(&contribution);
    if (auto referencingElementsIter = referencingElementsById.find(idString);
        referencingElementsIter != referencingElementsById.end()) {
      contribution.matchedElements = referencingElementsIter->second;
    }
    contribution.tooltip = tagName + " #" + idString + " is not referenced";
    contribution.chipTooltip =
        "Referenced " + std::to_string(referenceCount) + " time" + (referenceCount == 1 ? "" : "s");

    AddContribution(std::move(contribution),
                    ContributionKey{
                        .kind = StyleContributionKind::ReferenceResourceElement,
                        .elementEntity = element.entityHandle().entity(),
                        .propertyName = tagName,
                    },
                    annotations, contributionIndexByKey);
  }
}

void AddElementLocalContributions(
    const svg::SVGElement& element, std::string_view source, StyleSourceAnnotations* annotations,
    std::unordered_map<ContributionKey, std::size_t, ContributionKeyHash>* contributionIndexByKey) {
  std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return;
  }

  for (const xml::XMLQualifiedNameRef& attrName : xmlNode->attributes()) {
    const std::string propertyName = attrName.toString();
    if (!IsPresentationAttributeProperty(propertyName)) {
      continue;
    }

    std::optional<RcString> attrValue = xmlNode->getAttribute(attrName);
    std::optional<xml::XMLAttributeSourceLocation> location =
        xmlNode->getAttributeSourceLocation(attrName);
    if (!attrValue.has_value() || !location.has_value()) {
      continue;
    }

    std::optional<SourceByteRange> sourceRange = ToByteRange(location->fullRange, source.size());
    if (!sourceRange.has_value()) {
      continue;
    }

    StyleSourceContribution contribution;
    contribution.sourceRange = *sourceRange;
    contribution.chipRange = *sourceRange;
    contribution.propertyName = propertyName;
    contribution.kind = StyleContributionKind::PresentationAttribute;
    contribution.matchedElementCount = 1;
    contribution.matchedElements.push_back(element);
    contribution.tooltip = propertyName + " is overridden by higher-specificity CSS";

    AddContribution(std::move(contribution),
                    ContributionKey{
                        .kind = StyleContributionKind::PresentationAttribute,
                        .elementEntity = element.entityHandle().entity(),
                        .propertyName = propertyName,
                    },
                    annotations, contributionIndexByKey);
  }

  std::optional<RcString> styleValue = xmlNode->getAttribute(xml::XMLQualifiedNameRef("style"));
  std::optional<xml::XMLAttributeSourceLocation> styleLocation =
      xmlNode->getAttributeSourceLocation(xml::XMLQualifiedNameRef("style"));
  if (!styleValue.has_value() || !styleLocation.has_value()) {
    return;
  }

  const std::vector<css::Declaration> declarations = css::CSS::ParseStyleAttribute(*styleValue);
  for (std::size_t declarationIndex = 0; declarationIndex < declarations.size();
       ++declarationIndex) {
    const css::Declaration& declaration = declarations[declarationIndex];
    StyleSourceContribution contribution;
    contribution.sourceRange = InlineDeclarationDocumentRange(declaration, *styleLocation, source);
    contribution.chipRange = contribution.sourceRange;
    contribution.propertyName = std::string(declaration.name);
    contribution.kind = StyleContributionKind::InlineStyleDeclaration;
    contribution.matchedElementCount = 1;
    contribution.matchedElements.push_back(element);
    contribution.tooltip = std::string(declaration.name) + " is overridden by !important CSS";

    AddContribution(std::move(contribution),
                    ContributionKey{
                        .kind = StyleContributionKind::InlineStyleDeclaration,
                        .elementEntity = element.entityHandle().entity(),
                        .declarationIndex = declarationIndex,
                        .propertyName = std::string(declaration.name),
                    },
                    annotations, contributionIndexByKey);
  }
}

[[nodiscard]] const svg::SVGStylesheetRule* FindStylesheetRule(
    std::span<const svg::SVGStylesheetRule> styleRules, Entity stylesheetEntity,
    std::size_t ruleIndex) {
  auto iter = std::find_if(
      styleRules.begin(), styleRules.end(), [stylesheetEntity, ruleIndex](const auto& rule) {
        return rule.stylesheetEntity == stylesheetEntity && rule.ruleIndex == ruleIndex;
      });
  return iter == styleRules.end() ? nullptr : &*iter;
}

void AddStylesheetMatches(const std::vector<svg::SVGElement>& elements,
                          std::span<const svg::SVGStylesheetRule> styleRules,
                          StyleSourceAnnotations* annotations,
                          const std::unordered_map<ContributionKey, std::size_t,
                                                   ContributionKeyHash>& contributionIndexByKey) {
  for (const svg::SVGElement& element : elements) {
    const std::vector<svg::SVGMatchedStyleRule> matches = svg::CollectMatchedStyleRules(element);
    for (const svg::SVGMatchedStyleRule& match : matches) {
      if (match.isUserAgentStylesheet) {
        continue;
      }

      const svg::SVGStylesheetRule* rule =
          FindStylesheetRule(styleRules, match.stylesheetEntity, match.ruleIndex);
      if (rule == nullptr) {
        continue;
      }

      for (const svg::SVGStylesheetDeclaration& stylesheetDeclaration : rule->declarations) {
        const css::Declaration& declaration = stylesheetDeclaration.declaration;
        auto contributionIter = contributionIndexByKey.find(ContributionKey{
            .kind = StyleContributionKind::StylesheetDeclaration,
            .stylesheetEntity = match.stylesheetEntity,
            .ruleIndex = match.ruleIndex,
            .declarationIndex = stylesheetDeclaration.declarationIndex,
            .propertyName = std::string(declaration.name),
        });
        if (contributionIter == contributionIndexByKey.end()) {
          continue;
        }

        AddMatchedElement(&annotations->contributions[contributionIter->second], element);
      }
    }
  }

  for (StyleSourceContribution& contribution : annotations->contributions) {
    if (contribution.kind == StyleContributionKind::StylesheetDeclaration) {
      contribution.chipTooltip = "Selector matches " +
                                 std::to_string(contribution.matchedElementCount) + " element" +
                                 (contribution.matchedElementCount == 1 ? "" : "s");
      ApplyReverseReferenceFanoutMarker(&contribution);
    }
  }
}

void AddLocalCascadeEntries(const svg::SVGElement& element,
                            const std::unordered_map<ContributionKey, std::size_t,
                                                     ContributionKeyHash>& contributionIndexByKey,
                            std::vector<CascadeEntry>* entries) {
  std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return;
  }

  for (const xml::XMLQualifiedNameRef& attrName : xmlNode->attributes()) {
    const std::string propertyName = attrName.toString();
    if (!IsPresentationAttributeProperty(propertyName)) {
      continue;
    }

    std::optional<RcString> attrValue = xmlNode->getAttribute(attrName);
    auto contributionIter = contributionIndexByKey.find(ContributionKey{
        .kind = StyleContributionKind::PresentationAttribute,
        .elementEntity = element.entityHandle().entity(),
        .propertyName = propertyName,
    });
    if (!attrValue.has_value() || contributionIter == contributionIndexByKey.end()) {
      continue;
    }

    entries->push_back(CascadeEntry{
        .contributionIndex = contributionIter->second,
        .specificity = kPresentationSpecificity,
        .presentationValue = std::string(*attrValue),
    });
  }

  std::optional<RcString> styleValue = xmlNode->getAttribute(xml::XMLQualifiedNameRef("style"));
  if (!styleValue.has_value()) {
    return;
  }

  const std::vector<css::Declaration> declarations = css::CSS::ParseStyleAttribute(*styleValue);
  for (std::size_t declarationIndex = 0; declarationIndex < declarations.size();
       ++declarationIndex) {
    const css::Declaration& declaration = declarations[declarationIndex];
    auto contributionIter = contributionIndexByKey.find(ContributionKey{
        .kind = StyleContributionKind::InlineStyleDeclaration,
        .elementEntity = element.entityHandle().entity(),
        .declarationIndex = declarationIndex,
        .propertyName = std::string(declaration.name),
    });
    if (contributionIter == contributionIndexByKey.end()) {
      continue;
    }

    entries->push_back(CascadeEntry{
        .contributionIndex = contributionIter->second,
        .specificity = SpecificityForDeclaration(declaration, css::Specificity::StyleAttribute()),
        .declaration = declaration,
    });
  }
}

void AddStylesheetCascadeEntries(
    const svg::SVGElement& element, std::span<const svg::SVGStylesheetRule> styleRules,
    const std::unordered_map<ContributionKey, std::size_t, ContributionKeyHash>&
        contributionIndexByKey,
    std::vector<CascadeEntry>* entries) {
  const std::vector<svg::SVGMatchedStyleRule> matches = svg::CollectMatchedStyleRules(element);
  for (const svg::SVGMatchedStyleRule& match : matches) {
    if (match.isUserAgentStylesheet) {
      continue;
    }

    const svg::SVGStylesheetRule* rule =
        FindStylesheetRule(styleRules, match.stylesheetEntity, match.ruleIndex);
    if (rule == nullptr) {
      continue;
    }

    for (const svg::SVGStylesheetDeclaration& stylesheetDeclaration : rule->declarations) {
      const css::Declaration& declaration = stylesheetDeclaration.declaration;
      auto contributionIter = contributionIndexByKey.find(ContributionKey{
          .kind = StyleContributionKind::StylesheetDeclaration,
          .stylesheetEntity = match.stylesheetEntity,
          .ruleIndex = match.ruleIndex,
          .declarationIndex = stylesheetDeclaration.declarationIndex,
          .propertyName = std::string(declaration.name),
      });
      if (contributionIter == contributionIndexByKey.end()) {
        continue;
      }

      entries->push_back(CascadeEntry{
          .contributionIndex = contributionIter->second,
          .specificity = SpecificityForDeclaration(declaration, match.specificity),
          .declaration = declaration,
      });
    }
  }
}

void MarkEffectiveContributionsForElement(
    const svg::SVGElement& element, std::span<const svg::SVGStylesheetRule> styleRules,
    StyleSourceAnnotations* annotations,
    const std::unordered_map<ContributionKey, std::size_t, ContributionKeyHash>&
        contributionIndexByKey) {
  std::vector<CascadeEntry> entries;
  AddLocalCascadeEntries(element, contributionIndexByKey, &entries);
  AddStylesheetCascadeEntries(element, styleRules, contributionIndexByKey, &entries);

  std::unordered_map<std::string, Winner> winnersByProperty;
  std::size_t order = 0;
  for (const CascadeEntry& entry : entries) {
    ++order;
    const StyleSourceContribution& contribution =
        annotations->contributions[entry.contributionIndex];

    bool valid = false;
    if (contribution.kind == StyleContributionKind::PresentationAttribute) {
      valid = IsPresentationAttributeValid(contribution.propertyName, entry.presentationValue,
                                           element.entityHandle());
    } else if (entry.declaration.has_value()) {
      valid = IsPropertyDeclarationValid(*entry.declaration, entry.specificity);
    }

    if (!valid) {
      continue;
    }

    auto winnerIter = winnersByProperty.find(contribution.propertyName);
    if (winnerIter == winnersByProperty.end() ||
        entry.specificity > winnerIter->second.specificity ||
        (entry.specificity == winnerIter->second.specificity && order > winnerIter->second.order)) {
      winnersByProperty[contribution.propertyName] = Winner{
          .specificity = entry.specificity,
          .order = order,
          .contributionIndex = entry.contributionIndex,
      };
    }
  }

  for (const auto& [propertyName, winner] : winnersByProperty) {
    annotations->contributions[winner.contributionIndex].effective = true;
  }
}

}  // namespace

StyleSourceAnnotations ComputeStyleSourceAnnotations(svg::SVGDocument& document,
                                                     std::string_view source) {
  if (source.empty()) {
    return {};
  }

  return document.withWriteAccess([&document, source](svg::DocumentWriteAccess&) {
    StyleSourceAnnotations annotations;
    std::unordered_map<ContributionKey, std::size_t, ContributionKeyHash> contributionIndexByKey;
    const std::vector<svg::SVGStylesheetRule> styleRules = svg::CollectStylesheetRules(document);
    AddStylesheetContributions(styleRules, source, &annotations, &contributionIndexByKey);

    std::vector<svg::SVGElement> elements;
    CollectElements(document.svgElement(), &elements);
    AddReferenceResourceContributions(source, styleRules, elements, &annotations,
                                      &contributionIndexByKey);
    for (const svg::SVGElement& element : elements) {
      AddElementLocalContributions(element, source, &annotations, &contributionIndexByKey);
    }

    AddStylesheetMatches(elements, styleRules, &annotations, contributionIndexByKey);
    for (const svg::SVGElement& element : elements) {
      MarkEffectiveContributionsForElement(element, styleRules, &annotations,
                                           contributionIndexByKey);
    }

    return annotations;
  });
}

}  // namespace donner::editor
