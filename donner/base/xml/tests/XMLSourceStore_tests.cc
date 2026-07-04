#include "donner/base/xml/XMLSourceStore.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace donner::xml {
namespace {

ResolvedSourceSpan MustResolve(XMLSourceStore& store, SourceAnchorSpan span) {
  std::optional<ResolvedSourceSpan> resolved = store.resolveSpan(span);
  EXPECT_TRUE(resolved.has_value());
  return resolved.value_or(ResolvedSourceSpan{});
}

std::string_view SpanText(XMLSourceStore& store, SourceAnchorSpan span) {
  const ResolvedSourceSpan resolved = MustResolve(store, span);
  return store.source().substr(resolved.start, resolved.end - resolved.start);
}

TEST(XMLSourceStore, InsertBeforeSpanMovesResolvedOffsets) {
  XMLSourceStore store("<svg><rect/></svg>");
  std::optional<SourceAnchorSpan> span =
      store.createSpan(5, 12, SourceAnchorBias::After, SourceAnchorBias::Before);
  ASSERT_TRUE(span.has_value());
  EXPECT_EQ(SpanText(store, *span), "<rect/>");

  std::optional<XMLSourceDelta> delta = store.replace(5, 0, "<g/>");
  ASSERT_TRUE(delta.has_value());
  EXPECT_EQ(delta->offset, 5u);
  EXPECT_EQ(delta->removedLength, 0u);
  EXPECT_EQ(delta->insertedLength, 4u);
  EXPECT_EQ(delta->sourceVersion, 1u);

  EXPECT_EQ(store.source(), "<svg><g/><rect/></svg>");
  EXPECT_EQ(SpanText(store, *span), "<rect/>");
  EXPECT_EQ(MustResolve(store, *span), (ResolvedSourceSpan{9, 16}));
}

TEST(XMLSourceStore, BoundaryInsertionHonorsAnchorBias) {
  XMLSourceStore store("abcd");
  std::optional<SourceAnchorId> before = store.createAnchor(2, SourceAnchorBias::Before);
  std::optional<SourceAnchorId> after = store.createAnchor(2, SourceAnchorBias::After);
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(after.has_value());

  ASSERT_TRUE(store.replace(2, 0, "XY").has_value());

  EXPECT_EQ(store.source(), "abXYcd");
  EXPECT_EQ(store.resolveAnchor(*before), std::optional<std::size_t>(2));
  EXPECT_EQ(store.resolveAnchor(*after), std::optional<std::size_t>(4));
}

TEST(XMLSourceStore, ReplacementMovesBoundaryAnchorsAndInvalidatesInteriorAnchors) {
  XMLSourceStore store("0123456789");
  std::optional<SourceAnchorSpan> span = store.createSpan(2, 7);
  std::optional<SourceAnchorId> interior = store.createAnchor(4, SourceAnchorBias::Before);
  std::optional<SourceAnchorId> after = store.createAnchor(9, SourceAnchorBias::Before);
  ASSERT_TRUE(span.has_value());
  ASSERT_TRUE(interior.has_value());
  ASSERT_TRUE(after.has_value());

  ASSERT_TRUE(store.replace(2, 5, "AB").has_value());

  EXPECT_EQ(store.source(), "01AB789");
  EXPECT_EQ(MustResolve(store, *span), (ResolvedSourceSpan{2, 4}));
  EXPECT_EQ(SpanText(store, *span), "AB");
  EXPECT_EQ(store.resolveAnchor(*interior), std::nullopt);
  EXPECT_EQ(store.resolveAnchor(*after), std::optional<std::size_t>(6));
}

TEST(XMLSourceStore, DeleteExactSpanCollapsesBoundaryAnchors) {
  XMLSourceStore store("<a><b/></a>");
  std::optional<SourceAnchorSpan> span = store.createSpan(3, 7);
  ASSERT_TRUE(span.has_value());

  ASSERT_TRUE(store.replace(3, 4, "").has_value());

  EXPECT_EQ(store.source(), "<a></a>");
  EXPECT_EQ(MustResolve(store, *span), (ResolvedSourceSpan{3, 3}));
  EXPECT_EQ(SpanText(store, *span), "");
}

TEST(XMLSourceStore, ExplicitInvalidationMakesSpanUnresolvable) {
  XMLSourceStore store("abcdef");
  std::optional<SourceAnchorSpan> span = store.createSpan(1, 5);
  ASSERT_TRUE(span.has_value());

  store.invalidateAnchor(span->start);

  EXPECT_EQ(store.resolveAnchor(span->start), std::nullopt);
  EXPECT_EQ(store.resolveSpan(*span), std::nullopt);
}

TEST(XMLSourceStore, InvalidAnchorIdsResolveToNullopt) {
  XMLSourceStore store("abcdef");
  ASSERT_TRUE(store.createAnchor(1).has_value());

  EXPECT_EQ(store.resolveAnchor(SourceAnchorId{}), std::nullopt);
  EXPECT_EQ(store.resolveAnchor(SourceAnchorId{999}), std::nullopt);

  store.invalidateAnchor(SourceAnchorId{999});
  EXPECT_EQ(store.resolveAnchor(SourceAnchorId{999}), std::nullopt);
}

TEST(XMLSourceStore, ResolveSpanRejectsInvertedAnchorOrder) {
  XMLSourceStore store("abcdef");
  std::optional<SourceAnchorId> start = store.createAnchor(1);
  std::optional<SourceAnchorId> end = store.createAnchor(5);
  ASSERT_TRUE(start.has_value());
  ASSERT_TRUE(end.has_value());

  EXPECT_EQ(store.resolveSpan(SourceAnchorSpan{.start = *end, .end = *start}), std::nullopt);
}

TEST(XMLSourceStore, CreateSpanRejectsInvalidRangesAndInvalidatesPartialStart) {
  XMLSourceStore store(std::string("a\xC3\xA9z", 4));  // "aéz"

  EXPECT_FALSE(store.createSpan(3, 1).has_value());
  EXPECT_FALSE(store.createSpan(2, 3).has_value());

  EXPECT_FALSE(store.createSpan(0, 2).has_value());
  EXPECT_EQ(store.resolveAnchor(SourceAnchorId{1}), std::nullopt);
}

TEST(XMLSourceStore, ReplaceSkipsInvalidAnchors) {
  XMLSourceStore store("abcdef");
  std::optional<SourceAnchorId> invalidated = store.createAnchor(3);
  std::optional<SourceAnchorId> after = store.createAnchor(6);
  ASSERT_TRUE(invalidated.has_value());
  ASSERT_TRUE(after.has_value());

  store.invalidateAnchor(*invalidated);
  ASSERT_TRUE(store.replace(0, 1, "XY").has_value());

  EXPECT_EQ(store.resolveAnchor(*invalidated), std::nullopt);
  EXPECT_EQ(store.resolveAnchor(*after), std::optional<std::size_t>(7));
}

TEST(XMLSourceStore, RejectsOutOfBoundsEditsWithoutChangingVersion) {
  XMLSourceStore store("abc");

  EXPECT_FALSE(store.replace(4, 0, "x").has_value());
  EXPECT_FALSE(store.replace(2, 2, "x").has_value());

  EXPECT_EQ(store.source(), "abc");
  EXPECT_EQ(store.sourceVersion(), 0u);
}

TEST(XMLSourceStore, RejectsOffsetsInsideUtf8Codepoints) {
  XMLSourceStore store(std::string("a\xC3\xA9z", 4));  // "aéz"
  std::optional<SourceAnchorId> invalid = store.createAnchor(2, SourceAnchorBias::Before);
  EXPECT_FALSE(invalid.has_value());

  EXPECT_FALSE(store.replace(2, 0, "x").has_value());
  EXPECT_EQ(store.source(), std::string("a\xC3\xA9z", 4));
  EXPECT_EQ(store.sourceVersion(), 0u);
}

TEST(XMLSourceStore, RejectsInvalidUtf8Replacement) {
  XMLSourceStore store("abc");

  EXPECT_FALSE(store.replace(1, 0, std::string_view("\xC3", 1)).has_value());
  EXPECT_FALSE(store.replace(1, 0, std::string_view("\xC3x", 2)).has_value());
  EXPECT_FALSE(store.replace(1, 0, std::string_view("\xE2\x82", 2)).has_value());
  EXPECT_FALSE(store.replace(1, 0, std::string_view("\xF0\x9F\x98", 3)).has_value());

  EXPECT_EQ(store.source(), "abc");
  EXPECT_EQ(store.sourceVersion(), 0u);
}

TEST(XMLSourceStore, RejectsInvalidXmlSourceCodepoints) {
  XMLSourceStore store("abc");

  EXPECT_FALSE(store.replace(1, 0, std::string_view("\0", 1)).has_value());
  EXPECT_FALSE(store.replace(1, 0, std::string_view("\x01", 1)).has_value());
  EXPECT_FALSE(store.replace(1, 0, std::string_view("\xC0\xAF", 2)).has_value());
  EXPECT_FALSE(store.replace(1, 0, std::string_view("\xE0\x80\xAF", 3)).has_value());
  EXPECT_FALSE(store.replace(1, 0, std::string_view("\xF0\x80\x80\xAF", 4)).has_value());
  EXPECT_FALSE(store.replace(1, 0, std::string_view("\xF4\x90\x80\x80", 4)).has_value());
  EXPECT_FALSE(store.replace(1, 0, std::string_view("\xED\xA0\x80", 3)).has_value());
  EXPECT_FALSE(store.replace(1, 0, std::string_view("\xEF\xBF\xBE", 3)).has_value());
  EXPECT_FALSE(store.replace(1, 0, std::string_view("\xEF\xBF\xBF", 3)).has_value());

  EXPECT_EQ(store.source(), "abc");
  EXPECT_EQ(store.sourceVersion(), 0u);
}

TEST(XMLSourceStore, AcceptsValidMultibyteUtf8Replacement) {
  XMLSourceStore store("abc");

  ASSERT_TRUE(store.replace(1, 1, std::string_view("\xC3\xA9", 2)).has_value());
  ASSERT_TRUE(store.replace(3, 0, std::string_view("\xE2\x82\xAC", 3)).has_value());
  ASSERT_TRUE(store.replace(6, 0, std::string_view("\xF0\x9F\x98\x80", 4)).has_value());

  EXPECT_EQ(store.source(), std::string("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80", 10) + "c");
  EXPECT_EQ(store.sourceVersion(), 3u);
}

TEST(XMLSourceStore, RepeatedEditsBeforeSiblingDoNotRetargetSpan) {
  XMLSourceStore store("<svg><a/><b/><c/></svg>");
  const std::size_t bStart = store.source().find("<b/>");
  ASSERT_NE(bStart, std::string::npos);
  std::optional<SourceAnchorSpan> bSpan = store.createSpan(bStart, bStart + 4);
  ASSERT_TRUE(bSpan.has_value());

  ASSERT_TRUE(store.replace(5, 0, "<x/>").has_value());
  ASSERT_TRUE(store.replace(5, 0, "<y/>").has_value());
  ASSERT_TRUE(store.replace(5, 4, "").has_value());

  EXPECT_EQ(SpanText(store, *bSpan), "<b/>");
}

}  // namespace
}  // namespace donner::xml
