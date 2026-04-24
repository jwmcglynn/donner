#include <gtest/gtest.h>

#include <optional>

#include "donner/editor/ResourcePolicy.h"
#include "donner/editor/SvgFetcher.h"
#include "donner/editor/sandbox/SvgSource.h"

namespace donner::editor {
namespace {

class DesktopFetcherTest : public ::testing::Test {
protected:
  void SetUp() override { curlOverride_.emplace(CurlAvailability::State::kAvailable); }
  void TearDown() override { curlOverride_.reset(); }

private:
  std::optional<CurlAvailability::TestOverride> curlOverride_;
};

TEST_F(DesktopFetcherTest, StrictFetcherSurfacesFirstUseConsentAsError) {
  ResourceGatekeeper gatekeeper(DefaultDesktopPolicy());
  sandbox::SvgSource source;

  auto fetcher =
      MakeDesktopFetcher(gatekeeper, source, /*autoGrantFirstUse=*/false);

  std::optional<FetchError::Kind> errKind;
  (void)fetcher->fetch(
      "https://upload.wikimedia.org/wikipedia/commons/4/4f/SVG_Logo.svg",
      [&](std::optional<FetchBytes>, std::optional<FetchError> err) {
        if (err) errKind = err->kind;
      });

  ASSERT_TRUE(errKind.has_value());
  EXPECT_EQ(*errKind, FetchError::Kind::kNeedsUserConsent);
}

TEST_F(DesktopFetcherTest, UserInitiatedFetcherAutoGrantsFirstUseHost) {
  ResourceGatekeeper gatekeeper(DefaultDesktopPolicy());
  sandbox::SvgSource source;

  // Sanity: the gatekeeper would prompt on a fresh https host.
  {
    auto d = gatekeeper.resolve("https://upload.wikimedia.org/foo.svg");
    ASSERT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kNeedsUserConsent);
  }

  auto fetcher =
      MakeDesktopFetcher(gatekeeper, source, /*autoGrantFirstUse=*/true);

  std::optional<FetchError::Kind> errKind;
  (void)fetcher->fetch(
      "https://upload.wikimedia.org/wikipedia/commons/4/4f/SVG_Logo.svg",
      [&](std::optional<FetchBytes>, std::optional<FetchError> err) {
        if (err) errKind = err->kind;
      });

  // The outer fetch may succeed or fail depending on whether the sandbox
  // has network — we don't assert on that. What we DO assert: the
  // gatekeeper now considers the host granted, proving we got past the
  // first-use prompt transparently.
  auto d2 = gatekeeper.resolve("https://upload.wikimedia.org/other.svg");
  EXPECT_EQ(d2.outcome, ResourceGatekeeper::Decision::Outcome::kAllow);

  // Specifically, the fetch must NOT have surfaced kNeedsUserConsent to
  // the caller — that would defeat the whole point of the flag.
  EXPECT_NE(errKind, FetchError::Kind::kNeedsUserConsent);
}

}  // namespace
}  // namespace donner::editor
