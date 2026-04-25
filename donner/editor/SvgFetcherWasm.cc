#ifdef __EMSCRIPTEN__

/// @file
///
/// WASM implementation of \ref donner::editor::SvgFetcher using the Emscripten
/// Fetch API (`emscripten_fetch`).
///
/// **Security model — the browser is the sandbox.** CORS, mixed-content,
/// HTTPS-only enforcement happen in the JS runtime; this fetcher is
/// intentionally permissive. No `ResourcePolicy` or `ResourceGatekeeper` is
/// consulted — we trust the browser to enforce origin restrictions.
///
/// **Thread safety.** `emscripten_fetch` callbacks fire on the main JS thread
/// under SharedArrayBuffer builds. No extra synchronization is needed; all
/// state access is single-threaded.
///
/// See `docs/design_docs/0023-editor_sandbox.md` §S10.

#include <emscripten/fetch.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#include "donner/editor/SvgFetcher.h"

namespace donner::editor {

namespace {

class WasmSvgFetcher;

/// Heap-allocated context carried through `emscripten_fetch_t::userData`.
/// Lifetime: allocated in `fetch()`, freed in the success/error trampoline
/// (via `removeActive`) or on `cancel()`.
struct FetchCtx {
  FetchHandle handle = 0;
  FetchCallback cb;
  FetchProgressCallback progressCb;
  std::string uri;
  /// Weak back-pointer to the owning fetcher. Set to `nullptr` on destruction
  /// of the fetcher to make late-firing callbacks safe (defensive; close should
  /// prevent them).
  WasmSvgFetcher* owner = nullptr;
};

/// Concrete `SvgFetcher` for WASM builds backed by `emscripten_fetch`.
class WasmSvgFetcher : public SvgFetcher {
public:
  ~WasmSvgFetcher() override {
    // Close every outstanding fetch. emscripten_fetch_close guarantees that
    // onsuccess/onerror will NOT fire after close, so nulling the owner is
    // purely defensive.
    for (auto& [handle, ctx] : active_) {
      ctx->owner = nullptr;
    }
    for (auto& [handle, emFetch] : emFetches_) {
      emscripten_fetch_close(emFetch);
    }
  }

  FetchHandle fetch(std::string_view uri, FetchCallback cb,
                    FetchProgressCallback progressCb = {}) override {
    const FetchHandle handle = nextHandle_++;

    // Validate scheme: only http:// and https:// are allowed in the WASM
    // context. file:// and bare paths are rejected — local files arrive via
    // <input type="file"> through a different code path.
    if (!HasHttpScheme(uri)) {
      FetchError err;
      err.kind = FetchError::Kind::kSchemeNotSupported;
      err.message = "WASM fetcher only supports http:// and https:// URIs";
      cb(std::nullopt, std::move(err));
      return handle;
    }

    auto ctx = std::make_unique<FetchCtx>();
    ctx->handle = handle;
    ctx->cb = std::move(cb);
    ctx->progressCb = std::move(progressCb);
    ctx->uri = std::string(uri);
    ctx->owner = this;
    FetchCtx* rawCtx = ctx.get();

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");  // NOLINT: fixed-size buffer in emscripten API.
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_REPLACE;
    attr.onsuccess = &WasmSvgFetcher::OnSuccess;
    attr.onerror = &WasmSvgFetcher::OnError;
    attr.onprogress = &WasmSvgFetcher::OnProgress;
    attr.userData = rawCtx;

    emscripten_fetch_t* emFetch = emscripten_fetch(&attr, rawCtx->uri.c_str());
    if (!emFetch) {
      // Extremely unlikely — only on immediate allocation failure.
      FetchError err;
      err.kind = FetchError::Kind::kNetworkError;
      err.message = "emscripten_fetch returned null for " + std::string(uri);
      rawCtx->cb(std::nullopt, std::move(err));
      // ctx is not stored in active_, so the unique_ptr destructor frees it.
      return handle;
    }

    active_[handle] = std::move(ctx);
    emFetches_[handle] = emFetch;
    return handle;
  }

  void cancel(FetchHandle h) override {
    auto ctxIt = active_.find(h);
    if (ctxIt == active_.end()) {
      return;  // Already completed or invalid handle.
    }

    // Suppress any late callback (defensive).
    ctxIt->second->owner = nullptr;
    active_.erase(ctxIt);

    auto fetchIt = emFetches_.find(h);
    if (fetchIt != emFetches_.end()) {
      // emscripten_fetch_close cancels the network request and guarantees
      // that onsuccess/onerror will NOT fire afterward.
      emscripten_fetch_close(fetchIt->second);
      emFetches_.erase(fetchIt);
    }
  }

  /// Remove a completed fetch from the active bookkeeping. Called from the
  /// static trampolines after extracting the callback.
  void removeActive(FetchHandle h) {
    active_.erase(h);
    emFetches_.erase(h);
  }

private:
  /// @return true if \p uri starts with `http://` or `https://`.
  static bool HasHttpScheme(std::string_view uri) {
    return uri.starts_with("https://") || uri.starts_with("http://");
  }

  /// C-callable trampoline: successful fetch.
  static void OnSuccess(emscripten_fetch_t* emFetch) {
    auto* ctx = static_cast<FetchCtx*>(emFetch->userData);

    if (ctx->owner) {
      FetchBytes payload;
      payload.bytes.assign(emFetch->data, emFetch->data + emFetch->numBytes);
      payload.originUri = ctx->uri;
      // resolvedPath intentionally empty — no filesystem path in WASM.

      FetchCallback cb = std::move(ctx->cb);
      const FetchHandle handle = ctx->handle;
      ctx->owner->removeActive(handle);
      cb(std::move(payload), std::nullopt);
    }

    emscripten_fetch_close(emFetch);
  }

  /// C-callable trampoline: per-chunk progress tick from emscripten_fetch.
  /// `emFetch->dataOffset + emFetch->numBytes` is the running byte count
  /// (dataOffset is 0 under LOAD_TO_MEMORY, but staying explicit keeps
  /// this correct if the mode ever changes). `emFetch->totalBytes` is
  /// zero when Content-Length is absent — surfaced verbatim so the
  /// observer renders an indeterminate bar.
  static void OnProgress(emscripten_fetch_t* emFetch) {
    auto* ctx = static_cast<FetchCtx*>(emFetch->userData);
    if (ctx->owner && ctx->progressCb) {
      const uint64_t received = emFetch->dataOffset + emFetch->numBytes;
      ctx->progressCb(received, emFetch->totalBytes);
    }
  }

  /// C-callable trampoline: failed fetch.
  static void OnError(emscripten_fetch_t* emFetch) {
    auto* ctx = static_cast<FetchCtx*>(emFetch->userData);

    if (ctx->owner) {
      FetchError err;

      // Classify: 4xx → kNotFound, everything else → kNetworkError.
      if (emFetch->status >= 400 && emFetch->status < 500) {
        err.kind = FetchError::Kind::kNotFound;
      } else {
        err.kind = FetchError::Kind::kNetworkError;
      }

      err.message = "HTTP " + std::to_string(emFetch->status);
      if (emFetch->statusText[0] != '\0') {
        err.message += ": ";
        err.message += emFetch->statusText;
      }

      FetchCallback cb = std::move(ctx->cb);
      const FetchHandle handle = ctx->handle;
      ctx->owner->removeActive(handle);
      cb(std::nullopt, std::move(err));
    }

    emscripten_fetch_close(emFetch);
  }

  uint64_t nextHandle_ = 1;

  /// Owns the FetchCtx for each in-flight fetch.
  std::unordered_map<FetchHandle, std::unique_ptr<FetchCtx>> active_;

  /// Maps handle → emscripten_fetch_t* for cancellation.
  std::unordered_map<FetchHandle, emscripten_fetch_t*> emFetches_;
};

}  // namespace

std::unique_ptr<SvgFetcher> MakeWasmFetcher() {
  return std::make_unique<WasmSvgFetcher>();
}

}  // namespace donner::editor

#endif  // __EMSCRIPTEN__
