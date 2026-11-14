#include "donner/svg/resources/UrlReference.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace donner::svg {
namespace {

std::optional<std::string> Resolve(std::string_view base, std::string_view reference) {
  return ResolveUrlReference(base, reference);
}

TEST(UrlReference, EmptyBaseKeepsReferenceRelativeToTheDocument) {
  EXPECT_EQ(Resolve("", "../../../resources/green.css"), "../../../resources/green.css");
  EXPECT_EQ(Resolve("", "theme.css"), "theme.css");
  EXPECT_EQ(Resolve("", "./a/../theme.css"), "theme.css");
  EXPECT_EQ(Resolve("", "https://example.test/a/./b/../c.css"), "https://example.test/a/c.css");
}

TEST(UrlReference, RelativePathMergesWithRelativeBaseDirectory) {
  EXPECT_EQ(Resolve("styles/main.css", "theme.css"), "styles/theme.css");
  EXPECT_EQ(Resolve("styles/main.css", "./nested/theme.css"), "styles/nested/theme.css");
  EXPECT_EQ(Resolve("styles/main.css", "../theme.css"), "theme.css");
  EXPECT_EQ(Resolve("main.css", "theme.css"), "theme.css");
}

TEST(UrlReference, LeadingParentSegmentsOfARelativeBaseArePreserved) {
  // The base is itself relative to the document, so unmatched ".." segments still have meaning
  // for the caller's resource loader and must not be dropped.
  EXPECT_EQ(Resolve("../../../resources/green.css", "blue.css"), "../../../resources/blue.css");
  EXPECT_EQ(Resolve("../../../resources/green.css", "../../up.css"), "../../../../up.css");
  EXPECT_EQ(Resolve("../a.css", "../../b.css"), "../../../b.css");
}

TEST(UrlReference, AbsolutePathReferencesReplaceThePath) {
  EXPECT_EQ(Resolve("styles/main.css", "/root.css"), "/root.css");
  EXPECT_EQ(Resolve("https://example.test/a/b/main.css", "/root.css"),
            "https://example.test/root.css");
  EXPECT_EQ(Resolve("/a/b.css", "/../../c.css"), "/c.css");
}

TEST(UrlReference, HierarchicalBasesFollowRfc3986) {
  constexpr std::string_view base = "https://example.test/a/b/main.css?q=1#frag";
  EXPECT_EQ(Resolve(base, "theme.css"), "https://example.test/a/b/theme.css");
  EXPECT_EQ(Resolve(base, "../theme.css"), "https://example.test/a/theme.css");
  EXPECT_EQ(Resolve(base, "../../../../theme.css"), "https://example.test/theme.css");
  EXPECT_EQ(Resolve(base, "theme.css?v=2"), "https://example.test/a/b/theme.css?v=2");
  EXPECT_EQ(Resolve(base, "?v=2"), "https://example.test/a/b/main.css?v=2");
  EXPECT_EQ(Resolve(base, "#other"), "https://example.test/a/b/main.css?q=1#other");
  EXPECT_EQ(Resolve(base, "//cdn.example.test/x/../y.css"), "https://cdn.example.test/y.css");
  EXPECT_EQ(Resolve("https://example.test", "theme.css"), "https://example.test/theme.css");
}

TEST(UrlReference, AbsoluteReferencesIgnoreTheBase) {
  EXPECT_EQ(Resolve("styles/main.css", "https://example.test/theme.css"),
            "https://example.test/theme.css");
  EXPECT_EQ(Resolve("data:text/css,a{}", "data:text/css,b{}"), "data:text/css,b{}");
  EXPECT_EQ(Resolve("https://example.test/main.css", "file:///etc/theme.css"),
            "file:///etc/theme.css");
}

TEST(UrlReference, OpaqueReferencesAreKeptVerbatim) {
  constexpr std::string_view dataUrl = "data:text/css,a/../b/./c?d#e";
  EXPECT_EQ(Resolve("", dataUrl), dataUrl);
  EXPECT_EQ(Resolve("styles/main.css", dataUrl), dataUrl);
  EXPECT_EQ(Resolve("data:text/css,x", dataUrl), dataUrl);
}

TEST(UrlReference, RelativeReferencesAgainstOpaqueBasesAreUnresolvable) {
  EXPECT_EQ(Resolve("data:text/css,a{}", "theme.css"), std::nullopt);
  EXPECT_EQ(Resolve("data:text/css,a{}", "/theme.css"), std::nullopt);
  EXPECT_EQ(Resolve("mailto:someone", "theme.css"), std::nullopt);
}

TEST(UrlReference, DotSegmentsCannotEscapeAnAbsoluteRoot) {
  EXPECT_EQ(Resolve("/a/main.css", "../../../../etc/passwd"), "/etc/passwd");
  EXPECT_EQ(Resolve("https://example.test/main.css", "../../x.css"), "https://example.test/x.css");
}

TEST(UrlReference, NormalizationNeverChangesHowTheResultParses) {
  // Removing "./" must not turn a path segment into a scheme, nor "/." into an authority.
  EXPECT_EQ(Resolve("", "./a:b.css"), "./a:b.css");
  EXPECT_EQ(Resolve("x/main.css", "../a:b.css"), "./a:b.css");
  EXPECT_EQ(Resolve("", "/.//host/x.css"), "/.//host/x.css");
  EXPECT_EQ(Resolve("https://example.test/a/b.css", "/.//x.css"), "https://example.test//x.css");
}

TEST(UrlReference, TrailingDotSegmentsKeepADirectory) {
  EXPECT_EQ(Resolve("a/b/main.css", "."), "a/b/");
  EXPECT_EQ(Resolve("a/b/main.css", ".."), "a/");
  EXPECT_EQ(Resolve("a/b/main.css", "c/."), "a/b/c/");
}

}  // namespace
}  // namespace donner::svg
