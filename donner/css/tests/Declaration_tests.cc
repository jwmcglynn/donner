#include "donner/css/Declaration.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/css/CSS.h"

namespace donner::css {

using testing::Eq;

// Helper: parse a style string and return declarations.
static std::vector<Declaration> parse(std::string_view str) {
  return CSS::ParseStyleAttribute(str);
}

// ===========================================================================
// Token::toCssText
// ===========================================================================

TEST(TokenToCssText, Ident) {
  EXPECT_EQ(Token(Token::Ident("red"), 0).toCssText(), "red");
  EXPECT_EQ(Token(Token::Ident("sans-serif"), 0).toCssText(), "sans-serif");
  EXPECT_EQ(Token(Token::Ident("inherit"), 0).toCssText(), "inherit");
}

TEST(TokenToCssText, Number) {
  EXPECT_EQ(Token(Token::Number(0.8, "0.8", NumberType::Number), 0).toCssText(), "0.8");
  EXPECT_EQ(Token(Token::Number(42, "42", NumberType::Integer), 0).toCssText(), "42");
  EXPECT_EQ(Token(Token::Number(0, "0", NumberType::Integer), 0).toCssText(), "0");
  EXPECT_EQ(Token(Token::Number(-1.5, "-1.5", NumberType::Number), 0).toCssText(), "-1.5");
}

TEST(TokenToCssText, Percentage) {
  EXPECT_EQ(Token(Token::Percentage(50, "50", NumberType::Integer), 0).toCssText(), "50%");
  EXPECT_EQ(Token(Token::Percentage(0, "0", NumberType::Integer), 0).toCssText(), "0%");
  EXPECT_EQ(Token(Token::Percentage(100, "100", NumberType::Integer), 0).toCssText(), "100%");
  EXPECT_EQ(Token(Token::Percentage(33.3, "33.3", NumberType::Number), 0).toCssText(), "33.3%");
}

TEST(TokenToCssText, Dimension) {
  EXPECT_EQ(
      Token(Token::Dimension(10, "px", Lengthd::Unit::Px, "10", NumberType::Integer), 0)
          .toCssText(),
      "10px");
  EXPECT_EQ(
      Token(Token::Dimension(2.5, "em", Lengthd::Unit::Em, "2.5", NumberType::Number), 0)
          .toCssText(),
      "2.5em");
  EXPECT_EQ(
      Token(Token::Dimension(45, "deg", std::nullopt, "45", NumberType::Integer), 0).toCssText(),
      "45deg");
  EXPECT_EQ(
      Token(Token::Dimension(0, "rem", Lengthd::Unit::Rem, "0", NumberType::Integer), 0)
          .toCssText(),
      "0rem");
}

TEST(TokenToCssText, Hash) {
  EXPECT_EQ(Token(Token::Hash(Token::Hash::Type::Id, "ff0000"), 0).toCssText(), "#ff0000");
  EXPECT_EQ(Token(Token::Hash(Token::Hash::Type::Unrestricted, "123"), 0).toCssText(), "#123");
  EXPECT_EQ(Token(Token::Hash(Token::Hash::Type::Id, "myId"), 0).toCssText(), "#myId");
}

TEST(TokenToCssText, String) {
  EXPECT_EQ(Token(Token::String("hello"), 0).toCssText(), "\"hello\"");
  EXPECT_EQ(Token(Token::String(""), 0).toCssText(), "\"\"");
  EXPECT_EQ(Token(Token::String("with spaces"), 0).toCssText(), "\"with spaces\"");
}

TEST(TokenToCssText, Delim) {
  EXPECT_EQ(Token(Token::Delim('+'), 0).toCssText(), "+");
  EXPECT_EQ(Token(Token::Delim('-'), 0).toCssText(), "-");
  EXPECT_EQ(Token(Token::Delim('*'), 0).toCssText(), "*");
  EXPECT_EQ(Token(Token::Delim('/'), 0).toCssText(), "/");
  EXPECT_EQ(Token(Token::Delim('.'), 0).toCssText(), ".");
  EXPECT_EQ(Token(Token::Delim('!'), 0).toCssText(), "!");
}

TEST(TokenToCssText, Whitespace) {
  // All whitespace normalizes to a single space.
  EXPECT_EQ(Token(Token::Whitespace("  "), 0).toCssText(), " ");
  EXPECT_EQ(Token(Token::Whitespace("\t"), 0).toCssText(), " ");
  EXPECT_EQ(Token(Token::Whitespace(" \n "), 0).toCssText(), " ");
}

TEST(TokenToCssText, Punctuation) {
  EXPECT_EQ(Token(Token::Comma{}, 0).toCssText(), ",");
  EXPECT_EQ(Token(Token::Colon{}, 0).toCssText(), ":");
  EXPECT_EQ(Token(Token::Semicolon{}, 0).toCssText(), ";");
}

TEST(TokenToCssText, Url) {
  EXPECT_EQ(Token(Token::Url("image.png"), 0).toCssText(), "url(image.png)");
  EXPECT_EQ(Token(Token::Url("#gradient"), 0).toCssText(), "url(#gradient)");
}

TEST(TokenToCssText, Function) {
  EXPECT_EQ(Token(Token::Function("rgb"), 0).toCssText(), "rgb(");
  EXPECT_EQ(Token(Token::Function("translate"), 0).toCssText(), "translate(");
}

TEST(TokenToCssText, Brackets) {
  EXPECT_EQ(Token(Token::SquareBracket{}, 0).toCssText(), "[");
  EXPECT_EQ(Token(Token::CloseSquareBracket{}, 0).toCssText(), "]");
  EXPECT_EQ(Token(Token::Parenthesis{}, 0).toCssText(), "(");
  EXPECT_EQ(Token(Token::CloseParenthesis{}, 0).toCssText(), ")");
  EXPECT_EQ(Token(Token::CurlyBracket{}, 0).toCssText(), "{");
  EXPECT_EQ(Token(Token::CloseCurlyBracket{}, 0).toCssText(), "}");
}

TEST(TokenToCssText, CDOAndCDC) {
  EXPECT_EQ(Token(Token::CDO{}, 0).toCssText(), "<!--");
  EXPECT_EQ(Token(Token::CDC{}, 0).toCssText(), "-->");
}

TEST(TokenToCssText, AtKeyword) {
  EXPECT_EQ(Token(Token::AtKeyword("media"), 0).toCssText(), "@media");
  EXPECT_EQ(Token(Token::AtKeyword("import"), 0).toCssText(), "@import");
}

TEST(TokenToCssText, ErrorAndSpecialTokensReturnEmpty) {
  EXPECT_EQ(Token(Token::EofToken{}, 0).toCssText(), "");
  EXPECT_EQ(Token(Token::BadString("partial"), 0).toCssText(), "");
  EXPECT_EQ(Token(Token::BadUrl{}, 0).toCssText(), "");
  EXPECT_EQ(Token(Token::ErrorToken(Token::ErrorToken::Type::EofInString), 0).toCssText(), "");
  EXPECT_EQ(Token(Token::ErrorToken(Token::ErrorToken::Type::EofInComment), 0).toCssText(), "");
  EXPECT_EQ(Token(Token::ErrorToken(Token::ErrorToken::Type::EofInUrl), 0).toCssText(), "");
}

// ===========================================================================
// ComponentValue::toCssText
// ===========================================================================

TEST(ComponentValueToCssText, TokenIdent) {
  ComponentValue cv(Token(Token::Ident("red"), 0));
  EXPECT_EQ(cv.toCssText(), "red");
}

TEST(ComponentValueToCssText, TokenNumber) {
  ComponentValue cv(Token(Token::Number(42, "42", NumberType::Integer), 0));
  EXPECT_EQ(cv.toCssText(), "42");
}

TEST(ComponentValueToCssText, TokenWhitespace) {
  ComponentValue cv(Token(Token::Whitespace(" "), 0));
  EXPECT_EQ(cv.toCssText(), " ");
}

TEST(ComponentValueToCssText, FunctionWithArgs) {
  Function func("rgb", FileOffset::Offset(0));
  func.values.emplace_back(Token(Token::Number(255, "255", NumberType::Integer), 4));
  func.values.emplace_back(Token(Token::Comma{}, 7));
  func.values.emplace_back(Token(Token::Whitespace(" "), 8));
  func.values.emplace_back(Token(Token::Number(0, "0", NumberType::Integer), 9));
  func.values.emplace_back(Token(Token::Comma{}, 10));
  func.values.emplace_back(Token(Token::Whitespace(" "), 11));
  func.values.emplace_back(Token(Token::Number(0, "0", NumberType::Integer), 12));

  ComponentValue cv(std::move(func));
  EXPECT_EQ(cv.toCssText(), "rgb(255, 0, 0)");
}

TEST(ComponentValueToCssText, EmptyFunction) {
  Function func("var", FileOffset::Offset(0));

  ComponentValue cv(std::move(func));
  EXPECT_EQ(cv.toCssText(), "var()");
}

TEST(ComponentValueToCssText, NestedFunction) {
  // Build calc(100% - 20px)
  Function func("calc", FileOffset::Offset(0));
  func.values.emplace_back(Token(Token::Percentage(100, "100", NumberType::Integer), 5));
  func.values.emplace_back(Token(Token::Whitespace(" "), 9));
  func.values.emplace_back(Token(Token::Delim('-'), 10));
  func.values.emplace_back(Token(Token::Whitespace(" "), 11));
  func.values.emplace_back(
      Token(Token::Dimension(20, "px", Lengthd::Unit::Px, "20", NumberType::Integer), 12));

  ComponentValue cv(std::move(func));
  EXPECT_EQ(cv.toCssText(), "calc(100% - 20px)");
}

TEST(ComponentValueToCssText, FunctionWithStringArg) {
  Function func("url", FileOffset::Offset(0));
  func.values.emplace_back(Token(Token::String("image.png"), 4));

  ComponentValue cv(std::move(func));
  EXPECT_EQ(cv.toCssText(), "url(\"image.png\")");
}

TEST(ComponentValueToCssText, SimpleBlockSquare) {
  SimpleBlock block(Token::indexOf<Token::SquareBracket>(), FileOffset::Offset(0));
  block.values.emplace_back(Token(Token::Ident("href"), 1));

  ComponentValue cv(std::move(block));
  EXPECT_EQ(cv.toCssText(), "[href]");
}

TEST(ComponentValueToCssText, SimpleBlockCurly) {
  SimpleBlock block(Token::indexOf<Token::CurlyBracket>(), FileOffset::Offset(0));
  block.values.emplace_back(Token(Token::Whitespace(" "), 1));
  block.values.emplace_back(Token(Token::Ident("content"), 2));
  block.values.emplace_back(Token(Token::Whitespace(" "), 9));

  ComponentValue cv(std::move(block));
  EXPECT_EQ(cv.toCssText(), "{ content }");
}

TEST(ComponentValueToCssText, SimpleBlockParenthesis) {
  SimpleBlock block(Token::indexOf<Token::Parenthesis>(), FileOffset::Offset(0));
  block.values.emplace_back(Token(Token::Number(1, "1", NumberType::Integer), 1));
  block.values.emplace_back(Token(Token::Whitespace(" "), 2));
  block.values.emplace_back(Token(Token::Number(2, "2", NumberType::Integer), 3));

  ComponentValue cv(std::move(block));
  EXPECT_EQ(cv.toCssText(), "(1 2)");
}

TEST(ComponentValueToCssText, EmptySimpleBlock) {
  SimpleBlock block(Token::indexOf<Token::SquareBracket>(), FileOffset::Offset(0));

  ComponentValue cv(std::move(block));
  EXPECT_EQ(cv.toCssText(), "[]");
}

// ===========================================================================
// Declaration::toCssText
// ===========================================================================

TEST(DeclarationToCssText, BasicDeclaration) {
  auto decls = parse("fill: red");
  ASSERT_EQ(decls.size(), 1);
  EXPECT_EQ(decls[0].toCssText(), "fill: red");
}

TEST(DeclarationToCssText, DeclarationWithImportant) {
  auto decls = parse("fill: red !important");
  ASSERT_EQ(decls.size(), 1);
  EXPECT_EQ(decls[0].toCssText(), "fill: red !important");
}

TEST(DeclarationToCssText, NumericValue) {
  auto decls = parse("opacity: 0.8");
  ASSERT_EQ(decls.size(), 1);
  EXPECT_EQ(decls[0].toCssText(), "opacity: 0.8");
}

TEST(DeclarationToCssText, DimensionValue) {
  auto decls = parse("stroke-width: 2px");
  ASSERT_EQ(decls.size(), 1);
  EXPECT_EQ(decls[0].toCssText(), "stroke-width: 2px");
}

TEST(DeclarationToCssText, ColorHash) {
  auto decls = parse("fill: #ff0000");
  ASSERT_EQ(decls.size(), 1);
  EXPECT_EQ(decls[0].toCssText(), "fill: #ff0000");
}

TEST(DeclarationToCssText, FunctionValue) {
  auto decls = parse("fill: rgb(255, 0, 0)");
  ASSERT_EQ(decls.size(), 1);
  EXPECT_EQ(decls[0].toCssText(), "fill: rgb(255, 0, 0)");
}

TEST(DeclarationToCssText, UrlValue) {
  auto decls = parse("fill: url(#gradient)");
  ASSERT_EQ(decls.size(), 1);
  EXPECT_EQ(decls[0].toCssText(), "fill: url(#gradient)");
}

TEST(DeclarationToCssText, MultipleValues) {
  auto decls = parse("font-family: Arial, sans-serif");
  ASSERT_EQ(decls.size(), 1);
  EXPECT_EQ(decls[0].toCssText(), "font-family: Arial, sans-serif");
}

TEST(DeclarationToCssText, MultipleDeclarations) {
  auto decls = parse("fill: red; stroke: blue");
  ASSERT_EQ(decls.size(), 2);
  EXPECT_EQ(decls[0].toCssText(), "fill: red");
  EXPECT_EQ(decls[1].toCssText(), "stroke: blue");
}

TEST(DeclarationToCssText, PercentageValue) {
  auto decls = parse("opacity: 50%");
  ASSERT_EQ(decls.size(), 1);
  EXPECT_EQ(decls[0].toCssText(), "opacity: 50%");
}

// ===========================================================================
// Declaration round-trip: parse -> toCssText -> parse
// ===========================================================================

TEST(DeclarationRoundTrip, SimpleProperties) {
  const std::vector<std::string_view> inputs = {
      "fill: red",
      "stroke: blue",
      "opacity: 0.5",
      "visibility: hidden",
      "display: none",
  };

  for (const auto& input : inputs) {
    auto decls = parse(input);
    ASSERT_EQ(decls.size(), 1) << "Failed to parse: " << input;
    const std::string serialized = decls[0].toCssText();

    auto reparsed = parse(serialized);
    ASSERT_EQ(reparsed.size(), 1) << "Failed to reparse: " << serialized;
    EXPECT_EQ(reparsed[0].name, decls[0].name) << "Name mismatch for: " << input;
    EXPECT_EQ(reparsed[0].values.size(), decls[0].values.size())
        << "Value count mismatch for: " << input;
  }
}

TEST(DeclarationRoundTrip, ComplexValues) {
  const std::vector<std::string_view> inputs = {
      "fill: #ff0000",
      "fill: rgb(255, 0, 0)",
      "fill: url(#gradient)",
      "stroke-width: 2px",
      "font-family: Arial, sans-serif",
  };

  for (const auto& input : inputs) {
    auto decls = parse(input);
    ASSERT_EQ(decls.size(), 1) << "Failed to parse: " << input;
    const std::string serialized = decls[0].toCssText();

    auto reparsed = parse(serialized);
    ASSERT_EQ(reparsed.size(), 1) << "Failed to reparse serialized: " << serialized;
    EXPECT_EQ(reparsed[0].name, decls[0].name) << "Name mismatch for: " << input;
  }
}

TEST(DeclarationRoundTrip, MultiDeclarationStyle) {
  const std::string_view input = "fill: red; stroke: blue; opacity: 0.5";
  auto decls = parse(input);
  ASSERT_EQ(decls.size(), 3);

  // Reconstruct as a merged style and reparse.
  std::string serialized;
  for (size_t i = 0; i < decls.size(); ++i) {
    if (i > 0) {
      serialized += "; ";
    }
    serialized += decls[i].toCssText();
  }

  auto reparsed = parse(serialized);
  ASSERT_EQ(reparsed.size(), 3);
  for (size_t i = 0; i < decls.size(); ++i) {
    EXPECT_EQ(reparsed[i].name, decls[i].name) << "Name mismatch at index " << i;
  }
}

// ===========================================================================
// mergeStyleDeclarations
// ===========================================================================

TEST(MergeStyleDeclarations, EmptyInputs) {
  EXPECT_EQ(mergeStyleDeclarations({}, {}), "");

  auto updates = parse("stroke: green");
  EXPECT_EQ(mergeStyleDeclarations({}, updates), "stroke: green");

  auto existing = parse("fill: red");
  EXPECT_EQ(mergeStyleDeclarations(existing, {}), "fill: red");
}

TEST(MergeStyleDeclarations, AddsNewProperty) {
  auto existing = parse("fill: red; opacity: 0.8");
  auto updates = parse("visibility: hidden");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates),
            "fill: red; opacity: 0.8; visibility: hidden");
}

TEST(MergeStyleDeclarations, OverridesExistingProperty) {
  auto existing = parse("fill: red; stroke: blue; opacity: 0.8");
  auto updates = parse("stroke: green; visibility: hidden");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates),
            "fill: red; opacity: 0.8; stroke: green; visibility: hidden");
}

TEST(MergeStyleDeclarations, OverridesAllDuplicateExistingProperties) {
  auto existing = parse("stroke: blue; fill: red; stroke: orange");
  auto updates = parse("stroke: green");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates), "fill: red; stroke: green");
}

TEST(MergeStyleDeclarations, KeepsLastUpdatedDuplicateProperty) {
  auto existing = parse("fill: red");
  auto updates = parse("stroke: blue; stroke: green");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates), "fill: red; stroke: green");
}

TEST(MergeStyleDeclarations, NormalizesSpacing) {
  auto existing = parse(" fill: red ; opacity: 0.8");
  auto updates = parse(" visibility: hidden ");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates),
            "fill: red; opacity: 0.8; visibility: hidden");
}

TEST(MergeStyleDeclarations, CaseInsensitivePropertyNames) {
  auto existing = parse("fill: red; STROKE: blue");
  auto updates = parse("stroke: green");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates), "fill: red; stroke: green");
}

TEST(MergeStyleDeclarations, ComplexValuesUrl) {
  auto existing = parse("fill: url(#gradient)");
  auto updates = parse("fill: red");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates), "fill: red");
}

TEST(MergeStyleDeclarations, ComplexValuesColor) {
  auto existing = parse("fill: #ff0000");
  auto updates = parse("fill: rgb(0, 255, 0)");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates), "fill: rgb(0, 255, 0)");
}

TEST(MergeStyleDeclarations, ComplexValuesTransform) {
  auto existing = parse("transform: translate(10px, 20px)");
  auto updates = parse("transform: rotate(45deg)");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates), "transform: rotate(45deg)");
}

TEST(MergeStyleDeclarations, PreservesImportant) {
  auto existing = parse("fill: red !important");
  auto updates = parse("stroke: blue");
  const std::string merged = mergeStyleDeclarations(existing, updates);
  EXPECT_THAT(merged, testing::HasSubstr("fill: red !important"));
  EXPECT_THAT(merged, testing::HasSubstr("stroke: blue"));
}

TEST(MergeStyleDeclarations, OverridesImportantWithNonImportant) {
  auto existing = parse("fill: red !important");
  auto updates = parse("fill: blue");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates), "fill: blue");
}

TEST(MergeStyleDeclarations, MultipleOverrides) {
  auto existing = parse("fill: red; stroke: blue; opacity: 0.5; visibility: visible");
  auto updates = parse("fill: green; opacity: 1.0");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates),
            "stroke: blue; visibility: visible; fill: green; opacity: 1.0");
}

TEST(MergeStyleDeclarations, AllPropertiesOverridden) {
  auto existing = parse("fill: red; stroke: blue");
  auto updates = parse("fill: green; stroke: orange");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates), "fill: green; stroke: orange");
}

TEST(MergeStyleDeclarations, EmptyExistingMultipleUpdates) {
  auto updates = parse("fill: red; stroke: blue; opacity: 0.5");
  EXPECT_EQ(mergeStyleDeclarations({}, updates), "fill: red; stroke: blue; opacity: 0.5");
}

TEST(MergeStyleDeclarations, PercentageValues) {
  auto existing = parse("width: 50%");
  auto updates = parse("width: 100%");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates), "width: 100%");
}

TEST(MergeStyleDeclarations, DimensionValues) {
  auto existing = parse("stroke-width: 2px");
  auto updates = parse("stroke-width: 4em");
  EXPECT_EQ(mergeStyleDeclarations(existing, updates), "stroke-width: 4em");
}

}  // namespace donner::css
