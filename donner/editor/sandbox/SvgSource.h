#pragma once
/// @file
///
/// `SvgSource` resolves a user-supplied URI into a raw byte buffer that the
/// sandbox child can parse. This is the host-side half of the address bar
/// design: the host has filesystem (and eventually network) privilege, and
/// hands the sandbox child only the resulting bytes — never the filename,
/// never the URL.
///
/// Supported schemes in this milestone:
///  - `file://<absolute-path>` — spec-style file URIs.
///  - bare absolute paths (`/foo/bar.svg`).
///  - bare relative paths (`./icon.svg`, `icon.svg`) — resolved against the
///    caller-specified base directory.
///
/// `https://` / `http://` — fetched via the system `curl` CLI on the host
/// side. The host enforces a 10 MB cap, 10-second timeout, and max 5
/// redirects. The sandbox child never sees the URL or touches the network.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace donner::editor::sandbox {

/// Outcome of a fetch attempt. `kOk` means `bytes` is populated and the
/// caller can pass it straight to `SandboxHost::renderToBackend`.
enum class SvgFetchStatus {
  kOk,                  ///< Bytes retrieved successfully.
  kSchemeNotSupported,  ///< URI used a scheme this build doesn't implement.
  kInvalidUri,          ///< URI could not be parsed.
  kNotFound,            ///< Resolved path does not exist.
  kNotRegularFile,      ///< Path exists but isn't a regular file.
  kPermissionDenied,    ///< Resolved path isn't readable.
  kTooLarge,            ///< File exceeds the configured size cap.
  kReadFailed,          ///< I/O error mid-read.
  kNetworkError,        ///< HTTP(S) fetch failed (timeout, DNS, etc.).
};

/// Result payload. `bytes` is only populated on `kOk`; all other statuses
/// leave it empty and set `diagnostics` to a human-readable reason.
struct SvgFetchResult {
  SvgFetchStatus status = SvgFetchStatus::kOk;
  std::vector<uint8_t> bytes;
  /// Absolute filesystem path the loader resolved to, for diagnostics and
  /// provenance tracking (e.g., baking into `.rnr` headers in S4). Empty
  /// when the URI didn't map to a filesystem path.
  std::filesystem::path resolvedPath;
  std::string diagnostics;
};

/// Configuration knobs for a `SvgSource`. Defaults match the design doc's
/// address-bar section: 100 MB cap on local files.
struct SvgSourceOptions {
  /// Directory used to resolve relative paths. Defaults to the current
  /// working directory at `SvgSource` construction time.
  std::filesystem::path baseDirectory = std::filesystem::current_path();
  /// Maximum number of bytes the loader will read from a single file. The
  /// cap exists to keep a malicious or runaway path from exhausting memory
  /// before the sandbox even sees the bytes.
  std::size_t maxFileBytes = 100u * 1024u * 1024u;
  /// Maximum number of bytes to accept from an HTTP(S) response.
  std::size_t maxHttpBytes = 10u * 1024u * 1024u;
  /// Timeout in seconds for HTTP(S) fetches.
  int httpTimeoutSeconds = 10;
  /// Maximum number of redirects to follow for HTTP(S) fetches.
  int maxRedirects = 5;
};

/// Stateless-from-the-outside URI resolver. `SvgSource` holds the config
/// knobs, but every `fetch()` call is independent and thread-safe.
class SvgSource {
public:
  explicit SvgSource(SvgSourceOptions options = {});

  /// Resolves `uri` to raw bytes. The URI is classified into a scheme (or
  /// falls through to "bare path"), the path is canonicalized and
  /// size-checked, and the file is read into memory. Never throws.
  [[nodiscard]] SvgFetchResult fetch(std::string_view uri) const;

private:
  SvgFetchResult fetchFromPath(const std::filesystem::path& path) const;
  SvgFetchResult fetchFromUrl(std::string_view url) const;

  SvgSourceOptions options_;
};

}  // namespace donner::editor::sandbox
