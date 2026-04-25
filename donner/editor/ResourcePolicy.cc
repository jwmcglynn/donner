#include "donner/editor/ResourcePolicy.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace donner::editor {

namespace {

constexpr std::string_view kFileScheme = "file://";
constexpr std::string_view kHttpsScheme = "https://";
constexpr std::string_view kHttpScheme = "http://";
constexpr std::string_view kDataScheme = "data:";

/// Returns true if the wildcard pattern `pattern` matches `host`.
/// Supports exact match and leading-wildcard: `*.example.com` matches `foo.example.com`
/// but NOT `example.com` itself.
bool HostMatches(std::string_view pattern, std::string_view host) {
  if (pattern == host) {
    return true;
  }
  // Wildcard patterns: "*.example.com".
  if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
    std::string_view suffix = pattern.substr(1);  // ".example.com"
    // Host must be longer than the suffix (i.e. have at least one subdomain character).
    if (host.size() > suffix.size()) {
      std::string_view hostTail = host.substr(host.size() - suffix.size());
      return hostTail == suffix;
    }
  }
  return false;
}

/// Returns true if `host` matches any entry in `patterns`.
bool HostMatchesAny(const std::vector<std::string>& patterns, std::string_view host) {
  return std::any_of(patterns.begin(), patterns.end(),
                     [&](const std::string& p) { return HostMatches(p, host); });
}

/// Extracts the host portion from an http(s) URL. Returns empty if parse fails.
std::string_view ExtractHost(std::string_view url) {
  // Skip scheme.
  auto pos = url.find("://");
  if (pos == std::string_view::npos) {
    return {};
  }
  std::string_view remainder = url.substr(pos + 3);
  // Host ends at '/', '?', '#', or end-of-string. Strip optional port.
  auto end = remainder.find_first_of("/?#");
  std::string_view hostPort =
      (end != std::string_view::npos) ? remainder.substr(0, end) : remainder;
  // Strip port if present.
  auto colon = hostPort.rfind(':');
  if (colon != std::string_view::npos) {
    // Verify the part after colon is numeric (it's a port).
    bool allDigits = true;
    for (std::size_t i = colon + 1; i < hostPort.size(); ++i) {
      if (hostPort[i] < '0' || hostPort[i] > '9') {
        allDigits = false;
        break;
      }
    }
    if (allDigits && colon + 1 < hostPort.size()) {
      hostPort = hostPort.substr(0, colon);
    }
  }
  return hostPort;
}

/// Determines the scheme from a URI string.
enum class ParsedScheme { kFile, kHttp, kHttps, kData, kUnknown };

ParsedScheme ClassifyScheme(std::string_view uri) {
  if (uri.size() >= kFileScheme.size() && uri.substr(0, kFileScheme.size()) == kFileScheme) {
    return ParsedScheme::kFile;
  }
  if (uri.size() >= kHttpsScheme.size() && uri.substr(0, kHttpsScheme.size()) == kHttpsScheme) {
    return ParsedScheme::kHttps;
  }
  if (uri.size() >= kHttpScheme.size() && uri.substr(0, kHttpScheme.size()) == kHttpScheme) {
    return ParsedScheme::kHttp;
  }
  if (uri.size() >= kDataScheme.size() && uri.substr(0, kDataScheme.size()) == kDataScheme) {
    return ParsedScheme::kData;
  }
  // Check for another explicit scheme.
  auto colon = uri.find(':');
  if (colon != std::string_view::npos && colon > 0 && colon + 2 < uri.size() &&
      uri[colon + 1] == '/' && uri[colon + 2] == '/') {
    return ParsedScheme::kUnknown;
  }
  // Bare path → file.
  return ParsedScheme::kFile;
}

}  // namespace

ResourcePolicy DefaultDesktopPolicy() {
  ResourcePolicy p;
  p.allowHttps = true;
  p.allowHttp = false;
  p.allowFile = true;
  p.allowData = false;
  p.httpsPromptOnFirstUse = true;
  p.maxFileBytes = 100u * 1024u * 1024u;
  p.maxHttpBytes = 10u * 1024u * 1024u;
  p.httpTimeoutSeconds = 10;
  p.maxRedirects = 5;
  p.subresources = ResourcePolicy::SubresourcePolicy::kBlockAll;
  return p;
}

ResourceGatekeeper::ResourceGatekeeper(ResourcePolicy policy) : policy_(std::move(policy)) {}

ResourceGatekeeper::Decision ResourceGatekeeper::resolve(std::string_view uri) const {
  Decision decision;
  decision.outcome = Decision::Outcome::kDeny;

  if (uri.empty()) {
    decision.reason = "Empty URI.";
    return decision;
  }

  const ParsedScheme scheme = ClassifyScheme(uri);

  // Scheme check.
  switch (scheme) {
    case ParsedScheme::kFile:
      if (!policy_.allowFile) {
        decision.reason = "file:// URIs are blocked by policy.";
        return decision;
      }
      break;
    case ParsedScheme::kHttps:
      if (!policy_.allowHttps) {
        decision.reason = "https:// URIs are blocked by policy.";
        return decision;
      }
      break;
    case ParsedScheme::kHttp:
      if (!policy_.allowHttp) {
        decision.reason = "http:// URIs are blocked by policy (plaintext HTTP is disabled).";
        return decision;
      }
      break;
    case ParsedScheme::kData:
      if (!policy_.allowData) {
        decision.reason = "data: URIs are blocked by policy.";
        return decision;
      }
      break;
    case ParsedScheme::kUnknown: decision.reason = "Unsupported URI scheme."; return decision;
  }

  // Curl check for http/https.
  if (scheme == ParsedScheme::kHttps || scheme == ParsedScheme::kHttp) {
    const auto curlState = CurlAvailability::check();
    if (curlState == CurlAvailability::State::kMissing) {
      decision.reason = "curl is not installed. " + CurlAvailability::installHint();
      return decision;
    }
  }

  // Host check for http/https.
  if (scheme == ParsedScheme::kHttps || scheme == ParsedScheme::kHttp) {
    std::string_view host = ExtractHost(uri);
    if (host.empty()) {
      decision.reason = "Could not extract host from URL.";
      return decision;
    }
    std::string hostStr(host);

    // Deny list always wins.
    if (HostMatchesAny(policy_.httpsDenyHosts, host)) {
      decision.reason = "Host '" + hostStr + "' is blocked by policy.";
      return decision;
    }

    // Allow list: if non-empty, the host must appear in it.
    if (!policy_.httpsAllowHosts.empty() && !HostMatchesAny(policy_.httpsAllowHosts, host)) {
      // Check if user has already granted this host.
      if (grantedHosts_.count(hostStr) == 0) {
        if (policy_.httpsPromptOnFirstUse) {
          decision.outcome = Decision::Outcome::kNeedsUserConsent;
          decision.pendingHost = hostStr;
          decision.reason = "Host '" + hostStr + "' is not in the allow list. Grant access?";
          return decision;
        }
        decision.reason = "Host '" + hostStr + "' is not in the allow list.";
        return decision;
      }
    } else if (policy_.httpsAllowHosts.empty()) {
      // No allow list — prompt on first use if enabled and host not yet granted.
      if (policy_.httpsPromptOnFirstUse && grantedHosts_.count(hostStr) == 0) {
        decision.outcome = Decision::Outcome::kNeedsUserConsent;
        decision.pendingHost = hostStr;
        decision.reason = "First use of host '" + hostStr + "'. Grant access?";
        return decision;
      }
    }

    // Allow network fetch.
    decision.outcome = Decision::Outcome::kAllow;
    decision.resolved.scheme =
        (scheme == ParsedScheme::kHttps) ? ResolvedUri::Scheme::kHttps : ResolvedUri::Scheme::kHttp;
    decision.resolved.value = std::string(uri);
    decision.resolved.host = hostStr;
    return decision;
  }

  // data: scheme.
  if (scheme == ParsedScheme::kData) {
    decision.outcome = Decision::Outcome::kAllow;
    decision.resolved.scheme = ResolvedUri::Scheme::kData;
    decision.resolved.value = std::string(uri);
    return decision;
  }

  // File scheme — canonicalize and check roots.
  std::filesystem::path filePath;
  if (uri.size() >= kFileScheme.size() && uri.substr(0, kFileScheme.size()) == kFileScheme) {
    filePath = std::filesystem::path(std::string(uri.substr(kFileScheme.size())));
  } else {
    filePath = std::filesystem::path(std::string(uri));
  }

  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(filePath, ec);
  if (ec) {
    // Fall back to the raw path if canonicalization fails.
    canonical = filePath;
  }

  // Check file roots.
  if (!policy_.fileRoots.empty()) {
    bool underRoot = false;
    std::string canonStr = canonical.string();
    for (const auto& root : policy_.fileRoots) {
      std::error_code rootEc;
      std::filesystem::path canonRoot = std::filesystem::weakly_canonical(root, rootEc);
      if (rootEc) {
        canonRoot = root;
      }
      std::string rootStr = canonRoot.string();
      // Check prefix: canonical path starts with root.
      if (canonStr.size() >= rootStr.size() && canonStr.substr(0, rootStr.size()) == rootStr) {
        // Ensure the next character after root is '/' or end-of-string (exact match).
        if (canonStr.size() == rootStr.size() || canonStr[rootStr.size()] == '/') {
          underRoot = true;
          break;
        }
      }
    }
    if (!underRoot) {
      decision.reason = "Path '" + canonical.string() + "' is outside allowed file roots.";
      return decision;
    }
  }

  decision.outcome = Decision::Outcome::kAllow;
  decision.resolved.scheme = ResolvedUri::Scheme::kFile;
  decision.resolved.value = canonical.string();
  return decision;
}

void ResourceGatekeeper::grantHost(std::string host) {
  grantedHosts_.insert(std::move(host));
}

}  // namespace donner::editor
