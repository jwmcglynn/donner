#pragma once
/// @file
///
/// **`SvgFetcher`** — abstract fetch interface for loading SVG bytes from a
/// URI. Desktop implementation wraps `SvgSource` behind `ResourceGatekeeper`.
/// WASM implementation uses `emscripten_fetch`. See
/// docs/design_docs/0023-editor_sandbox.md §S10.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace donner::editor {

class ResourceGatekeeper;

namespace sandbox {
class SvgSource;
}  // namespace sandbox

/// Opaque handle identifying an in-flight fetch.
using FetchHandle = uint64_t;

/// Successful fetch payload.
struct FetchBytes {
  std::vector<uint8_t> bytes;
  std::filesystem::path resolvedPath;  ///< Only for file fetches.
  std::string originUri;
};

/// Fetch failure payload.
struct FetchError {
  enum class Kind {
    kSchemeNotSupported,
    kInvalidUri,
    kNotFound,
    kNotRegularFile,
    kPermissionDenied,
    kTooLarge,
    kReadFailed,
    kNetworkError,
    kPolicyDenied,
    kCurlMissing,
    kNeedsUserConsent,
  };
  Kind kind = Kind::kReadFailed;
  std::string message;
  std::string pendingHost;  ///< Only for kNeedsUserConsent.
};

/// Callback invoked when a fetch completes. Exactly one of the optionals is populated.
using FetchCallback = std::function<void(std::optional<FetchBytes>, std::optional<FetchError>)>;

/// Progress hint invoked zero-or-more times during a fetch. `totalBytes`
/// is `0` when the server didn't announce a Content-Length and the total
/// is unknown — observers should render indeterminate progress in that
/// case. Called on the same thread as `FetchCallback`.
using FetchProgressCallback = std::function<void(uint64_t bytesReceived, uint64_t totalBytes)>;

/// Abstract interface for fetching SVG resources from a URI.
class SvgFetcher {
public:
  virtual ~SvgFetcher() = default;

  /// Initiates a fetch for `uri`. The `cb` is invoked with the result
  /// (possibly synchronously). Optional `progressCb` receives per-chunk
  /// byte-counter updates for async fetchers; synchronous fetchers may
  /// ignore it (their callers see progress as 0 → done atomically).
  /// Returns a handle that can be passed to `cancel()`.
  virtual FetchHandle fetch(std::string_view uri, FetchCallback cb,
                            FetchProgressCallback progressCb = {}) = 0;

  /// Cancel a previously-issued fetch. No-op if the handle is invalid or
  /// the fetch has already completed.
  virtual void cancel(FetchHandle h) = 0;
};

/// Creates the desktop fetcher implementation backed by `SvgSource` and
/// gated by `ResourceGatekeeper`. The gatekeeper and source must outlive
/// the returned fetcher.
///
/// @param autoGrantFirstUse when `true`, a `kNeedsUserConsent` verdict on
///        the first fetch for a host is treated as implicit user consent:
///        the fetcher calls `ResourceGatekeeper::grantHost(...)` and
///        retries the resolve in-place, never surfacing the prompt to
///        the caller. Intended for user-initiated flows like the address
///        bar, where typing a URL *is* the consent. Keep the default
///        (`false`) for derived / sub-resource fetches where consent
///        must remain an explicit user gesture.
std::unique_ptr<SvgFetcher> MakeDesktopFetcher(ResourceGatekeeper& gatekeeper,
                                               sandbox::SvgSource& source,
                                               bool autoGrantFirstUse = false);

#ifdef __EMSCRIPTEN__
/// Creates the WASM fetcher implementation backed by `emscripten_fetch`.
/// CORS, HTTPS-only rules, and mixed-content blocking are enforced by the
/// browser — this fetcher is intentionally permissive. See
/// `SvgFetcherWasm.cc` for the threading / lifetime notes.
std::unique_ptr<SvgFetcher> MakeWasmFetcher();
#endif

}  // namespace donner::editor
