#include "donner/base/ChunkedString.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner {

using ::testing::Eq;
using ::testing::StrEq;

/**
 * Construct an empty ChunkedString and verify its size.
 */
TEST(ChunkedString, Construct) {
  {
    // Default constructor
    ChunkedString chunks;
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 0);
  }

  {
    // Constructor from string_view
    ChunkedString chunks(std::string_view("hello"));
    EXPECT_EQ(chunks.size(), 5);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "hello");
  }

  {
    // Constructor from RcString
    RcString str("world");
    ChunkedString chunks(str);
    EXPECT_EQ(chunks.size(), 5);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "world");
  }

  {
    // Constructor from RcStringOrRef
    RcStringOrRef str("hello world");
    ChunkedString chunks(str);
    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world");
  }

  {
    // Constructor from C-string
    ChunkedString chunks("test");
    EXPECT_EQ(chunks.size(), 4);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "test");
  }

  {
    // Constructor from empty string
    ChunkedString chunks("");
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "");
  }

  {
    // Copy constructor
    ChunkedString original;
    original.append(std::string_view("hello"));
    original.append(std::string_view(" "));
    original.append(std::string_view("world"));

    ChunkedString copy(original);
    EXPECT_EQ(copy.size(), 11);
    EXPECT_EQ(copy.numChunks(), 3);
    EXPECT_EQ(copy.toSingleRcString(), "hello world");

    // Verify original is unchanged
    EXPECT_EQ(original.size(), 11);
    EXPECT_EQ(original.numChunks(), 3);
    EXPECT_EQ(original.toSingleRcString(), "hello world");
  }
}

/**
 * Test assignment operator.
 */
TEST(ChunkedString, Assignment) {
  {
    // Basic assignment
    ChunkedString chunks1;
    chunks1.append(std::string_view("hello"));

    ChunkedString chunks2;
    chunks2 = chunks1;

    EXPECT_EQ(chunks2.size(), 5);
    EXPECT_EQ(chunks2.numChunks(), 1);
    EXPECT_EQ(chunks2.toSingleRcString(), "hello");
  }

  {
    // Assignment with multiple chunks
    ChunkedString chunks1;
    chunks1.append(std::string_view("hello"));
    chunks1.append(std::string_view(" "));
    chunks1.append(std::string_view("world"));

    ChunkedString chunks2;
    chunks2 = chunks1;

    EXPECT_EQ(chunks2.size(), 11);
    EXPECT_EQ(chunks2.numChunks(), 3);
    EXPECT_EQ(chunks2.toSingleRcString(), "hello world");
  }

  {
    // Self-assignment
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" world"));

// Suppress self-assignment warning
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"
    chunks = chunks;  // Self-assignment
#pragma clang diagnostic pop

    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 2);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world");
  }

  {
    // Assignment after previous content
    ChunkedString chunks1;
    chunks1.append(std::string_view("original"));

    ChunkedString chunks2;
    chunks2.append(std::string_view("hello"));

    chunks1 = chunks2;

    EXPECT_EQ(chunks1.size(), 5);
    EXPECT_EQ(chunks1.numChunks(), 1);
    EXPECT_EQ(chunks1.toSingleRcString(), "hello");
  }
}

/**
 * Append string views.
 */
TEST(ChunkedString, AppendStringView) {
  {
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    EXPECT_EQ(chunks.size(), 5);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "hello");
  }

  {
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" world"));
    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 2);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world");
  }

  {
    ChunkedString chunks;
    chunks.append(std::string_view(""));
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "");
  }
}

TEST(ChunkedString, AppendRcStringOrRef) {
  {
    ChunkedString chunks;
    RcStringOrRef str("hello");
    chunks.append(str);
    EXPECT_EQ(chunks.size(), 5);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "hello");
  }

  {
    ChunkedString chunks;
    RcStringOrRef str1("hello");
    RcStringOrRef str2(" world");
    chunks.append(str1);
    chunks.append(str2);
    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 2);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world");
  }

  {
    ChunkedString chunks;
    RcStringOrRef empty("");
    chunks.append(empty);
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "");
  }
}

TEST(ChunkedString, AppendRcString) {
  {
    ChunkedString chunks;
    RcString str("hello");
    chunks.append(str);
    EXPECT_THAT(chunks.size(), Eq(5));
    EXPECT_THAT(chunks.numChunks(), Eq(1));
    EXPECT_THAT(chunks.toSingleRcString(), StrEq("hello"));
  }

  {
    ChunkedString chunks;
    RcString str1("hello");
    RcString str2(" world");
    chunks.append(str1);
    chunks.append(str2);
    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 2);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world");
  }

  {
    ChunkedString chunks;
    RcString empty("");
    chunks.append(empty);
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "");
  }

  {
    ChunkedString chunks;
    RcString longString("test STRING that is longer than 30 characters");
    chunks.append(longString);
    EXPECT_EQ(chunks.size(), 45);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "test STRING that is longer than 30 characters");
  }
}

TEST(ChunkedString, AppendChunkedString) {
  {
    ChunkedString chunks1;
    chunks1.append(std::string_view("hello"));

    ChunkedString chunks2;
    chunks2.append(std::string_view(" world"));

    chunks1.append(chunks2);
    EXPECT_EQ(chunks1.size(), 11);
    EXPECT_EQ(chunks1.numChunks(), 2);
    EXPECT_EQ(chunks1.toSingleRcString(), "hello world");
  }

  {
    ChunkedString chunks1;
    chunks1.append(RcString("hello"));

    ChunkedString chunks2;
    chunks2.append(RcString(" "));
    chunks2.append(RcString("world"));

    chunks1.append(chunks2);
    EXPECT_EQ(chunks1.size(), 11);
    EXPECT_EQ(chunks1.numChunks(), 3);
    EXPECT_EQ(chunks1.toSingleRcString(), "hello world");
  }

  {
    ChunkedString chunks1;
    ChunkedString chunks2;
    chunks1.append(chunks2);
    EXPECT_EQ(chunks1.size(), 0);
    EXPECT_EQ(chunks1.numChunks(), 0);
    EXPECT_EQ(chunks1.toSingleRcString(), "");
  }

  {
    ChunkedString chunks1;
    ChunkedString chunks2;
    chunks2.append(std::string_view(""));
    chunks1.append(chunks2);
    EXPECT_EQ(chunks1.size(), 0);
    EXPECT_EQ(chunks1.numChunks(), 1);  // Empty string is still a chunk
    EXPECT_EQ(chunks1.toSingleRcString(), "");
  }
}

TEST(ChunkedString, ToSingleRcString) {
  {
    // Empty case
    ChunkedString chunks;
    EXPECT_EQ(chunks.toSingleRcString(), "");
  }

  {
    // Single chunk case
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    RcString result = chunks.toSingleRcString();
    EXPECT_EQ(result, "hello");
  }

  {
    // Multiple chunks case
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" "));
    chunks.append(std::string_view("world"));
    RcString result = chunks.toSingleRcString();
    EXPECT_EQ(result, "hello world");
  }

  {
    // Test with nulls
    ChunkedString chunks;
    chunks.append(std::string_view("hello\0world", 11));
    RcString result = chunks.toSingleRcString();
    EXPECT_EQ(result, std::string_view("hello\0world", 11));
  }
}

TEST(ChunkedString, Size) {
  {
    ChunkedString chunks;
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 0);

    chunks.append(std::string_view("hello"));
    EXPECT_EQ(chunks.size(), 5);
    EXPECT_EQ(chunks.numChunks(), 1);

    chunks.append(std::string_view(" world"));
    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 2);

    chunks.append(RcString("!"));
    EXPECT_EQ(chunks.size(), 12);
    EXPECT_EQ(chunks.numChunks(), 3);
  }

  {
    // Test with empty chunks
    ChunkedString chunks;
    chunks.append(std::string_view(""));
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 1);

    chunks.append(std::string_view(""));
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 2);
  }
}

TEST(ChunkedString, LargeStrings) {
  {
    // Test with large strings
    ChunkedString chunks;
    std::string longText(1000, 'a');
    chunks.append(std::string_view(longText));
    EXPECT_EQ(chunks.size(), 1000);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), longText);

    std::string longText2(2000, 'b');
    chunks.append(std::string_view(longText2));
    EXPECT_EQ(chunks.size(), 3000);
    EXPECT_EQ(chunks.numChunks(), 2);

    RcString result = chunks.toSingleRcString();
    EXPECT_EQ(result.size(), 3000);
    EXPECT_EQ(result, longText + longText2);
  }
}

/**
 * Prepend string views.
 */
TEST(ChunkedString, PrependStringView) {
  {
    ChunkedString chunks;
    chunks.prepend(std::string_view("world"));
    EXPECT_EQ(chunks.size(), 5);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "world");
  }

  {
    ChunkedString chunks;
    chunks.prepend(std::string_view("world"));
    chunks.prepend(std::string_view("hello "));
    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 2);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world");
  }

  {
    ChunkedString chunks;
    chunks.prepend(std::string_view(""));
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "");
  }
}

TEST(ChunkedString, PrependRcStringOrRef) {
  {
    ChunkedString chunks;
    RcStringOrRef str("world");
    chunks.prepend(str);
    EXPECT_EQ(chunks.size(), 5);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "world");
  }

  {
    ChunkedString chunks;
    RcStringOrRef str1("world");
    RcStringOrRef str2("hello ");
    chunks.prepend(str1);
    chunks.prepend(str2);
    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 2);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world");
  }

  {
    ChunkedString chunks;
    RcStringOrRef empty("");
    chunks.prepend(empty);
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "");
  }
}

TEST(ChunkedString, PrependRcString) {
  {
    ChunkedString chunks;
    RcString str("world");
    chunks.prepend(str);
    EXPECT_THAT(chunks.size(), Eq(5));
    EXPECT_THAT(chunks.numChunks(), Eq(1));
    EXPECT_THAT(chunks.toSingleRcString(), StrEq("world"));
  }

  {
    ChunkedString chunks;
    RcString str1("world");
    RcString str2("hello ");
    chunks.prepend(str1);
    chunks.prepend(str2);
    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 2);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world");
  }

  {
    ChunkedString chunks;
    RcString empty("");
    chunks.prepend(empty);
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "");
  }

  {
    ChunkedString chunks;
    RcString longString("test STRING that is longer than 30 characters");
    chunks.prepend(longString);
    EXPECT_EQ(chunks.size(), 45);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "test STRING that is longer than 30 characters");
  }
}

TEST(ChunkedString, PrependChunkedString) {
  {
    ChunkedString chunks1;
    chunks1.append(std::string_view("world"));

    ChunkedString chunks2;
    chunks2.append(std::string_view("hello "));

    chunks1.prepend(chunks2);
    EXPECT_EQ(chunks1.size(), 11);
    EXPECT_EQ(chunks1.numChunks(), 2);
    EXPECT_EQ(chunks1.toSingleRcString(), "hello world");
  }

  {
    ChunkedString chunks1;
    chunks1.append(RcString("world"));

    ChunkedString chunks2;
    chunks2.append(RcString("he"));
    chunks2.append(RcString("llo "));

    chunks1.prepend(chunks2);
    EXPECT_EQ(chunks1.size(), 11);
    EXPECT_EQ(chunks1.numChunks(), 3);
    EXPECT_EQ(chunks1.toSingleRcString(), "hello world");
  }

  {
    ChunkedString chunks1;
    ChunkedString chunks2;
    chunks1.prepend(chunks2);
    EXPECT_EQ(chunks1.size(), 0);
    EXPECT_EQ(chunks1.numChunks(), 0);
    EXPECT_EQ(chunks1.toSingleRcString(), "");
  }

  {
    ChunkedString chunks1;
    ChunkedString chunks2;
    chunks2.append(std::string_view(""));
    chunks1.prepend(chunks2);
    EXPECT_EQ(chunks1.size(), 0);
    EXPECT_EQ(chunks1.numChunks(), 1);  // Empty string is still a chunk
    EXPECT_EQ(chunks1.toSingleRcString(), "");
  }
}

TEST(ChunkedString, AppendAndPrepend) {
  {
    ChunkedString chunks;
    chunks.append(std::string_view("world"));
    chunks.prepend(std::string_view("hello "));
    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 2);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world");
  }

  {
    ChunkedString chunks;
    chunks.prepend(std::string_view("world"));
    chunks.append(std::string_view("!"));
    chunks.prepend(std::string_view("hello "));
    EXPECT_EQ(chunks.size(), 12);
    EXPECT_EQ(chunks.numChunks(), 3);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world!");
  }

  {
    // Test complex interactions between append and prepend
    ChunkedString chunks1;
    chunks1.append(RcString("456"));

    ChunkedString chunks2;
    chunks2.append(RcString("123"));

    ChunkedString chunks3;
    chunks3.append(RcString("789"));

    chunks1.prepend(chunks2);  // chunks1 = "123456"
    chunks1.append(chunks3);   // chunks1 = "123456789"

    EXPECT_EQ(chunks1.size(), 9);
    EXPECT_EQ(chunks1.toSingleRcString(), "123456789");
  }
}

/**
 * @test Getting the first chunk of a ChunkedString
 */
TEST(ChunkedString, FirstChunk) {
  {
    // Single chunk
    ChunkedString chunks(std::string_view("hello"));
    EXPECT_EQ(chunks.firstChunk(), "hello");
  }

  {
    // Multiple chunks
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" "));
    chunks.append(std::string_view("world"));

    EXPECT_EQ(chunks.firstChunk(), "hello");
  }

  {
    // Empty string
    ChunkedString chunks;
    EXPECT_EQ(chunks.firstChunk(), "");
  }

  {
    // Single empty chunk
    ChunkedString chunks(std::string_view(""));
    EXPECT_EQ(chunks.firstChunk(), "");
  }

  {
    // Multiple empty chunks
    ChunkedString chunks;
    chunks.append(std::string_view(""));
    chunks.append(std::string_view(" "));
    chunks.append(std::string_view(""));

    EXPECT_EQ(chunks.firstChunk(), "");
  }
}

/**
 * Test appendLiteral and prependLiteral methods
 */
TEST(ChunkedString, LiteralMethods) {
  {
    // Test appendLiteral
    ChunkedString chunks;
    chunks.appendLiteral("hello");
    chunks.appendLiteral(" ");
    chunks.appendLiteral("world");

    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 3);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world");
  }

  {
    // Test prependLiteral
    ChunkedString chunks;
    chunks.prependLiteral("world");
    chunks.prependLiteral(" ");
    chunks.prependLiteral("hello");

    EXPECT_EQ(chunks.size(), 11);
    EXPECT_EQ(chunks.numChunks(), 3);
    EXPECT_EQ(chunks.toSingleRcString(), "hello world");
  }

  {
    // Test combining literal methods
    ChunkedString chunks;
    chunks.appendLiteral("middle");
    chunks.prependLiteral("start ");
    chunks.appendLiteral(" end");

    EXPECT_EQ(chunks.size(), 16);
    EXPECT_EQ(chunks.numChunks(), 3);
    EXPECT_EQ(chunks.toSingleRcString(), "start middle end");
  }
}

/**
 * Test the empty method
 */
TEST(ChunkedString, Empty) {
  // Empty string
  {
    ChunkedString chunks;
    EXPECT_TRUE(chunks.empty());
  }

  // Non-empty string
  {
    ChunkedString chunks(std::string_view("hello"));
    EXPECT_FALSE(chunks.empty());
  }

  // String that becomes empty
  {
    ChunkedString chunks(std::string_view("hello"));
    EXPECT_FALSE(chunks.empty());

    chunks = ChunkedString();
    EXPECT_TRUE(chunks.empty());
  }
}

/**
 * Test operator[] for accessing characters
 */
TEST(ChunkedString, SubscriptOperator) {
  {
    // Single chunk
    ChunkedString chunks(std::string_view("hello"));
    EXPECT_EQ(chunks[0], 'h');
    EXPECT_EQ(chunks[1], 'e');
    EXPECT_EQ(chunks[4], 'o');
  }

  {
    // Multiple chunks
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" "));
    chunks.append(std::string_view("world"));

    EXPECT_EQ(chunks[0], 'h');
    EXPECT_EQ(chunks[4], 'o');
    EXPECT_EQ(chunks[5], ' ');
    EXPECT_EQ(chunks[6], 'w');
    EXPECT_EQ(chunks[10], 'd');
  }

  // We remove the out-of-range test since operator[] now uses assert
}

/**
 * @test Substr for a single chunk
 */
TEST(ChunkedString, Substr) {
  {
    // Test basic functionality
    ChunkedString chunks(std::string_view("test"));

    EXPECT_EQ(chunks.substr(0, 2), "te");
    EXPECT_EQ(chunks.substr(0, 2).size(), 2);  // "te"
    EXPECT_EQ(chunks.substr(1, 2), "es");
    EXPECT_EQ(chunks.substr(1, 2).size(), 2);  // "es"
    EXPECT_EQ(chunks.substr(0), "test");
    EXPECT_EQ(chunks.substr(0).size(), 4);  // "test"
    EXPECT_EQ(chunks.substr(2), "st");
    EXPECT_EQ(chunks.substr(2).size(), 2);  // "st"
  }

  {
    // Test edge case
    ChunkedString chunks(std::string_view("test"));

    // Empty substring at the end
    EXPECT_EQ(chunks.substr(4), "");
    EXPECT_TRUE(chunks.substr(4).empty());

    // We don't test beyond the bounds since that would trigger an assert
  }
}

/**
 * @test substr for multiple chunks
 */
TEST(ChunkedString, SubstrMultipleChunks) {
  {
    // Create a ChunkedString with multiple chunks
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" "));
    chunks.append(std::string_view("world"));

    // Test substring within first chunk
    EXPECT_EQ(chunks.substr(0, 2), "he");
    EXPECT_EQ(chunks.substr(0, 2).size(), 2);
    EXPECT_EQ(chunks.substr(0, 2).numChunks(), 1);

    // Test substring spanning multiple chunks
    EXPECT_EQ(chunks.substr(3, 5), "lo wo");
    EXPECT_EQ(chunks.substr(3, 5).size(), 5);
    EXPECT_EQ(chunks.substr(3, 5).numChunks(), 3);

    // Test substring starting in the middle chunk
    EXPECT_EQ(chunks.substr(5, 2), " w");
    EXPECT_EQ(chunks.substr(5, 2).size(), 2);
    EXPECT_EQ(chunks.substr(5, 2).numChunks(), 2);

    // Test substring entirely in the last chunk
    EXPECT_EQ(chunks.substr(7, 3), "orl");
    EXPECT_EQ(chunks.substr(7, 3).size(), 3);
    EXPECT_EQ(chunks.substr(7, 3).numChunks(), 1);

    // Test substring to end of string
    EXPECT_EQ(chunks.substr(6), "world");
    EXPECT_EQ(chunks.substr(6).size(), 5);
    EXPECT_EQ(chunks.substr(6).numChunks(), 1);

    // Test substring spanning first and last chunk
    EXPECT_EQ(chunks.substr(4, 3), "o w");
    EXPECT_EQ(chunks.substr(4, 3).size(), 3);
    EXPECT_EQ(chunks.substr(4, 3).numChunks(), 3);
  }

  {
    // Test with chunks of different sizes
    ChunkedString chunks;
    chunks.append(std::string_view("a"));
    chunks.append(std::string_view("bc"));
    chunks.append(std::string_view("def"));
    chunks.append(std::string_view("ghij"));

    // Test substring spanning all chunks
    EXPECT_EQ(chunks.substr(0), "abcdefghij");
    EXPECT_EQ(chunks.substr(0).size(), 10);
    EXPECT_EQ(chunks.substr(0).numChunks(), 4);

    // Test substring spanning multiple middle chunks
    EXPECT_EQ(chunks.substr(1, 5), "bcdef");
    EXPECT_EQ(chunks.substr(1, 5).size(), 5);
    EXPECT_EQ(chunks.substr(1, 5).numChunks(), 2);
  }

  {
    // Test edge cases
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" "));
    chunks.append(std::string_view("world"));

    // Empty substring at beginning
    EXPECT_EQ(chunks.substr(0, 0), "");
    EXPECT_EQ(chunks.substr(0, 0).size(), 0);
    EXPECT_TRUE(chunks.substr(0, 0).empty());

    // Empty substring in middle
    EXPECT_EQ(chunks.substr(5, 0), "");
    EXPECT_EQ(chunks.substr(5, 0).size(), 0);
    EXPECT_TRUE(chunks.substr(5, 0).empty());

    // Empty substring at end
    EXPECT_EQ(chunks.substr(11, 0), "");
    EXPECT_EQ(chunks.substr(11, 0).size(), 0);
    EXPECT_TRUE(chunks.substr(11, 0).empty());

    // Zero-length chunks within substring
    ChunkedString chunks2;
    chunks2.append(std::string_view("hello"));
    chunks2.append(std::string_view(""));
    chunks2.append(std::string_view("world"));

    EXPECT_EQ(chunks2.substr(0), "helloworld");
    EXPECT_EQ(chunks2.substr(0).size(), 10);
    EXPECT_EQ(chunks2.substr(0).numChunks(), 3);  // Still includes the empty chunk
  }
}

/**
 * @test remove_prefix method
 */
TEST(ChunkedString, RemovePrefix) {
  {
    // Single chunk - partial removal
    ChunkedString chunks(std::string_view("hello world"));
    chunks.remove_prefix(6);
    EXPECT_EQ(chunks.size(), 5);
    EXPECT_EQ(chunks.toSingleRcString(), "world");
  }

  {
    // Single chunk - full removal
    ChunkedString chunks(std::string_view("hello"));
    chunks.remove_prefix(5);
    EXPECT_TRUE(chunks.empty());
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 0);
  }

  {
    // Single chunk - excessive removal
    ChunkedString chunks(std::string_view("hello"));
    chunks.remove_prefix(100);
    EXPECT_TRUE(chunks.empty());
    EXPECT_EQ(chunks.size(), 0);
    EXPECT_EQ(chunks.numChunks(), 0);
  }

  {
    // Multiple chunks - remove first chunk entirely
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" "));
    chunks.append(std::string_view("world"));

    chunks.remove_prefix(6);  // Remove "hello " completely
    EXPECT_EQ(chunks.size(), 5);
    EXPECT_EQ(chunks.numChunks(), 1);
    EXPECT_EQ(chunks.toSingleRcString(), "world");
  }

  {
    // Multiple chunks - remove first chunk partially
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" "));
    chunks.append(std::string_view("world"));

    chunks.remove_prefix(3);  // Remove "hel" from "hello"
    EXPECT_EQ(chunks.size(), 8);
    EXPECT_EQ(chunks.numChunks(), 3);
    EXPECT_EQ(chunks.toSingleRcString(), "lo world");
  }

  {
    // Multiple chunks - remove multiple chunks
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));  // 5 chars
    chunks.append(std::string_view(" "));      // 1 char
    chunks.append(std::string_view("world"));  // 5 chars
    chunks.append(std::string_view("!"));      // 1 char

    std::string original = chunks.toSingleRcString().str();
    chunks.remove_prefix(7);  // Remove "hello w" (5 + 1 + 1 chars)

    // The result should be the original string with 7 chars removed from the front
    EXPECT_EQ(chunks.toSingleRcString(), original.substr(7));
    EXPECT_EQ(chunks.size(), original.size() - 7);
  }

  {
    // No-op removal
    ChunkedString chunks(std::string_view("hello"));
    chunks.remove_prefix(0);
    EXPECT_EQ(chunks.size(), 5);
    EXPECT_EQ(chunks.toSingleRcString(), "hello");
  }
}

/**
 * Test starts_with method
 */
TEST(ChunkedString, StartsWith) {
  {
    // Single chunk tests
    ChunkedString chunks(std::string_view("hello world"));

    // Positive tests
    EXPECT_TRUE(chunks.starts_with(""));        // Empty prefix
    EXPECT_TRUE(chunks.starts_with("h"));       // Single char
    EXPECT_TRUE(chunks.starts_with("hello"));   // Prefix
    EXPECT_TRUE(chunks.starts_with("hello "));  // Prefix with space

    // Negative tests
    EXPECT_FALSE(chunks.starts_with("world"));         // Not a prefix
    EXPECT_FALSE(chunks.starts_with("hello!"));        // Different char
    EXPECT_FALSE(chunks.starts_with("hello world!"));  // Longer than string
  }

  {
    // Multiple chunks with full prefix in first chunk
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" "));
    chunks.append(std::string_view("world"));

    EXPECT_TRUE(chunks.starts_with("hello"));
    EXPECT_FALSE(chunks.starts_with("world"));
  }

  {
    // Multiple chunks with prefix spanning across chunks
    ChunkedString chunks;
    chunks.append(std::string_view("hel"));
    chunks.append(std::string_view("lo"));
    chunks.append(std::string_view(" world"));

    EXPECT_TRUE(chunks.starts_with("hell"));     // Spans first and second chunk
    EXPECT_TRUE(chunks.starts_with("hello"));    // Exactly first and second chunk
    EXPECT_TRUE(chunks.starts_with("hello w"));  // Spans into third chunk
    EXPECT_FALSE(chunks.starts_with("help"));    // Different content
  }

  {
    // Edge cases
    ChunkedString empty;
    EXPECT_TRUE(empty.starts_with(""));    // Empty string starts with empty prefix
    EXPECT_FALSE(empty.starts_with("a"));  // Empty string doesn't start with non-empty prefix

    ChunkedString singleChar(std::string_view("a"));
    EXPECT_TRUE(singleChar.starts_with("a"));
    EXPECT_FALSE(singleChar.starts_with("ab"));
  }
}

/**
 * Test find method
 */
TEST(ChunkedString, Find) {
  static constexpr size_t npos = std::string_view::npos;

  {
    // Single chunk
    ChunkedString chunks(std::string_view("hello world"));

    // Basic find tests
    EXPECT_EQ(chunks.find(""), 0);              // Empty string
    EXPECT_EQ(chunks.find("h"), 0);             // First char
    EXPECT_EQ(chunks.find("world"), 6);         // Word in the middle
    EXPECT_EQ(chunks.find("d"), 10);            // Last char
    EXPECT_EQ(chunks.find("hello world"), 0);   // Entire string
    EXPECT_EQ(chunks.find("not found"), npos);  // Not found

    // Find with pos
    EXPECT_EQ(chunks.find("l", 0), 2);         // First 'l'
    EXPECT_EQ(chunks.find("l", 3), 3);         // Second 'l'
    EXPECT_EQ(chunks.find("l", 4), 9);         // Third 'l'
    EXPECT_EQ(chunks.find("o", 5), 7);         // 'o' after first 'o'
    EXPECT_EQ(chunks.find("hello", 1), npos);  // Can't find "hello" starting at pos 1
  }

  {
    // Multiple chunks
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" "));
    chunks.append(std::string_view("world"));

    // Testing across chunk boundaries
    EXPECT_EQ(chunks.find("hello"), 0);         // First chunk
    EXPECT_EQ(chunks.find(" "), 5);             // Middle chunk
    EXPECT_EQ(chunks.find("world"), 6);         // Last chunk
    EXPECT_EQ(chunks.find("o w"), 4);           // Spans first and second chunks
    EXPECT_EQ(chunks.find(" wo"), 5);           // Spans second and third chunks
    EXPECT_EQ(chunks.find("o worl"), 4);        // Spans all three chunks
    EXPECT_EQ(chunks.find("not found"), npos);  // Not found
  }

  {
    // Edge cases
    ChunkedString empty;
    EXPECT_EQ(empty.find(""), 0);      // Empty string in empty string found at pos 0
    EXPECT_EQ(empty.find("", 0), 0);   // Empty string in empty string found at pos 0
    EXPECT_EQ(empty.find("a"), npos);  // Non-empty string not found in empty string

    ChunkedString singleChar(std::string_view("a"));
    EXPECT_EQ(singleChar.find(""), 0);      // Empty string in any string found at pos 0
    EXPECT_EQ(singleChar.find("a"), 0);     // Same single char
    EXPECT_EQ(singleChar.find("b"), npos);  // Different char

    // Out of bounds pos
    ChunkedString str(std::string_view("test"));
    EXPECT_EQ(str.find("t", 4), npos);  // pos at the end
    EXPECT_EQ(str.find("t", 5), npos);  // pos beyond the end

    // Not enough characters left
    EXPECT_EQ(str.find("test", 1), npos);  // Can't find "test" starting at pos 1
  }
}

/**
 * Test equality operators
 */
TEST(ChunkedString, Equality) {
  {
    // Empty strings
    ChunkedString empty1;
    ChunkedString empty2;
    EXPECT_TRUE(empty1 == empty2);

    // Empty string_view
    EXPECT_TRUE(empty1 == std::string_view(""));
    EXPECT_TRUE(empty1 == "");
    EXPECT_TRUE(std::string_view("") == empty1);
    EXPECT_TRUE("" == empty1);
  }

  {
    // Single chunk equality
    ChunkedString str1("hello");
    ChunkedString str2("hello");
    ChunkedString str3("world");

    EXPECT_TRUE(str1 == str2);
    EXPECT_FALSE(str1 == str3);

    // string_view comparison
    EXPECT_TRUE(str1 == std::string_view("hello"));
    EXPECT_TRUE(std::string_view("hello") == str1);
    EXPECT_TRUE(str1 == "hello");
    EXPECT_TRUE("hello" == str1);

    EXPECT_FALSE(str1 == std::string_view("world"));
    EXPECT_FALSE(std::string_view("world") == str1);
    EXPECT_FALSE(str1 == "world");
    EXPECT_FALSE("world" == str1);
  }

  {
    // Different chunk counts but same content
    ChunkedString str1;
    str1.appendLiteral("hello");

    ChunkedString str2;
    str2.appendLiteral("he");
    str2.appendLiteral("llo");

    EXPECT_TRUE(str1 == str2);
    EXPECT_TRUE(str1 == std::string_view("hello"));
    EXPECT_TRUE(std::string_view("hello") == str1);
  }

  {
    // Multiple chunks
    ChunkedString str1;
    str1.appendLiteral("hello");
    str1.appendLiteral(" ");
    str1.appendLiteral("world");

    ChunkedString str2;
    str2.appendLiteral("hello ");
    str2.appendLiteral("world");

    ChunkedString str3;
    str3.appendLiteral("hello");
    str3.appendLiteral(" world");

    EXPECT_TRUE(str1 == str2);
    EXPECT_TRUE(str2 == str3);
    EXPECT_TRUE(str1 == str3);

    EXPECT_TRUE(str1 == std::string_view("hello world"));
    EXPECT_TRUE(std::string_view("hello world") == str1);
  }

  {
    // Different content
    ChunkedString str1;
    str1.appendLiteral("hello");
    str1.appendLiteral(" ");
    str1.appendLiteral("world");

    ChunkedString str2;
    str2.appendLiteral("hello");
    str2.appendLiteral(" ");
    str2.appendLiteral("there");

    EXPECT_FALSE(str1 == str2);
    EXPECT_FALSE(str1 == std::string_view("hello there"));
    EXPECT_FALSE(std::string_view("hello there") == str1);
  }

  {
    // Different lengths
    ChunkedString str1;
    str1.appendLiteral("hello");

    ChunkedString str2;
    str2.appendLiteral("hello!");

    EXPECT_FALSE(str1 == str2);
    EXPECT_FALSE(str1 == std::string_view("hello!"));
    EXPECT_FALSE(std::string_view("hello!") == str1);
  }

  {
    // RcString comparisons
    ChunkedString str("hello world");
    RcString rcStr1("hello world");
    RcString rcStr2("different text");

    EXPECT_TRUE(str == rcStr1);
    EXPECT_TRUE(rcStr1 == str);
    EXPECT_FALSE(str == rcStr2);
    EXPECT_FALSE(rcStr2 == str);
  }

  {
    // RcStringOrRef comparisons
    ChunkedString str("hello world");
    RcStringOrRef ref1("hello world");
    RcStringOrRef ref2(RcString("hello world"));
    RcStringOrRef ref3("different text");

    EXPECT_TRUE(str == ref1);
    EXPECT_TRUE(ref1 == str);
    EXPECT_TRUE(str == ref2);
    EXPECT_TRUE(ref2 == str);
    EXPECT_FALSE(str == ref3);
    EXPECT_FALSE(ref3 == str);
  }
}

/**
 * Test ostream operator<<
 */
TEST(ChunkedString, OstreamOutput) {
  {
    // Empty string
    ChunkedString empty;
    std::ostringstream oss;
    oss << empty;
    EXPECT_EQ(oss.str(), "");
  }

  {
    // Single chunk
    ChunkedString chunks("hello world");
    std::ostringstream oss;
    oss << chunks;
    EXPECT_EQ(oss.str(), "hello world");
  }

  {
    // Multiple chunks
    ChunkedString chunks;
    chunks.appendLiteral("hello");
    chunks.appendLiteral(" ");
    chunks.appendLiteral("world");

    std::ostringstream oss;
    oss << chunks;
    EXPECT_EQ(oss.str(), "hello world");
  }
}

/**
 * Test ends_with method
 */
TEST(ChunkedString, EndsWith) {
  {
    // Single chunk tests
    ChunkedString chunks(std::string_view("hello world"));

    // Positive tests
    EXPECT_TRUE(chunks.ends_with(""));        // Empty suffix
    EXPECT_TRUE(chunks.ends_with("d"));       // Single char
    EXPECT_TRUE(chunks.ends_with("world"));   // Suffix
    EXPECT_TRUE(chunks.ends_with(" world"));  // Suffix with space

    // Negative tests
    EXPECT_FALSE(chunks.ends_with("hello"));              // Not a suffix
    EXPECT_FALSE(chunks.ends_with("world!"));             // Different char
    EXPECT_FALSE(chunks.ends_with("hello hello world"));  // Longer than string
  }

  {
    // Multiple chunks with full suffix in last chunk
    ChunkedString chunks;
    chunks.append(std::string_view("hello"));
    chunks.append(std::string_view(" "));
    chunks.append(std::string_view("world"));

    EXPECT_TRUE(chunks.ends_with("world"));
    EXPECT_FALSE(chunks.ends_with("hello"));
  }

  {
    // Multiple chunks with suffix spanning across chunks
    ChunkedString chunks;
    chunks.append(std::string_view("hello "));
    chunks.append(std::string_view("wo"));
    chunks.append(std::string_view("rld"));

    EXPECT_TRUE(chunks.ends_with("rld"));     // Last chunk
    EXPECT_TRUE(chunks.ends_with("world"));   // Spans second and third chunk
    EXPECT_TRUE(chunks.ends_with(" world"));  // Spans all three chunks
    EXPECT_FALSE(chunks.ends_with("werld"));  // Different content
  }

  {
    // Edge cases
    ChunkedString empty;
    EXPECT_TRUE(empty.ends_with(""));    // Empty string ends with empty suffix
    EXPECT_FALSE(empty.ends_with("a"));  // Empty string doesn't end with non-empty suffix

    ChunkedString singleChar(std::string_view("a"));
    EXPECT_TRUE(singleChar.ends_with("a"));
    EXPECT_FALSE(singleChar.ends_with("ab"));
  }
}

}  // namespace donner
