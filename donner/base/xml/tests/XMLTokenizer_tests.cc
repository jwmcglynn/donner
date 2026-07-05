#include "donner/base/xml/XMLTokenizer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace donner::xml {

using testing::ElementsAre;
using testing::IsEmpty;

// Helper to collect all tokens from a source string.
std::vector<XMLToken> TokenizeAll(std::string_view source) {
  std::vector<XMLToken> tokens;
  Tokenize(source, [&](XMLToken token) { tokens.push_back(token); });
  return tokens;
}

// Convenience: extract the text of each token from the source.
struct TokenWithText {
  XMLTokenType type;
  std::string text;

  bool operator==(const TokenWithText& other) const = default;

  friend std::ostream& operator<<(std::ostream& os, const TokenWithText& t) {
    return os << t.type << "(\"" << t.text << "\")";
  }
};

std::vector<TokenWithText> TokenizeWithText(std::string_view source) {
  std::vector<TokenWithText> result;
  Tokenize(source, [&](XMLToken token) {
    result.push_back({token.type, std::string(token.text(source))});
  });
  return result;
}

std::string Reconstruct(std::string_view source) {
  std::string reconstructed;
  Tokenize(source, [&](XMLToken token) { reconstructed += token.text(source); });
  return reconstructed;
}

MATCHER_P2(Tok, type, text, "") {
  return arg.type == type && arg.text == text;
}

using T = XMLTokenType;

struct NoTokenSink {
  void operator()(XMLToken) const noexcept {}
};

static_assert(sizeof(NoTokenSink) == 1);

std::string ToString(XMLTokenType type) {
  std::ostringstream os;
  os << type;
  return os.str();
}

// =============================================================================
// Token helpers
// =============================================================================

TEST(XMLTokenType, OstreamOutput) {
  struct Case {
    XMLTokenType type;
    std::string_view output;
  };

  constexpr std::array<Case, 15> kCases = {{
      {T::TagOpen, "TagOpen"},
      {T::TagName, "TagName"},
      {T::TagClose, "TagClose"},
      {T::TagSelfClose, "TagSelfClose"},
      {T::AttributeName, "AttributeName"},
      {T::AttributeValue, "AttributeValue"},
      {T::Comment, "Comment"},
      {T::CData, "CData"},
      {T::TextContent, "TextContent"},
      {T::XmlDeclaration, "XmlDeclaration"},
      {T::Doctype, "Doctype"},
      {T::EntityRef, "EntityRef"},
      {T::ProcessingInstruction, "ProcessingInstruction"},
      {T::Whitespace, "Whitespace"},
      {T::ErrorRecovery, "ErrorRecovery"},
  }};

  for (const Case& testCase : kCases) {
    EXPECT_EQ(ToString(testCase.type), testCase.output);
  }

  EXPECT_EQ(ToString(static_cast<XMLTokenType>(255)), "Unknown");
}

TEST(XMLToken, TextReturnsEmptyForInvalidRanges) {
  constexpr std::string_view kSource = "abc";

  EXPECT_EQ(
      (XMLToken{T::TextContent, {FileOffset::Offset(1), FileOffset::Offset(3)}}).text(kSource),
      "bc");
  EXPECT_THAT(
      (XMLToken{T::TextContent, {FileOffset::EndOfString(), FileOffset::Offset(1)}}).text(kSource),
      IsEmpty());
  EXPECT_THAT(
      (XMLToken{T::TextContent, {FileOffset::Offset(1), FileOffset::EndOfString()}}).text(kSource),
      IsEmpty());
  EXPECT_THAT(
      (XMLToken{T::TextContent, {FileOffset::Offset(3), FileOffset::Offset(3)}}).text(kSource),
      IsEmpty());
  EXPECT_THAT(
      (XMLToken{T::TextContent, {FileOffset::Offset(1), FileOffset::Offset(4)}}).text(kSource),
      IsEmpty());
  EXPECT_THAT(
      (XMLToken{T::TextContent, {FileOffset::Offset(2), FileOffset::Offset(1)}}).text(kSource),
      IsEmpty());
}

// =============================================================================
// Basic element tokenization
// =============================================================================

TEST(XMLTokenizer, Empty) {
  EXPECT_THAT(TokenizeAll(""), IsEmpty());
}

TEST(XMLTokenizer, SelfClosingElement) {
  EXPECT_THAT(TokenizeWithText("<br/>"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "br"), Tok(T::TagSelfClose, "/>")));
}

TEST(XMLTokenizer, ElementWithChildren) {
  EXPECT_THAT(TokenizeWithText("<a>text</a>"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "a"), Tok(T::TagClose, ">"),
                          Tok(T::TextContent, "text"), Tok(T::TagOpen, "</"), Tok(T::TagName, "a"),
                          Tok(T::TagClose, ">")));
}

TEST(XMLTokenizer, Attributes) {
  EXPECT_THAT(TokenizeWithText(R"(<rect fill="red"/>)"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "rect"), Tok(T::Whitespace, " "),
                          Tok(T::AttributeName, "fill"), Tok(T::Whitespace, "="),
                          Tok(T::AttributeValue, R"("red")"), Tok(T::TagSelfClose, "/>")));
}

TEST(XMLTokenizer, MultipleAttributes) {
  EXPECT_THAT(TokenizeWithText(R"(<rect x="1" y="2"/>)"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "rect"), Tok(T::Whitespace, " "),
                          Tok(T::AttributeName, "x"), Tok(T::Whitespace, "="),
                          Tok(T::AttributeValue, R"("1")"), Tok(T::Whitespace, " "),
                          Tok(T::AttributeName, "y"), Tok(T::Whitespace, "="),
                          Tok(T::AttributeValue, R"("2")"), Tok(T::TagSelfClose, "/>")));
}

TEST(XMLTokenizer, SingleQuotedAttribute) {
  EXPECT_THAT(TokenizeWithText("<a b='c'/>"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "a"), Tok(T::Whitespace, " "),
                          Tok(T::AttributeName, "b"), Tok(T::Whitespace, "="),
                          Tok(T::AttributeValue, "'c'"), Tok(T::TagSelfClose, "/>")));
}

TEST(XMLTokenizer, WhitespaceAroundEquals) {
  EXPECT_THAT(
      TokenizeWithText(R"(<a x = "1" />)"),
      ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "a"), Tok(T::Whitespace, " "),
                  Tok(T::AttributeName, "x"), Tok(T::Whitespace, " "), Tok(T::Whitespace, "="),
                  Tok(T::Whitespace, " "), Tok(T::AttributeValue, R"("1")"),
                  Tok(T::Whitespace, " "), Tok(T::TagSelfClose, "/>")));
}

TEST(XMLTokenizer, TabNewlineAndCarriageReturnWhitespaceAroundAttributeEquals) {
  EXPECT_THAT(TokenizeWithText("<a\tb\n=\r\"c\"/>"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "a"), Tok(T::Whitespace, "\t"),
                          Tok(T::AttributeName, "b"), Tok(T::Whitespace, "\n"),
                          Tok(T::Whitespace, "="), Tok(T::Whitespace, "\r"),
                          Tok(T::AttributeValue, R"("c")"), Tok(T::TagSelfClose, "/>")));
}

TEST(XMLTokenizer, NonAsciiAndPunctuationNameCharacters) {
  EXPECT_THAT(
      TokenizeWithText("<é.attr-1/>"),
      ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "é.attr-1"), Tok(T::TagSelfClose, "/>")));
}

// =============================================================================
// Special node types
// =============================================================================

TEST(XMLTokenizer, Comment) {
  EXPECT_THAT(TokenizeWithText("<!-- hello -->"), ElementsAre(Tok(T::Comment, "<!-- hello -->")));
}

TEST(XMLTokenizer, CData) {
  EXPECT_THAT(TokenizeWithText("<![CDATA[some <data>]]>"),
              ElementsAre(Tok(T::CData, "<![CDATA[some <data>]]>")));
}

TEST(XMLTokenizer, Doctype) {
  EXPECT_THAT(TokenizeWithText("<!DOCTYPE svg>"), ElementsAre(Tok(T::Doctype, "<!DOCTYPE svg>")));
}

TEST(XMLTokenizer, DoctypeWithInternalSubset) {
  EXPECT_THAT(TokenizeWithText("<!DOCTYPE test [ <!ENTITY a \"b\"> ]>"),
              ElementsAre(Tok(T::Doctype, "<!DOCTYPE test [ <!ENTITY a \"b\"> ]>")));
}

TEST(XMLTokenizer, XmlDeclaration) {
  EXPECT_THAT(TokenizeWithText(R"(<?xml version="1.0"?>)"),
              ElementsAre(Tok(T::XmlDeclaration, R"(<?xml version="1.0"?>)")));
}

TEST(XMLTokenizer, ProcessingInstruction) {
  EXPECT_THAT(TokenizeWithText("<?php echo 1; ?>"),
              ElementsAre(Tok(T::ProcessingInstruction, "<?php echo 1; ?>")));
}

TEST(XMLTokenizer, UnterminatedSpecialMarkupUsesErrorRecovery) {
  EXPECT_THAT(TokenizeWithText("<![CDATA[unterminated"),
              ElementsAre(Tok(T::ErrorRecovery, "<![CDATA[unterminated")));
  EXPECT_THAT(TokenizeWithText("<!DOCTYPE svg [ <!ENTITY a \"b\"> "),
              ElementsAre(Tok(T::ErrorRecovery, "<!DOCTYPE svg [ <!ENTITY a \"b\"> ")));
  EXPECT_THAT(TokenizeWithText(R"(<?xml version="1.0")"),
              ElementsAre(Tok(T::ErrorRecovery, R"(<?xml version="1.0")")));
  EXPECT_THAT(TokenizeWithText("<?pi no end"), ElementsAre(Tok(T::ErrorRecovery, "<?pi no end")));
}

// =============================================================================
// Mixed content
// =============================================================================

TEST(XMLTokenizer, MixedContent) {
  const auto tokens = TokenizeWithText("<svg><!-- c --><rect/></svg>");
  EXPECT_THAT(tokens,
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "svg"), Tok(T::TagClose, ">"),
                          Tok(T::Comment, "<!-- c -->"), Tok(T::TagOpen, "<"),
                          Tok(T::TagName, "rect"), Tok(T::TagSelfClose, "/>"),
                          Tok(T::TagOpen, "</"), Tok(T::TagName, "svg"), Tok(T::TagClose, ">")));
}

TEST(XMLTokenizer, TextBetweenElements) {
  EXPECT_THAT(TokenizeWithText("<a>hello<b/>world</a>"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "a"), Tok(T::TagClose, ">"),
                          Tok(T::TextContent, "hello"), Tok(T::TagOpen, "<"), Tok(T::TagName, "b"),
                          Tok(T::TagSelfClose, "/>"), Tok(T::TextContent, "world"),
                          Tok(T::TagOpen, "</"), Tok(T::TagName, "a"), Tok(T::TagClose, ">")));
}

// =============================================================================
// Gap-free property: token ranges reconstruct the input
// =============================================================================

TEST(XMLTokenizer, GapFreeSimple) {
  constexpr std::string_view kInput = R"(<svg xmlns="http://www.w3.org/2000/svg"><rect/></svg>)";
  std::string reconstructed;
  Tokenize(kInput, [&](XMLToken token) { reconstructed += token.text(kInput); });
  EXPECT_EQ(reconstructed, kInput);
}

TEST(XMLTokenizer, GapFreeComplex) {
  constexpr std::string_view kInput =
      R"(<?xml version="1.0"?><!-- comment --><!DOCTYPE svg><svg><text>hello</text></svg>)";
  std::string reconstructed;
  Tokenize(kInput, [&](XMLToken token) { reconstructed += token.text(kInput); });
  EXPECT_EQ(reconstructed, kInput);
}

TEST(XMLTokenizer, GapFreeWithWhitespace) {
  constexpr std::string_view kInput = R"(<a  x = "1"  y = '2' />)";
  std::string reconstructed;
  Tokenize(kInput, [&](XMLToken token) { reconstructed += token.text(kInput); });
  EXPECT_EQ(reconstructed, kInput);
}

// =============================================================================
// Error recovery
// =============================================================================

TEST(XMLTokenizer, UnterminatedComment) {
  const auto tokens = TokenizeWithText("<!-- no end");
  ASSERT_EQ(tokens.size(), 1u);
  EXPECT_EQ(tokens[0].type, T::ErrorRecovery);
}

TEST(XMLTokenizer, UnterminatedAttributeValue) {
  const auto tokens = TokenizeWithText(R"(<rect fill="unterminated)");
  // Should emit TagOpen, TagName, Whitespace, AttributeName, =, then ErrorRecovery.
  bool hasError = false;
  for (const auto& t : tokens) {
    if (t.type == T::ErrorRecovery) hasError = true;
  }
  EXPECT_TRUE(hasError) << "Expected an ErrorRecovery token for unterminated attribute";
}

TEST(XMLTokenizer, MalformedTagName) {
  // `<1bad>` - '1' is not a valid name start character.
  const auto tokens = TokenizeWithText("<1bad>");
  bool hasError = false;
  for (const auto& t : tokens) {
    if (t.type == T::ErrorRecovery) hasError = true;
  }
  EXPECT_TRUE(hasError) << "Expected ErrorRecovery for invalid element name";
}

TEST(XMLTokenizer, RecoveryAfterMalformedInput) {
  // The tokenizer should recover after the malformed part and continue
  // tokenizing the next valid element.
  const auto tokens = TokenizeWithText("<!-- unterminated <rect/>");
  // The unterminated comment should consume everything until EOF because
  // `-->` never appears. But if the tokenizer error-recovers on `<rect`,
  // it might emit something for that. Key assertion: at least one token
  // exists, and we don't crash.
  EXPECT_FALSE(tokens.empty());
}

TEST(XMLTokenizer, NamespacedElement) {
  EXPECT_THAT(
      TokenizeWithText("<ns:elem/>"),
      ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "ns:elem"), Tok(T::TagSelfClose, "/>")));
}

TEST(XMLTokenizer, NamespacedAttribute) {
  EXPECT_THAT(TokenizeWithText(R"(<a xmlns:xlink="http://foo"/>)"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "a"), Tok(T::Whitespace, " "),
                          Tok(T::AttributeName, "xmlns:xlink"), Tok(T::Whitespace, "="),
                          Tok(T::AttributeValue, R"("http://foo")"), Tok(T::TagSelfClose, "/>")));
}

// =============================================================================
// Edge cases
// =============================================================================

TEST(XMLTokenizer, EmptyElement) {
  EXPECT_THAT(TokenizeWithText("<a></a>"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "a"), Tok(T::TagClose, ">"),
                          Tok(T::TagOpen, "</"), Tok(T::TagName, "a"), Tok(T::TagClose, ">")));
}

TEST(XMLTokenizer, JustText) {
  EXPECT_THAT(TokenizeWithText("hello world"), ElementsAre(Tok(T::TextContent, "hello world")));
}

TEST(XMLTokenizer, EntityInTextEmitsEntityRefWithoutExpanding) {
  // Entities are NOT expanded - the token text remains the exact source bytes.
  EXPECT_THAT(
      TokenizeWithText("a&amp;b&#x20;c&#32;d&notClosed"),
      ElementsAre(Tok(T::TextContent, "a"), Tok(T::EntityRef, "&amp;"), Tok(T::TextContent, "b"),
                  Tok(T::EntityRef, "&#x20;"), Tok(T::TextContent, "c"), Tok(T::EntityRef, "&#32;"),
                  Tok(T::TextContent, "d&notClosed")));
}

TEST(XMLTokenizer, InvalidEntityFormsRemainTextContent) {
  EXPECT_THAT(TokenizeWithText("& &#; &#x; &1; &name"),
              ElementsAre(Tok(T::TextContent, "& &#; &#x; &1; &name")));
}

TEST(XMLTokenizer, ClosingTagWhitespaceAndMalformedCloseRecover) {
  EXPECT_THAT(TokenizeWithText("</a \t>"),
              ElementsAre(Tok(T::TagOpen, "</"), Tok(T::TagName, "a"), Tok(T::Whitespace, " \t"),
                          Tok(T::TagClose, ">")));

  EXPECT_THAT(TokenizeWithText("</a attr><b/>"),
              ElementsAre(Tok(T::TagOpen, "</"), Tok(T::TagName, "a"), Tok(T::Whitespace, " "),
                          Tok(T::ErrorRecovery, "attr>"), Tok(T::TagOpen, "<"),
                          Tok(T::TagName, "b"), Tok(T::TagSelfClose, "/>")));
}

TEST(XMLTokenizer, AttributeNameRecoveryAndUnclosedOpeningTag) {
  EXPECT_THAT(TokenizeWithText(R"(<a ="x"><b/>)"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "a"), Tok(T::Whitespace, " "),
                          Tok(T::ErrorRecovery, R"(="x">)"), Tok(T::TagOpen, "<"),
                          Tok(T::TagName, "b"), Tok(T::TagSelfClose, "/>")));

  const auto tokens = TokenizeWithText("<a attr");
  ASSERT_FALSE(tokens.empty());
  EXPECT_EQ(tokens.back().type, T::ErrorRecovery);
  EXPECT_EQ(Reconstruct("<a attr"), "<a attr");
}

TEST(XMLTokenizer, RepresentativeCorpusReconstructsInputByteForByte) {
  constexpr std::array<std::string_view, 10> kCorpus = {
      "",
      "plain text",
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect fill="red"/></svg>)",
      R"(<?xml version="1.0"?><svg><!-- c --><text>a&amp;b</text></svg>)",
      R"(<!DOCTYPE svg [ <!ENTITY a "b"> ]><svg>&a;</svg>)",
      "<svg><![CDATA[<not markup>&still bytes]]></svg>",
      "<?xml-stylesheet href=\"style.css\"?><svg/>",
      R"(<a  x = "1"  y = '2' />)",
      "<svg><1bad><rect/></svg>",
      R"(<svg><rect fill="unterminated</svg>)",
  };

  for (std::string_view input : kCorpus) {
    EXPECT_EQ(Reconstruct(input), input) << input;
  }
}

TEST(XMLTokenizer, MalformedInputKeepsWellFormedPrefixThenErrorRecovery) {
  EXPECT_THAT(TokenizeWithText("<svg><1bad><rect/></svg>"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "svg"), Tok(T::TagClose, ">"),
                          Tok(T::TagOpen, "<"), Tok(T::ErrorRecovery, "1bad>"),
                          Tok(T::TagOpen, "<"), Tok(T::TagName, "rect"), Tok(T::TagSelfClose, "/>"),
                          Tok(T::TagOpen, "</"), Tok(T::TagName, "svg"), Tok(T::TagClose, ">")));
}

TEST(XMLTokenizer, EmptySinkInstantiatesAndRuns) {
  Tokenize("<svg><rect fill=\"red\"/></svg>", NoTokenSink{});
}

}  // namespace donner::xml
