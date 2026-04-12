#include "donner/editor/sandbox/SvgSource.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

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

  // Validate the URL contains no shell metacharacters. We allow a restricted
  // character set to prevent shell injection: alphanumeric, ':', '/', '.', '-',
  // '_', '~', '?', '&', '=', '%', '#', '+', '@', ',', ';', '!', '(', ')'.
  for (const char c : url) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          c == ':' || c == '/' || c == '.' || c == '-' || c == '_' ||
          c == '~' || c == '?' || c == '&' || c == '=' || c == '%' ||
          c == '#' || c == '+' || c == '@' || c == ',' || c == ';' ||
          c == '!' || c == '(' || c == ')' || c == '[' || c == ']')) {
      result.status = SvgFetchStatus::kInvalidUri;
      result.diagnostics = "URL contains disallowed character for shell safety: '";
      result.diagnostics += c;
      result.diagnostics += "'";
      return result;
    }
  }

  // Build the curl command. Flags:
  //   -sS          — silent but show errors
  //   -L           — follow redirects
  //   --max-time   — total timeout
  //   --max-redirs — redirect limit
  //   --max-filesize — abort if Content-Length exceeds the cap
  //   -f           — fail on HTTP errors (4xx/5xx)
  std::string cmd = "curl -sS -L -f";
  cmd += " --max-time " + std::to_string(options_.httpTimeoutSeconds);
  cmd += " --max-redirs " + std::to_string(options_.maxRedirects);
  cmd += " --max-filesize " + std::to_string(options_.maxHttpBytes);
  cmd += " -- '";
  cmd += url;
  cmd += "' 2>&1";

  // NOLINTNEXTLINE(cert-env33-c) — popen is acceptable for the trusted host side.
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) {
    result.status = SvgFetchStatus::kNetworkError;
    result.diagnostics = "failed to launch curl subprocess";
    return result;
  }

  // Read the response in chunks, enforcing the byte cap on the host side even
  // if the server doesn't send Content-Length (--max-filesize only works when
  // the server advertises it).
  constexpr std::size_t kChunkSize = 65536;
  std::array<char, kChunkSize> buf{};
  while (true) {
    const std::size_t n = std::fread(buf.data(), 1, buf.size(), pipe);
    if (n == 0) break;
    if (result.bytes.size() + n > options_.maxHttpBytes) {
      ::pclose(pipe);
      result.status = SvgFetchStatus::kTooLarge;
      result.bytes.clear();
      result.diagnostics = "HTTP response exceeded maxHttpBytes (" +
                           std::to_string(options_.maxHttpBytes) + ")";
      return result;
    }
    result.bytes.insert(result.bytes.end(), buf.data(), buf.data() + n);
  }

  const int exitCode = ::pclose(pipe);
  if (exitCode != 0) {
    // If we got no data at all, it's a pure network error. If we got some
    // data, it's likely the error message from curl (because of 2>&1).
    std::string curlOutput(result.bytes.begin(), result.bytes.end());
    result.bytes.clear();
    result.status = SvgFetchStatus::kNetworkError;
    result.diagnostics = "curl exited with code " + std::to_string(exitCode);
    if (!curlOutput.empty()) {
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
