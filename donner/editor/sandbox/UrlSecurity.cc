#include "donner/editor/sandbox/UrlSecurity.h"

#include <netinet/in.h>

#include <cstdint>
#include <string_view>

namespace donner::editor::sandbox::url_security {

bool IsPrivateIPv4(std::uint32_t h) {
  const std::uint8_t a = static_cast<std::uint8_t>(h >> 24);
  const std::uint8_t b = static_cast<std::uint8_t>(h >> 16);
  const std::uint8_t c = static_cast<std::uint8_t>(h >> 8);
  if (a == 0) return true;                             // 0.0.0.0/8 "this network"
  if (a == 10) return true;                            // 10.0.0.0/8
  if (a == 127) return true;                           // 127.0.0.0/8 loopback
  if (a == 169 && b == 254) return true;               // 169.254.0.0/16 link-local + metadata
  if (a == 172 && (b >= 16 && b <= 31)) return true;   // 172.16.0.0/12
  if (a == 192 && b == 0 && c == 0) return true;       // 192.0.0.0/24 IETF reserved
  if (a == 192 && b == 168) return true;               // 192.168.0.0/16
  if (a == 100 && (b >= 64 && b <= 127)) return true;  // 100.64.0.0/10 CGNAT
  if (a >= 224) return true;                           // 224.0.0.0/4 multicast + 240/4 reserved
  return false;
}

bool IsPrivateIPv6(const in6_addr& a) {
  if (IN6_IS_ADDR_LOOPBACK(&a)) return true;
  if (IN6_IS_ADDR_LINKLOCAL(&a)) return true;
  if (IN6_IS_ADDR_SITELOCAL(&a)) return true;
  if (IN6_IS_ADDR_MULTICAST(&a)) return true;
  if (IN6_IS_ADDR_UNSPECIFIED(&a)) return true;
  // fc00::/7 unique-local (RFC 4193) — IN6_IS_ADDR_SITELOCAL covers the
  // deprecated fec0::/10 form but not the modern ULA range.
  if ((a.s6_addr[0] & 0xFE) == 0xFC) return true;
  // IPv4-mapped IPv6 (::ffff:a.b.c.d) — dispatch to the v4 classifier
  // so `::ffff:127.0.0.1` and friends are rejected.
  if (IN6_IS_ADDR_V4MAPPED(&a)) {
    const std::uint32_t v4 = (std::uint32_t(a.s6_addr[12]) << 24) |
                             (std::uint32_t(a.s6_addr[13]) << 16) |
                             (std::uint32_t(a.s6_addr[14]) << 8) | std::uint32_t(a.s6_addr[15]);
    return IsPrivateIPv4(v4);
  }
  return false;
}

ParsedHttpUrl ParseHttpUrl(std::string_view url) {
  ParsedHttpUrl out;
  std::string_view rest;
  constexpr std::string_view kHttps = "https://";
  constexpr std::string_view kHttp = "http://";
  if (url.substr(0, kHttps.size()) == kHttps) {
    out.isHttps = true;
    out.port = 443;
    rest = url.substr(kHttps.size());
  } else if (url.substr(0, kHttp.size()) == kHttp) {
    out.isHttps = false;
    out.port = 80;
    rest = url.substr(kHttp.size());
  } else {
    return out;
  }

  const auto authorityEnd = rest.find_first_of("/?#");
  std::string_view authority =
      (authorityEnd != std::string_view::npos) ? rest.substr(0, authorityEnd) : rest;

  // Strip userinfo (`user:pass@`).
  if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
    authority = authority.substr(at + 1);
  }

  if (!authority.empty() && authority.front() == '[') {
    // IPv6 literal in brackets: `[::1]` or `[::1]:8080`.
    const auto close = authority.find(']');
    if (close == std::string_view::npos) return out;
    out.host.assign(authority.substr(1, close - 1));
    out.ipv6Literal = true;
    std::string_view after = authority.substr(close + 1);
    if (!after.empty() && after.front() == ':') {
      after.remove_prefix(1);
      std::uint32_t p = 0;
      for (const char ch : after) {
        if (ch < '0' || ch > '9') return out;
        p = p * 10 + static_cast<std::uint32_t>(ch - '0');
        if (p > 65535) return out;
      }
      if (p > 0) out.port = static_cast<std::uint16_t>(p);
    }
  } else {
    // Plain `host[:port]`.
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
      std::string_view afterColon = authority.substr(colon + 1);
      std::uint32_t p = 0;
      bool allDigits = !afterColon.empty();
      for (const char ch : afterColon) {
        if (ch < '0' || ch > '9') {
          allDigits = false;
          break;
        }
        p = p * 10 + static_cast<std::uint32_t>(ch - '0');
      }
      if (allDigits && p > 0 && p <= 65535) {
        out.host.assign(authority.substr(0, colon));
        out.port = static_cast<std::uint16_t>(p);
      } else {
        out.host.assign(authority);
      }
    } else {
      out.host.assign(authority);
    }
  }

  if (out.host.empty()) return out;
  out.valid = true;
  return out;
}

}  // namespace donner::editor::sandbox::url_security
