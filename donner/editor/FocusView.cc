#include "donner/editor/FocusView.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/editor/ReferenceFanout.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/SVGStyleQuery.h"

namespace donner::editor {
namespace {

struct ByteRange {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct FragmentReference {
  std::string fragmentId;
  std::optional<std::size_t> sourceOffset;
};

struct FocusCssRule {
  ByteRange ruleRange;
  std::optional<ByteRange> stylesheetNodeRange;
};

struct ElementReferenceLink {
  std::size_t fromOffset = 0;
  svg::SVGElement referenced;
  bool reverseReference = false;
};

struct FocusElementCollection {
  std::vector<svg::SVGElement> elements;
  std::vector<ElementReferenceLink> links;
  std::vector<FocusCssRule> cssRules;
  std::vector<FocusReferenceLink> cssLinks;
};

struct CssRuleKey {
  Entity stylesheetEntity = entt::null;
  std::size_t ruleIndex = 0;

  bool operator==(const CssRuleKey& other) const = default;
};

std::optional<ByteRange> ResolveSourceRange(std::string_view source, const SourceRange& range) {
  const FileOffset start = range.start.resolveOffset(source);
  const FileOffset end = range.end.resolveOffset(source);
  if (!start.offset.has_value() || !end.offset.has_value()) {
    return std::nullopt;
  }

  const std::size_t clampedStart = std::min(*start.offset, source.size());
  const std::size_t clampedEnd = std::min(*end.offset, source.size());
  if (clampedEnd < clampedStart) {
    return std::nullopt;
  }

  return ByteRange{.start = clampedStart, .end = clampedEnd};
}

std::vector<std::size_t> BuildLineStarts(std::string_view source) {
  std::vector<std::size_t> result;
  result.push_back(0);
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '\n') {
      result.push_back(i + 1);
    }
  }
  return result;
}

int LineForOffset(const std::vector<std::size_t>& lineStarts, std::size_t offset) {
  const auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
  return static_cast<int>(std::distance(lineStarts.begin(), it) - 1);
}

LineRange RangeToLines(const std::vector<std::size_t>& lineStarts, ByteRange range) {
  const int startLine = LineForOffset(lineStarts, range.start);
  const std::size_t inclusiveEnd = range.end > range.start ? range.end - 1 : range.start;
  const int endLine = LineForOffset(lineStarts, inclusiveEnd) + 1;
  return LineRange{.startLine = startLine, .endLine = endLine};
}

SourcePoint PointForOffset(const std::vector<std::size_t>& lineStarts, std::size_t offset) {
  const int line = LineForOffset(lineStarts, offset);
  return SourcePoint{.line = line, .column = static_cast<int>(offset - lineStarts[line])};
}

std::optional<ByteRange> NodeRange(std::string_view source, const xml::XMLNode& xmlNode) {
  std::optional<SourceRange> sourceRange = xmlNode.getNodeLocation();
  if (!sourceRange.has_value()) {
    return std::nullopt;
  }

  return ResolveSourceRange(source, *sourceRange);
}

std::optional<ByteRange> NodeRange(std::string_view source, const svg::SVGElement& element) {
  std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return std::nullopt;
  }

  return NodeRange(source, *xmlNode);
}

std::optional<ByteRange> OpeningTagRange(std::string_view source, const svg::SVGElement& element) {
  std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return std::nullopt;
  }

  std::optional<SourceRange> sourceRange = xmlNode->getOpeningTagLocation();
  if (!sourceRange.has_value()) {
    return std::nullopt;
  }

  return ResolveSourceRange(source, *sourceRange);
}

std::optional<ByteRange> NodeRangeForEntity(std::string_view source, Registry& registry,
                                            Entity entity) {
  std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(EntityHandle(registry, entity));
  if (!xmlNode.has_value()) {
    return std::nullopt;
  }

  return NodeRange(source, *xmlNode);
}

bool HasTreeComponent(const svg::SVGElement& element) {
  return xml::XMLNode::TryCast(element.entityHandle()).has_value();
}

std::optional<svg::ElementType> SafeElementType(const svg::SVGElement& element) {
  return element.tryType();
}

std::optional<xml::XMLQualifiedNameRef> SafeTagName(const svg::SVGElement& element) {
  return element.tryTagName();
}

RcString SafeId(const svg::SVGElement& element) {
  if (!SafeElementType(element).has_value()) {
    return "";
  }

  return element.id();
}

bool HasLiveSvgTreeComponents(const svg::SVGElement& element) {
  return HasTreeComponent(element) && SafeElementType(element).has_value();
}

std::optional<svg::SVGElement> SafeFirstChild(const svg::SVGElement& element) {
  if (!HasLiveSvgTreeComponents(element)) {
    return std::nullopt;
  }

  return element.firstChild();
}

std::optional<svg::SVGElement> SafeNextSibling(const svg::SVGElement& element) {
  if (!HasTreeComponent(element)) {
    return std::nullopt;
  }

  return element.nextSibling();
}

std::optional<svg::SVGElement> SafeParentElement(const svg::SVGElement& element) {
  if (!HasTreeComponent(element)) {
    return std::nullopt;
  }

  std::optional<svg::SVGElement> parent = element.parentElement();
  if (parent.has_value() && !HasLiveSvgTreeComponents(*parent)) {
    return std::nullopt;
  }

  return parent;
}

bool IsAsciiSpace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

void AppendFragmentReference(std::vector<FragmentReference>* references,
                             std::string_view fragmentId, std::optional<std::size_t> sourceOffset) {
  if (fragmentId.empty()) {
    return;
  }

  references->push_back(FragmentReference{
      .fragmentId = std::string(fragmentId),
      .sourceOffset = sourceOffset,
  });
}

void AppendUrlFragmentReferences(std::string_view value, std::optional<std::size_t> valueOffset,
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

    std::optional<std::size_t> sourceOffset;
    if (valueOffset.has_value()) {
      sourceOffset = *valueOffset + index;
    }

    AppendFragmentReference(references, value.substr(fragmentStart, fragmentEnd - fragmentStart),
                            sourceOffset);
    searchPos = fragmentEnd;
  }
}

void AppendHrefFragmentReference(std::string_view value, std::optional<std::size_t> valueOffset,
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

  std::optional<std::size_t> sourceOffset;
  if (valueOffset.has_value()) {
    sourceOffset = *valueOffset + start;
  }

  AppendFragmentReference(references, value.substr(start + 1, end - start - 1), sourceOffset);
}

std::vector<FragmentReference> ReferencedFragments(std::string_view source,
                                                   const svg::SVGElement& element) {
  std::vector<FragmentReference> result;
  std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return result;
  }

  for (const xml::XMLQualifiedNameRef& attrName : xmlNode->attributes()) {
    std::optional<RcString> attrValue = xmlNode->getAttribute(attrName);
    if (!attrValue.has_value()) {
      continue;
    }

    std::optional<std::size_t> valueOffset;
    if (std::optional<xml::XMLAttributeSourceLocation> location =
            xmlNode->getAttributeSourceLocation(attrName)) {
      if (std::optional<ByteRange> valueRange = ResolveSourceRange(source, location->valueRange)) {
        valueOffset = valueRange->start;
      }
    }

    const std::string_view value = *attrValue;
    AppendUrlFragmentReferences(value, valueOffset, &result);
    if (attrName.name == "href") {
      AppendHrefFragmentReference(value, valueOffset, &result);
    }
  }

  return result;
}

std::optional<std::size_t> AttributeValueStartOffset(std::string_view source,
                                                     const xml::XMLNode& xmlNode,
                                                     std::string_view attrName) {
  if (std::optional<xml::XMLAttributeSourceLocation> location =
          xmlNode.getAttributeSourceLocation(xml::XMLQualifiedNameRef(attrName))) {
    if (std::optional<ByteRange> valueRange = ResolveSourceRange(source, location->valueRange)) {
      return valueRange->start;
    }
  }

  return std::nullopt;
}

std::optional<std::size_t> SelectorLinkSourceOffset(std::string_view source,
                                                    const svg::SVGElement& selected) {
  std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(selected.entityHandle());
  if (!xmlNode.has_value()) {
    return std::nullopt;
  }

  for (std::string_view attrName : {std::string_view("class"), std::string_view("id")}) {
    if (std::optional<std::size_t> offset = AttributeValueStartOffset(source, *xmlNode, attrName)) {
      return offset;
    }
  }

  if (std::optional<ByteRange> nodeRange = NodeRange(source, selected)) {
    return nodeRange->start;
  }

  return std::nullopt;
}

void AppendCssFragmentReferences(std::string_view source, ByteRange range,
                                 std::vector<FragmentReference>* references) {
  if (range.start > source.size() || range.end > source.size() || range.end < range.start) {
    return;
  }

  AppendUrlFragmentReferences(source.substr(range.start, range.end - range.start), range.start,
                              references);
}

void AppendReferencedFragmentsInSubtree(std::string_view source, const svg::SVGElement& root,
                                        std::vector<FragmentReference>* references) {
  for (const FragmentReference& reference : ReferencedFragments(source, root)) {
    references->push_back(reference);
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendReferencedFragmentsInSubtree(source, current, references);
  }
}

std::optional<svg::SVGElement> FindElementById(const svg::SVGElement& root, std::string_view id) {
  if (!HasLiveSvgTreeComponents(root)) {
    return std::nullopt;
  }

  if (SafeId(root) == id) {
    return root;
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    if (std::optional<svg::SVGElement> result = FindElementById(current, id)) {
      return result;
    }
  }

  return std::nullopt;
}

bool MatchesStyleSourceRule(const svg::SVGMatchedStyleRule& match,
                            const svg::SVGStyleRuleAtSourceOffset& sourceRule) {
  if (match.stylesheetEntity != sourceRule.stylesheetEntity ||
      match.ruleIndex != sourceRule.ruleIndex) {
    return false;
  }

  return !sourceRule.selectorEntryIndex.has_value() ||
         match.selectorEntryIndex == *sourceRule.selectorEntryIndex;
}

bool IsNonRenderedStyleContainer(svg::ElementType type) {
  switch (type) {
    case svg::ElementType::Defs:
    case svg::ElementType::ClipPath:
    case svg::ElementType::Mask:
    case svg::ElementType::Filter:
    case svg::ElementType::Pattern:
    case svg::ElementType::LinearGradient:
    case svg::ElementType::RadialGradient:
    case svg::ElementType::Symbol:
    case svg::ElementType::Marker:
    case svg::ElementType::Style: return true;
    default: return false;
  }
}

bool HasNonRenderedStyleAncestor(const svg::SVGElement& element) {
  for (std::optional<svg::SVGElement> ancestor = SafeParentElement(element); ancestor.has_value();
       ancestor = SafeParentElement(*ancestor)) {
    const std::optional<svg::ElementType> type = SafeElementType(*ancestor);
    if (type.has_value() && IsNonRenderedStyleContainer(*type)) {
      return true;
    }
  }

  return false;
}

bool IsRenderedStyleTarget(const svg::SVGElement& element) {
  const std::optional<svg::ElementType> type = SafeElementType(element);
  if (!type.has_value() || !HasTreeComponent(element)) {
    return false;
  }

  if (IsNonRenderedStyleContainer(*type) || HasNonRenderedStyleAncestor(element)) {
    return false;
  }

  switch (*type) {
    case svg::ElementType::Circle:
    case svg::ElementType::Ellipse:
    case svg::ElementType::G:
    case svg::ElementType::Image:
    case svg::ElementType::Line:
    case svg::ElementType::Path:
    case svg::ElementType::Polygon:
    case svg::ElementType::Polyline:
    case svg::ElementType::Rect:
    case svg::ElementType::SVG:
    case svg::ElementType::Text:
    case svg::ElementType::TextPath:
    case svg::ElementType::TSpan:
    case svg::ElementType::Use: return true;
    default: return false;
  }
}

void AppendImpactedElementsForStyleRule(const svg::SVGElement& root,
                                        const svg::SVGStyleRuleAtSourceOffset& sourceRule,
                                        std::vector<svg::SVGElement>* impactedElements,
                                        std::size_t maxElements) {
  if (impactedElements->size() > maxElements) {
    return;
  }

  if (IsRenderedStyleTarget(root)) {
    for (const svg::SVGMatchedStyleRule& match : svg::CollectMatchedStyleRules(root)) {
      if (MatchesStyleSourceRule(match, sourceRule)) {
        impactedElements->push_back(root);
        if (impactedElements->size() > maxElements) {
          return;
        }
        break;
      }
    }
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendImpactedElementsForStyleRule(current, sourceRule, impactedElements, maxElements);
    if (impactedElements->size() > maxElements) {
      return;
    }
  }
}

bool AddFocusElement(const svg::SVGElement& element, FocusElementCollection* result,
                     std::vector<Entity>* visited) {
  if (!HasLiveSvgTreeComponents(element)) {
    return false;
  }

  const Entity entity = element.entityHandle().entity();
  if (std::ranges::find(*visited, entity) != visited->end()) {
    return false;
  }

  visited->push_back(entity);
  result->elements.push_back(element);
  return true;
}

void AddElementReferenceLink(std::size_t fromOffset, const svg::SVGElement& referenced,
                             bool reverseReference, FocusElementCollection* result) {
  if (!HasLiveSvgTreeComponents(referenced)) {
    return;
  }

  const Entity referencedEntity = referenced.entityHandle().entity();
  const auto it = std::ranges::find_if(result->links, [&](const ElementReferenceLink& link) {
    return link.fromOffset == fromOffset &&
           link.referenced.entityHandle().entity() == referencedEntity &&
           link.reverseReference == reverseReference;
  });
  if (it == result->links.end()) {
    result->links.push_back(ElementReferenceLink{
        .fromOffset = fromOffset,
        .referenced = referenced,
        .reverseReference = reverseReference,
    });
  }
}

void AddSourceReferenceLink(FocusReferenceLink link, std::vector<FocusReferenceLink>* links) {
  if (std::ranges::find(*links, link) == links->end()) {
    links->push_back(link);
  }
}

std::optional<std::size_t> ReferenceLinkTargetOffset(std::string_view source,
                                                     const svg::SVGElement& referenced);

void AppendElementReferenceLinks(std::string_view source,
                                 const std::vector<std::size_t>& lineStarts,
                                 const std::vector<ElementReferenceLink>& links,
                                 FocusPartition* partition) {
  const std::size_t reverseLinkCount = std::ranges::count_if(
      links, [](const ElementReferenceLink& link) { return link.reverseReference; });
  const bool drawReverseLinks = !IsLargeReverseReferenceFanout(reverseLinkCount);

  for (const ElementReferenceLink& link : links) {
    if (link.reverseReference && !drawReverseLinks) {
      continue;
    }

    std::optional<std::size_t> targetOffset = ReferenceLinkTargetOffset(source, link.referenced);
    if (!targetOffset.has_value() || link.fromOffset > source.size()) {
      continue;
    }

    partition->referenceLinks.push_back(FocusReferenceLink{
        .from = PointForOffset(lineStarts, link.fromOffset),
        .to = PointForOffset(lineStarts, *targetOffset),
    });
  }
}

bool AddUniqueElement(const svg::SVGElement& element, std::vector<svg::SVGElement>* elements) {
  if (!HasLiveSvgTreeComponents(element)) {
    return false;
  }

  const Entity entity = element.entityHandle().entity();
  const auto it = std::ranges::find_if(*elements, [entity](const svg::SVGElement& existing) {
    return existing.entityHandle().entity() == entity;
  });
  if (it != elements->end()) {
    return false;
  }

  elements->push_back(element);
  return true;
}

bool HasAncestorNamed(const svg::SVGElement& element, std::string_view tagName) {
  for (std::optional<svg::SVGElement> ancestor = SafeParentElement(element); ancestor.has_value();
       ancestor = SafeParentElement(*ancestor)) {
    const std::optional<xml::XMLQualifiedNameRef> ancestorTagName = SafeTagName(*ancestor);
    if (ancestorTagName.has_value() && std::string_view(ancestorTagName->name) == tagName) {
      return true;
    }
  }

  return false;
}

bool IsReferenceResourceElement(const svg::SVGElement& element) {
  const std::optional<xml::XMLQualifiedNameRef> tagNameRef = SafeTagName(element);
  if (!SafeElementType(element).has_value() || !tagNameRef.has_value()) {
    return false;
  }

  const std::string_view tagName = tagNameRef->name;
  return tagName == "clipPath" || tagName == "filter" || tagName == "linearGradient" ||
         tagName == "marker" || tagName == "mask" || tagName == "pattern" ||
         tagName == "radialGradient" || tagName == "symbol" || HasAncestorNamed(element, "defs");
}

std::optional<std::size_t> ReferenceLinkTargetOffset(std::string_view source,
                                                     const svg::SVGElement& referenced) {
  if (IsReferenceResourceElement(referenced)) {
    if (std::optional<ByteRange> openingTagRange = OpeningTagRange(source, referenced)) {
      return openingTagRange->end;
    }
  }

  if (std::optional<ByteRange> targetRange = NodeRange(source, referenced)) {
    return targetRange->start;
  }

  return std::nullopt;
}

void MarkReverseExpandable(const svg::SVGElement& element,
                           std::vector<Entity>* reverseExpandableEntities) {
  if (!HasLiveSvgTreeComponents(element)) {
    return;
  }

  const Entity entity = element.entityHandle().entity();
  if (std::ranges::find(*reverseExpandableEntities, entity) == reverseExpandableEntities->end()) {
    reverseExpandableEntities->push_back(entity);
  }
}

void MarkReverseExpandableIfResource(const svg::SVGElement& element,
                                     std::vector<Entity>* reverseExpandableEntities) {
  if (IsReferenceResourceElement(element)) {
    MarkReverseExpandable(element, reverseExpandableEntities);
  }
}

bool CanReverseExpand(const svg::SVGElement& element,
                      const std::vector<Entity>& reverseExpandableEntities) {
  const Entity entity = element.entityHandle().entity();
  return std::ranges::find(reverseExpandableEntities, entity) != reverseExpandableEntities.end();
}

std::optional<FocusCssRule> FocusCssRuleFromMatchedRule(
    std::string_view source, Registry& registry, const svg::SVGMatchedStyleRule& matchedRule) {
  if (!matchedRule.ruleSourceRange.has_value()) {
    return std::nullopt;
  }

  std::optional<ByteRange> ruleRange = ResolveSourceRange(source, *matchedRule.ruleSourceRange);
  if (!ruleRange.has_value()) {
    return std::nullopt;
  }

  return FocusCssRule{
      .ruleRange = *ruleRange,
      .stylesheetNodeRange = NodeRangeForEntity(source, registry, matchedRule.stylesheetEntity),
  };
}

void AddMatchedCssRule(std::string_view source, Registry& registry,
                       const svg::SVGMatchedStyleRule& matchedRule, FocusElementCollection* result,
                       std::vector<CssRuleKey>* visitedCssRules) {
  const CssRuleKey key{
      .stylesheetEntity = matchedRule.stylesheetEntity,
      .ruleIndex = matchedRule.ruleIndex,
  };
  if (std::ranges::find(*visitedCssRules, key) != visitedCssRules->end()) {
    return;
  }

  std::optional<FocusCssRule> cssRule = FocusCssRuleFromMatchedRule(source, registry, matchedRule);
  if (!cssRule.has_value()) {
    return;
  }

  visitedCssRules->push_back(key);
  result->cssRules.push_back(*cssRule);
}

void AppendMatchedCssRulesInSubtree(std::string_view source,
                                    const std::vector<std::size_t>& lineStarts,
                                    const svg::SVGElement& root, FocusElementCollection* result,
                                    std::vector<CssRuleKey>* visitedCssRules,
                                    std::vector<FragmentReference>* references) {
  if (!HasLiveSvgTreeComponents(root)) {
    return;
  }

  Registry& registry = *root.entityHandle().registry();
  const std::optional<std::size_t> selectorLinkSource = SelectorLinkSourceOffset(source, root);
  if (IsRenderedStyleTarget(root)) {
    for (const svg::SVGMatchedStyleRule& matchedRule : svg::CollectMatchedStyleRules(root)) {
      if (!matchedRule.ruleSourceRange.has_value() ||
          !matchedRule.selectorSourceRange.has_value()) {
        continue;
      }

      std::optional<ByteRange> ruleRange = ResolveSourceRange(source, *matchedRule.ruleSourceRange);
      std::optional<ByteRange> selectorRange =
          ResolveSourceRange(source, *matchedRule.selectorSourceRange);
      if (!ruleRange.has_value() || !selectorRange.has_value()) {
        continue;
      }

      AddMatchedCssRule(source, registry, matchedRule, result, visitedCssRules);

      if (selectorLinkSource.has_value() && *selectorLinkSource <= source.size()) {
        AddSourceReferenceLink(
            FocusReferenceLink{
                .from = PointForOffset(lineStarts, *selectorLinkSource),
                .to = PointForOffset(lineStarts, selectorRange->start),
            },
            &result->cssLinks);
      }

      AppendCssFragmentReferences(source, *ruleRange, references);
    }
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendMatchedCssRulesInSubtree(source, lineStarts, current, result, visitedCssRules,
                                   references);
  }
}

void AppendReverseAttributeReferences(std::string_view source, const svg::SVGElement& root,
                                      std::string_view targetId,
                                      const svg::SVGElement& targetElement,
                                      FocusElementCollection* result, std::vector<Entity>* visited,
                                      std::vector<Entity>* reverseExpandableEntities) {
  if (!HasLiveSvgTreeComponents(root)) {
    return;
  }

  for (const FragmentReference& reference : ReferencedFragments(source, root)) {
    if (reference.fragmentId != targetId) {
      continue;
    }

    if (reference.sourceOffset.has_value()) {
      AddElementReferenceLink(*reference.sourceOffset, targetElement, /*reverseReference=*/true,
                              result);
    }
    AddFocusElement(root, result, visited);
    MarkReverseExpandableIfResource(root, reverseExpandableEntities);
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendReverseAttributeReferences(source, current, targetId, targetElement, result, visited,
                                     reverseExpandableEntities);
  }
}

void AppendReverseCssReferences(std::string_view source, const std::vector<std::size_t>& lineStarts,
                                const svg::SVGElement& root, std::string_view targetId,
                                const svg::SVGElement& targetElement,
                                FocusElementCollection* result, std::vector<Entity>* visited,
                                std::vector<CssRuleKey>* visitedCssRules,
                                std::vector<Entity>* reverseExpandableEntities) {
  if (!HasLiveSvgTreeComponents(root)) {
    return;
  }

  Registry& registry = *root.entityHandle().registry();
  if (IsRenderedStyleTarget(root)) {
    for (const svg::SVGMatchedStyleRule& matchedRule : svg::CollectMatchedStyleRules(root)) {
      if (!matchedRule.ruleSourceRange.has_value()) {
        continue;
      }

      std::optional<ByteRange> ruleRange = ResolveSourceRange(source, *matchedRule.ruleSourceRange);
      if (!ruleRange.has_value()) {
        continue;
      }

      std::vector<FragmentReference> references;
      AppendCssFragmentReferences(source, *ruleRange, &references);

      bool referencesTarget = false;
      for (const FragmentReference& reference : references) {
        if (reference.fragmentId != targetId) {
          continue;
        }

        referencesTarget = true;
        if (reference.sourceOffset.has_value()) {
          AddElementReferenceLink(*reference.sourceOffset, targetElement,
                                  /*reverseReference=*/true, result);
        }
      }

      if (!referencesTarget) {
        continue;
      }

      AddMatchedCssRule(source, registry, matchedRule, result, visitedCssRules);
      if (matchedRule.selectorSourceRange.has_value()) {
        std::optional<ByteRange> selectorRange =
            ResolveSourceRange(source, *matchedRule.selectorSourceRange);
        std::optional<std::size_t> selectorLinkSource = SelectorLinkSourceOffset(source, root);
        if (selectorRange.has_value() && selectorLinkSource.has_value() &&
            *selectorLinkSource <= source.size()) {
          AddSourceReferenceLink(
              FocusReferenceLink{
                  .from = PointForOffset(lineStarts, *selectorLinkSource),
                  .to = PointForOffset(lineStarts, selectorRange->start),
              },
              &result->cssLinks);
        }
      }
      AddFocusElement(root, result, visited);
      MarkReverseExpandableIfResource(root, reverseExpandableEntities);
    }
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendReverseCssReferences(source, lineStarts, current, targetId, targetElement, result,
                               visited, visitedCssRules, reverseExpandableEntities);
  }
}

void AppendReverseAttributeReferenceElements(std::string_view source, const svg::SVGElement& root,
                                             std::string_view targetId,
                                             std::vector<svg::SVGElement>* elements,
                                             std::size_t maxElements) {
  if (!HasLiveSvgTreeComponents(root) || elements->size() > maxElements) {
    return;
  }

  for (const FragmentReference& reference : ReferencedFragments(source, root)) {
    if (reference.fragmentId == targetId) {
      AddUniqueElement(root, elements);
      if (elements->size() > maxElements) {
        return;
      }
      break;
    }
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendReverseAttributeReferenceElements(source, current, targetId, elements, maxElements);
    if (elements->size() > maxElements) {
      return;
    }
  }
}

void AppendReverseCssReferenceElements(std::string_view source, const svg::SVGElement& root,
                                       std::string_view targetId,
                                       std::vector<svg::SVGElement>* elements,
                                       std::size_t maxElements) {
  if (!HasLiveSvgTreeComponents(root) || elements->size() > maxElements) {
    return;
  }

  if (IsRenderedStyleTarget(root)) {
    for (const svg::SVGMatchedStyleRule& matchedRule : svg::CollectMatchedStyleRules(root)) {
      if (!matchedRule.ruleSourceRange.has_value()) {
        continue;
      }

      std::optional<ByteRange> ruleRange = ResolveSourceRange(source, *matchedRule.ruleSourceRange);
      if (!ruleRange.has_value()) {
        continue;
      }

      std::vector<FragmentReference> references;
      AppendCssFragmentReferences(source, *ruleRange, &references);
      if (std::ranges::any_of(references, [targetId](const FragmentReference& reference) {
            return reference.fragmentId == targetId;
          })) {
        AddUniqueElement(root, elements);
        if (elements->size() > maxElements) {
          return;
        }
        break;
      }
    }
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendReverseCssReferenceElements(source, current, targetId, elements, maxElements);
    if (elements->size() > maxElements) {
      return;
    }
  }
}

FocusElementCollection CollectFocusElements(const svg::SVGDocument& document,
                                            std::vector<svg::SVGElement> initialElements,
                                            const std::vector<FragmentReference>& initialReferences,
                                            const std::vector<std::size_t>& lineStarts) {
  FocusElementCollection result;
  std::vector<Entity> visited;
  std::vector<Entity> reverseExpandableEntities;
  std::vector<std::string> reverseProcessedIds;
  std::vector<CssRuleKey> visitedCssRules;
  for (const svg::SVGElement& element : initialElements) {
    AddFocusElement(element, &result, &visited);
    MarkReverseExpandableIfResource(element, &reverseExpandableEntities);
  }

  const svg::SVGElement root = document.svgElement();
  for (std::size_t i = 0; i < result.elements.size(); ++i) {
    const svg::SVGElement current = result.elements[i];
    std::vector<FragmentReference> references;
    if (i == 0) {
      references.insert(references.end(), initialReferences.begin(), initialReferences.end());
    }

    AppendMatchedCssRulesInSubtree(document.source(), lineStarts, current, &result,
                                   &visitedCssRules, &references);
    AppendReferencedFragmentsInSubtree(document.source(), current, &references);

    for (const FragmentReference& reference : references) {
      std::optional<svg::SVGElement> referenced = FindElementById(root, reference.fragmentId);
      if (!referenced.has_value()) {
        continue;
      }

      if (reference.sourceOffset.has_value()) {
        const bool reverseReference =
            std::ranges::find(reverseProcessedIds, reference.fragmentId) !=
            reverseProcessedIds.end();
        AddElementReferenceLink(*reference.sourceOffset, *referenced, reverseReference, &result);
      }

      AddFocusElement(*referenced, &result, &visited);
    }

    const RcString currentId = SafeId(current);
    if (currentId.empty() || !CanReverseExpand(current, reverseExpandableEntities) ||
        std::ranges::find(reverseProcessedIds, std::string_view(currentId)) !=
            reverseProcessedIds.end()) {
      continue;
    }

    std::vector<svg::SVGElement> reverseReferenceElements;
    AppendReverseAttributeReferenceElements(document.source(), root, std::string_view(currentId),
                                            &reverseReferenceElements,
                                            kMaxExpandedReverseReferences);
    AppendReverseCssReferenceElements(document.source(), root, std::string_view(currentId),
                                      &reverseReferenceElements, kMaxExpandedReverseReferences);
    if (IsLargeReverseReferenceFanout(reverseReferenceElements.size())) {
      continue;
    }

    reverseProcessedIds.push_back(std::string(currentId));
    AppendReverseAttributeReferences(document.source(), root, std::string_view(currentId), current,
                                     &result, &visited, &reverseExpandableEntities);
    AppendReverseCssReferences(document.source(), lineStarts, root, std::string_view(currentId),
                               current, &result, &visited, &visitedCssRules,
                               &reverseExpandableEntities);
  }

  return result;
}

void AppendForwardReferenceElements(const svg::SVGDocument& document,
                                    const std::vector<std::size_t>& lineStarts,
                                    const svg::SVGElement& element,
                                    std::vector<svg::SVGElement>* elements) {
  if (!HasLiveSvgTreeComponents(element)) {
    return;
  }

  FocusElementCollection ignoredCollection;
  std::vector<CssRuleKey> ignoredCssRules;
  std::vector<FragmentReference> references;
  AppendMatchedCssRulesInSubtree(document.source(), lineStarts, element, &ignoredCollection,
                                 &ignoredCssRules, &references);
  AppendReferencedFragmentsInSubtree(document.source(), element, &references);

  const svg::SVGElement root = document.svgElement();
  for (const FragmentReference& reference : references) {
    std::optional<svg::SVGElement> referenced = FindElementById(root, reference.fragmentId);
    if (referenced.has_value()) {
      AddUniqueElement(*referenced, elements);
    }
  }
}

std::vector<ByteRange> AncestorTagRanges(std::string_view source, ByteRange nodeRange) {
  std::vector<ByteRange> result;
  if (nodeRange.start >= nodeRange.end || nodeRange.start >= source.size()) {
    return result;
  }

  const std::size_t openEnd = source.find('>', nodeRange.start);
  if (openEnd == std::string_view::npos || openEnd >= nodeRange.end) {
    result.push_back(nodeRange);
    return result;
  }

  const ByteRange openTag{.start = nodeRange.start, .end = openEnd + 1};
  result.push_back(openTag);

  const std::size_t closeStart = source.rfind("</", nodeRange.end - 1);
  if (closeStart != std::string_view::npos && closeStart >= openTag.end &&
      closeStart < nodeRange.end) {
    result.push_back(ByteRange{.start = closeStart, .end = nodeRange.end});
  }

  return result;
}

void AddNodeTagLineRanges(std::string_view source, const std::vector<std::size_t>& lineStarts,
                          ByteRange nodeRange, FocusPartition* partition) {
  for (ByteRange tagRange : AncestorTagRanges(source, nodeRange)) {
    partition->dimmed.push_back(RangeToLines(lineStarts, tagRange));
  }
}

bool AddAncestorTagLineRanges(std::string_view source, const std::vector<std::size_t>& lineStarts,
                              const svg::SVGElement& element, FocusPartition* partition,
                              bool required) {
  for (std::optional<svg::SVGElement> ancestor = SafeParentElement(element); ancestor.has_value();
       ancestor = SafeParentElement(*ancestor)) {
    std::optional<ByteRange> ancestorRange = NodeRange(source, *ancestor);
    if (!ancestorRange.has_value()) {
      return !required;
    }

    AddNodeTagLineRanges(source, lineStarts, *ancestorRange, partition);
  }

  return true;
}

void Normalize(std::vector<LineRange>* ranges) {
  std::erase_if(*ranges, [](const LineRange& range) { return range.endLine <= range.startLine; });
  std::sort(ranges->begin(), ranges->end(), [](const LineRange& a, const LineRange& b) {
    return a.startLine != b.startLine ? a.startLine < b.startLine : a.endLine < b.endLine;
  });

  std::vector<LineRange> merged;
  for (const LineRange& range : *ranges) {
    if (merged.empty() || range.startLine > merged.back().endLine) {
      merged.push_back(range);
    } else {
      merged.back().endLine = std::max(merged.back().endLine, range.endLine);
    }
  }
  *ranges = std::move(merged);
}

void DeduplicateLinks(std::vector<FocusReferenceLink>* links) {
  std::vector<FocusReferenceLink> unique;
  for (const FocusReferenceLink& link : *links) {
    if (std::ranges::find(unique, link) == unique.end()) {
      unique.push_back(link);
    }
  }
  *links = std::move(unique);
}

bool ContainsLine(const std::vector<LineRange>& ranges, int line) {
  return std::ranges::any_of(ranges, [line](const LineRange& range) {
    return line >= range.startLine && line < range.endLine;
  });
}

std::vector<LineRange> HiddenLineRanges(int lineCount, const std::vector<LineRange>& visible) {
  std::vector<LineRange> result;
  int hiddenStart = -1;
  for (int line = 0; line < lineCount; ++line) {
    if (ContainsLine(visible, line)) {
      if (hiddenStart != -1) {
        result.push_back(LineRange{.startLine = hiddenStart, .endLine = line});
        hiddenStart = -1;
      }
    } else if (hiddenStart == -1) {
      hiddenStart = line;
    }
  }

  if (hiddenStart != -1) {
    result.push_back(LineRange{.startLine = hiddenStart, .endLine = lineCount});
  }
  return result;
}

}  // namespace

FocusPartition ComputeFocusPartition(const svg::SVGDocument& document,
                                     const svg::SVGElement& selected) {
  const std::array<svg::SVGElement, 1> selectedElements{selected};
  return ComputeFocusPartition(document, std::span<const svg::SVGElement>(selectedElements));
}

FocusPartition ComputeFocusPartition(const svg::SVGDocument& document,
                                     std::span<const svg::SVGElement> selectedElements) {
  // Hold a scoped read access for the whole analysis so the nested SVGElement / EntityHandle reads
  // below are valid under ThreadingMode::ConcurrentDom (read access is reentrant per thread).
  [[maybe_unused]] const svg::DocumentReadAccess focusReadAccess = document.readAccess();
  if (!document.hasSourceStore()) {
    return {};
  }
  if (selectedElements.empty()) {
    return {};
  }

  const std::string_view source = document.source();
  const std::vector<std::size_t> lineStarts = BuildLineStarts(source);
  std::vector<svg::SVGElement> initialElements;
  initialElements.reserve(selectedElements.size());
  std::vector<Entity> selectedEntities;
  selectedEntities.reserve(selectedElements.size());
  for (const svg::SVGElement& selected : selectedElements) {
    if (!HasLiveSvgTreeComponents(selected) || !NodeRange(source, selected).has_value()) {
      return {};
    }

    initialElements.push_back(selected);
    selectedEntities.push_back(selected.entityHandle().entity());
  }

  FocusPartition partition;
  const FocusElementCollection focusElements =
      CollectFocusElements(document, std::move(initialElements), {}, lineStarts);

  for (std::size_t i = 0; i < focusElements.elements.size(); ++i) {
    std::optional<ByteRange> elementRange = NodeRange(source, focusElements.elements[i]);
    const bool selectedElement =
        std::ranges::find(selectedEntities, focusElements.elements[i].entityHandle().entity()) !=
        selectedEntities.end();
    if (elementRange.has_value()) {
      (selectedElement ? partition.fullColor : partition.referenceColor)
          .push_back(RangeToLines(lineStarts, *elementRange));
    }

    if (!AddAncestorTagLineRanges(source, lineStarts, focusElements.elements[i], &partition,
                                  /*required=*/selectedElement)) {
      return {};
    }
  }

  for (const FocusCssRule& cssRule : focusElements.cssRules) {
    partition.referenceColor.push_back(RangeToLines(lineStarts, cssRule.ruleRange));
    if (cssRule.stylesheetNodeRange.has_value()) {
      AddNodeTagLineRanges(source, lineStarts, *cssRule.stylesheetNodeRange, &partition);
    }
  }

  partition.referenceLinks.insert(partition.referenceLinks.end(), focusElements.cssLinks.begin(),
                                  focusElements.cssLinks.end());

  AppendElementReferenceLinks(source, lineStarts, focusElements.links, &partition);

  Normalize(&partition.fullColor);
  Normalize(&partition.referenceColor);
  Normalize(&partition.dimmed);
  DeduplicateLinks(&partition.referenceLinks);

  std::vector<LineRange> visible = partition.fullColor;
  visible.insert(visible.end(), partition.referenceColor.begin(), partition.referenceColor.end());
  visible.insert(visible.end(), partition.dimmed.begin(), partition.dimmed.end());
  Normalize(&visible);
  partition.hidden = HiddenLineRanges(static_cast<int>(lineStarts.size()), visible);
  return partition;
}

std::optional<StyleFocus> ComputeStyleFocusAtSourceOffset(const svg::SVGDocument& document,
                                                          std::size_t sourceOffset) {
  // Hold a scoped read access for the whole analysis so the nested SVGElement / EntityHandle reads
  // below are valid under ThreadingMode::ConcurrentDom (read access is reentrant per thread).
  [[maybe_unused]] const svg::DocumentReadAccess focusReadAccess = document.readAccess();
  if (!document.hasSourceStore()) {
    return std::nullopt;
  }

  const std::string_view source = document.source();
  if (sourceOffset >= source.size()) {
    return std::nullopt;
  }

  const std::vector<std::size_t> lineStarts = BuildLineStarts(source);
  std::optional<svg::SVGStyleRuleAtSourceOffset> sourceRule =
      svg::FindStyleRuleAtSourceOffset(document, sourceOffset);
  if (!sourceRule.has_value()) {
    return std::nullopt;
  }

  std::optional<ByteRange> ruleRange = ResolveSourceRange(source, sourceRule->ruleSourceRange);
  std::optional<ByteRange> selectorRange =
      ResolveSourceRange(source, sourceRule->selectorSourceRange);
  if (!ruleRange.has_value() || !selectorRange.has_value()) {
    return std::nullopt;
  }

  const svg::SVGElement root = document.svgElement();
  Registry& registry = *root.entityHandle().registry();
  std::vector<svg::SVGElement> impactedElements;
  AppendImpactedElementsForStyleRule(root, *sourceRule, &impactedElements,
                                     kMaxExpandedReverseReferences);
  const bool suppressReverseReferenceExpansion =
      IsLargeReverseReferenceFanout(impactedElements.size());

  std::vector<FragmentReference> cssReferences;
  AppendCssFragmentReferences(source, *ruleRange, &cssReferences);
  FocusElementCollection focusElements;
  if (!suppressReverseReferenceExpansion) {
    focusElements = CollectFocusElements(document, impactedElements, cssReferences, lineStarts);
  }

  FocusPartition partition;
  partition.fullColor.push_back(RangeToLines(lineStarts, *ruleRange));

  if (std::optional<ByteRange> stylesheetNodeRange =
          NodeRangeForEntity(source, registry, sourceRule->stylesheetEntity)) {
    AddNodeTagLineRanges(source, lineStarts, *stylesheetNodeRange, &partition);
  }

  if (!suppressReverseReferenceExpansion) {
    for (const svg::SVGElement& element : focusElements.elements) {
      if (std::optional<ByteRange> elementRange = NodeRange(source, element)) {
        partition.referenceColor.push_back(RangeToLines(lineStarts, *elementRange));
      }

      std::ignore = AddAncestorTagLineRanges(source, lineStarts, element, &partition,
                                             /*required=*/false);
    }

    for (const FocusCssRule& cssRule : focusElements.cssRules) {
      partition.referenceColor.push_back(RangeToLines(lineStarts, cssRule.ruleRange));
      if (cssRule.stylesheetNodeRange.has_value()) {
        AddNodeTagLineRanges(source, lineStarts, *cssRule.stylesheetNodeRange, &partition);
      }
    }
    partition.referenceLinks.insert(partition.referenceLinks.end(), focusElements.cssLinks.begin(),
                                    focusElements.cssLinks.end());

    for (const svg::SVGElement& element : focusElements.elements) {
      if (std::optional<std::size_t> elementStart = ReferenceLinkTargetOffset(source, element)) {
        partition.referenceLinks.push_back(FocusReferenceLink{
            .from = PointForOffset(lineStarts, selectorRange->start),
            .to = PointForOffset(lineStarts, *elementStart),
        });
      }
    }

    AppendElementReferenceLinks(source, lineStarts, focusElements.links, &partition);
  }

  Normalize(&partition.fullColor);
  Normalize(&partition.referenceColor);
  Normalize(&partition.dimmed);
  DeduplicateLinks(&partition.referenceLinks);

  std::vector<LineRange> visible = partition.fullColor;
  visible.insert(visible.end(), partition.referenceColor.begin(), partition.referenceColor.end());
  visible.insert(visible.end(), partition.dimmed.begin(), partition.dimmed.end());
  Normalize(&visible);
  partition.hidden = HiddenLineRanges(static_cast<int>(lineStarts.size()), visible);
  return StyleFocus{
      .partition = std::move(partition),
      .impactedElements = suppressReverseReferenceExpansion ? std::vector<svg::SVGElement>{}
                                                            : std::move(impactedElements),
      .reverseReferenceExpansionSuppressed = suppressReverseReferenceExpansion,
  };
}

std::optional<FocusPartition> ComputeStyleFocusPartitionAtSourceOffset(
    const svg::SVGDocument& document, std::size_t sourceOffset) {
  std::optional<StyleFocus> focus = ComputeStyleFocusAtSourceOffset(document, sourceOffset);
  if (!focus.has_value()) {
    return std::nullopt;
  }

  return std::move(focus->partition);
}

ReferenceHighlightSummary ComputeReferenceHighlightSummary(
    const svg::SVGDocument& document, std::span<const svg::SVGElement> selectedElements) {
  ReferenceHighlightSummary summary;
  if (!document.hasSourceStore() || selectedElements.empty()) {
    return summary;
  }

  const std::string_view source = document.source();
  const std::vector<std::size_t> lineStarts = BuildLineStarts(source);
  const svg::SVGElement root = document.svgElement();
  for (const svg::SVGElement& selected : selectedElements) {
    if (!HasLiveSvgTreeComponents(selected)) {
      continue;
    }

    AppendForwardReferenceElements(document, lineStarts, selected, &summary.referencedElements);

    const RcString id = SafeId(selected);
    if (id.empty()) {
      continue;
    }

    AppendReverseAttributeReferenceElements(source, root, std::string_view(id),
                                            &summary.referencingElements,
                                            std::numeric_limits<std::size_t>::max());
    AppendReverseCssReferenceElements(source, root, std::string_view(id),
                                      &summary.referencingElements,
                                      std::numeric_limits<std::size_t>::max());
    std::erase_if(summary.referencingElements, [&](const svg::SVGElement& element) {
      return element.entityHandle().entity() == selected.entityHandle().entity();
    });
  }

  return summary;
}

}  // namespace donner::editor
