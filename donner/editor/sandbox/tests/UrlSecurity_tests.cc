#include "donner/editor/sandbox/UrlSecurity.h"

#include <arpa/inet.h>  // IWYU pragma: keep
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace donner::editor::sandbox::url_security {
namespace {

// ───────────────────────────────────────────────────────────────────────
// IsPrivateIPv4
// ───────────────────────────────────────────────────────────────────────

struct Ipv4Case {
  std::string address;
  bool expectPrivate;
};

class Ipv4Test : public ::testing::TestWithParam<Ipv4Case> {};

TEST_P(Ipv4Test, ClassifiesCorrectly) {
  const Ipv4Case& c = GetParam();
  in_addr a{};
  ASSERT_EQ(::inet_pton(AF_INET, c.address.c_str(), &a), 1);
  const std::uint32_t h = ntohl(a.s_addr);
  EXPECT_EQ(IsPrivateIPv4(h), c.expectPrivate) << c.address;
}

INSTANTIATE_TEST_SUITE_P(Addresses, Ipv4Test,
                         ::testing::Values(
                             // Loopback.
                             Ipv4Case{"127.0.0.1", true}, Ipv4Case{"127.255.255.255", true},
                             // RFC 1918 private.
                             Ipv4Case{"10.0.0.1", true}, Ipv4Case{"10.255.255.255", true},
                             Ipv4Case{"172.16.0.1", true}, Ipv4Case{"172.31.255.255", true},
                             Ipv4Case{"172.32.0.1", false},  // just outside 172.16/12
                             Ipv4Case{"172.15.0.1", false},  // just outside 172.16/12
                             Ipv4Case{"192.168.1.1", true}, Ipv4Case{"192.168.255.255", true},
                             // Link-local + cloud metadata — the whole point of this module.
                             Ipv4Case{"169.254.0.1", true},
                             Ipv4Case{"169.254.169.254", true},  // AWS/GCE metadata endpoint
                             Ipv4Case{"169.253.255.255", false},
                             // CGNAT 100.64/10.
                             Ipv4Case{"100.64.0.1", true}, Ipv4Case{"100.127.255.255", true},
                             Ipv4Case{"100.63.255.255", false}, Ipv4Case{"100.128.0.1", false},
                             // "This network" 0.0.0.0/8.
                             Ipv4Case{"0.0.0.0", true}, Ipv4Case{"0.1.2.3", true},
                             // Multicast + reserved.
                             Ipv4Case{"224.0.0.1", true}, Ipv4Case{"239.255.255.255", true},
                             Ipv4Case{"255.255.255.255", true},
                             // Normal public addresses that MUST classify as non-private —
                             // if any of these regress the SSRF gate will lock users out.
                             Ipv4Case{"1.1.1.1", false}, Ipv4Case{"8.8.8.8", false},
                             Ipv4Case{"208.80.154.224", false},  // upload.wikimedia.org
                             Ipv4Case{"151.101.1.140", false},   // fastly
                             Ipv4Case{"172.15.255.255", false},  // one off from 172.16/12
                             Ipv4Case{"192.0.1.1", false}));

// ───────────────────────────────────────────────────────────────────────
// IsPrivateIPv6
// ───────────────────────────────────────────────────────────────────────

struct Ipv6Case {
  std::string address;
  bool expectPrivate;
};

class Ipv6Test : public ::testing::TestWithParam<Ipv6Case> {};

TEST_P(Ipv6Test, ClassifiesCorrectly) {
  const Ipv6Case& c = GetParam();
  in6_addr a{};
  ASSERT_EQ(::inet_pton(AF_INET6, c.address.c_str(), &a), 1);
  EXPECT_EQ(IsPrivateIPv6(a), c.expectPrivate) << c.address;
}

INSTANTIATE_TEST_SUITE_P(
    Addresses, Ipv6Test,
    ::testing::Values(Ipv6Case{"::1", true},                // loopback
                      Ipv6Case{"::", true},                 // unspecified
                      Ipv6Case{"fe80::1", true},            // link-local
                      Ipv6Case{"fc00::1", true},            // unique-local
                      Ipv6Case{"fd12:3456:789a::1", true},  // unique-local
                      Ipv6Case{"ff02::1", true},            // multicast
                      // IPv4-mapped private should still be caught.
                      Ipv6Case{"::ffff:127.0.0.1", true},
                      Ipv6Case{"::ffff:169.254.169.254", true},  // metadata via v4-mapped
                      Ipv6Case{"::ffff:10.0.0.1", true},
                      // IPv4-mapped public is allowed.
                      Ipv6Case{"::ffff:1.1.1.1", false},
                      // Public v6.
                      Ipv6Case{"2606:4700:4700::1111", false},  // cloudflare
                      Ipv6Case{"2001:db8::1", false}));  // documentation range is technically
                                                         // reserved but we don't block it (not in
                                                         // the IN6_IS_ADDR_* predicates)

// ───────────────────────────────────────────────────────────────────────
// ParseHttpUrl
// ───────────────────────────────────────────────────────────────────────

TEST(ParseHttpUrlTest, PlainHttps) {
  auto p = ParseHttpUrl("https://example.com/foo");
  ASSERT_TRUE(p.valid);
  EXPECT_EQ(p.host, "example.com");
  EXPECT_EQ(p.port, 443);
  EXPECT_TRUE(p.isHttps);
  EXPECT_FALSE(p.ipv6Literal);
}

TEST(ParseHttpUrlTest, PlainHttp) {
  auto p = ParseHttpUrl("http://example.com");
  ASSERT_TRUE(p.valid);
  EXPECT_EQ(p.host, "example.com");
  EXPECT_EQ(p.port, 80);
  EXPECT_FALSE(p.isHttps);
}

TEST(ParseHttpUrlTest, ExplicitPort) {
  auto p = ParseHttpUrl("https://example.com:8443/foo");
  ASSERT_TRUE(p.valid);
  EXPECT_EQ(p.host, "example.com");
  EXPECT_EQ(p.port, 8443);
}

TEST(ParseHttpUrlTest, UserinfoStripped) {
  auto p = ParseHttpUrl("https://alice:secret@example.com/foo");
  ASSERT_TRUE(p.valid);
  EXPECT_EQ(p.host, "example.com");
  EXPECT_EQ(p.port, 443);
}

TEST(ParseHttpUrlTest, UserinfoAndPort) {
  auto p = ParseHttpUrl("https://alice@example.com:8443/foo?q=1#frag");
  ASSERT_TRUE(p.valid);
  EXPECT_EQ(p.host, "example.com");
  EXPECT_EQ(p.port, 8443);
}

TEST(ParseHttpUrlTest, Ipv6Literal) {
  auto p = ParseHttpUrl("https://[2606:4700:4700::1111]/foo");
  ASSERT_TRUE(p.valid);
  EXPECT_EQ(p.host, "2606:4700:4700::1111");
  EXPECT_EQ(p.port, 443);
  EXPECT_TRUE(p.ipv6Literal);
}

TEST(ParseHttpUrlTest, Ipv6LiteralWithPort) {
  auto p = ParseHttpUrl("https://[::1]:8443/foo");
  ASSERT_TRUE(p.valid);
  EXPECT_EQ(p.host, "::1");
  EXPECT_EQ(p.port, 8443);
  EXPECT_TRUE(p.ipv6Literal);
}

TEST(ParseHttpUrlTest, BareHostNoPath) {
  auto p = ParseHttpUrl("https://example.com");
  ASSERT_TRUE(p.valid);
  EXPECT_EQ(p.host, "example.com");
}

TEST(ParseHttpUrlTest, EmptyRejected) {
  EXPECT_FALSE(ParseHttpUrl("").valid);
}

TEST(ParseHttpUrlTest, NonHttpSchemeRejected) {
  EXPECT_FALSE(ParseHttpUrl("file:///etc/passwd").valid);
  EXPECT_FALSE(ParseHttpUrl("gopher://example.com/").valid);
  EXPECT_FALSE(ParseHttpUrl("ftp://example.com/").valid);
  EXPECT_FALSE(ParseHttpUrl("data:text/plain,hi").valid);
}

TEST(ParseHttpUrlTest, EmptyHostRejected) {
  EXPECT_FALSE(ParseHttpUrl("https:///").valid);
  EXPECT_FALSE(ParseHttpUrl("https://").valid);
}

TEST(ParseHttpUrlTest, UnclosedIpv6BracketRejected) {
  EXPECT_FALSE(ParseHttpUrl("https://[::1/foo").valid);
}

TEST(ParseHttpUrlTest, InvalidPortCharsFallThroughToHost) {
  // "example.com:abc" is treated as a literal host (no valid port),
  // which ends up invalid downstream (DNS will fail). We specifically
  // DO NOT treat it as valid host + default port — a future stricter
  // parser may reject it outright, but for now the important thing is
  // we never crash or interpret it as `example.com:abc` → `example.com`
  // with port 443 (which would let attackers smuggle metadata).
  auto p = ParseHttpUrl("https://example.com:abc/foo");
  ASSERT_TRUE(p.valid);
  EXPECT_EQ(p.host, "example.com:abc");
  EXPECT_EQ(p.port, 443);
}

TEST(ParseHttpUrlTest, QueryAndFragmentIgnored) {
  auto p = ParseHttpUrl("https://example.com/a?b=c#d");
  ASSERT_TRUE(p.valid);
  EXPECT_EQ(p.host, "example.com");
}

TEST(ParseHttpUrlTest, PortAtBoundary) {
  auto p1 = ParseHttpUrl("https://example.com:0/");
  // Port 0 is meaningless for a client fetch; we keep the default in that case.
  ASSERT_TRUE(p1.valid);
  EXPECT_EQ(p1.host, "example.com:0");
  EXPECT_EQ(p1.port, 443);

  auto p2 = ParseHttpUrl("https://example.com:65535/");
  ASSERT_TRUE(p2.valid);
  EXPECT_EQ(p2.port, 65535);

  auto p3 = ParseHttpUrl("https://example.com:65536/");
  // Out of range falls through to host (like alpha chars).
  ASSERT_TRUE(p3.valid);
  EXPECT_EQ(p3.host, "example.com:65536");
}

}  // namespace
}  // namespace donner::editor::sandbox::url_security
