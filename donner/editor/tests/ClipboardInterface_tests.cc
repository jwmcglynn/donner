#include "donner/editor/ClipboardInterface.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "donner/editor/InMemoryClipboard.h"

namespace donner::editor {

/**
 * A freshly constructed InMemoryClipboard starts empty: hasText() is
 * false and getText() returns the empty string.
 */
TEST(InMemoryClipboardTests, EmptyByDefault) {
  InMemoryClipboard clipboard;
  EXPECT_FALSE(clipboard.hasText()) << "A default InMemoryClipboard should not report text.";
  EXPECT_EQ(clipboard.getText(), "")
      << "A default InMemoryClipboard should return an empty string.";
}

/**
 * setText followed by getText round-trips the value, and hasText
 * flips to true once non-empty content is stored.
 */
TEST(InMemoryClipboardTests, SetThenGetRoundTrip) {
  InMemoryClipboard clipboard;
  clipboard.setText("hello");
  EXPECT_EQ(clipboard.getText(), "hello") << "getText should return the value just stored.";
  EXPECT_TRUE(clipboard.hasText()) << "hasText should be true after setText with non-empty input.";
}

/**
 * A second setText replaces the first value rather than appending.
 */
TEST(InMemoryClipboardTests, OverwritesPreviousContent) {
  InMemoryClipboard clipboard;
  clipboard.setText("first");
  clipboard.setText("second");
  EXPECT_EQ(clipboard.getText(), "second")
      << "setText should overwrite, not append to, the previous value.";
  EXPECT_TRUE(clipboard.hasText());
}

/**
 * Newlines and other control characters round-trip verbatim — the
 * clipboard stores raw bytes, not a single visual line.
 */
TEST(InMemoryClipboardTests, MultilineStringPreserved) {
  InMemoryClipboard clipboard;
  const std::string multiline = "line one\nline two\nline three";
  clipboard.setText(multiline);
  EXPECT_EQ(clipboard.getText(), multiline)
      << "Multiline content should round-trip byte-for-byte through the clipboard.";
  EXPECT_TRUE(clipboard.hasText());
}

/**
 * UTF-8-encoded text with multibyte code points round-trips without
 * mangling.
 */
TEST(InMemoryClipboardTests, Utf8Preserved) {
  InMemoryClipboard clipboard;
  // Mix of ASCII, accented Latin, Japanese, and a four-byte emoji.
  const std::string utf8 = "Hello, caf\xc3\xa9 \xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf \xf0\x9f\x8d\xa9";
  clipboard.setText(utf8);
  EXPECT_EQ(clipboard.getText(), utf8)
      << "UTF-8 content should round-trip byte-for-byte through the clipboard.";
  EXPECT_EQ(clipboard.getText().size(), utf8.size())
      << "Byte length of UTF-8 content should be preserved.";
  EXPECT_TRUE(clipboard.hasText());
}

/**
 * Setting an explicit empty string is equivalent to a freshly
 * constructed clipboard: hasText() returns false.
 */
TEST(InMemoryClipboardTests, ExplicitEmptyStringHasNoText) {
  InMemoryClipboard clipboard;
  clipboard.setText("something");
  ASSERT_TRUE(clipboard.hasText());
  clipboard.setText("");
  EXPECT_FALSE(clipboard.hasText())
      << "setText(\"\") should leave the clipboard in the 'no text' state.";
  EXPECT_EQ(clipboard.getText(), "");
}

/**
 * A single character round-trips as well — guards against the
 * trivial off-by-one where 1-byte inputs might be mishandled.
 */
TEST(InMemoryClipboardTests, SingleCharacter) {
  InMemoryClipboard clipboard;
  clipboard.setText("x");
  EXPECT_TRUE(clipboard.hasText());
  EXPECT_EQ(clipboard.getText(), "x");
}

/**
 * Embedded NUL bytes survive round-trip — the clipboard stores a
 * std::string, not a C string, so NULs are payload, not terminators.
 */
TEST(InMemoryClipboardTests, EmbeddedNulByteSurvives) {
  InMemoryClipboard clipboard;
  const std::string withNul("a\0b\0c", 5);
  clipboard.setText(withNul);
  EXPECT_EQ(clipboard.getText(), withNul)
      << "Embedded NUL bytes should survive round-trip (byte-accurate).";
  EXPECT_EQ(clipboard.getText().size(), 5U)
      << "Size should reflect all 5 bytes, including the NULs.";
  EXPECT_TRUE(clipboard.hasText());
}

/**
 * Two separate InMemoryClipboard instances do NOT share state — this
 * is documented behavior and matters for tests that want isolation.
 */
TEST(InMemoryClipboardTests, InstancesDoNotShareState) {
  InMemoryClipboard a;
  InMemoryClipboard b;
  a.setText("only in A");
  EXPECT_TRUE(a.hasText());
  EXPECT_FALSE(b.hasText()) << "A separate instance should not see writes from another instance.";
  EXPECT_EQ(b.getText(), "");
}

/**
 * getText() is const and repeatable — calling it twice returns the
 * same value and does not mutate the clipboard.
 */
TEST(InMemoryClipboardTests, GetTextIsRepeatable) {
  InMemoryClipboard clipboard;
  clipboard.setText("stable");
  const std::string first = clipboard.getText();
  const std::string second = clipboard.getText();
  EXPECT_EQ(first, second) << "Two consecutive getText() calls should return the same value.";
  EXPECT_EQ(first, "stable");
  EXPECT_TRUE(clipboard.hasText());
}

/**
 * Polymorphic access via a ClipboardInterface pointer works the
 * same as direct access — this is the shape TextEditorCore will
 * use when it holds a non-owning ClipboardInterface*.
 */
TEST(InMemoryClipboardTests, PolymorphicAccessThroughInterface) {
  InMemoryClipboard concrete;
  ClipboardInterface* iface = &concrete;

  EXPECT_FALSE(iface->hasText());
  iface->setText("via interface");
  EXPECT_TRUE(iface->hasText());
  EXPECT_EQ(iface->getText(), "via interface");

  // Writes via the interface are visible through the concrete type.
  EXPECT_EQ(concrete.getText(), "via interface")
      << "Writes through ClipboardInterface* should be visible on the underlying object.";
}

/**
 * Polymorphic access via std::unique_ptr<ClipboardInterface> also
 * works — covers the common ownership shape in tests.
 */
TEST(InMemoryClipboardTests, PolymorphicAccessViaUniquePtr) {
  std::unique_ptr<ClipboardInterface> clipboard = std::make_unique<InMemoryClipboard>();
  EXPECT_FALSE(clipboard->hasText());
  clipboard->setText("owned");
  EXPECT_TRUE(clipboard->hasText());
  EXPECT_EQ(clipboard->getText(), "owned");
}

}  // namespace donner::editor
