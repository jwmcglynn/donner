#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"

namespace donner {

/// Immutable encoded resource bytes. An owning lease remains valid across session teardown.
using ResourceBytes = std::shared_ptr<const std::vector<uint8_t>>;

struct PackagedResource;

/// An owning immutable view; retaining a view keeps its encoded bytes alive across sessions.
class ResourceLease {
public:
  ResourceLease() = default;
  ResourceLease(const PackagedResource& resource);
  ResourceLease(ResourceBytes bytes) : bytes_(std::move(bytes)) {}
  ResourceLease(std::span<const uint8_t> bytes)
      : bytes_(std::make_shared<const std::vector<uint8_t>>(bytes.begin(), bytes.end())) {}
  const uint8_t* data() const& UTILS_LIFETIME_BOUND { return bytes_ ? bytes_->data() : nullptr; }
  const uint8_t* data() const&& = delete;
  size_t size() const { return bytes_ ? bytes_->size() : 0; }
  bool empty() const { return size() == 0; }
  std::span<const uint8_t> bytes() const& UTILS_LIFETIME_BOUND { return {data(), size()}; }
  std::span<const uint8_t> bytes() const&& = delete;
  operator std::span<const uint8_t>() const& UTILS_LIFETIME_BOUND { return bytes(); }
  operator std::span<const uint8_t>() const&& = delete;
  std::string_view text() const& UTILS_LIFETIME_BOUND {
    return empty() ? std::string_view()
                   : std::string_view(reinterpret_cast<const char*>(data()), size());
  }
  std::string_view text() const&& = delete;

private:
  ResourceBytes bytes_;
};

/// Authenticated identity of a packaged asset; this type contains no payload bytes.
struct PackagedResource {
  const char* id;
  const char* sha256;
  size_t encodedSize;
  size_t decodedSize;
  const char* mediaType;
  const char* group;
  const char* path;
  const char* testRunfilesRoot;

  PackagedResource(const char* id, const char* sha256, size_t encodedSize, size_t decodedSize,
                   const char* mediaType, const char* group, const char* path,
                   const char* testRunfilesRoot);

  ~PackagedResource();
  PackagedResource(const PackagedResource&) = delete;
  PackagedResource& operator=(const PackagedResource&) = delete;
  bool ready() const;
  bool failed() const;

  /// Read verified bytes, retaining them in the bounded application resource session.
  [[nodiscard]] ResourceBytes load() const;
  /// Borrow from the active application session. Destroy consumers before ending that session.
  [[nodiscard]] std::span<const uint8_t> view() const;
  operator std::span<const uint8_t>() const { return view(); }
  const uint8_t* data() const { return view().data(); }
  size_t size() const { return view().size(); }
  bool empty() const { return view().empty(); }
  const uint8_t* begin() const { return data(); }
  const uint8_t* end() const {
    const auto bytes = view();
    return bytes.empty() ? bytes.data() : bytes.data() + bytes.size();
  }
};

/// Supplies encoded bytes only. The session independently validates identity and all limits.
using PackagedResourceProvider = std::function<ResourceBytes(const PackagedResource&)>;

/** Owns resource retention for one application lifetime.
 *
 * The aggregate bound includes every installed encoded resource, including provider returns.
 * Configure a provider before the first access. Views borrow this session; leases own their bytes.
 * This API has no dependency on fonts, an editor, a document loader, or network access.
 */
class PackagedResourceSession {
public:
  static constexpr size_t kMaximumEncodedBytes = 16 * 1024 * 1024;
  explicit PackagedResourceSession(PackagedResourceProvider provider = {});
  ~PackagedResourceSession();
  PackagedResourceSession(const PackagedResourceSession&) = delete;
  PackagedResourceSession& operator=(const PackagedResourceSession&) = delete;
  ResourceBytes load(const PackagedResource& resource);
  bool install(const PackagedResource& resource, std::span<const uint8_t> bytes);
  /// Dispatch one deferred request per identity. The callback runs immediately outside the lock.
  bool request(const PackagedResource& resource, const std::function<void()>& dispatch);
  /// Allow an explicit retry only after a completed failure/cancellation.
  bool retry(const PackagedResource& resource);
  void markBorrowed();
  bool hasBorrowedViews() const;
  bool ready(const PackagedResource& resource) const;
  bool failed(const PackagedResource& resource) const;
  void fail(const PackagedResource& resource);
  bool verify(const PackagedResource& resource);
  size_t retainedBytes() const;
  std::string error() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Set the application session before creating resource consumers; end it after their teardown.
/// A null session restores the default installed-package provider for subsequent applications.
/// Reject replacement after an unowned legacy span was returned; use ResourceLease to permit
/// teardown. On failure the old session remains installed, so an outstanding borrowed view cannot
/// dangle.
bool SetPackagedResourceSession(std::shared_ptr<PackagedResourceSession> session);
std::shared_ptr<PackagedResourceSession> GetPackagedResourceSession();

/// Registered compiled identities. Enumeration does not read any payload.
std::vector<const PackagedResource*> PackagedResources();
/// Native preflight validates the entire linked set; only essential resources are retained.
bool PreparePackagedResources(std::string* error);
/// A portable SHA-256 implementation used to authenticate disk, provider and browser bytes.
std::string ResourceSha256(std::span<const uint8_t> bytes);
/// File provider rooted in an explicit installation directory; rejects symlinks and traversal.
PackagedResourceProvider PackagedResourceDirectoryProvider(std::string root);

#ifdef __EMSCRIPTEN__
/// Queue browser I/O on a normal browser task; never block a resource consumer.
void RequestBrowserPackagedResource(const char* sha256);
#endif

}  // namespace donner

extern "C" {
/// Compiled identities as JSON, valid after C++ initialization. No payload is included.
const char* DonnerPackagedResourceManifest();
/// Install an authenticated known resource. Returns 0 on unknown identity, corruption or budget.
void DonnerFailPackagedResource(const char* sha256);
int DonnerInstallPackagedResource(const char* sha256, const uint8_t* bytes, size_t size);
}
