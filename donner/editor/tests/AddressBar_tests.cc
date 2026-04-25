// NOLINTBEGIN(llvm-include-order)
#include <cstdint>
// NOLINTEND(llvm-include-order)

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "donner/editor/AddressBar.h"
#include "donner/editor/AddressBarStatus.h"

namespace donner::editor {
namespace {

TEST(AddressBarTest, DefaultStateHasNoNavigation) {
  AddressBar bar;
  EXPECT_FALSE(bar.consumeNavigation().has_value());
}

TEST(AddressBarTest, SetInitialUriSeedsBuffer) {
  AddressBar bar;
  bar.setInitialUri("file:///tmp/test.svg");
  // setInitialUri does not trigger navigation — it only seeds the display.
  EXPECT_FALSE(bar.consumeNavigation().has_value());
}

TEST(AddressBarTest, NotifyDropPayloadQueuesNavigation) {
  AddressBar bar;
  std::vector<uint8_t> bytes = {0x3C, 0x73, 0x76, 0x67};
  bar.notifyDropPayload("dropped.svg", bytes);

  auto nav = bar.consumeNavigation();
  ASSERT_TRUE(nav.has_value());
  EXPECT_EQ(nav->uri, "dropped.svg");
  EXPECT_EQ(nav->bytes, bytes);
}

TEST(AddressBarTest, ConsumeNavigationClearsPending) {
  AddressBar bar;
  bar.notifyDropPayload("a.svg", {});

  auto nav1 = bar.consumeNavigation();
  ASSERT_TRUE(nav1.has_value());

  auto nav2 = bar.consumeNavigation();
  EXPECT_FALSE(nav2.has_value());
}

TEST(AddressBarTest, PushHistoryDeduplicates) {
  AddressBar bar;
  bar.pushHistory("a.svg");
  bar.pushHistory("b.svg");
  bar.pushHistory("a.svg");

  const auto& h = bar.history();
  ASSERT_EQ(h.size(), 2u);
  EXPECT_EQ(h[0], "a.svg");
  EXPECT_EQ(h[1], "b.svg");
}

TEST(AddressBarTest, PushHistoryCapsAtLimit) {
  AddressBar bar;
  for (int i = 0; i < 20; ++i) {
    bar.pushHistory("uri_" + std::to_string(i) + ".svg");
  }

  const auto& h = bar.history();
  EXPECT_EQ(h.size(), 16u);
  // Most recent is first.
  EXPECT_EQ(h[0], "uri_19.svg");
  // Oldest that fits is index 15.
  EXPECT_EQ(h[15], "uri_4.svg");
}

TEST(AddressBarTest, PushHistoryNewestFirst) {
  AddressBar bar;
  bar.pushHistory("first.svg");
  bar.pushHistory("second.svg");

  const auto& h = bar.history();
  ASSERT_EQ(h.size(), 2u);
  EXPECT_EQ(h[0], "second.svg");
  EXPECT_EQ(h[1], "first.svg");
}

TEST(AddressBarTest, SetStatusReplacesChip) {
  AddressBar bar;
  bar.setStatus({AddressBarStatus::kLoading, "Fetching…", "http://example.com/foo.svg"});
  // The chip is exposed only through draw() rendering; we verify no crash.
  bar.setStatus({AddressBarStatus::kRendered, "OK", "http://example.com/foo.svg"});
}

TEST(AddressBarTest, NotifyDropPayloadOverwritesPrevious) {
  AddressBar bar;
  bar.notifyDropPayload("first.svg", {1, 2, 3});
  bar.notifyDropPayload("second.svg", {4, 5, 6});

  auto nav = bar.consumeNavigation();
  ASSERT_TRUE(nav.has_value());
  EXPECT_EQ(nav->uri, "second.svg");
  EXPECT_EQ(nav->bytes, (std::vector<uint8_t>{4, 5, 6}));
}

// Regression gate for the on-demand render loop: `isLoadingAnimationActive()`
// is read every frame by main.cc to decide whether to keep polling.
// Default / terminal / determinate-progress chips must NOT register as
// animating — otherwise the UI would burn CPU at 60 FPS whenever the
// chip sits on `kRendered` or whenever a downloading chip has a real
// percentage.
TEST(AddressBarTest, IsLoadingAnimationActiveFalseByDefault) {
  AddressBar bar;
  EXPECT_FALSE(bar.isLoadingAnimationActive());
}

TEST(AddressBarTest, IsLoadingAnimationActiveTrueForIndeterminateLoading) {
  AddressBar bar;
  bar.setStatus({AddressBarStatus::kLoading, "Fetching…", "http://example.com/foo.svg"});
  bar.setLoadProgress(std::nullopt);
  EXPECT_TRUE(bar.isLoadingAnimationActive());
}

TEST(AddressBarTest, IsLoadingAnimationActiveFalseForDeterminateLoading) {
  AddressBar bar;
  bar.setStatus({AddressBarStatus::kLoading, "Fetching…", "http://example.com/foo.svg"});
  bar.setLoadProgress(0.5f);
  EXPECT_FALSE(bar.isLoadingAnimationActive());
}

TEST(AddressBarTest, IsLoadingAnimationActiveClearsOnTerminalChip) {
  AddressBar bar;
  bar.setStatus({AddressBarStatus::kLoading, "Fetching…", "http://example.com/foo.svg"});
  bar.setLoadProgress(std::nullopt);
  EXPECT_TRUE(bar.isLoadingAnimationActive());

  bar.setStatus({AddressBarStatus::kRendered, "OK", "http://example.com/foo.svg"});
  EXPECT_FALSE(bar.isLoadingAnimationActive());

  bar.setStatus({AddressBarStatus::kFetchError, "timed out", "http://example.com/foo.svg"});
  EXPECT_FALSE(bar.isLoadingAnimationActive());
}

}  // namespace
}  // namespace donner::editor
