#include "donner/editor/SvgFetcher.h"

#include <atomic>

#include "donner/editor/ResourcePolicy.h"
#include "donner/editor/sandbox/SvgSource.h"

namespace donner::editor {

namespace {

/// Maps `SvgFetchStatus` to `FetchError::Kind`.
FetchError::Kind MapStatus(sandbox::SvgFetchStatus status) {
  switch (status) {
    case sandbox::SvgFetchStatus::kOk:
      return FetchError::Kind::kReadFailed;  // Should never be called with kOk.
    case sandbox::SvgFetchStatus::kSchemeNotSupported: return FetchError::Kind::kSchemeNotSupported;
    case sandbox::SvgFetchStatus::kInvalidUri: return FetchError::Kind::kInvalidUri;
    case sandbox::SvgFetchStatus::kNotFound: return FetchError::Kind::kNotFound;
    case sandbox::SvgFetchStatus::kNotRegularFile: return FetchError::Kind::kNotRegularFile;
    case sandbox::SvgFetchStatus::kPermissionDenied: return FetchError::Kind::kPermissionDenied;
    case sandbox::SvgFetchStatus::kTooLarge: return FetchError::Kind::kTooLarge;
    case sandbox::SvgFetchStatus::kReadFailed: return FetchError::Kind::kReadFailed;
    case sandbox::SvgFetchStatus::kNetworkError: return FetchError::Kind::kNetworkError;
  }
  return FetchError::Kind::kReadFailed;
}

class DesktopFetcher : public SvgFetcher {
public:
  DesktopFetcher(ResourceGatekeeper& gatekeeper, sandbox::SvgSource& source, bool autoGrantFirstUse)
      : gatekeeper_(gatekeeper), source_(source), autoGrantFirstUse_(autoGrantFirstUse) {}

  FetchHandle fetch(std::string_view uri, FetchCallback cb,
                    FetchProgressCallback /*progressCb*/ = {}) override {
    // The desktop fetcher is synchronous (shell-out curl); per-chunk
    // progress updates aren't available. Progress observers will see
    // `kLoading` flip to done atomically.
    const FetchHandle handle = nextHandle_.fetch_add(1, std::memory_order_relaxed);

    // Step 1: Policy check.
    auto decision = gatekeeper_.resolve(uri);

    // The address bar uses `autoGrantFirstUse_ = true`: a URL typed into
    // the bar is itself the user's consent to hit that host, so we grant
    // the host in-place and re-resolve rather than surfacing a prompt.
    // Derived / sub-resource fetchers (none yet, see §S11 Future Work)
    // would leave this off so consent stays an explicit user gesture.
    if (decision.outcome == ResourceGatekeeper::Decision::Outcome::kNeedsUserConsent &&
        autoGrantFirstUse_) {
      gatekeeper_.grantHost(decision.pendingHost);
      decision = gatekeeper_.resolve(uri);
    }

    switch (decision.outcome) {
      case ResourceGatekeeper::Decision::Outcome::kDeny: {
        FetchError err;
        err.kind = FetchError::Kind::kPolicyDenied;
        err.message = std::move(decision.reason);
        cb(std::nullopt, std::move(err));
        return handle;
      }
      case ResourceGatekeeper::Decision::Outcome::kNeedsUserConsent: {
        FetchError err;
        err.kind = FetchError::Kind::kNeedsUserConsent;
        err.message = std::move(decision.reason);
        err.pendingHost = std::move(decision.pendingHost);
        cb(std::nullopt, std::move(err));
        return handle;
      }
      case ResourceGatekeeper::Decision::Outcome::kAllow: break;
    }

    // Step 2: Dispatch via SvgSource.
    const std::string fetchUri = decision.resolved.value;
    auto result = source_.fetch(fetchUri);

    if (result.status == sandbox::SvgFetchStatus::kOk) {
      FetchBytes payload;
      payload.bytes = std::move(result.bytes);
      payload.resolvedPath = std::move(result.resolvedPath);
      payload.originUri = std::string(uri);
      cb(std::move(payload), std::nullopt);
    } else {
      FetchError err;
      err.kind = MapStatus(result.status);
      err.message = std::move(result.diagnostics);
      cb(std::nullopt, std::move(err));
    }

    return handle;
  }

  void cancel(FetchHandle /*h*/) override {
    // Desktop fetcher is synchronous — fetches complete inline in `fetch()`.
    // Cancel is a no-op.
  }

private:
  ResourceGatekeeper& gatekeeper_;
  sandbox::SvgSource& source_;
  bool autoGrantFirstUse_;
  std::atomic<FetchHandle> nextHandle_{1};
};

}  // namespace

std::unique_ptr<SvgFetcher> MakeDesktopFetcher(ResourceGatekeeper& gatekeeper,
                                               sandbox::SvgSource& source, bool autoGrantFirstUse) {
  return std::make_unique<DesktopFetcher>(gatekeeper, source, autoGrantFirstUse);
}

}  // namespace donner::editor
