#include "donner/editor/sandbox/SvgSource.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include "donner/editor/sandbox/UrlSecurity.h"

namespace donner::editor::sandbox {

namespace {

constexpr std::string_view kFileScheme = "file://";
constexpr std::string_view kHttpsScheme = "https://";
constexpr std::string_view kHttpScheme = "http://";

// Returns true if `uri` begins with a scheme like "xxxx://".
bool HasExplicitScheme(std::string_view uri) {
  const auto colon = uri.find(':');
  if (colon == std::string_view::npos || colon == 0) return false;
  if (uri.size() < colon + 3) return false;
  return uri.substr(colon, 3) == "://";
}

// Strips a leading "file://" prefix and returns the remaining path. Handles
// the common malformed "file://relative" shape by treating it as relative.
std::string StripFileScheme(std::string_view uri) {
  return std::string(uri.substr(kFileScheme.size()));
}

std::string Diagnose(const std::filesystem::path& path, std::string_view verb,
                     const std::error_code& ec) {
  std::string out = std::string(verb);
  out += " '";
  out += path.string();
  out += "': ";
  out += ec.message();
  return out;
}

// ───────────────────────────────────────────────────────────────────────
// curl shell-out hardening helpers
// ───────────────────────────────────────────────────────────────────────

using url_security::IsPrivateIPv4;
using url_security::IsPrivateIPv6;
using url_security::ParsedHttpUrl;
using url_security::ParseHttpUrl;

/// Resolve \p host via getaddrinfo and return the first public address as
/// a text form (IPv4 dotted-quad or IPv6 hex). Any private / loopback /
/// link-local / multicast address is rejected — if the DNS answer mixes
/// public and private IPs we fall through to the first public entry
/// (matches curl's own happy-eyeballs ordering). Returns empty on
/// failure; \p err is populated with a human-readable reason.
std::string ResolvePublicIp(const std::string& host, std::uint16_t port, bool ipv6Literal,
                            std::string& err) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_ADDRCONFIG | (ipv6Literal ? AI_NUMERICHOST : 0);

  addrinfo* res = nullptr;
  const std::string portStr = std::to_string(port);
  const int gai = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
  if (gai != 0 || res == nullptr) {
    err = "DNS lookup failed for '" + host + "': " + ::gai_strerror(gai);
    if (res) ::freeaddrinfo(res);
    return {};
  }

  std::string firstPrivate;
  std::string pick;
  for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
    char buf[INET6_ADDRSTRLEN] = {0};
    if (p->ai_family == AF_INET) {
      const auto* sa = reinterpret_cast<const sockaddr_in*>(p->ai_addr);
      const uint32_t h = ntohl(sa->sin_addr.s_addr);
      if (!::inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) continue;
      if (IsPrivateIPv4(h)) {
        if (firstPrivate.empty()) firstPrivate = buf;
        continue;
      }
      pick = buf;
      break;
    }
    if (p->ai_family == AF_INET6) {
      const auto* sa = reinterpret_cast<const sockaddr_in6*>(p->ai_addr);
      if (!::inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof(buf))) continue;
      if (IsPrivateIPv6(sa->sin6_addr)) {
        if (firstPrivate.empty()) firstPrivate = buf;
        continue;
      }
      pick = buf;
      break;
    }
  }
  ::freeaddrinfo(res);

  if (pick.empty()) {
    err = "Refusing to connect: '" + host + "' only resolves to private / loopback addresses";
    if (!firstPrivate.empty()) err += " (e.g. " + firstPrivate + ")";
    err += ".";
  }
  return pick;
}

/// Build argv for the hardened curl invocation. Kept as a free function
/// so it's testable without spawning a process. The returned strings are
/// owned by the vector; caller must materialize `char*` pointers into a
/// separate array for `posix_spawnp`.
std::vector<std::string> BuildCurlArgv(const ParsedHttpUrl& parsed, std::string_view rawUrl,
                                       const std::string& resolvePin,
                                       const SvgSourceOptions& opts) {
  const bool allowHttpRedirects = !parsed.isHttps;  // we only redirect to scheme we entered
  std::vector<std::string> argv;
  argv.reserve(32);
  argv.emplace_back("curl");
  // -q (alias --disable): don't read `~/.curlrc`. Must come first — curl
  // parses `~/.curlrc` BEFORE any subsequent flags, so placing this
  // anywhere else is a no-op.
  argv.emplace_back("-q");
  argv.emplace_back("-sS");                       // silent, but show errors on stderr
  argv.emplace_back("-f");                        // fail on HTTP 4xx/5xx
  argv.emplace_back("-L");                        // follow redirects (bounded by --max-redirs)
  // Lock protocols to what the user permitted. For the initial request
  // and for redirects. `=` means "only these schemes, ignore defaults" —
  // crucial because curl's default `--proto-redir` silently allows
  // `file://` and `ftp://` on redirect, which is CVE bait.
  argv.emplace_back("--proto");
  argv.emplace_back(allowHttpRedirects ? "=http,https" : "=https");
  argv.emplace_back("--proto-redir");
  argv.emplace_back(allowHttpRedirects ? "=http,https" : "=https");
  // TLS floor. System curl on older distros still defaults to accepting
  // TLS 1.0, which has known weaknesses.
  argv.emplace_back("--tlsv1.2");
  // Fixed, versionless User-Agent. The default "curl/8.x.y" leaks
  // version info to every server the user touches.
  argv.emplace_back("-A");
  argv.emplace_back("Donner");
  argv.emplace_back("--max-time");
  argv.emplace_back(std::to_string(opts.httpTimeoutSeconds));
  argv.emplace_back("--max-redirs");
  argv.emplace_back(std::to_string(opts.maxRedirects));
  argv.emplace_back("--max-filesize");
  argv.emplace_back(std::to_string(opts.maxHttpBytes));
  // First-hop SSRF defense: pin the hostname to the public IP we
  // pre-validated via getaddrinfo. Closes TOCTOU between our DNS check
  // and curl's (the attacker's DNS could return a public IP to us and a
  // private one to curl). The TLS handshake still uses the original
  // hostname for SNI + cert validation.
  //
  // Caveat: redirects to a DIFFERENT host bypass this pin — curl does
  // its own resolve for the new hostname. Documented as a follow-up.
  if (!resolvePin.empty()) {
    argv.emplace_back("--resolve");
    std::string pin = parsed.host;
    pin += ':';
    pin += std::to_string(parsed.port);
    pin += ':';
    pin += resolvePin;
    argv.emplace_back(std::move(pin));
  }
  argv.emplace_back("--");
  argv.emplace_back(std::string(rawUrl));
  return argv;
}

}  // namespace

SvgSource::SvgSource(SvgSourceOptions options) : options_(std::move(options)) {}

SvgFetchResult SvgSource::fetch(std::string_view uri) const {
  SvgFetchResult result;

  if (uri.empty()) {
    result.status = SvgFetchStatus::kInvalidUri;
    result.diagnostics = "empty uri";
    return result;
  }

  if (HasExplicitScheme(uri)) {
    if (uri.size() >= kFileScheme.size() &&
        uri.substr(0, kFileScheme.size()) == kFileScheme) {
      return fetchFromPath(std::filesystem::path(StripFileScheme(uri)));
    }
    if ((uri.size() >= kHttpsScheme.size() &&
         uri.substr(0, kHttpsScheme.size()) == kHttpsScheme) ||
        (uri.size() >= kHttpScheme.size() &&
         uri.substr(0, kHttpScheme.size()) == kHttpScheme)) {
      return fetchFromUrl(uri);
    }
    result.status = SvgFetchStatus::kSchemeNotSupported;
    result.diagnostics = "unsupported scheme in: ";
    result.diagnostics.append(uri);
    return result;
  }

  // Bare path — absolute or relative.
  std::filesystem::path path(std::string{uri});
  if (path.is_relative()) {
    path = options_.baseDirectory / path;
  }
  return fetchFromPath(path);
}

SvgFetchResult SvgSource::fetchFromPath(const std::filesystem::path& path) const {
  SvgFetchResult result;
  result.resolvedPath = path;

  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(path, ec);
  if (!ec) {
    result.resolvedPath = canonical;
  }

  // Check existence up-front so we can differentiate "missing" from
  // "exists-but-unreadable".
  std::error_code existsEc;
  const bool exists = std::filesystem::exists(result.resolvedPath, existsEc);
  if (existsEc || !exists) {
    result.status = SvgFetchStatus::kNotFound;
    result.diagnostics = Diagnose(result.resolvedPath, "stat",
                                  existsEc ? existsEc : std::make_error_code(std::errc::no_such_file_or_directory));
    return result;
  }

  std::error_code typeEc;
  const auto status = std::filesystem::status(result.resolvedPath, typeEc);
  if (typeEc) {
    result.status = SvgFetchStatus::kReadFailed;
    result.diagnostics = Diagnose(result.resolvedPath, "status", typeEc);
    return result;
  }
  if (!std::filesystem::is_regular_file(status)) {
    result.status = SvgFetchStatus::kNotRegularFile;
    result.diagnostics = "not a regular file: " + result.resolvedPath.string();
    return result;
  }

  std::error_code sizeEc;
  const auto byteCount = std::filesystem::file_size(result.resolvedPath, sizeEc);
  if (sizeEc) {
    result.status = SvgFetchStatus::kReadFailed;
    result.diagnostics = Diagnose(result.resolvedPath, "file_size", sizeEc);
    return result;
  }
  if (byteCount > options_.maxFileBytes) {
    result.status = SvgFetchStatus::kTooLarge;
    result.diagnostics = "file exceeds maxFileBytes (" + std::to_string(byteCount) +
                         " > " + std::to_string(options_.maxFileBytes) + "): " +
                         result.resolvedPath.string();
    return result;
  }

  std::ifstream in(result.resolvedPath, std::ios::binary);
  if (!in.is_open()) {
    // Distinguish permission-denied from generic read failure when we can.
    if (errno == EACCES) {
      result.status = SvgFetchStatus::kPermissionDenied;
    } else {
      result.status = SvgFetchStatus::kReadFailed;
    }
    result.diagnostics = "failed to open: " + result.resolvedPath.string() +
                         " (" + std::strerror(errno) + ")";
    return result;
  }

  result.bytes.resize(static_cast<std::size_t>(byteCount));
  if (byteCount > 0) {
    in.read(reinterpret_cast<char*>(result.bytes.data()),
            static_cast<std::streamsize>(byteCount));
    if (!in || static_cast<std::size_t>(in.gcount()) != byteCount) {
      result.status = SvgFetchStatus::kReadFailed;
      result.bytes.clear();
      result.diagnostics = "short read on: " + result.resolvedPath.string();
      return result;
    }
  }

  result.status = SvgFetchStatus::kOk;
  return result;
}

SvgFetchResult SvgSource::fetchFromUrl(std::string_view url) const {
  SvgFetchResult result;
  // resolvedPath is left empty for network fetches — there is no filesystem path.

  // Sanity-check URL characters. We no longer shell out through `/bin/sh`
  // (posix_spawnp uses an argv array, so shell metacharacters can't
  // escape), so this allow-list is pure defense-in-depth — it catches
  // control bytes, whitespace, and quote characters that have no
  // legitimate place in a bare URL and would only be there if something
  // is trying to smuggle. A dedicated URL parser would be stricter; this
  // is intentionally permissive enough to allow the RFC 3986 "reserved"
  // character set plus a few common extras.
  for (const char c : url) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          c == ':' || c == '/' || c == '.' || c == '-' || c == '_' ||
          c == '~' || c == '?' || c == '&' || c == '=' || c == '%' ||
          c == '#' || c == '+' || c == '@' || c == ',' || c == ';' ||
          c == '!' || c == '(' || c == ')' || c == '[' || c == ']')) {
      result.status = SvgFetchStatus::kInvalidUri;
      result.diagnostics = "URL contains disallowed character: '";
      result.diagnostics += c;
      result.diagnostics += "'";
      return result;
    }
  }

  // Parse the URL into (host, port, scheme) so we can pre-validate the
  // destination before curl touches the network.
  const ParsedHttpUrl parsed = ParseHttpUrl(url);
  if (!parsed.valid) {
    result.status = SvgFetchStatus::kInvalidUri;
    result.diagnostics = "could not parse URL: ";
    result.diagnostics.append(url);
    return result;
  }

  // First-hop SSRF defense: resolve the hostname ourselves, reject if it
  // only maps to private / loopback / link-local addresses, and pin the
  // mapping via `--resolve` so curl can't be TOCTOU'd into hitting a
  // different IP than the one we approved.
  std::string pinnedIp;
  if (!parsed.ipv6Literal) {
    // Skip the pre-resolve step for bare IP literals — ParseHttpUrl
    // already put the address into `parsed.host`, and ResolvePublicIp
    // would just round-trip it. Handle those inline below.
    std::string resolveErr;
    pinnedIp = ResolvePublicIp(parsed.host, parsed.port, /*ipv6Literal=*/false, resolveErr);
    if (pinnedIp.empty()) {
      result.status = SvgFetchStatus::kNetworkError;
      result.diagnostics = std::move(resolveErr);
      return result;
    }
  } else {
    // IPv6 literal — parse directly and validate.
    in6_addr addr{};
    if (::inet_pton(AF_INET6, parsed.host.c_str(), &addr) != 1) {
      result.status = SvgFetchStatus::kInvalidUri;
      result.diagnostics = "invalid IPv6 literal: " + parsed.host;
      return result;
    }
    if (IsPrivateIPv6(addr)) {
      result.status = SvgFetchStatus::kNetworkError;
      result.diagnostics = "Refusing to connect to private / loopback IPv6 literal: " + parsed.host;
      return result;
    }
    // No pin needed — curl will use the literal as-is.
  }

  // Build hardened argv. See `BuildCurlArgv` for the full flag inventory.
  const std::vector<std::string> argvOwners = BuildCurlArgv(parsed, url, pinnedIp, options_);
  std::vector<char*> argv;
  argv.reserve(argvOwners.size() + 1);
  for (const auto& s : argvOwners) {
    argv.push_back(const_cast<char*>(s.c_str()));
  }
  argv.push_back(nullptr);

  // Minimal env. curl otherwise honors $http_proxy, $HTTPS_PROXY,
  // $SSL_CERT_FILE, $SSL_CERT_DIR, $CURL_CA_BUNDLE, $CURL_HOME, and
  // $HOME (the last one feeds `~/.curlrc` reads even with -q gone
  // nowhere — -q suppresses, but an attacker who can set $HOME to a
  // directory they wrote to could still plant config). Wiping the env
  // to just PATH + LANG removes the entire class.
  //
  // `PATH` stays so `posix_spawnp` can still find `curl`. `LANG=C`
  // keeps curl's error messages in English, consistent for our log
  // parsing.
  char pathEnv[] = "PATH=/usr/bin:/bin:/usr/local/bin:/opt/homebrew/bin";
  char langEnv[] = "LANG=C";
  char* safeEnv[] = {pathEnv, langEnv, nullptr};

  // Stdout (and stderr) → pipe.
  int stdoutPipe[2] = {-1, -1};
  if (::pipe(stdoutPipe) != 0) {
    result.status = SvgFetchStatus::kNetworkError;
    result.diagnostics = std::string("pipe() failed: ") + std::strerror(errno);
    return result;
  }

  posix_spawn_file_actions_t actions;
  ::posix_spawn_file_actions_init(&actions);
  ::posix_spawn_file_actions_addclose(&actions, stdoutPipe[0]);
  ::posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
  ::posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDERR_FILENO);
  ::posix_spawn_file_actions_addclose(&actions, stdoutPipe[1]);

  pid_t pid = 0;
  const int spawnRc =
      ::posix_spawnp(&pid, "curl", &actions, /*attrp=*/nullptr, argv.data(), safeEnv);

  ::posix_spawn_file_actions_destroy(&actions);
  ::close(stdoutPipe[1]);  // parent only reads

  if (spawnRc != 0) {
    ::close(stdoutPipe[0]);
    result.status = SvgFetchStatus::kNetworkError;
    result.diagnostics =
        std::string("failed to launch curl subprocess: ") + std::strerror(spawnRc);
    return result;
  }

  // Read the response in chunks, enforcing the byte cap ourselves in
  // case the server omits Content-Length (curl's --max-filesize only
  // works when the server advertises it up front).
  constexpr std::size_t kChunkSize = 65536;
  std::array<char, kChunkSize> buf{};
  bool capExceeded = false;
  while (true) {
    const ssize_t n = ::read(stdoutPipe[0], buf.data(), buf.size());
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (result.bytes.size() + static_cast<std::size_t>(n) > options_.maxHttpBytes) {
      capExceeded = true;
      ::kill(pid, SIGKILL);
      break;
    }
    result.bytes.insert(result.bytes.end(), buf.data(), buf.data() + n);
  }
  ::close(stdoutPipe[0]);

  // Always waitpid — otherwise we leak zombies and the status is
  // unavailable.
  int status = 0;
  while (::waitpid(pid, &status, 0) == -1) {
    if (errno != EINTR) break;
  }

  if (capExceeded) {
    result.bytes.clear();
    result.status = SvgFetchStatus::kTooLarge;
    result.diagnostics = "HTTP response exceeded maxHttpBytes (" +
                         std::to_string(options_.maxHttpBytes) + ")";
    return result;
  }

  const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (exitCode != 0) {
    // Merged stderr: the bytes buffer likely holds curl's error message.
    std::string curlOutput(result.bytes.begin(), result.bytes.end());
    result.bytes.clear();
    result.status = SvgFetchStatus::kNetworkError;
    result.diagnostics = "curl exited with code " + std::to_string(exitCode);
    if (!curlOutput.empty()) {
      // Trim trailing newline for a tidier diagnostic.
      while (!curlOutput.empty() && (curlOutput.back() == '\n' || curlOutput.back() == '\r')) {
        curlOutput.pop_back();
      }
      result.diagnostics += ": " + curlOutput;
    }
    return result;
  }

  if (result.bytes.empty()) {
    result.status = SvgFetchStatus::kNetworkError;
    result.diagnostics = "curl returned empty response";
    return result;
  }

  result.status = SvgFetchStatus::kOk;
  return result;
}

}  // namespace donner::editor::sandbox
