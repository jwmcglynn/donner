#include "donner/editor/FocusView.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/StringUtils.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/ValueParser.h"
#include "donner/editor/CssSourceReference.h"
#include "donner/editor/ReferenceFanout.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/SVGStyleQuery.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {
namespace {

struct ByteRange {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct FragmentReference {
  std::string fragmentId;
  std::optional<std::size_t> sourceOffset;
  bool url = false;
};

struct FocusCssRule {
  ByteRange ruleRange;
  std::optional<ByteRange> stylesheetNodeRange;
};

struct ElementReferenceLink {
  std::size_t fromOffset = 0;
  svg::SVGElement referenced;
  bool reverseReference = false;
  bool targetId = false;
};

struct ElementReferenceLinkKey {
  std::size_t fromOffset = 0;
  Entity referencedEntity = entt::null;
  bool reverseReference = false;

  bool operator==(const ElementReferenceLinkKey& other) const = default;
};

struct ElementReferenceLinkKeyHash {
  std::size_t operator()(const ElementReferenceLinkKey& key) const {
    std::size_t result = std::hash<std::size_t>{}(key.fromOffset);
    result ^= std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(key.referencedEntity)) +
              0x9e3779b9u + (result << 6u) + (result >> 2u);
    result ^=
        std::hash<bool>{}(key.reverseReference) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
    return result;
  }
};

struct FocusReferenceLinkHash {
  std::size_t operator()(const FocusReferenceLink& link) const {
    std::size_t result = std::hash<int>{}(link.from.line);
    for (const int value : {link.from.column, link.to.line, link.to.column}) {
      result ^= std::hash<int>{}(value) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
    }
    return result;
  }
};

struct CssRuleKey {
  Entity stylesheetEntity = entt::null;
  std::size_t ruleIndex = 0;

  bool operator==(const CssRuleKey& other) const = default;
};

struct CssRuleKeyHash {
  std::size_t operator()(const CssRuleKey& key) const {
    std::size_t result =
        std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(key.stylesheetEntity));
    result ^=
        std::hash<std::size_t>{}(key.ruleIndex) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
    return result;
  }
};

struct IndexedCssReference {
  enum class Kind { Variable, Url, Fallback } kind;
  std::string name;
  std::optional<SourceRange> location;
  std::size_t fallbackEnd = 0;
};

struct IndexedCssRule {
  SourceRange cssRange;
  std::vector<IndexedCssReference> references;
};

using CssRuleReferenceIndex = std::unordered_map<CssRuleKey, IndexedCssRule, CssRuleKeyHash>;

class FocusTraversalBudget {
public:
  [[nodiscard]] bool consume(std::size_t work = 1) {
    if (rejected_ || work > kMaximumFocusTraversalWork - work_) {
      rejected_ = true;
      return false;
    }
    work_ += work;
    return true;
  }

  void reject() { rejected_ = true; }
  [[nodiscard]] std::size_t work() const { return work_; }
  [[nodiscard]] bool rejected() const { return rejected_; }

  CssRuleReferenceIndex& cssRules(const Registry* registry) { return cssRules_[registry]; }

private:
  std::size_t work_ = 0;
  bool rejected_ = false;
  std::unordered_map<const Registry*, CssRuleReferenceIndex> cssRules_;
};

struct FocusVariableUse {
  std::string name;
  std::size_t sourceOffset;
};

struct ElementVariableUses {
  svg::SVGElement element;
  std::vector<FocusVariableUse> uses;
};

struct FocusElementCollection {
  std::vector<LineRange> declarationLines;
  std::vector<svg::SVGElement> elements;
  std::vector<ElementReferenceLink> links;
  std::vector<FocusCssRule> cssRules;
  std::vector<FocusReferenceLink> cssLinks;
  std::unordered_set<Entity> visitedElements;
  std::unordered_set<ElementReferenceLinkKey, ElementReferenceLinkKeyHash> elementReferenceKeys;
  std::unordered_set<FocusReferenceLink, FocusReferenceLinkHash> cssLinkKeys;
  std::vector<ElementVariableUses> variableGroups;
  std::unordered_map<Entity, std::size_t> variableGroupIndices;
  std::size_t variableUseCount = 0;
  FocusTraversalBudget traversalBudget;
};

enum class FocusLinkSourceMode {
  All,       ///< Links may originate from this element and descendants.
  RootOnly,  ///< Links may originate from this element, but not descendants.
  None,      ///< This subtree contributes context only, with no links.
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

std::optional<std::vector<std::size_t>> BuildLineStarts(std::string_view source) {
  std::vector<std::size_t> result;
  result.push_back(0);
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (source[i] == '\n') {
      if (result.size() >= kMaximumFocusLineStarts) {
        return std::nullopt;
      }
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

void AppendFocusLineRange(std::vector<LineRange>* ranges, LineRange range,
                          FocusTraversalBudget* budget) {
  if (ranges->size() < kMaximumFocusLineRanges) {
    ranges->push_back(range);
  } else {
    budget->reject();
  }
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

std::optional<xml::XMLQualifiedName> SafeTagName(const svg::SVGElement& element) {
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

void AppendFragmentReference(std::vector<FragmentReference>* references,
                             std::string_view fragmentId, std::optional<std::size_t> sourceOffset) {
  if (fragmentId.empty() || references->size() >= kMaximumFocusReferenceLinks) {
    return;
  }

  references->push_back(FragmentReference{
      .fragmentId = std::string(fragmentId),
      .sourceOffset = sourceOffset,
  });
}

using CssRangeMap = std::function<std::optional<SourceRange>(const SourceRange&)>;
using VariableLinkSink =
    std::function<void(const svg::SVGElement&, std::string_view, const SourceRange&)>;

void AppendParsedReferences(std::span<const css::ComponentValue> values, std::string_view cssText,
                            const CssRangeMap& project, const svg::SVGElement* element,
                            const VariableLinkSink& variableLink,
                            std::vector<FragmentReference>* references,
                            FocusTraversalBudget* budget) {
  const auto hasValue = [element](std::string_view name) {
    return element ? svg::HasPreparedCustomPropertyValue(*element, name) : std::nullopt;
  };
  CssSourceReferenceHooks hooks;
  if (budget) {
    hooks.consume = [&] { return budget->consume(); };
  }
  VisitCssSourceReferences(
      values, cssText,
      [&](const CssSourceReference& ref) {
        if (references->size() >= kMaximumFocusReferenceLinks) {
          return false;
        }
        const auto location = project(ref.spelling);
        if (ref.kind == CssSourceReference::Kind::FragmentUrl) {
          references->push_back(
              {std::string(ref.name), location ? location->start.offset : std::nullopt, true});
        } else if (location && variableLink && element) {
          variableLink(*element, ref.name, *location);
        }
        return true;
      },
      hasValue, hooks);
}

void AppendAttributeCssReferences(const xml::XMLNode& node, const xml::XMLQualifiedNameRef& attr,
                                  std::string_view value, const svg::SVGElement& element,
                                  const VariableLinkSink& variables,
                                  std::vector<FragmentReference>* out,
                                  FocusTraversalBudget* budget) {
  // Both var() and url() require a literal function-opening parenthesis.
  if (value.find('(') == std::string_view::npos) {
    return;
  }
  const auto project = [&](const SourceRange& local) {
    return node.mapDecodedAttributeRange(attr, local);
  };
  if (!attr.namespacePrefix.empty()) {
    return;
  }
  if (attr.name == "style") {
    for (const auto& declaration :
         css::parser::DeclarationListParser::ParseOnlyDeclarations(value)) {
      AppendParsedReferences(declaration.values, value, project, &element, variables, out, budget);
    }
  } else if (svg::PropertyRegistry::isPresentationAttributeName(attr.name)) {
    AppendParsedReferences(css::parser::ValueParser::Parse(value), value, project, &element,
                           variables, out, budget);
  }
}

void AppendHrefFragmentReference(std::string_view value, std::optional<std::size_t> valueOffset,
                                 std::vector<FragmentReference>* references) {
  std::size_t start = 0;
  while (start < value.size() && StringUtils::IsAsciiWhitespace(value[start])) {
    ++start;
  }

  if (start >= value.size() || value[start] != '#') {
    return;
  }

  std::size_t end = value.size();
  while (end > start + 1 && StringUtils::IsAsciiWhitespace(value[end - 1])) {
    --end;
  }

  std::optional<std::size_t> sourceOffset;
  if (valueOffset.has_value()) {
    sourceOffset = *valueOffset + start;
  }

  AppendFragmentReference(references, value.substr(start + 1, end - start - 1), sourceOffset);
}

std::vector<FragmentReference> ReferencedFragments(std::string_view source,
                                                   const svg::SVGElement& element,
                                                   FocusTraversalBudget* budget,
                                                   const VariableLinkSink& variables = {}) {
  std::vector<FragmentReference> result;
  std::optional<xml::XMLNode> xmlNode = xml::XMLNode::TryCast(element.entityHandle());
  if (!xmlNode.has_value()) {
    return result;
  }

  for (const xml::XMLQualifiedNameRef& attrName : xmlNode->attributes()) {
    if (budget && budget->rejected()) {
      break;
    }
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
    AppendAttributeCssReferences(*xmlNode, attrName, value, element, variables, &result, budget);
    if (attrName.name == "href") {
      AppendHrefFragmentReference(value, valueOffset, &result);
    }
  }

  return result;
}

std::optional<std::size_t> SelectorLinkSourceOffset(std::string_view source,
                                                    const svg::SVGElement& selected) {
  if (std::optional<ByteRange> openingTagRange = OpeningTagRange(source, selected)) {
    return openingTagRange->end;
  }

  return std::nullopt;
}

bool IndexRuleReferences(const svg::SVGStyleElement& owner, std::string_view text,
                         std::span<const css::Declaration> declarations,
                         FocusTraversalBudget& budget,
                         std::vector<IndexedCssReference>& references) {
  std::vector<std::size_t> fallbacks;
  const CssSourceReferenceHooks hooks{
      .consume = [&] { return budget.consume(); },
      .enterFallback =
          [&](std::string_view name) {
            fallbacks.push_back(references.size());
            references.push_back({IndexedCssReference::Kind::Fallback, std::string(name), {}});
          },
      .leaveFallback =
          [&] {
            const auto start = fallbacks.back();
            fallbacks.pop_back();
            if (references.size() == start + 1) {
              references.pop_back();
            } else {
              references[start].fallbackEnd = references.size();
            }
          },
  };
  const auto emit = [&](const CssSourceReference& ref) {
    using Kind = IndexedCssReference::Kind;
    const auto kind =
        ref.kind == CssSourceReference::Kind::CustomProperty ? Kind::Variable : Kind::Url;
    references.push_back(
        {kind, std::string(ref.name), svg::MapStylesheetSourceRange(owner, ref.spelling)});
    return true;
  };
  for (const auto& declaration : declarations) {
    if (!budget.consume() || !VisitCssSourceReferences(declaration.values, text, emit, {}, hooks)) {
      budget.reject();
      return false;
    }
  }
  return true;
}

const IndexedCssRule* GetIndexedRuleReferences(const svg::SVGStyleElement& owner,
                                               std::size_t ruleIndex, const SourceRange& range,
                                               FocusTraversalBudget& budget) {
  const auto handle = owner.entityHandle();
  auto& cache = budget.cssRules(handle.registry());
  const auto [entry, added] = cache.try_emplace(
      {handle.entity(), ruleIndex}, IndexedCssRule{.cssRange = range, .references = {}});
  if (added) {
    auto& references = entry->second.references;
    const bool found = svg::VisitAuthoredStylesheetDeclarations(
        owner, ruleIndex, range, [&](std::string_view text, auto declarations) {
          if (!IndexRuleReferences(owner, text, declarations, budget, references)) {
            references.clear();
          }
        });
    if (!found) {
      return nullptr;
    }
  }
  return entry->second.cssRange == range ? &entry->second : nullptr;
}

bool FallbackIsActive(const IndexedCssReference& reference, const svg::SVGElement* subject,
                      bool allFallbacks) {
  return allFallbacks ||
         (subject && !svg::HasPreparedCustomPropertyValue(*subject, reference.name).value_or(true));
}

bool ContainsSourceOffset(const SourceRange& range, std::size_t offset) {
  return range.start.offset && range.end.offset && *range.start.offset <= offset &&
         offset < *range.end.offset;
}

void EmitIndexedReference(const IndexedCssReference& ref, const svg::SVGElement* subject,
                          const VariableLinkSink& variables,
                          std::vector<FragmentReference>* fragments,
                          std::optional<std::size_t> caret) {
  if (caret && (!ref.location || !ContainsSourceOffset(*ref.location, *caret))) {
    return;
  }
  if (ref.kind == IndexedCssReference::Kind::Url) {
    fragments->push_back(
        {ref.name, ref.location ? ref.location->start.offset : std::nullopt, true});
  } else if (ref.location && variables && subject) {
    variables(*subject, ref.name, *ref.location);
  }
}

void AppendIndexedRuleReferences(const IndexedCssRule& indexed, const svg::SVGElement* subject,
                                 const VariableLinkSink& variables,
                                 std::vector<FragmentReference>* fragments,
                                 FocusTraversalBudget& budget,
                                 std::optional<std::size_t> caret = std::nullopt) {
  const bool allFallbacks = caret && !subject;
  for (std::size_t i = 0; i < indexed.references.size();) {
    if (fragments->size() >= kMaximumFocusReferenceLinks || !budget.consume()) {
      return;
    }
    const auto& reference = indexed.references[i];
    if (reference.kind != IndexedCssReference::Kind::Fallback) {
      EmitIndexedReference(reference, subject, variables, fragments, caret);
    } else if (!FallbackIsActive(reference, subject, allFallbacks)) {
      i = reference.fallbackEnd;
      continue;
    }
    ++i;
  }
}

void AppendMatchedRuleReferences(const svg::SVGMatchedStyleRule& match,
                                 const svg::SVGElement& element,
                                 std::vector<FragmentReference>* references,
                                 FocusTraversalBudget* budget,
                                 const VariableLinkSink& variables = {}) {
  if (!match.stylesheetElement || !match.ruleCssRange || !budget) {
    return;
  }
  if (const auto* indexed = GetIndexedRuleReferences(*match.stylesheetElement, match.ruleIndex,
                                                     *match.ruleCssRange, *budget)) {
    AppendIndexedRuleReferences(*indexed, &element, variables, references, *budget);
  }
}

FocusLinkSourceMode ChildLinkSourceMode(FocusLinkSourceMode mode) {
  return mode == FocusLinkSourceMode::All ? FocusLinkSourceMode::All : FocusLinkSourceMode::None;
}

void AppendFragmentReferences(std::vector<FragmentReference> input, FocusLinkSourceMode mode,
                              std::vector<FragmentReference>* references) {
  if (mode == FocusLinkSourceMode::None) {
    for (FragmentReference& reference : input) {
      reference.sourceOffset = std::nullopt;
    }
  }

  const std::size_t remaining = kMaximumFocusReferenceLinks - references->size();
  const std::size_t insertCount = std::min(input.size(), remaining);
  references->insert(references->end(), input.begin(), input.begin() + insertCount);
}

void AppendReferencedFragmentsInSubtree(std::string_view source, const svg::SVGElement& root,
                                        FocusLinkSourceMode linkSourceMode,
                                        std::vector<FragmentReference>* references,
                                        FocusTraversalBudget* budget = nullptr,
                                        std::size_t depth = 0,
                                        const VariableLinkSink& variables = {}) {
  if (depth > kMaximumFocusTraversalDepth) {
    if (budget != nullptr) {
      budget->reject();
    }
    return;
  }
  if (budget != nullptr && !budget->consume()) {
    return;
  }
  if (references->size() >= kMaximumFocusReferenceLinks) {
    return;
  }
  AppendFragmentReferences(
      ReferencedFragments(
          source, root, budget,
          linkSourceMode == FocusLinkSourceMode::None ? VariableLinkSink{} : variables),
      linkSourceMode, references);

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendReferencedFragmentsInSubtree(source, current, ChildLinkSourceMode(linkSourceMode),
                                       references, budget, depth + 1u, variables);
    if ((budget != nullptr && budget->rejected()) ||
        references->size() >= kMaximumFocusReferenceLinks) {
      return;
    }
  }
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

bool AddUniqueElement(const svg::SVGElement& element, std::vector<svg::SVGElement>* elements);

bool ReferencesAnyFragmentId(std::span<const FragmentReference> references,
                             const std::unordered_set<std::string>& fragmentIds) {
  return std::ranges::any_of(references, [&fragmentIds](const FragmentReference& reference) {
    return fragmentIds.contains(reference.fragmentId);
  });
}

void AppendMatchedCssFragmentReferencesInSubtree(std::string_view source,
                                                 const svg::SVGElement& root,
                                                 std::vector<FragmentReference>* references,
                                                 FocusTraversalBudget* budget = nullptr,
                                                 std::size_t depth = 0) {
  if (depth > kMaximumFocusTraversalDepth) {
    if (budget != nullptr) {
      budget->reject();
    }
    return;
  }
  if (!HasLiveSvgTreeComponents(root) || (budget != nullptr && !budget->consume())) {
    return;
  }

  if (IsRenderedStyleTarget(root)) {
    for (const svg::SVGMatchedStyleRule& matchedRule :
         svg::CollectMatchedStyleRules(root, /*includeDeclarations=*/false)) {
      if (budget != nullptr && !budget->consume()) {
        return;
      }
      if (!matchedRule.ruleSourceRange.has_value()) {
        continue;
      }

      const std::optional<ByteRange> ruleRange =
          ResolveSourceRange(source, *matchedRule.ruleSourceRange);
      if (ruleRange.has_value()) {
        AppendMatchedRuleReferences(matchedRule, root, references, budget);
      }
    }
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendMatchedCssFragmentReferencesInSubtree(source, current, references, budget, depth + 1u);
    if (budget != nullptr && budget->rejected()) {
      return;
    }
  }
}

void AppendDocumentElements(const svg::SVGElement& root, std::vector<svg::SVGElement>* elements,
                            FocusTraversalBudget* budget = nullptr, std::size_t depth = 0,
                            std::size_t maximumElements = std::numeric_limits<std::size_t>::max()) {
  if (depth > kMaximumFocusTraversalDepth) {
    if (budget != nullptr) {
      budget->reject();
    }
    return;
  }
  if (!HasLiveSvgTreeComponents(root) || (budget != nullptr && !budget->consume())) {
    return;
  }
  if (elements->size() >= maximumElements) {
    if (budget != nullptr) {
      budget->reject();
    }
    return;
  }

  elements->push_back(root);
  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendDocumentElements(current, elements, budget, depth + 1u, maximumElements);
    if (budget != nullptr && budget->rejected()) {
      return;
    }
  }
}

std::unordered_map<std::string, svg::SVGElement> BuildElementByIdIndex(
    std::span<const svg::SVGElement> elements, FocusTraversalBudget* budget = nullptr) {
  std::unordered_map<std::string, svg::SVGElement> result;
  result.reserve(elements.size());
  for (const svg::SVGElement& element : elements) {
    if (budget != nullptr && !budget->consume()) {
      break;
    }
    const RcString id = SafeId(element);
    if (!id.empty()) {
      result.emplace(std::string(std::string_view(id)), element);
    }
  }

  return result;
}

bool HasSelectedAncestor(const svg::SVGElement& element,
                         const std::unordered_set<Entity>& selectedEntities,
                         FocusTraversalBudget* budget) {
  std::size_t depth = 0;
  for (std::optional<svg::SVGElement> ancestor = SafeParentElement(element); ancestor.has_value();
       ancestor = SafeParentElement(*ancestor)) {
    if (depth++ >= kMaximumFocusTraversalDepth || !budget->consume()) {
      budget->reject();
      return true;
    }
    if (selectedEntities.contains(ancestor->unsafeEntityHandle().entity())) {
      return true;
    }
  }

  return false;
}

bool MatchedCssReferencesAnyFragmentId(std::string_view source, const svg::SVGElement& element,
                                       const std::unordered_set<std::string>& fragmentIds,
                                       FocusTraversalBudget* budget) {
  if (!IsRenderedStyleTarget(element)) {
    return false;
  }

  for (const svg::SVGMatchedStyleRule& matchedRule :
       svg::CollectMatchedStyleRules(element, /*includeDeclarations=*/false)) {
    if (!budget->consume()) {
      return false;
    }
    if (!matchedRule.ruleSourceRange.has_value()) {
      continue;
    }

    const std::optional<ByteRange> ruleRange =
        ResolveSourceRange(source, *matchedRule.ruleSourceRange);
    if (!ruleRange.has_value()) {
      continue;
    }

    std::vector<FragmentReference> references;
    AppendMatchedRuleReferences(matchedRule, element, &references, budget);
    if (ReferencesAnyFragmentId(references, fragmentIds)) {
      return true;
    }
  }

  return false;
}

bool AddUniqueSummaryElement(const svg::SVGElement& element, std::vector<svg::SVGElement>* elements,
                             std::unordered_set<Entity>* elementEntities,
                             FocusTraversalBudget* budget) {
  if (!HasLiveSvgTreeComponents(element)) {
    return false;
  }
  const Entity entity = element.unsafeEntityHandle().entity();
  if (!elementEntities->insert(entity).second) {
    return false;
  }
  if (elements->size() >= kMaximumFocusElements) {
    budget->reject();
    return false;
  }
  elements->push_back(element);
  return true;
}

void AppendReverseReferenceElementsForSelectedIds(
    std::string_view source, std::span<const svg::SVGElement> documentElements,
    const std::unordered_set<std::string>& selectedIds,
    const std::unordered_set<Entity>& selectedEntities,
    std::vector<svg::SVGElement>* referencingElements,
    std::unordered_set<Entity>* referencingEntities, FocusTraversalBudget* budget) {
  if (selectedIds.empty()) {
    return;
  }

  for (const svg::SVGElement& element : documentElements) {
    if (!budget->consume()) {
      return;
    }
    const Entity entity = element.unsafeEntityHandle().entity();
    if (selectedEntities.contains(entity)) {
      continue;
    }

    const std::vector<FragmentReference> attributeReferences =
        ReferencedFragments(source, element, budget);
    if (ReferencesAnyFragmentId(attributeReferences, selectedIds) ||
        MatchedCssReferencesAnyFragmentId(source, element, selectedIds, budget)) {
      AddUniqueSummaryElement(element, referencingElements, referencingEntities, budget);
      if (budget->rejected()) {
        return;
      }
    }
  }
}

bool IsGroupElement(const svg::SVGElement& element) {
  const std::optional<svg::ElementType> type = SafeElementType(element);
  return type.has_value() && *type == svg::ElementType::G;
}

void AppendImpactedElementsForStyleRule(const svg::SVGElement& root,
                                        const svg::SVGStyleRuleAtSourceOffset& sourceRule,
                                        std::vector<svg::SVGElement>* impactedElements,
                                        std::size_t maxElements) {
  if (impactedElements->size() > maxElements) {
    return;
  }

  if (IsRenderedStyleTarget(root)) {
    for (const svg::SVGMatchedStyleRule& match :
         svg::CollectMatchedStyleRules(root, /*includeDeclarations=*/false)) {
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

bool AddFocusElement(const svg::SVGElement& element, FocusElementCollection* result) {
  if (!HasLiveSvgTreeComponents(element)) {
    return false;
  }

  const Entity entity = element.unsafeEntityHandle().entity();
  if (result->visitedElements.contains(entity)) {
    return false;
  }
  if (result->elements.size() >= kMaximumFocusElements) {
    result->traversalBudget.reject();
    return false;
  }

  result->visitedElements.insert(entity);
  result->elements.push_back(element);
  return true;
}

void AddElementReferenceLink(std::size_t fromOffset, const svg::SVGElement& referenced,
                             bool reverseReference, FocusElementCollection* result,
                             bool targetId = false) {
  if (!HasLiveSvgTreeComponents(referenced)) {
    return;
  }

  const Entity referencedEntity = referenced.unsafeEntityHandle().entity();
  const ElementReferenceLinkKey key{
      .fromOffset = fromOffset,
      .referencedEntity = referencedEntity,
      .reverseReference = reverseReference,
  };
  if (result->elementReferenceKeys.contains(key)) {
    return;
  }
  if (result->links.size() >= kMaximumFocusReferenceLinks) {
    result->traversalBudget.reject();
    return;
  }
  if (result->elementReferenceKeys.insert(key).second) {
    result->links.push_back(ElementReferenceLink{
        .fromOffset = fromOffset,
        .referenced = referenced,
        .reverseReference = reverseReference,
        .targetId = targetId,
    });
  }
}

void AddSourceReferenceLink(FocusReferenceLink link, FocusElementCollection* result) {
  if (result->cssLinks.size() >= kMaximumFocusReferenceLinks) {
    result->traversalBudget.reject();
    return;
  }
  if (result->cssLinkKeys.insert(link).second) {
    result->cssLinks.push_back(link);
  }
}

std::optional<std::size_t> ReferenceLinkTargetOffset(std::string_view source,
                                                     const svg::SVGElement& referenced);

void AppendElementReferenceLinks(std::string_view source,
                                 const std::vector<std::size_t>& lineStarts,
                                 const std::vector<ElementReferenceLink>& links,
                                 FocusPartition* partition, FocusTraversalBudget* budget) {
  const std::size_t reverseLinkCount = std::ranges::count_if(
      links, [](const ElementReferenceLink& link) { return link.reverseReference; });
  const bool drawReverseLinks = !IsLargeReverseReferenceFanout(reverseLinkCount);

  for (const ElementReferenceLink& link : links) {
    if (!budget->consume()) {
      return;
    }
    if (link.reverseReference && !drawReverseLinks) {
      continue;
    }

    std::optional<std::size_t> targetOffset;
    if (link.targetId) {
      if (const auto node = xml::XMLNode::TryCast(link.referenced.entityHandle())) {
        const auto id = SafeId(link.referenced);
        const SourceRange name{FileOffset::Offset(0), FileOffset::Offset(id.size())};
        if (const auto mapped =
                node->mapDecodedAttributeRange(xml::XMLQualifiedNameRef("id"), name)) {
          targetOffset = mapped->start.offset;
        }
      }
    } else {
      targetOffset = ReferenceLinkTargetOffset(source, link.referenced);
    }
    if (!targetOffset.has_value() || link.fromOffset > source.size()) {
      continue;
    }

    if (partition->referenceLinks.size() >= kMaximumFocusReferenceLinks) {
      return;
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

  const Entity entity = element.unsafeEntityHandle().entity();
  const auto it = std::ranges::find_if(*elements, [entity](const svg::SVGElement& existing) {
    return existing.unsafeEntityHandle().entity() == entity;
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
    const std::optional<xml::XMLQualifiedName> ancestorTagName = SafeTagName(*ancestor);
    if (ancestorTagName.has_value() && std::string_view(ancestorTagName->name) == tagName) {
      return true;
    }
  }

  return false;
}

bool IsReferenceResourceElement(const svg::SVGElement& element) {
  const std::optional<xml::XMLQualifiedName> tagNameRef = SafeTagName(element);
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

  const Entity entity = element.unsafeEntityHandle().entity();
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
  const Entity entity = element.unsafeEntityHandle().entity();
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
                       std::unordered_set<CssRuleKey, CssRuleKeyHash>* visitedCssRules) {
  const CssRuleKey key{
      .stylesheetEntity = matchedRule.stylesheetEntity,
      .ruleIndex = matchedRule.ruleIndex,
  };
  if (visitedCssRules->contains(key)) {
    return;
  }
  if (result->cssRules.size() >= kMaximumFocusElements) {
    result->traversalBudget.reject();
    return;
  }

  std::optional<FocusCssRule> cssRule = FocusCssRuleFromMatchedRule(source, registry, matchedRule);
  if (!cssRule.has_value()) {
    return;
  }

  visitedCssRules->insert(key);
  result->cssRules.push_back(*cssRule);
}

VariableLinkSink VariableLinksForFocus(std::string_view source, FocusElementCollection* result,
                                       FocusLinkSourceMode mode = FocusLinkSourceMode::All) {
  if (mode == FocusLinkSourceMode::None) {
    return {};
  }
  return [source, result](const svg::SVGElement& element, std::string_view name,
                          const SourceRange& use) {
    const auto from = ResolveSourceRange(source, use);
    if (!from || !result->traversalBudget.consume(name.size() + 1)) {
      return;
    }
    if (result->variableUseCount >= kMaximumFocusReferenceLinks) {
      result->traversalBudget.reject();
      return;
    }
    const auto [it, inserted] = result->variableGroupIndices.try_emplace(
        element.unsafeEntityHandle().entity(), result->variableGroups.size());
    if (inserted) {
      result->variableGroups.push_back({element, {}});
    }
    result->variableGroups[it->second].uses.push_back({std::string(name), from->start});
    ++result->variableUseCount;
  };
}

void ResolveVariableGroup(std::string_view source, const std::vector<std::size_t>& lineStarts,
                          const ElementVariableUses& group, FocusElementCollection& result) {
  std::vector<std::string_view> names;
  names.reserve(group.uses.size());
  for (const auto& use : group.uses) {
    names.push_back(use.name);
  }
  const auto remaining = kMaximumFocusTraversalWork - result.traversalBudget.work();
  const auto batch = svg::FindCustomPropertyDeclarations(group.element, names, remaining);
  if (!result.traversalBudget.consume(batch.work) || batch.budgetExceeded) {
    result.traversalBudget.reject();
    return;
  }
  if (!batch.complete) {
    return;
  }
  for (std::size_t i = 0; i < group.uses.size(); ++i) {
    if (!batch.declarations[i]) {
      continue;
    }
    const auto target = ResolveSourceRange(source, *batch.declarations[i]);
    if (target) {
      AddSourceReferenceLink({PointForOffset(lineStarts, group.uses[i].sourceOffset),
                              PointForOffset(lineStarts, target->start)},
                             &result);
      AppendFocusLineRange(&result.declarationLines, RangeToLines(lineStarts, *target),
                           &result.traversalBudget);
    }
  }
}

void ResolveVariableLinks(std::string_view source, const std::vector<std::size_t>& lineStarts,
                          FocusElementCollection& result) {
  for (const auto& group : result.variableGroups) {
    if (result.traversalBudget.rejected()) {
      break;
    }
    ResolveVariableGroup(source, lineStarts, group, result);
  }
}

void AppendMatchedCssRulesInSubtree(std::string_view source,
                                    const std::vector<std::size_t>& lineStarts,
                                    const svg::SVGElement& root, FocusElementCollection* result,
                                    std::unordered_set<CssRuleKey, CssRuleKeyHash>* visitedCssRules,
                                    FocusLinkSourceMode linkSourceMode,
                                    std::vector<FragmentReference>* references,
                                    FocusTraversalBudget* budget, std::size_t depth = 0) {
  if (depth > kMaximumFocusTraversalDepth) {
    budget->reject();
    return;
  }
  if (!HasLiveSvgTreeComponents(root) || !budget->consume()) {
    return;
  }

  Registry& registry = *root.entityHandle().registry();
  const bool emitLinks = linkSourceMode != FocusLinkSourceMode::None;
  const std::optional<std::size_t> selectorLinkSource = SelectorLinkSourceOffset(source, root);
  if (IsRenderedStyleTarget(root)) {
    for (const svg::SVGMatchedStyleRule& matchedRule :
         svg::CollectMatchedStyleRules(root, /*includeDeclarations=*/false)) {
      if (!budget->consume()) {
        return;
      }
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

      if (emitLinks && selectorLinkSource.has_value() && *selectorLinkSource <= source.size()) {
        AddSourceReferenceLink(
            FocusReferenceLink{
                .from = PointForOffset(lineStarts, *selectorLinkSource),
                .to = PointForOffset(lineStarts, selectorRange->start),
            },
            result);
      }

      std::vector<FragmentReference> ruleReferences;
      AppendMatchedRuleReferences(matchedRule, root, &ruleReferences, budget,
                                  VariableLinksForFocus(source, result, linkSourceMode));
      AppendFragmentReferences(std::move(ruleReferences), linkSourceMode, references);
    }
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendMatchedCssRulesInSubtree(source, lineStarts, current, result, visitedCssRules,
                                   ChildLinkSourceMode(linkSourceMode), references, budget,
                                   depth + 1u);
    if (budget->rejected()) {
      return;
    }
  }
}

void AppendReverseAttributeReferences(std::string_view source, const svg::SVGElement& root,
                                      std::string_view targetId,
                                      const svg::SVGElement& targetElement,
                                      FocusElementCollection* result,
                                      std::vector<Entity>* reverseExpandableEntities,
                                      FocusTraversalBudget* budget, std::size_t depth = 0) {
  if (depth > kMaximumFocusTraversalDepth) {
    budget->reject();
    return;
  }
  if (!HasLiveSvgTreeComponents(root) || !budget->consume()) {
    return;
  }

  for (const FragmentReference& reference : ReferencedFragments(source, root, budget)) {
    if (reference.fragmentId != targetId) {
      continue;
    }

    if (reference.sourceOffset.has_value()) {
      AddElementReferenceLink(*reference.sourceOffset, targetElement, /*reverseReference=*/true,
                              result, reference.url);
    }
    AddFocusElement(root, result);
    MarkReverseExpandableIfResource(root, reverseExpandableEntities);
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendReverseAttributeReferences(source, current, targetId, targetElement, result,
                                     reverseExpandableEntities, budget, depth + 1u);
    if (budget->rejected()) {
      return;
    }
  }
}

void AppendReverseCssReferences(std::string_view source, const std::vector<std::size_t>& lineStarts,
                                const svg::SVGElement& root, std::string_view targetId,
                                const svg::SVGElement& targetElement,
                                FocusElementCollection* result,
                                std::unordered_set<CssRuleKey, CssRuleKeyHash>* visitedCssRules,
                                std::vector<Entity>* reverseExpandableEntities,
                                FocusTraversalBudget* budget, std::size_t depth = 0) {
  if (depth > kMaximumFocusTraversalDepth) {
    budget->reject();
    return;
  }
  if (!HasLiveSvgTreeComponents(root) || !budget->consume()) {
    return;
  }

  Registry& registry = *root.entityHandle().registry();
  if (IsRenderedStyleTarget(root)) {
    for (const svg::SVGMatchedStyleRule& matchedRule :
         svg::CollectMatchedStyleRules(root, /*includeDeclarations=*/false)) {
      if (!budget->consume()) {
        return;
      }
      if (!matchedRule.ruleSourceRange.has_value()) {
        continue;
      }

      std::optional<ByteRange> ruleRange = ResolveSourceRange(source, *matchedRule.ruleSourceRange);
      if (!ruleRange.has_value()) {
        continue;
      }

      std::vector<FragmentReference> references;
      AppendMatchedRuleReferences(matchedRule, root, &references, budget);

      bool referencesTarget = false;
      for (const FragmentReference& reference : references) {
        if (reference.fragmentId != targetId) {
          continue;
        }

        referencesTarget = true;
        if (reference.sourceOffset.has_value()) {
          AddElementReferenceLink(*reference.sourceOffset, targetElement,
                                  /*reverseReference=*/true, result, reference.url);
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
              result);
        }
      }
      AddFocusElement(root, result);
      MarkReverseExpandableIfResource(root, reverseExpandableEntities);
    }
  }

  for (auto child = SafeFirstChild(root); child.has_value();) {
    svg::SVGElement current = *child;
    child = SafeNextSibling(current);
    AppendReverseCssReferences(source, lineStarts, current, targetId, targetElement, result,
                               visitedCssRules, reverseExpandableEntities, budget, depth + 1u);
    if (budget->rejected()) {
      return;
    }
  }
}

void AppendReverseAttributeReferenceElements(std::string_view source, const svg::SVGElement& root,
                                             std::string_view targetId,
                                             std::vector<svg::SVGElement>* elements,
                                             std::size_t maxElements, FocusTraversalBudget* budget,
                                             std::size_t depth = 0) {
  if (depth > kMaximumFocusTraversalDepth) {
    budget->reject();
    return;
  }
  if (!HasLiveSvgTreeComponents(root) || elements->size() > maxElements || !budget->consume()) {
    return;
  }

  for (const FragmentReference& reference : ReferencedFragments(source, root, budget)) {
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
    AppendReverseAttributeReferenceElements(source, current, targetId, elements, maxElements,
                                            budget, depth + 1u);
    if (budget->rejected() || elements->size() > maxElements) {
      return;
    }
  }
}

void AppendReverseCssReferenceElements(std::string_view source, const svg::SVGElement& root,
                                       std::string_view targetId,
                                       std::vector<svg::SVGElement>* elements,
                                       std::size_t maxElements, FocusTraversalBudget* budget,
                                       std::size_t depth = 0) {
  if (depth > kMaximumFocusTraversalDepth) {
    budget->reject();
    return;
  }
  if (!HasLiveSvgTreeComponents(root) || elements->size() > maxElements || !budget->consume()) {
    return;
  }

  if (IsRenderedStyleTarget(root)) {
    for (const svg::SVGMatchedStyleRule& matchedRule :
         svg::CollectMatchedStyleRules(root, /*includeDeclarations=*/false)) {
      if (!budget->consume()) {
        return;
      }
      if (!matchedRule.ruleSourceRange.has_value()) {
        continue;
      }

      std::optional<ByteRange> ruleRange = ResolveSourceRange(source, *matchedRule.ruleSourceRange);
      if (!ruleRange.has_value()) {
        continue;
      }

      std::vector<FragmentReference> references;
      AppendMatchedRuleReferences(matchedRule, root, &references, budget);
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
    AppendReverseCssReferenceElements(source, current, targetId, elements, maxElements, budget,
                                      depth + 1u);
    if (budget->rejected() || elements->size() > maxElements) {
      return;
    }
  }
}

FocusElementCollection CollectFocusElements(const svg::SVGDocument& document,
                                            std::vector<svg::SVGElement> initialElements,
                                            const std::vector<FragmentReference>& initialReferences,
                                            const std::vector<std::size_t>& lineStarts,
                                            FocusTraversalBudget budget = {}) {
  FocusElementCollection result;
  result.traversalBudget = std::move(budget);
  std::vector<Entity> selectedGroupEntities;
  std::vector<Entity> reverseExpandableEntities;
  std::vector<std::string> reverseProcessedIds;
  std::unordered_set<CssRuleKey, CssRuleKeyHash> visitedCssRules;
  for (const svg::SVGElement& element : initialElements) {
    if (AddFocusElement(element, &result)) {
      if (IsGroupElement(element)) {
        selectedGroupEntities.push_back(element.unsafeEntityHandle().entity());
      }
      MarkReverseExpandableIfResource(element, &reverseExpandableEntities);
    }
  }

  const svg::SVGElement root = document.svgElement();
  std::optional<std::unordered_map<std::string, svg::SVGElement>> elementById;
  for (std::size_t i = 0; i < result.elements.size(); ++i) {
    if (result.traversalBudget.rejected()) {
      break;
    }
    const svg::SVGElement current = result.elements[i];
    const Entity currentEntity = current.unsafeEntityHandle().entity();
    const bool currentIsSelectedGroup =
        std::ranges::find(selectedGroupEntities, currentEntity) != selectedGroupEntities.end();
    const FocusLinkSourceMode linkSourceMode =
        currentIsSelectedGroup ? FocusLinkSourceMode::RootOnly : FocusLinkSourceMode::All;
    std::vector<FragmentReference> references;
    if (i == 0) {
      references.insert(references.end(), initialReferences.begin(), initialReferences.end());
    }

    AppendMatchedCssRulesInSubtree(document.source(), lineStarts, current, &result,
                                   &visitedCssRules, linkSourceMode, &references,
                                   &result.traversalBudget);
    AppendReferencedFragmentsInSubtree(document.source(), current, linkSourceMode, &references,
                                       &result.traversalBudget, 0,
                                       VariableLinksForFocus(document.source(), &result));
    if (result.traversalBudget.rejected()) {
      break;
    }

    for (const FragmentReference& reference : references) {
      if (!elementById.has_value()) {
        std::vector<svg::SVGElement> documentElements;
        AppendDocumentElements(root, &documentElements, &result.traversalBudget);
        elementById = BuildElementByIdIndex(documentElements, &result.traversalBudget);
        if (result.traversalBudget.rejected()) {
          break;
        }
      }
      const auto referenced = elementById->find(reference.fragmentId);
      if (referenced == elementById->end()) {
        continue;
      }

      if (reference.sourceOffset.has_value()) {
        const bool reverseReference =
            std::ranges::find(reverseProcessedIds, reference.fragmentId) !=
            reverseProcessedIds.end();
        AddElementReferenceLink(*reference.sourceOffset, referenced->second, reverseReference,
                                &result, reference.url);
      }

      AddFocusElement(referenced->second, &result);
      if (result.traversalBudget.rejected()) {
        break;
      }
    }
    if (result.traversalBudget.rejected()) {
      break;
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
                                            kMaxExpandedReverseReferences, &result.traversalBudget);
    AppendReverseCssReferenceElements(document.source(), root, std::string_view(currentId),
                                      &reverseReferenceElements, kMaxExpandedReverseReferences,
                                      &result.traversalBudget);
    if (IsLargeReverseReferenceFanout(reverseReferenceElements.size())) {
      continue;
    }

    reverseProcessedIds.push_back(std::string(currentId));
    AppendReverseAttributeReferences(document.source(), root, std::string_view(currentId), current,
                                     &result, &reverseExpandableEntities, &result.traversalBudget);
    AppendReverseCssReferences(document.source(), lineStarts, root, std::string_view(currentId),
                               current, &result, &visitedCssRules, &reverseExpandableEntities,
                               &result.traversalBudget);
  }

  return result;
}

void AppendForwardReferenceElements(
    std::string_view source, const svg::SVGElement& element,
    const std::unordered_map<std::string, svg::SVGElement>& elementById,
    std::vector<svg::SVGElement>* elements, std::unordered_set<Entity>* elementEntities,
    FocusTraversalBudget* budget) {
  if (!HasLiveSvgTreeComponents(element) || !budget->consume()) {
    return;
  }

  std::vector<FragmentReference> references;
  AppendMatchedCssFragmentReferencesInSubtree(source, element, &references, budget);
  AppendReferencedFragmentsInSubtree(source, element, FocusLinkSourceMode::All, &references,
                                     budget);

  for (const FragmentReference& reference : references) {
    if (!budget->consume()) {
      return;
    }
    auto referenced = elementById.find(reference.fragmentId);
    if (referenced != elementById.end()) {
      AddUniqueSummaryElement(referenced->second, elements, elementEntities, budget);
      if (budget->rejected()) {
        return;
      }
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
                          ByteRange nodeRange, FocusPartition* partition,
                          FocusTraversalBudget* budget) {
  for (ByteRange tagRange : AncestorTagRanges(source, nodeRange)) {
    AppendFocusLineRange(&partition->dimmed, RangeToLines(lineStarts, tagRange), budget);
  }
}

void AddXmlAncestorTagLineRanges(std::string_view source,
                                 const std::vector<std::size_t>& lineStarts,
                                 const xml::XMLNode& node, FocusPartition* partition,
                                 FocusTraversalBudget* budget) {
  for (std::optional<xml::XMLNode> ancestor = node.parentElement(); ancestor.has_value();
       ancestor = ancestor->parentElement()) {
    if (!budget->consume()) {
      return;
    }
    if (std::optional<ByteRange> ancestorRange = NodeRange(source, *ancestor)) {
      AddNodeTagLineRanges(source, lineStarts, *ancestorRange, partition, budget);
    }
  }
}

bool AddAncestorTagLineRanges(std::string_view source, const std::vector<std::size_t>& lineStarts,
                              const svg::SVGElement& element, FocusPartition* partition,
                              bool required, FocusTraversalBudget* budget) {
  for (std::optional<svg::SVGElement> ancestor = SafeParentElement(element); ancestor.has_value();
       ancestor = SafeParentElement(*ancestor)) {
    if (!budget->consume()) {
      return true;
    }
    std::optional<ByteRange> ancestorRange = NodeRange(source, *ancestor);
    if (!ancestorRange.has_value()) {
      return !required;
    }

    AddNodeTagLineRanges(source, lineStarts, *ancestorRange, partition, budget);
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
  std::unordered_set<FocusReferenceLink, FocusReferenceLinkHash> seen;
  unique.reserve(std::min(links->size(), kMaximumFocusReferenceLinks));
  for (const FocusReferenceLink& link : *links) {
    if (unique.size() >= kMaximumFocusReferenceLinks) {
      break;
    }
    if (seen.insert(link).second) {
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

// A reference under the caret has its own destination even if the rule has no
// renderable matches, or the reverse links for a broad selector were suppressed.
void AppendReferenceUnderCaret(const svg::SVGStylesheetRule& rule, const svg::SVGDocument& document,
                               std::size_t offset, const svg::SVGElement* subject,
                               FocusElementCollection& out) {
  if (!rule.stylesheetElement || !rule.ruleCssRange) {
    return;
  }
  const auto* indexed = GetIndexedRuleReferences(*rule.stylesheetElement, rule.ruleIndex,
                                                 *rule.ruleCssRange, out.traversalBudget);
  if (!indexed) {
    return;
  }
  std::vector<FragmentReference> fragments;
  AppendIndexedRuleReferences(*indexed, subject, VariableLinksForFocus(document.source(), &out),
                              &fragments, out.traversalBudget, offset);
  if (fragments.empty()) {
    return;
  }
  std::vector<svg::SVGElement> elements;
  AppendDocumentElements(document.svgElement(), &elements, &out.traversalBudget);
  const auto byId = BuildElementByIdIndex(elements, &out.traversalBudget);
  if (out.traversalBudget.rejected()) {
    return;
  }
  for (const auto& ref : fragments) {
    if (const auto it = byId.find(ref.fragmentId); it != byId.end()) {
      AddFocusElement(it->second, &out);
      AddElementReferenceLink(*ref.sourceOffset, it->second, false, &out, true);
    }
  }
}

const svg::SVGElement* SelectedRuleSubject(const svg::SVGDocument& document,
                                           const svg::SVGStyleRuleAtSourceOffset& rule,
                                           std::span<const svg::SVGElement> selection) {
  if (selection.size() != 1 ||
      selection.front().unsafeEntityHandle().registry() !=
          document.svgElement().entityHandle().registry() ||
      !IsRenderedStyleTarget(selection.front())) {
    return nullptr;
  }
  for (const auto& match :
       svg::CollectMatchedStyleRules(selection.front(), /*includeDeclarations=*/false)) {
    if (MatchesStyleSourceRule(match, rule)) {
      return &selection.front();
    }
  }
  return nullptr;
}

FocusElementCollection CollectRuleFocusContext(const svg::SVGDocument& document, std::size_t offset,
                                               const svg::SVGStyleRuleAtSourceOffset& rule,
                                               std::span<const svg::SVGElement> selection,
                                               const std::vector<svg::SVGElement>& impacted,
                                               const std::vector<std::size_t>& lineStarts) {
  const auto parsed =
      svg::CollectStylesheetRuleAtSourceOffset(document, offset, /*includeDeclarations=*/false);
  if (!impacted.empty() && !IsLargeReverseReferenceFanout(impacted.size())) {
    FocusTraversalBudget budget;
    std::vector<FragmentReference> references;
    if (parsed && parsed->stylesheetElement && parsed->ruleCssRange) {
      if (const auto* indexed = GetIndexedRuleReferences(
              *parsed->stylesheetElement, parsed->ruleIndex, *parsed->ruleCssRange, budget)) {
        AppendIndexedRuleReferences(*indexed, nullptr, {}, &references, budget);
      }
    }
    return CollectFocusElements(document, impacted, references, lineStarts, std::move(budget));
  }
  FocusElementCollection result;
  if (parsed) {
    AppendReferenceUnderCaret(*parsed, document, offset,
                              SelectedRuleSubject(document, rule, selection), result);
  }
  return result;
}

void AppendFocusedRuleContext(std::string_view source, const std::vector<std::size_t>& lineStarts,
                              FocusElementCollection& focusElements, FocusPartition& partition) {
  for (const svg::SVGElement& element : focusElements.elements) {
    if (std::optional<ByteRange> elementRange = NodeRange(source, element)) {
      AppendFocusLineRange(&partition.referenceColor, RangeToLines(lineStarts, *elementRange),
                           &focusElements.traversalBudget);
    }

    std::ignore = AddAncestorTagLineRanges(source, lineStarts, element, &partition,
                                           /*required=*/false, &focusElements.traversalBudget);
  }

  for (const FocusCssRule& cssRule : focusElements.cssRules) {
    AppendFocusLineRange(&partition.referenceColor, RangeToLines(lineStarts, cssRule.ruleRange),
                         &focusElements.traversalBudget);
    if (cssRule.stylesheetNodeRange.has_value()) {
      AddNodeTagLineRanges(source, lineStarts, *cssRule.stylesheetNodeRange, &partition,
                           &focusElements.traversalBudget);
    }
  }
  partition.referenceColor.insert(partition.referenceColor.end(),
                                  focusElements.declarationLines.begin(),
                                  focusElements.declarationLines.end());
  partition.referenceLinks.insert(partition.referenceLinks.end(), focusElements.cssLinks.begin(),
                                  focusElements.cssLinks.end());

  AppendElementReferenceLinks(source, lineStarts, focusElements.links, &partition,
                              &focusElements.traversalBudget);
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
  const std::optional<std::vector<std::size_t>> lineStarts = BuildLineStarts(source);
  if (!lineStarts.has_value()) {
    return FocusPartition{.resourceLimitExceeded = true};
  }
  std::vector<svg::SVGElement> initialElements;
  initialElements.reserve(selectedElements.size());
  std::vector<Entity> selectedEntities;
  selectedEntities.reserve(selectedElements.size());
  for (const svg::SVGElement& selected : selectedElements) {
    if (!HasLiveSvgTreeComponents(selected) || !NodeRange(source, selected).has_value()) {
      return {};
    }

    initialElements.push_back(selected);
    selectedEntities.push_back(selected.unsafeEntityHandle().entity());
  }

  FocusPartition partition;
  FocusElementCollection focusElements =
      CollectFocusElements(document, std::move(initialElements), {}, *lineStarts);
  ResolveVariableLinks(source, *lineStarts, focusElements);

  for (std::size_t i = 0; i < focusElements.elements.size(); ++i) {
    std::optional<ByteRange> elementRange = NodeRange(source, focusElements.elements[i]);
    const bool selectedElement =
        std::ranges::find(selectedEntities,
                          focusElements.elements[i].unsafeEntityHandle().entity()) !=
        selectedEntities.end();
    if (elementRange.has_value()) {
      AppendFocusLineRange(selectedElement ? &partition.fullColor : &partition.referenceColor,
                           RangeToLines(*lineStarts, *elementRange),
                           &focusElements.traversalBudget);
    }

    if (!AddAncestorTagLineRanges(source, *lineStarts, focusElements.elements[i], &partition,
                                  /*required=*/selectedElement, &focusElements.traversalBudget)) {
      return {};
    }
  }

  for (const FocusCssRule& cssRule : focusElements.cssRules) {
    AppendFocusLineRange(&partition.referenceColor, RangeToLines(*lineStarts, cssRule.ruleRange),
                         &focusElements.traversalBudget);
    if (cssRule.stylesheetNodeRange.has_value()) {
      AddNodeTagLineRanges(source, *lineStarts, *cssRule.stylesheetNodeRange, &partition,
                           &focusElements.traversalBudget);
    }
  }

  partition.referenceColor.insert(partition.referenceColor.end(),
                                  focusElements.declarationLines.begin(),
                                  focusElements.declarationLines.end());
  partition.referenceLinks.insert(partition.referenceLinks.end(), focusElements.cssLinks.begin(),
                                  focusElements.cssLinks.end());

  AppendElementReferenceLinks(source, *lineStarts, focusElements.links, &partition,
                              &focusElements.traversalBudget);

  Normalize(&partition.fullColor);
  Normalize(&partition.referenceColor);
  Normalize(&partition.dimmed);
  DeduplicateLinks(&partition.referenceLinks);

  std::vector<LineRange> visible = partition.fullColor;
  visible.insert(visible.end(), partition.referenceColor.begin(), partition.referenceColor.end());
  visible.insert(visible.end(), partition.dimmed.begin(), partition.dimmed.end());
  Normalize(&visible);
  partition.hidden = HiddenLineRanges(static_cast<int>(lineStarts->size()), visible);
  partition.traversalWork = focusElements.traversalBudget.work();
  partition.resourceLimitExceeded = focusElements.traversalBudget.rejected();
  return partition;
}

std::optional<StyleFocus> ComputeStyleFocusAtSourceOffset(
    const svg::SVGDocument& document, std::size_t sourceOffset,
    std::span<const svg::SVGElement> selection) {
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

  const std::optional<std::vector<std::size_t>> lineStarts = BuildLineStarts(source);
  if (!lineStarts.has_value()) {
    return StyleFocus{
        .partition = FocusPartition{.resourceLimitExceeded = true},
        .reverseReferenceExpansionSuppressed = true,
    };
  }
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

  auto focusElements = CollectRuleFocusContext(document, sourceOffset, *sourceRule, selection,
                                               impactedElements, *lineStarts);

  ResolveVariableLinks(source, *lineStarts, focusElements);

  FocusPartition partition;
  AppendFocusLineRange(&partition.fullColor, RangeToLines(*lineStarts, *ruleRange),
                       &focusElements.traversalBudget);

  if (std::optional<ByteRange> stylesheetNodeRange =
          NodeRangeForEntity(source, registry, sourceRule->stylesheetEntity)) {
    AddNodeTagLineRanges(source, *lineStarts, *stylesheetNodeRange, &partition,
                         &focusElements.traversalBudget);
  }
  if (std::optional<xml::XMLNode> stylesheetNode =
          xml::XMLNode::TryCast(EntityHandle(registry, sourceRule->stylesheetEntity))) {
    AddXmlAncestorTagLineRanges(source, *lineStarts, *stylesheetNode, &partition,
                                &focusElements.traversalBudget);
  }

  AppendFocusedRuleContext(source, *lineStarts, focusElements, partition);

  Normalize(&partition.fullColor);
  Normalize(&partition.referenceColor);
  Normalize(&partition.dimmed);
  DeduplicateLinks(&partition.referenceLinks);

  std::vector<LineRange> visible = partition.fullColor;
  visible.insert(visible.end(), partition.referenceColor.begin(), partition.referenceColor.end());
  visible.insert(visible.end(), partition.dimmed.begin(), partition.dimmed.end());
  Normalize(&visible);
  partition.hidden = HiddenLineRanges(static_cast<int>(lineStarts->size()), visible);
  partition.traversalWork = focusElements.traversalBudget.work();
  partition.resourceLimitExceeded = focusElements.traversalBudget.rejected();
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
  FocusTraversalBudget budget;
  [[maybe_unused]] const svg::DocumentReadAccess focusReadAccess = document.readAccess();
  if (!document.hasSourceStore() || selectedElements.empty()) {
    return summary;
  }

  const svg::SVGElement root = document.svgElement();
  std::vector<svg::SVGElement> documentElements;
  AppendDocumentElements(root, &documentElements, &budget, 0, kMaximumFocusElements);
  const std::unordered_map<std::string, svg::SVGElement> elementById =
      BuildElementByIdIndex(documentElements, &budget);

  std::vector<svg::SVGElement> liveSelectedElements;
  std::unordered_set<Entity> selectedEntities;
  std::unordered_set<std::string> selectedIds;
  for (const svg::SVGElement& selected : selectedElements) {
    if (!budget.consume()) {
      break;
    }
    if (!HasLiveSvgTreeComponents(selected)) {
      continue;
    }
    const Entity entity = selected.unsafeEntityHandle().entity();
    if (!selectedEntities.insert(entity).second) {
      continue;
    }
    if (liveSelectedElements.size() >= kMaximumFocusElements) {
      budget.reject();
      break;
    }
    liveSelectedElements.push_back(selected);

    const RcString id = SafeId(selected);
    if (!id.empty()) {
      selectedIds.insert(std::string(std::string_view(id)));
    }
  }

  if (liveSelectedElements.empty()) {
    summary.traversalWork = budget.work();
    summary.resourceLimitExceeded = budget.rejected();
    return summary;
  }

  const std::string_view source = document.source();
  std::unordered_set<Entity> referencedEntities;
  std::unordered_set<Entity> referencingEntities;
  for (const svg::SVGElement& selected : liveSelectedElements) {
    if (!budget.consume()) {
      break;
    }
    if (!HasSelectedAncestor(selected, selectedEntities, &budget)) {
      AppendForwardReferenceElements(source, selected, elementById, &summary.referencedElements,
                                     &referencedEntities, &budget);
    }
    if (budget.rejected()) {
      break;
    }
  }

  if (!budget.rejected()) {
    AppendReverseReferenceElementsForSelectedIds(source, documentElements, selectedIds,
                                                 selectedEntities, &summary.referencingElements,
                                                 &referencingEntities, &budget);
  }
  summary.traversalWork = budget.work();
  summary.resourceLimitExceeded = budget.rejected();

  return summary;
}

}  // namespace donner::editor
