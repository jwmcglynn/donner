#include "donner/editor/AddressBarDispatcher.h"

#include <iostream>
#include <utility>

namespace donner::editor {

namespace {

AddressBarStatus StatusForFetchError(FetchError::Kind kind) {
  switch (kind) {
    case FetchError::Kind::kPolicyDenied:
    case FetchError::Kind::kNeedsUserConsent: return AddressBarStatus::kPolicyDenied;
    case FetchError::Kind::kSchemeNotSupported:
    case FetchError::Kind::kInvalidUri:
    case FetchError::Kind::kNotFound:
    case FetchError::Kind::kNotRegularFile:
    case FetchError::Kind::kPermissionDenied:
    case FetchError::Kind::kTooLarge:
    case FetchError::Kind::kReadFailed:
    case FetchError::Kind::kNetworkError:
    case FetchError::Kind::kCurlMissing: return AddressBarStatus::kFetchError;
  }
  return AddressBarStatus::kFetchError;
}

}  // namespace

AddressBarDispatcher::AddressBarDispatcher(AddressBar& addressBar, SvgFetcher& fetcher,
                                           AddressBarLoadCallback onLoad)
    : addressBar_(addressBar), fetcher_(fetcher), onLoad_(std::move(onLoad)) {}

AddressBarDispatcher::~AddressBarDispatcher() {
  if (activeHandle_.has_value()) {
    fetcher_.cancel(*activeHandle_);
  }
}

void AddressBarDispatcher::pump() {
  auto nav = addressBar_.consumeNavigation();
  if (!nav.has_value()) {
    return;
  }

  std::cerr << "[addrbar] navigation uri=" << nav->uri << " droppedBytes=" << nav->bytes.size()
            << "\n";

  // Drop / picker short-circuit: bytes are already in hand, no fetch.
  if (!nav->bytes.empty()) {
    addressBar_.setStatus({AddressBarStatus::kLoading, "Loading dropped file…", nav->uri});
    AddressBarLoadRequest req;
    req.originUri = nav->uri;
    req.bytes = std::move(nav->bytes);
    // Drop payloads can't resolve a real path — the browser/host gave us
    // bytes only. Callers fall back to the URI for display.
    req.resolvedPath.reset();
    onLoad_(req);
    addressBar_.pushHistory(nav->uri);
    return;
  }

  startFetch(nav->uri);
}

void AddressBarDispatcher::startFetch(std::string uri) {
  // Cancel anything still in flight — every new navigation supersedes.
  if (activeHandle_.has_value()) {
    fetcher_.cancel(*activeHandle_);
    activeHandle_.reset();
  }

  addressBar_.setStatus({AddressBarStatus::kLoading, "Fetching…", uri});
  // Indeterminate while the first progress tick (or completion) arrives.
  addressBar_.setLoadProgress(std::nullopt);

  const FetchHandle handle = fetcher_.fetch(
      uri,
      [this, uri](std::optional<FetchBytes> bytes, std::optional<FetchError> err) {
        onFetchResult(uri, std::move(bytes), std::move(err));
      },
      [this](uint64_t received, uint64_t total) {
        // Only switch to determinate mode once the server confirms a
        // total. Some servers stream without a Content-Length — keep
        // the indeterminate slider for those rather than pinning to 0.
        if (total > 0) {
          addressBar_.setLoadProgress(static_cast<float>(received) / static_cast<float>(total));
        }
      });
  activeHandle_ = handle;
}

void AddressBarDispatcher::onFetchResult(const std::string& uri, std::optional<FetchBytes> bytes,
                                         std::optional<FetchError> err) {
  activeHandle_.reset();
  // Clear the progress bar — the chip is about to transition to a
  // terminal status (kRendered / kFetchError / etc.), which hides the
  // bar regardless, but clearing here keeps the next `startFetch` from
  // inheriting a stale value.
  addressBar_.setLoadProgress(std::nullopt);

  if (bytes.has_value()) {
    std::cerr << "[addrbar] fetch ok bytes=" << bytes->bytes.size() << " uri=" << uri << "\n";
    AddressBarLoadRequest req;
    req.originUri = uri;
    req.bytes = std::move(bytes->bytes);
    if (!bytes->resolvedPath.empty()) {
      req.resolvedPath = bytes->resolvedPath.string();
    }
    onLoad_(req);
    addressBar_.pushHistory(uri);
    // Final chip flip (`kRendered`) is driven by `markRendered()` once the
    // backend reports parse/render success — we only know bytes arrived here.
    return;
  }

  const std::string msg = err.has_value() ? err->message : std::string("unknown fetch error");
  const AddressBarStatus status =
      err.has_value() ? StatusForFetchError(err->kind) : AddressBarStatus::kFetchError;
  std::cerr << "[addrbar] fetch err=" << msg << " uri=" << uri << "\n";
  addressBar_.setStatus({status, msg, uri});
}

void AddressBarDispatcher::markRendered(std::string uri) {
  addressBar_.setStatus({AddressBarStatus::kRendered, "OK", std::move(uri)});
}

void AddressBarDispatcher::markParseError(std::string uri, std::string message) {
  addressBar_.setStatus({AddressBarStatus::kParseError, std::move(message), std::move(uri)});
}

}  // namespace donner::editor
