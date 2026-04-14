#include "donner/base/xml/XMLTokenizer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
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

MATCHER_P2(Tok, type, text, "") {
  return arg.type == type && arg.text == text;
}

using T = XMLTokenType;

// =============================================================================
// Basic element tokenization
// =============================================================================

TEST(XMLTokenizer, Empty) {
  EXPECT_THAT(TokenizeAll(""), IsEmpty());
}

TEST(XMLTokenizer, SelfClosingElement) {
  EXPECT_THAT(TokenizeWithText("<br/>"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "br"),
                           Tok(T::TagSelfClose, "/>")));
}

TEST(XMLTokenizer, ElementWithChildren) {
  EXPECT_THAT(TokenizeWithText("<a>text</a>"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "a"), Tok(T::TagClose, ">"),
                           Tok(T::TextContent, "text"), Tok(T::TagOpen, "</"),
                           Tok(T::TagName, "a"), Tok(T::TagClose, ">")));
}

TEST(XMLTokenizer, Attributes) {
  EXPECT_THAT(
      TokenizeWithText(R"(<rect fill="red"/>)"),
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

// =============================================================================
// Special node types
// =============================================================================

TEST(XMLTokenizer, Comment) {
  EXPECT_THAT(TokenizeWithText("<!-- hello -->"),
              ElementsAre(Tok(T::Comment, "<!-- hello -->")));
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

// =============================================================================
// Mixed content
// =============================================================================

TEST(XMLTokenizer, MixedContent) {
  const auto tokens = TokenizeWithText("<svg><!-- c --><rect/></svg>");
  EXPECT_THAT(tokens, ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "svg"),
                                   Tok(T::TagClose, ">"), Tok(T::Comment, "<!-- c -->"),
                                   Tok(T::TagOpen, "<"), Tok(T::TagName, "rect"),
                                   Tok(T::TagSelfClose, "/>"), Tok(T::TagOpen, "</"),
                                   Tok(T::TagName, "svg"), Tok(T::TagClose, ">")));
}

TEST(XMLTokenizer, TextBetweenElements) {
  EXPECT_THAT(TokenizeWithText("<a>hello<b/>world</a>"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "a"), Tok(T::TagClose, ">"),
                           Tok(T::TextContent, "hello"), Tok(T::TagOpen, "<"),
                           Tok(T::TagName, "b"), Tok(T::TagSelfClose, "/>"),
                           Tok(T::TextContent, "world"), Tok(T::TagOpen, "</"),
                           Tok(T::TagName, "a"), Tok(T::TagClose, ">")));
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
  // `<1bad>` — '1' is not a valid name start character.
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
  EXPECT_THAT(TokenizeWithText("<ns:elem/>"),
              ElementsAre(Tok(T::TagOpen, "<"), Tok(T::TagName, "ns:elem"),
                           Tok(T::TagSelfClose, "/>")));
}

TEST(XMLTokenizer, NamespacedAttribute) {
  EXPECT_THAT(
      TokenizeWithText(R"(<a xmlns:xlink="http://foo"/>)"),
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

TEST(XMLTokenizer, EntityInTextNotExpanded) {
  // Entities are NOT expanded — they appear as part of TextContent.
  EXPECT_THAT(TokenizeWithText("a&amp;b"), ElementsAre(Tok(T::TextContent, "a&amp;b")));
}

}  // namespace donner::xml
