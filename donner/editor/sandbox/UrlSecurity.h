#pragma once
/// @file
///
/// Security-critical URL helpers used by `SvgSource`'s http(s) path.
/// Split out of the main source so the private-IP checks and the URL
/// parser have their own unit tests — they're the only thing standing
/// between a typed URL and an SSRF-class bug, and we want them hammered
/// on with table tests rather than only exercised end-to-end.

#include <netinet/in.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace donner::editor::sandbox::url_security {

/// True iff the IPv4 address (host byte order) falls in a range we
/// refuse to connect to: loopback, RFC1918 private, link-local (incl.
/// `169.254.169.254` cloud metadata), CGNAT, multicast, and any
/// reserved / future range. Deliberately strict — we're defending
/// against SSRF, so anything that isn't clearly public unicast is
/// treated as private.
[[nodiscard]] bool IsPrivateIPv4(std::uint32_t hostOrderAddr);

/// True iff the IPv6 address is loopback, link-local, unique-local,
/// multicast, unspecified, or IPv4-mapped onto a private IPv4.
[[nodiscard]] bool IsPrivateIPv6(const in6_addr& addr);

/// Parsed pieces of an http(s) URL needed for SSRF pinning. `valid`
/// is `false` when the input couldn't be parsed — callers should
/// reject the fetch in that case.
struct ParsedHttpUrl {
  std::string host;  ///< Hostname, brackets stripped for IPv6 literals.
  std::uint16_t port = 0;
  bool isHttps = false;
  bool ipv6Literal = false;
  bool valid = false;
};

/// Parse `[scheme]://[user:pass@]host[:port][/...]` for scheme ∈
/// {http, https}. Populates `ParsedHttpUrl`. Never throws. Rejects
/// anything that doesn't cleanly decode (returns with `valid = false`).
[[nodiscard]] ParsedHttpUrl ParseHttpUrl(std::string_view url);

}  // namespace donner::editor::sandbox::url_security
