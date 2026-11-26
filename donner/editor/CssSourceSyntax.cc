#include "donner/editor/CssSourceSyntax.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <optional>
#include <utility>

#include "donner/base/StringUtils.h"
#include "donner/base/Utf8.h"
#include "donner/base/xml/XMLEscape.h"
#include "donner/base/xml/XMLParser.h"
#include "donner/base/xml/XMLTokenizer.h"
#include "donner/css/FontFace.h"
#include "donner/css/parser/details/Tokenizer.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {
namespace {

using css::Token;
using xml::XMLToken;
using xml::XMLTokenType;

std::size_t TokenStart(const XMLToken& token) {
  return token.range.start.offset.value_or(0);
}

std::size_t TokenEnd(const XMLToken& token) {
  return token.range.end.offset.value_or(TokenStart(token));
}

std::optional<std::pair<std::size_t, std::size_t>> QuotedValueRange(const XMLToken& token,
                                                                    std::string_view source) {
  const std::size_t start = TokenStart(token);
  const std::size_t end = TokenEnd(token);
  if (end <= start || end > source.size()) {
    return std::nullopt;
  }
  const char quote = source[start];
  if (quote != '\'' && quote != '"') {
    return std::nullopt;
  }
  const bool closed = end > start + 1 && source[end - 1] == quote;
  return std::pair(start + 1, closed ? end - 1 : end);
}

struct ElementFrame {
  std::string_view name;
  std::size_t namespaceDepth;
  bool stylesheet;
};

struct NamespaceChange {
  std::string_view prefix;
  std::optional<bool> previous;
};

/// Read namespace scopes from the same token stream as the source editor. All views borrow source.
class RegionReader {
public:
  explicit RegionReader(std::string_view source) : source_(source) {}

  std::vector<CssSourceRegion> read() {
    xml::Tokenize(source_, [&](XMLToken token) { accept(token); });
    finishStyleText(source_.size());
    if (!attributes_.empty() && !closing_ && svgElement()) {
      std::ranges::move(attributes_, std::back_inserter(regions_));
    }
    return std::move(regions_);
  }

private:
  void finishStyleText(std::size_t end) {
    if (styleStart_ && *styleStart_ < end) {
      regions_.push_back({*styleStart_, end, CssSourceRegionKind::Stylesheet, {}});
    }
    styleStart_.reset();
  }

  void bindNamespace(std::string_view prefix, std::string_view uri) {
    const auto previous = svgBindings_.find(prefix);
    changes_.push_back(
        {prefix, previous == svgBindings_.end() ? std::nullopt : std::optional(previous->second)});
    svgBindings_[prefix] = uri == "http://www.w3.org/2000/svg";
  }

  void restoreNamespace(std::size_t depth) {
    while (changes_.size() > depth) {
      const auto [prefix, previous] = changes_.back();
      if (previous) {
        svgBindings_[prefix] = *previous;
      } else {
        svgBindings_.erase(prefix);
      }
      changes_.pop_back();
    }
  }

  bool svgElement() const {
    const auto colon = name_.find(':');
    const auto prefix = colon == name_.npos ? std::string_view{} : name_.substr(0, colon);
    const auto it = svgBindings_.find(prefix);
    // Donner also accepts SVG documents without an explicit default namespace.
    return it == svgBindings_.end() ? colon == name_.npos : it->second;
  }

  void attributeValue(const XMLToken& token) {
    const auto range = QuotedValueRange(token, source_);
    if (!range || closing_) {
      return;
    }
    const auto [begin, end] = *range;
    if (attribute_ == "xmlns" || attribute_.starts_with("xmlns:")) {
      const auto prefix = attribute_ == "xmlns" ? std::string_view{} : attribute_.substr(6);
      bindNamespace(prefix, source_.substr(begin, end - begin));
    } else if (attribute_ == "style") {
      attributes_.push_back({begin, end, CssSourceRegionKind::DeclarationList, {}});
    } else if (svg::PropertyRegistry::isPresentationAttributeName(attribute_)) {
      attributes_.push_back(
          {begin, end, CssSourceRegionKind::PropertyValue, std::string(attribute_)});
    }
    attribute_ = {};
  }

  void closeElement() {
    // A valid close matches the top element. Keep recovery from incomplete edits bounded.
    constexpr std::size_t kRecoveryDepth = 32;
    const auto limit = elements_.rbegin() + std::min(elements_.size(), kRecoveryDepth);
    const auto found = std::find_if(elements_.rbegin(), limit,
                                    [&](const ElementFrame& frame) { return frame.name == name_; });
    if (found != limit) {
      restoreNamespace(found->namespaceDepth);
      elements_.erase(found.base() - 1, elements_.end());
    }
  }

  void finishTag(const XMLToken& token) {
    if (closing_) {
      closeElement();
    } else {
      const bool svg = svgElement();
      if (svg) {
        std::ranges::move(attributes_, std::back_inserter(regions_));
      }
      if (token.type == XMLTokenType::TagSelfClose || name_.empty()) {
        restoreNamespace(tagDepth_);
      } else {
        const auto colon = name_.find(':');
        const auto local = colon == name_.npos ? name_ : name_.substr(colon + 1);
        elements_.push_back({name_, tagDepth_, svg && local == "style"});
      }
    }
    attributes_.clear();
    if (!elements_.empty() && elements_.back().stylesheet) {
      styleStart_ = TokenEnd(token);
    }
  }

  void beginTag(const XMLToken& token) {
    finishStyleText(TokenStart(token));
    closing_ = token.text(source_) == "</";
    tagDepth_ = changes_.size();
    name_ = {};
    attribute_ = {};
    attributes_.clear();
    skipTag_ =
        skippedDepth_ != 0 ||
        (!closing_ && elements_.size() >= xml::XMLParser::Options::kDefaultMaximumNestingDepth);
  }

  // Over-depth content keeps ordinary XML coloring. Count its nesting without allocating
  // frames or namespace bindings, then resume at the next sibling in the enclosing SVG.
  void skipMarkup(const XMLToken& token) {
    if (token.type != XMLTokenType::TagClose && token.type != XMLTokenType::TagSelfClose) {
      return;
    }
    if (token.type == XMLTokenType::TagClose) {
      if (closing_) {
        --skippedDepth_;
      } else {
        ++skippedDepth_;
      }
    }
    skipTag_ = false;
    if (skippedDepth_ == 0 && !elements_.empty() && elements_.back().stylesheet) {
      styleStart_ = TokenEnd(token);
    }
  }

  void accept(const XMLToken& token) {
    if (token.type == XMLTokenType::TagOpen) {
      beginTag(token);
      return;
    }
    if (skipTag_) {
      skipMarkup(token);
      return;
    }
    switch (token.type) {
      case XMLTokenType::TagName: name_ = token.text(source_); break;
      case XMLTokenType::AttributeName: attribute_ = token.text(source_); break;
      case XMLTokenType::AttributeValue: attributeValue(token); break;
      case XMLTokenType::TagSelfClose:
      case XMLTokenType::TagClose: finishTag(token); break;
      default: break;
    }
  }

  std::string_view source_, name_, attribute_;
  std::map<std::string_view, bool> svgBindings_;
  std::vector<NamespaceChange> changes_;
  std::vector<ElementFrame> elements_;
  std::vector<CssSourceRegion> regions_, attributes_;
  std::optional<std::size_t> styleStart_;
  std::size_t tagDepth_ = 0;
  std::size_t skippedDepth_ = 0;
  bool closing_ = false;
  bool skipTag_ = false;
};

std::vector<CssSourceRegion> FindRegions(std::string_view source) {
  return RegionReader(source).read();
}

bool IsKnownProperty(std::string_view name) {
  const auto names = svg::PropertyRegistry::propertyNames();
  return std::ranges::any_of(
      names, [&](std::string_view known) { return StringUtils::EqualsLowercase(name, known); });
}

enum class CssMode { SelectorPrelude, AtRulePrelude, DeclarationName, DeclarationValue };
enum class BlockKind { RuleList, Declarations, FontFace };

struct CssState {
  explicit CssState(CssSourceRegionKind kind)
      : mode(kind == CssSourceRegionKind::Stylesheet ? CssMode::SelectorPrelude
                                                     : CssMode::DeclarationName),
        rootDeclarationList(kind == CssSourceRegionKind::DeclarationList) {
    if (kind == CssSourceRegionKind::PropertyValue) {
      mode = CssMode::DeclarationValue;
    }
  }

  CssMode mode;
  bool rootDeclarationList = false;
  std::vector<BlockKind> blocks;
  std::string atRuleName;
  bool classNameNext = false;
  int parenthesisDepth = 0;
  int stringFunctionDepth = -1;
};

bool InDeclarationList(const CssState& state) {
  return !state.blocks.empty() ? state.blocks.back() != BlockKind::RuleList
                               : state.rootDeclarationList;
}

ColorIndex DeclarationNameColor(std::string_view name, const CssState& state) {
  if (!state.blocks.empty() && state.blocks.back() == BlockKind::FontFace) {
    return css::IdentifyFontFaceDescriptor(name) == css::FontFaceDescriptor::Unknown
               ? ColorIndex::ErrorMarker
               : ColorIndex::KnownIdentifier;
  }
  if (name.starts_with("--")) {
    return ColorIndex::Identifier;
  }
  return IsKnownProperty(name) ? ColorIndex::KnownIdentifier : ColorIndex::ErrorMarker;
}

ColorIndex IdentColor(const Token::Ident& ident, CssState& state) {
  const std::string_view name = ident.value;
  if (state.classNameNext) {
    state.classNameNext = false;
    return ColorIndex::Identifier;
  }
  switch (state.mode) {
    case CssMode::SelectorPrelude: return ColorIndex::Keyword;
    case CssMode::AtRulePrelude: return ColorIndex::Identifier;
    case CssMode::DeclarationName: return DeclarationNameColor(name, state);
    case CssMode::DeclarationValue:
      return name.starts_with("--") ? ColorIndex::Identifier : ColorIndex::Keyword;
  }
  return ColorIndex::Default;
}

void OpenBlock(CssState& state) {
  BlockKind block = BlockKind::RuleList;
  if (state.mode == CssMode::SelectorPrelude) {
    block = BlockKind::Declarations;
  } else if (state.mode == CssMode::AtRulePrelude && state.atRuleName == "font-face") {
    block = BlockKind::FontFace;
  }
  state.blocks.push_back(block);
  state.mode = InDeclarationList(state) ? CssMode::DeclarationName : CssMode::SelectorPrelude;
  state.atRuleName.clear();
}

void CloseBlock(CssState& state) {
  if (!state.blocks.empty()) {
    state.blocks.pop_back();
  }
  state.mode = InDeclarationList(state) ? CssMode::DeclarationName : CssMode::SelectorPrelude;
}

void EndDeclaration(CssState& state) {
  state.mode = InDeclarationList(state) ? CssMode::DeclarationName : CssMode::SelectorPrelude;
  state.atRuleName.clear();
}

ColorIndex StructuralColor(const Token& token, CssState& state) {
  if (token.is<Token::CurlyBracket>()) {
    OpenBlock(state);
  } else if (token.is<Token::CloseCurlyBracket>()) {
    CloseBlock(state);
  } else if (token.is<Token::Colon>() && state.mode == CssMode::DeclarationName) {
    state.mode = CssMode::DeclarationValue;
  } else if (token.is<Token::Semicolon>() && state.parenthesisDepth == 0) {
    EndDeclaration(state);
  }
  return ColorIndex::Punctuation;
}

std::optional<ColorIndex> NestedStringColor(const Token& token, CssState& state) {
  if (state.stringFunctionDepth < 0) {
    return std::nullopt;
  }
  if (token.is<Token::Parenthesis>()) {
    ++state.parenthesisDepth;
  } else if (token.is<Token::CloseParenthesis>()) {
    --state.parenthesisDepth;
    if (state.parenthesisDepth < state.stringFunctionDepth) {
      state.stringFunctionDepth = -1;
    }
  }
  return ColorIndex::String;
}

std::optional<ColorIndex> NamedTokenColor(const Token& token, CssState& state) {
  if (const auto* ident = token.tryGet<Token::Ident>()) {
    return IdentColor(*ident, state);
  }
  if (const auto* function = token.tryGet<Token::Function>()) {
    ++state.parenthesisDepth;
    if (function->name.equalsLowercase("url")) {
      state.stringFunctionDepth = state.parenthesisDepth;
      return ColorIndex::String;
    }
    return ColorIndex::Keyword;
  }
  if (const auto* at = token.tryGet<Token::AtKeyword>()) {
    state.mode = CssMode::AtRulePrelude;
    state.atRuleName = StringUtils::ToLowerAscii(at->value);
    return ColorIndex::Keyword;
  }
  if (token.is<Token::Hash>()) {
    return ColorIndex::Identifier;
  }
  if (const auto* delim = token.tryGet<Token::Delim>()) {
    state.classNameNext = state.mode == CssMode::SelectorPrelude && delim->value == '.';
    return ColorIndex::Punctuation;
  }
  return std::nullopt;
}

std::optional<ColorIndex> LiteralTokenColor(const Token& token) {
  if (token.is<Token::Number>() || token.is<Token::Percentage>() || token.is<Token::Dimension>()) {
    return ColorIndex::Number;
  }
  if (token.is<Token::String>() || token.is<Token::Url>()) {
    return ColorIndex::String;
  }
  if (token.is<Token::BadString>() || token.is<Token::BadUrl>() || token.is<Token::ErrorToken>()) {
    return ColorIndex::ErrorMarker;
  }
  if (token.is<Token::Whitespace>()) {
    return ColorIndex::Default;
  }
  return std::nullopt;
}

ColorIndex TokenColor(const Token& token, CssState& state) {
  if (const auto color = NestedStringColor(token, state)) {
    return *color;
  }
  if (const auto color = NamedTokenColor(token, state)) {
    return *color;
  }
  if (const auto color = LiteralTokenColor(token)) {
    return *color;
  }
  if (token.is<Token::Parenthesis>()) {
    ++state.parenthesisDepth;
  } else if (token.is<Token::CloseParenthesis>()) {
    state.parenthesisDepth = std::max(0, state.parenthesisDepth - 1);
  }
  return StructuralColor(token, state);
}

}  // namespace

std::vector<CssSourceRegion> FindSvgCssSourceRegions(std::string_view source) {
  return FindRegions(source);
}

namespace {

std::optional<char32_t> BuiltinReference(std::string_view ref) {
  using namespace std::string_view_literals;
  static constexpr std::pair<std::string_view, char32_t> kNames[] = {{"&amp;"sv, U'&'},
                                                                     {"&apos;"sv, U'\''},
                                                                     {"&quot;"sv, U'"'},
                                                                     {"&lt;"sv, U'<'},
                                                                     {"&gt;"sv, U'>'}};
  for (const auto& [name, value] : kNames) {
    if (ref == name) {
      return value;
    }
  }
  return std::nullopt;
}

std::optional<char32_t> ReferenceValue(std::string_view ref) {
  if (!ref.starts_with("&#")) {
    return BuiltinReference(ref);
  }
  ref.remove_prefix(2);
  ref.remove_suffix(1);  // XMLTokenizer already recognized the closing semicolon.
  const bool hex = ref.starts_with('x');
  ref.remove_prefix(hex ? 1 : 0);
  std::uint32_t value = 0;
  const auto [end, error] =
      std::from_chars(ref.data(), ref.data() + ref.size(), value, hex ? 16 : 10);
  if (error != std::errc{} || end != ref.data() + ref.size() || !xml::IsValidXmlCharacter(value)) {
    return std::nullopt;
  }
  return static_cast<char32_t>(value);
}

struct CssTextPiece {
  std::size_t sourceStart;
  std::size_t sourceEnd;
  std::string_view text;
};

template <class Visit>
void VisitCssText(std::string_view raw, Visit visit) {
  xml::Tokenize(raw, [&](const XMLToken& token) {
    const auto begin = TokenStart(token);
    const auto end = TokenEnd(token);
    const auto bytes = token.text(raw);
    switch (token.type) {
      case XMLTokenType::CData: {
        constexpr std::size_t kOpen = std::string_view("<![CDATA[").size();
        const auto contentEnd = end - 3;  // CData tokens include both delimiters.
        visit(CssTextPiece{begin + kOpen, contentEnd,
                           raw.substr(begin + kOpen, contentEnd - begin - kOpen)});
        break;
      }
      case XMLTokenType::EntityRef: {
        if (const auto cp = ReferenceValue(bytes)) {
          char utf8[4];
          const auto length = Utf8::Append(*cp, utf8) - utf8;
          visit(CssTextPiece{begin, end, std::string_view(utf8, length)});
          break;
        }
        [[fallthrough]];
      }
      case XMLTokenType::TextContent:
      case XMLTokenType::ErrorRecovery: visit(CssTextPiece{begin, end, bytes}); break;
      default: break;
    }
  });
}

// Stream one token and at most one skipped comment gap at a time. The editor can paint a
// complete stylesheet without keeping an array proportional to its number of tokens.
class TokenColors {
public:
  TokenColors(std::string_view text, CssSourceRegionKind kind)
      : tokenizer_(text), state_(kind), size_(text.size()) {}

  std::optional<CssColorSpan> next() {
    if (pending_) {
      return std::exchange(pending_, std::nullopt);
    }
    if (end_ == size_) {
      return std::nullopt;
    }
    const auto token = tokenizer_.next();
    const auto previous = end_;
    if (token.is<Token::EofToken>()) {
      end_ = size_;
      return CssColorSpan{previous, end_, ColorIndex::Comment};
    }
    end_ = tokenizer_.endOffset();
    const auto start = token.offset().offset.value_or(previous);
    const CssColorSpan tokenSpan{start, end_, TokenColor(token, state_)};
    if (previous < start) {
      pending_ = tokenSpan;
      return CssColorSpan{previous, start, ColorIndex::Comment};
    }
    return tokenSpan;
  }

private:
  css::parser::details::Tokenizer tokenizer_;
  CssState state_;
  std::size_t size_;
  std::size_t end_ = 0;
  std::optional<CssColorSpan> pending_;
};

}  // namespace

void ClassifySvgCssRegion(std::string_view source, const CssSourceRegion& region,
                          const std::function<void(CssColorSpan)>& paint) {
  const auto raw = source.substr(region.startOffset, region.endOffset - region.startOffset);
  if (raw.find_first_of("&<") == raw.npos) {
    TokenColors tokens(raw, region.kind);
    while (const auto span = tokens.next()) {
      paint({region.startOffset + span->startOffset, region.startOffset + span->endOffset,
             span->color});
    }
    return;
  }
  std::string decoded;
  VisitCssText(raw, [&](const CssTextPiece& piece) { decoded.append(piece.text); });
  TokenColors tokens(decoded, region.kind);
  auto span = tokens.next();
  std::size_t cssStart = 0;
  VisitCssText(raw, [&](const CssTextPiece& piece) {
    const auto cssEnd = cssStart + piece.text.size();
    const bool literal = piece.sourceEnd - piece.sourceStart == piece.text.size();
    while (span && span->startOffset < cssEnd) {
      const auto begin = literal
                             ? piece.sourceStart + std::max(span->startOffset, cssStart) - cssStart
                             : piece.sourceStart;
      const auto end = literal ? piece.sourceStart + std::min(span->endOffset, cssEnd) - cssStart
                               : piece.sourceEnd;
      paint({region.startOffset + begin, region.startOffset + end, span->color});
      if (span->endOffset > cssEnd) {
        break;
      }
      span = tokens.next();
    }
    cssStart = cssEnd;
  });
}
}  // namespace donner::editor
