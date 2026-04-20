#pragma once
/// @file
///
/// **`ResourcePolicy`** — desktop-only policy gating every fetch from the
/// address bar. See docs/design_docs/0023-editor_sandbox.md §S11.
///
/// The policy is a value struct that a `ResourceGatekeeper` evaluates per
/// URI before dispatching to `SvgSource`. The goal is to centralize the
/// "is this thing safe to load?" decision so that additions (new schemes,
/// stricter host allowlists, workspace roots) only touch one file.
///
/// The WASM build does not use `ResourcePolicy` — the browser's CORS,
/// mixed-content, and user-gesture rules are our policy there.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace donner::editor {

/// Per-session policy knobs. Populated with `DefaultDesktopPolicy()` at
/// editor start-up; the settings UI (future) can mutate live.
struct ResourcePolicy {
  bool allowHttps = true;
  bool allowHttp = false;    ///< Default-deny plaintext; opt-in per session.
  bool allowFile = true;
  bool allowData = false;    ///< `data:` URIs — off by default.

  /// Filesystem scoping. Empty = anywhere readable; non-empty = reads must
  /// canonicalize into one of the listed roots.
  std::vector<std::filesystem::path> fileRoots;

  /// HTTPS host policy. When `httpsAllowHosts` is non-empty the address bar
  /// only accepts exact / wildcard matches there. `httpsDenyHosts` always
  /// wins over the allow list.
  std::vector<std::string> httpsAllowHosts;
  std::vector<std::string> httpsDenyHosts;

  /// If true, a host not in `httpsAllowHosts` that also isn't explicitly
  /// denied triggers a one-time user-consent prompt for the session.
  bool httpsPromptOnFirstUse = true;

  std::size_t maxFileBytes = 100u * 1024u * 1024u;
  std::size_t maxHttpBytes = 10u * 1024u * 1024u;
  int httpTimeoutSeconds = 10;
  int maxRedirects = 5;

  /// Sub-resource policy reserved for when the sandbox gains a
  /// request-from-sandbox protocol. `kBlockAll` matches current behavior.
  enum class SubresourcePolicy {
    kBlockAll,
    kSameDocumentOnly,
    kPolicyFetch,
  };
  SubresourcePolicy subresources = SubresourcePolicy::kBlockAll;
};

/// The default policy the editor starts with on desktop. Documented in
/// §S11; tests should pin this so behavior changes are visible in the
/// diff.
[[nodiscard]] ResourcePolicy DefaultDesktopPolicy();

/// Canonicalized URI suitable to feed into `SvgSource::fetch`. Produced by
/// `ResourceGatekeeper::resolve` on approval. Kept tiny on purpose so the
/// decision layer and the fetch layer don't share more than they must.
struct ResolvedUri {
  enum class Scheme { kFile, kHttp, kHttps, kData };
  Scheme scheme = Scheme::kFile;
  /// For `file:`, an absolute canonicalized path string. For http(s):, the
  /// URL itself (pre-validated for shell safety).
  std::string value;
  /// Hostname extracted from `value` for http(s) URIs; empty otherwise.
  std::string host;
};

/// The single legal caller of `SvgSource::fetch`. Enforces the policy,
/// handles first-use host consent, and produces `ResolvedUri` on approval.
class ResourceGatekeeper {
public:
  explicit ResourceGatekeeper(ResourcePolicy policy);

  struct Decision {
    enum class Outcome {
      kAllow,            ///< Dispatch `resolved` to the fetcher.
      kDeny,             ///< Surface `reason` as an error chip.
      kNeedsUserConsent, ///< Host wasn't in allow/deny; prompt the user. On
                         ///< approval, call `grantHost(host)` and retry.
    };
    Outcome outcome = Outcome::kDeny;
    std::string reason;       ///< Human-readable. Safe to surface as-is.
    ResolvedUri resolved;     ///< Only populated on `kAllow`.
    std::string pendingHost;  ///< Only populated on `kNeedsUserConsent`.
  };

  [[nodiscard]] Decision resolve(std::string_view uri) const;

  /// Grants a host for the rest of this session. Called after the user
  /// confirms the first-use prompt. Thread-safe so the UI thread can call
  /// it while a queued fetch is blocked waiting.
  void grantHost(std::string host);

  /// Mutable accessor used by the settings UI. Callers must avoid racing
  /// with `resolve()` unless they hold the session-level mutex.
  [[nodiscard]] ResourcePolicy& policy() { return policy_; }
  [[nodiscard]] const ResourcePolicy& policy() const { return policy_; }

private:
  ResourcePolicy policy_;
  /// Hosts the user explicitly approved in the first-use prompt. Populated
  /// at runtime; not persisted.
  std::unordered_set<std::string> grantedHosts_;
};

/// One-shot, cached probe: is `curl` on `PATH` and runnable? Used by
/// `ResourceGatekeeper` to surface an actionable error when a user tries
/// to load a http(s) URL without curl installed.
class CurlAvailability {
public:
  enum class State {
    kUnknown,    ///< `check()` has not been called yet.
    kAvailable,  ///< `curl --version` succeeded.
    kMissing,    ///< No curl on PATH, or it failed to run.
  };

  /// Runs `curl --version` the first time called; memoizes the result for
  /// every subsequent call. Thread-safe — synchronized via a function-
  /// local static.
  [[nodiscard]] static State check();

  /// Platform-specific install instructions suitable for showing in an
  /// error chip. Returns one short line.
  [[nodiscard]] static std::string installHint();

  /// Test hook: override the memoized state. Resets to `kUnknown` on
  /// destruction. Tests use this to simulate "curl missing" without
  /// mutating the real PATH.
  struct TestOverride {
    explicit TestOverride(State forced);
    ~TestOverride();

    TestOverride(const TestOverride&) = delete;
    TestOverride& operator=(const TestOverride&) = delete;
  };
};

}  // namespace donner::editor
