/// @file ComponentValueStream_tests.cc
/// @brief Unit tests for \ref donner::css::parser::details::ComponentValueStream.

#include "donner/css/parser/details/ComponentValueStream.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/Token.h"

namespace donner::css::parser::details {

namespace {

/// Build a ComponentValue wrapping a fresh Token at offset 0. A couple of
/// the multi-element fixtures bump offsets per element so that `currentOffset`
/// is meaningfully observable.
template <typename TokenType, typename... Args>
ComponentValue MakeTokenCV(uint32_t offset, Args&&... args) {
  return ComponentValue(Token(TokenType(std::forward<Args>(args)...), offset));
}

}  // namespace

TEST(ComponentValueStreamTest, EmptyInputIsImmediatelyEOF) {
  std::vector<ComponentValue> components;
  ComponentValueStream stream(components);

  EXPECT_TRUE(stream.isEOF());
  EXPECT_EQ(stream.remaining(), 0u);
  EXPECT_EQ(stream.peek(), nullptr);
  EXPECT_EQ(stream.peek(5), nullptr);  // Out-of-bounds peek is nullptr, not UB.
  EXPECT_EQ(stream.peekAs<Token>(), nullptr);
  EXPECT_FALSE(stream.peekIs<Token>());
  EXPECT_FALSE(stream.peekIsToken<Token::Ident>());
  EXPECT_EQ(stream.peekDelim(), std::nullopt);
  EXPECT_FALSE(stream.peekDelimIs('('));
  EXPECT_EQ(stream.currentOffset(), FileOffset::EndOfString());
}

TEST(ComponentValueStreamTest, SingleElementPeekThenAdvance) {
  std::vector<ComponentValue> components;
  components.push_back(MakeTokenCV<Token::Ident>(7, "hello"));
  ComponentValueStream stream(components);

  EXPECT_FALSE(stream.isEOF());
  EXPECT_EQ(stream.remaining(), 1u);
  EXPECT_EQ(stream.currentOffset(), FileOffset::Offset(7));

  // peek without consuming
  ASSERT_NE(stream.peek(), nullptr);
  EXPECT_TRUE(stream.peekIs<Token>());
  EXPECT_TRUE(stream.peekIsToken<Token::Ident>());
  ASSERT_NE(stream.peekAs<Token>(), nullptr);
  EXPECT_EQ(stream.peekAs<Token>()->get<Token::Ident>().value, "hello");

  // peek is idempotent
  EXPECT_EQ(stream.remaining(), 1u);
  EXPECT_FALSE(stream.isEOF());

  // advance consumes
  stream.advance();
  EXPECT_TRUE(stream.isEOF());
  EXPECT_EQ(stream.remaining(), 0u);
  EXPECT_EQ(stream.peek(), nullptr);
}

TEST(ComponentValueStreamTest, PeekAtForwardOffset) {
  std::vector<ComponentValue> components;
  components.push_back(MakeTokenCV<Token::Ident>(0, "foo"));
  components.push_back(MakeTokenCV<Token::Whitespace>(3, " "));
  components.push_back(MakeTokenCV<Token::Ident>(4, "bar"));
  ComponentValueStream stream(components);

  EXPECT_EQ(stream.remaining(), 3u);

  // Index 0: "foo"
  EXPECT_TRUE(stream.peekIsToken<Token::Ident>(0));
  EXPECT_EQ(stream.peekAs<Token>(0)->get<Token::Ident>().value, "foo");

  // Index 1: whitespace
  EXPECT_FALSE(stream.peekIsToken<Token::Ident>(1));
  EXPECT_TRUE(stream.peekIsToken<Token::Whitespace>(1));

  // Index 2: "bar"
  EXPECT_TRUE(stream.peekIsToken<Token::Ident>(2));
  EXPECT_EQ(stream.peekAs<Token>(2)->get<Token::Ident>().value, "bar");

  // Index 3 (past end): nullptr / false, never UB
  EXPECT_EQ(stream.peek(3), nullptr);
  EXPECT_EQ(stream.peekAs<Token>(3), nullptr);
  EXPECT_FALSE(stream.peekIs<Token>(3));
  EXPECT_FALSE(stream.peekIsToken<Token::Ident>(3));

  // None of the peeks should have advanced the cursor.
  EXPECT_EQ(stream.remaining(), 3u);
}

TEST(ComponentValueStreamTest, AdvanceByMultiple) {
  std::vector<ComponentValue> components;
  for (uint32_t i = 0; i < 5; ++i) {
    components.push_back(MakeTokenCV<Token::Ident>(i, "x"));
  }
  ComponentValueStream stream(components);

  EXPECT_EQ(stream.remaining(), 5u);
  stream.advance(2);
  EXPECT_EQ(stream.remaining(), 3u);
  EXPECT_EQ(stream.currentOffset(), FileOffset::Offset(2));

  stream.advance(3);
  EXPECT_TRUE(stream.isEOF());
  EXPECT_EQ(stream.currentOffset(), FileOffset::EndOfString());
}

TEST(ComponentValueStreamTest, PeekDelimExtractsCharOnlyForDelimTokens) {
  std::vector<ComponentValue> components;
  components.push_back(MakeTokenCV<Token::Delim>(0, '>'));
  components.push_back(MakeTokenCV<Token::Ident>(1, "foo"));
  components.push_back(MakeTokenCV<Token::Delim>(4, '+'));
  ComponentValueStream stream(components);

  EXPECT_EQ(stream.peekDelim(), '>');
  EXPECT_TRUE(stream.peekDelimIs('>'));
  EXPECT_FALSE(stream.peekDelimIs('+'));

  // A non-delim token returns nullopt, not a wrong character.
  EXPECT_EQ(stream.peekDelim(1), std::nullopt);
  EXPECT_FALSE(stream.peekDelimIs('>', 1));

  EXPECT_EQ(stream.peekDelim(2), '+');
  EXPECT_TRUE(stream.peekDelimIs('+', 2));

  // Past EOF: nullopt, not UB.
  EXPECT_EQ(stream.peekDelim(3), std::nullopt);
  EXPECT_FALSE(stream.peekDelimIs('+', 3));
}

TEST(ComponentValueStreamTest, SkipWhitespaceAdvancesOverLeadingWhitespace) {
  std::vector<ComponentValue> components;
  components.push_back(MakeTokenCV<Token::Whitespace>(0, " "));
  components.push_back(MakeTokenCV<Token::Whitespace>(1, "\t"));
  components.push_back(MakeTokenCV<Token::Ident>(2, "hello"));
  components.push_back(MakeTokenCV<Token::Whitespace>(7, " "));
  ComponentValueStream stream(components);

  EXPECT_EQ(stream.remaining(), 4u);
  stream.skipWhitespace();

  // Two whitespace tokens consumed; the trailing whitespace after "hello" is
  // deliberately NOT consumed — skipWhitespace only eats leading whitespace.
  EXPECT_EQ(stream.remaining(), 2u);
  EXPECT_TRUE(stream.peekIsToken<Token::Ident>());
  EXPECT_EQ(stream.peekAs<Token>()->get<Token::Ident>().value, "hello");
  EXPECT_EQ(stream.currentOffset(), FileOffset::Offset(2));
}

TEST(ComponentValueStreamTest, SkipWhitespaceOnAllWhitespaceReachesEOF) {
  std::vector<ComponentValue> components;
  components.push_back(MakeTokenCV<Token::Whitespace>(0, " "));
  components.push_back(MakeTokenCV<Token::Whitespace>(1, " "));
  ComponentValueStream stream(components);

  stream.skipWhitespace();
  EXPECT_TRUE(stream.isEOF());
}

TEST(ComponentValueStreamTest, SkipWhitespaceOnEmptyStreamIsNoOp) {
  std::vector<ComponentValue> components;
  ComponentValueStream stream(components);

  stream.skipWhitespace();
  EXPECT_TRUE(stream.isEOF());
}

TEST(ComponentValueStreamTest, CurrentOffsetTracksCursor) {
  std::vector<ComponentValue> components;
  components.push_back(MakeTokenCV<Token::Ident>(10, "a"));
  components.push_back(MakeTokenCV<Token::Ident>(12, "b"));
  components.push_back(MakeTokenCV<Token::Ident>(14, "c"));
  ComponentValueStream stream(components);

  EXPECT_EQ(stream.currentOffset(), FileOffset::Offset(10));
  stream.advance();
  EXPECT_EQ(stream.currentOffset(), FileOffset::Offset(12));
  stream.advance();
  EXPECT_EQ(stream.currentOffset(), FileOffset::Offset(14));
  stream.advance();
  EXPECT_EQ(stream.currentOffset(), FileOffset::EndOfString());
}

}  // namespace donner::css::parser::details
