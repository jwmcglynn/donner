#include "donner/editor/ResourcePolicy.h"

#include <gtest/gtest.h>

namespace donner::editor {
namespace {

// Force curl to appear "available" for all gatekeeper tests so that network scheme checks
// don't trip over a missing binary in CI.
class ResourceGatekeeperTest : public ::testing::Test {
protected:
  void SetUp() override { curlOverride_.emplace(CurlAvailability::State::kAvailable); }
  void TearDown() override { curlOverride_.reset(); }

private:
  std::optional<CurlAvailability::TestOverride> curlOverride_;
};

TEST_F(ResourceGatekeeperTest, DefaultPolicyValues) {
  ResourcePolicy p = DefaultDesktopPolicy();
  EXPECT_TRUE(p.allowHttps);
  EXPECT_FALSE(p.allowHttp);
  EXPECT_TRUE(p.allowFile);
  EXPECT_FALSE(p.allowData);
  EXPECT_TRUE(p.httpsPromptOnFirstUse);
  EXPECT_EQ(p.maxFileBytes, 100u * 1024u * 1024u);
  EXPECT_EQ(p.maxHttpBytes, 10u * 1024u * 1024u);
  EXPECT_EQ(p.httpTimeoutSeconds, 10);
  EXPECT_EQ(p.maxRedirects, 5);
}

TEST_F(ResourceGatekeeperTest, FileSchemeAllowed) {
  ResourcePolicy p = DefaultDesktopPolicy();
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("/tmp/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kAllow);
  EXPECT_EQ(d.resolved.scheme, ResolvedUri::Scheme::kFile);
}

TEST_F(ResourceGatekeeperTest, FileSchemeBlocked) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.allowFile = false;
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("/tmp/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
  EXPECT_FALSE(d.reason.empty());
}

TEST_F(ResourceGatekeeperTest, HttpsSchemeBlocked) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.allowHttps = false;
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("https://example.com/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
}

TEST_F(ResourceGatekeeperTest, HttpBlockedByDefault) {
  ResourcePolicy p = DefaultDesktopPolicy();
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("http://example.com/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
  EXPECT_NE(d.reason.find("plaintext HTTP"), std::string::npos);
}

TEST_F(ResourceGatekeeperTest, DataSchemeBlocked) {
  ResourcePolicy p = DefaultDesktopPolicy();
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("data:image/svg+xml,<svg/>");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
}

TEST_F(ResourceGatekeeperTest, DataSchemeAllowed) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.allowData = true;
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("data:image/svg+xml,<svg/>");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kAllow);
  EXPECT_EQ(d.resolved.scheme, ResolvedUri::Scheme::kData);
}

TEST_F(ResourceGatekeeperTest, UnsupportedScheme) {
  ResourcePolicy p = DefaultDesktopPolicy();
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("ftp://example.com/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
  EXPECT_NE(d.reason.find("Unsupported"), std::string::npos);
}

TEST_F(ResourceGatekeeperTest, EmptyUri) {
  ResourcePolicy p = DefaultDesktopPolicy();
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
}

TEST_F(ResourceGatekeeperTest, HttpsDenyHostTakesPrecedence) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.httpsPromptOnFirstUse = false;
  p.httpsAllowHosts = {"example.com"};
  p.httpsDenyHosts = {"example.com"};
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("https://example.com/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
  EXPECT_NE(d.reason.find("blocked"), std::string::npos);
}

TEST_F(ResourceGatekeeperTest, HttpsAllowHostMatch) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.httpsPromptOnFirstUse = false;
  p.httpsAllowHosts = {"example.com"};
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("https://example.com/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kAllow);
  EXPECT_EQ(d.resolved.host, "example.com");
}

TEST_F(ResourceGatekeeperTest, HttpsAllowHostNoMatch) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.httpsPromptOnFirstUse = false;
  p.httpsAllowHosts = {"example.com"};
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("https://other.com/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
}

TEST_F(ResourceGatekeeperTest, WildcardHostMatch) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.httpsPromptOnFirstUse = false;
  p.httpsAllowHosts = {"*.example.com"};
  ResourceGatekeeper gk(p);

  auto d1 = gk.resolve("https://foo.example.com/test.svg");
  EXPECT_EQ(d1.outcome, ResourceGatekeeper::Decision::Outcome::kAllow);

  // Wildcard *.example.com should NOT match "example.com" itself.
  auto d2 = gk.resolve("https://example.com/test.svg");
  EXPECT_EQ(d2.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
}

TEST_F(ResourceGatekeeperTest, FirstUseHostPrompt) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.httpsPromptOnFirstUse = true;
  ResourceGatekeeper gk(p);

  auto d = gk.resolve("https://example.com/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kNeedsUserConsent);
  EXPECT_EQ(d.pendingHost, "example.com");
}

TEST_F(ResourceGatekeeperTest, GrantHostAllowsSubsequent) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.httpsPromptOnFirstUse = true;
  ResourceGatekeeper gk(p);

  auto d1 = gk.resolve("https://example.com/test.svg");
  EXPECT_EQ(d1.outcome, ResourceGatekeeper::Decision::Outcome::kNeedsUserConsent);

  gk.grantHost("example.com");
  auto d2 = gk.resolve("https://example.com/test.svg");
  EXPECT_EQ(d2.outcome, ResourceGatekeeper::Decision::Outcome::kAllow);
}

TEST_F(ResourceGatekeeperTest, FileRootsRejectOutsidePath) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.fileRoots = {"/home/user/projects"};
  ResourceGatekeeper gk(p);

  auto d = gk.resolve("/etc/passwd");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
  EXPECT_NE(d.reason.find("outside"), std::string::npos);
}

TEST_F(ResourceGatekeeperTest, FileRootsAllowInsidePath) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.fileRoots = {"/tmp"};
  ResourceGatekeeper gk(p);

  auto d = gk.resolve("/tmp/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kAllow);
}

TEST_F(ResourceGatekeeperTest, PathTraversalRejected) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.fileRoots = {"/tmp/safe"};
  ResourceGatekeeper gk(p);

  auto d = gk.resolve("/tmp/safe/../etc/passwd");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
}

TEST_F(ResourceGatekeeperTest, FileSchemeUri) {
  ResourcePolicy p = DefaultDesktopPolicy();
  ResourceGatekeeper gk(p);
  auto d = gk.resolve("file:///tmp/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kAllow);
  EXPECT_EQ(d.resolved.scheme, ResolvedUri::Scheme::kFile);
}

TEST_F(ResourceGatekeeperTest, CurlMissingDeniesHttps) {
  CurlAvailability::TestOverride curlMissing(CurlAvailability::State::kMissing);

  ResourcePolicy p = DefaultDesktopPolicy();
  p.httpsPromptOnFirstUse = false;
  ResourceGatekeeper gk(p);

  auto d = gk.resolve("https://example.com/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kDeny);
  EXPECT_NE(d.reason.find("curl"), std::string::npos);
}

TEST_F(ResourceGatekeeperTest, HttpsWithPort) {
  ResourcePolicy p = DefaultDesktopPolicy();
  p.httpsPromptOnFirstUse = false;
  p.httpsAllowHosts = {"example.com"};
  ResourceGatekeeper gk(p);

  auto d = gk.resolve("https://example.com:8443/test.svg");
  EXPECT_EQ(d.outcome, ResourceGatekeeper::Decision::Outcome::kAllow);
  EXPECT_EQ(d.resolved.host, "example.com");
}

}  // namespace
}  // namespace donner::editor
