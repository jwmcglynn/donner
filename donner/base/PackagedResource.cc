#include "donner/base/PackagedResource.h"

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#ifndef __EMSCRIPTEN__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <utility>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define DONNER_RESOURCE_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define DONNER_RESOURCE_EXPORT
#endif

namespace donner {
namespace {
std::vector<const PackagedResource*>& Registry() {
  static std::vector<const PackagedResource*> resources;
  return resources;
}
std::mutex& SessionMutex() {
  static std::mutex mutex;
  return mutex;
}
std::shared_ptr<PackagedResourceSession>& ApplicationSession() {
  static std::shared_ptr<PackagedResourceSession> session;
  return session;
}

std::shared_ptr<PackagedResourceSession> BorrowingSession() {
  std::lock_guard lock(SessionMutex());
  if (!ApplicationSession()) {
    ApplicationSession() = std::make_shared<PackagedResourceSession>();
  }
  ApplicationSession()->markBorrowed();
  return ApplicationSession();
}

std::string InstalledRoot() {
  std::filesystem::path executable;
#ifdef __APPLE__
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  std::error_code ec;
  executable = std::filesystem::canonical(buffer.data(), ec);
  if (ec) {
    return {};
  }
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
  std::array<char, 4096> buffer{};
  const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (length <= 0 || static_cast<size_t>(length) == buffer.size()) {
    return {};
  }
  executable = std::string(buffer.data(), static_cast<size_t>(length));
#else
  return {};
#endif
  const auto directory = executable.parent_path();
  if (directory.filename() == "MacOS" && directory.parent_path().filename() == "Contents") {
    return (directory.parent_path() / "Resources" / "donner-assets").string();
  }
  return (directory / "donner-assets").string();
}

bool ValidIdentity(const PackagedResource& resource) {
  const std::string_view digest(resource.sha256);
  const std::string_view path(resource.path);
  if (digest.size() != 64 ||
      !std::all_of(digest.begin(), digest.end(),
                   [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
      resource.encodedSize == 0 ||
      resource.encodedSize > PackagedResourceSession::kMaximumEncodedBytes ||
      resource.decodedSize > 64 * 1024 * 1024) {
    return false;
  }
  const std::string prefix = "assets/" + std::string(digest) + ".";
  return path.starts_with(prefix) && path.size() > prefix.size() &&
         path.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789", prefix.size()) ==
             std::string_view::npos;
}

std::string JsonString(std::string_view value) {
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') {
      result += '\\';
      result += static_cast<char>(c);
    } else if (c >= 32 && c < 127) {
      result += static_cast<char>(c);
    } else {
      result += '?';
    }
  }
  return result + '"';
}
}  // namespace

std::string ResourceSha256(std::span<const uint8_t> bytes) {
  constexpr std::array<uint32_t, 64> k = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  std::array<uint32_t, 8> state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const size_t blocks = bytes.size() / 64 + ((bytes.size() % 64 >= 56) ? 2 : 1);
  for (size_t block = 0; block < blocks; ++block) {
    std::array<uint32_t, 64> words{};
    for (size_t i = 0; i < 64; ++i) {
      const size_t pos = block * 64 + i;
      uint8_t value = pos < bytes.size() ? bytes[pos] : pos == bytes.size() ? 0x80 : 0;
      if (pos >= blocks * 64 - 8) {
        value = static_cast<uint8_t>((static_cast<uint64_t>(bytes.size()) * 8) >>
                                     ((blocks * 64 - 1 - pos) * 8));
      }
      words[i / 4] |= static_cast<uint32_t>(value) << (24 - (i % 4) * 8);
    }
    for (size_t i = 16; i < 64; ++i) {
      const uint32_t x = words[i - 15], y = words[i - 2];
      words[i] = words[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + words[i - 7] +
                 (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
    }
    auto [a, b, c, d, e, f, g, h] = state;
    for (size_t i = 0; i < 64; ++i) {
      const uint32_t first = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                             ((e & f) ^ (~e & g)) + k[i] + words[i];
      const uint32_t second =
          (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + first;
      d = c;
      c = b;
      b = a;
      a = first + second;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (uint32_t word : state) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      result += digits[(word >> shift) & 15];
    }
  }
  return result;
}

PackagedResourceProvider PackagedResourceDirectoryProvider(std::string root) {
  return [root = std::move(root)](const PackagedResource& resource) -> ResourceBytes {
    if (root.empty() || !ValidIdentity(resource)) {
      return {};
    }
#ifdef __EMSCRIPTEN__
    return {};
#else
    // Traverse using directory handles, refusing symlinks at each open. Keeping the parent open
    // closes the check/use race that pathname-only symlink inspection would permit.
    int directory = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) {
      return {};
    }
    const std::filesystem::path relative(resource.path);
    const auto parent = relative.parent_path();
    for (const auto& component : parent) {
      const int next =
          openat(directory, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      close(directory);
      if (next < 0) {
        return {};
      }
      directory = next;
    }
    const int file = openat(directory, relative.filename().c_str(),
                            O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    close(directory);
    if (file < 0) {
      return {};
    }
    struct stat status{};
    if (fstat(file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<uint64_t>(status.st_size) != resource.encodedSize) {
      close(file);
      return {};
    }
    auto bytes = std::make_shared<std::vector<uint8_t>>(resource.encodedSize);
    size_t offset = 0;
    while (offset < bytes->size()) {
      const auto count = read(file, bytes->data() + offset, bytes->size() - offset);
      if (count <= 0) {
        close(file);
        return {};
      }
      offset += static_cast<size_t>(count);
    }
    uint8_t extra;
    const auto trailing = read(file, &extra, 1);
    close(file);
    return trailing == 0 ? bytes : ResourceBytes{};
#endif
  };
}

struct PackagedResourceSession::Impl {
  explicit Impl(PackagedResourceProvider source) : provider(std::move(source)) {}
  mutable std::mutex mutex;
  PackagedResourceProvider provider;
  std::map<std::string, ResourceBytes> bytes;
  std::set<std::string> failures;
  std::set<std::string> pending;
  size_t retained = 0;
  bool borrowed = false;
  std::string error;
};
PackagedResourceSession::PackagedResourceSession(PackagedResourceProvider provider)
    : impl_(std::make_unique<Impl>(provider ? std::move(provider)
                                            : PackagedResourceDirectoryProvider(InstalledRoot()))) {
}
PackagedResourceSession::~PackagedResourceSession() = default;

bool PackagedResourceSession::install(const PackagedResource& resource,
                                      std::span<const uint8_t> bytes) {
  if (!ValidIdentity(resource) || bytes.size() != resource.encodedSize ||
      ResourceSha256(bytes) != resource.sha256) {
    std::lock_guard lock(impl_->mutex);
    impl_->error = std::string("Invalid packaged resource: ") + resource.id;
    impl_->failures.insert(resource.sha256);
    return false;
  }
  std::lock_guard lock(impl_->mutex);
  if (impl_->bytes.contains(resource.sha256)) {
    return true;
  }
  if (bytes.size() > kMaximumEncodedBytes - impl_->retained) {
    impl_->error = "Packaged resource retention budget exceeded";
    impl_->failures.insert(resource.sha256);
    return false;
  }
  impl_->failures.erase(resource.sha256);
  impl_->pending.erase(resource.sha256);
  impl_->bytes.emplace(resource.sha256,
                       std::make_shared<const std::vector<uint8_t>>(bytes.begin(), bytes.end()));
  impl_->retained += bytes.size();
  return true;
}
ResourceBytes PackagedResourceSession::load(const PackagedResource& resource) {
  if (!ValidIdentity(resource)) {
    fail(resource);
    return {};
  }
  {
    std::lock_guard lock(impl_->mutex);
    if (const auto found = impl_->bytes.find(resource.sha256); found != impl_->bytes.end()) {
      if (found->second->size() != resource.encodedSize) {
        impl_->error = std::string("Invalid packaged resource length: ") + resource.id;
        return {};
      }
      return found->second;
    }
  }
  const auto bytes = impl_->provider(resource);
  if (!bytes) {
    std::lock_guard lock(impl_->mutex);
    impl_->error = std::string("Missing packaged resource: ") + resource.id;
#ifndef __EMSCRIPTEN__
    impl_->failures.insert(resource.sha256);
#endif
    return {};
  }
  if (!install(resource, *bytes)) {
    return {};
  }
  std::lock_guard lock(impl_->mutex);
  return impl_->bytes.at(resource.sha256);
}
bool PackagedResourceSession::request(const PackagedResource& resource,
                                      const std::function<void()>& dispatch) {
  if (!dispatch || !ValidIdentity(resource)) {
    fail(resource);
    return false;
  }
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->bytes.contains(resource.sha256) || impl_->failures.contains(resource.sha256) ||
        impl_->pending.contains(resource.sha256)) {
      return false;
    }
    if (impl_->pending.size() >= 128) {
      impl_->failures.insert(resource.sha256);
      impl_->error = "Too many pending packaged resources";
      return false;
    }
    impl_->pending.insert(resource.sha256);
  }
  dispatch();
  return true;
}
bool PackagedResourceSession::retry(const PackagedResource& resource) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->pending.contains(resource.sha256) || impl_->bytes.contains(resource.sha256)) {
    return false;
  }
  return impl_->failures.erase(resource.sha256) != 0;
}
void PackagedResourceSession::markBorrowed() {
  std::lock_guard lock(impl_->mutex);
  impl_->borrowed = true;
}
bool PackagedResourceSession::hasBorrowedViews() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->borrowed;
}
bool PackagedResourceSession::ready(const PackagedResource& resource) const {
  std::lock_guard lock(impl_->mutex);
  return impl_->bytes.contains(resource.sha256);
}
bool PackagedResourceSession::failed(const PackagedResource& resource) const {
  std::lock_guard lock(impl_->mutex);
  return impl_->failures.contains(resource.sha256);
}
void PackagedResourceSession::fail(const PackagedResource& resource) {
  std::lock_guard lock(impl_->mutex);
  impl_->failures.insert(resource.sha256);
  impl_->pending.erase(resource.sha256);
  impl_->error = std::string("Unavailable packaged resource: ") + resource.id;
}
bool PackagedResourceSession::verify(const PackagedResource& resource) {
  if (ready(resource)) {
    return true;
  }
  if (!ValidIdentity(resource)) {
    fail(resource);
    return false;
  }
  const auto bytes = impl_->provider(resource);
  if (!bytes || bytes->size() != resource.encodedSize ||
      ResourceSha256(*bytes) != resource.sha256) {
    fail(resource);
    return false;
  }
  return true;
}
size_t PackagedResourceSession::retainedBytes() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->retained;
}
std::string PackagedResourceSession::error() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->error;
}
bool SetPackagedResourceSession(std::shared_ptr<PackagedResourceSession> session) {
  std::lock_guard lock(SessionMutex());
  if (ApplicationSession() && ApplicationSession() != session &&
      ApplicationSession()->hasBorrowedViews()) {
    return false;
  }
  ApplicationSession() = std::move(session);
  return true;
}
std::shared_ptr<PackagedResourceSession> GetPackagedResourceSession() {
  std::lock_guard lock(SessionMutex());
  if (!ApplicationSession()) {
    ApplicationSession() = std::make_shared<PackagedResourceSession>();
  }
  return ApplicationSession();
}
PackagedResource::PackagedResource(const char* id, const char* sha256, size_t encodedSize,
                                   size_t decodedSize, const char* mediaType, const char* group,
                                   const char* path, const char* testRunfilesRoot)
    : id(id),
      sha256(sha256),
      encodedSize(encodedSize),
      decodedSize(decodedSize),
      mediaType(mediaType),
      group(group),
      path(path),
      testRunfilesRoot(testRunfilesRoot) {
  Registry().push_back(this);
}
PackagedResource::~PackagedResource() {
  auto& resources = Registry();
  resources.erase(std::remove(resources.begin(), resources.end(), this), resources.end());
}
bool PackagedResource::ready() const {
  return GetPackagedResourceSession()->ready(*this);
}
bool PackagedResource::failed() const {
  return GetPackagedResourceSession()->failed(*this);
}
ResourceBytes PackagedResource::load() const {
  return GetPackagedResourceSession()->load(*this);
}
ResourceLease::ResourceLease(const PackagedResource& resource) : bytes_(resource.load()) {}
std::span<const uint8_t> PackagedResource::view() const {
  const auto session = BorrowingSession();
  const auto bytes = session->load(*this);
  return bytes ? std::span<const uint8_t>(*bytes) : std::span<const uint8_t>();
}
std::vector<const PackagedResource*> PackagedResources() {
  return Registry();
}
bool PreparePackagedResources(std::string* error) {
  const auto session = GetPackagedResourceSession();
  for (const auto* resource : PackagedResources()) {
    if (std::string_view(resource->group) == "sample" ||
        std::string_view(resource->group) == "catalog-font" ||
        std::string_view(resource->group) == "generic-font") {
#ifndef __EMSCRIPTEN__
      if (!session->verify(*resource)) {
        if (error) {
          *error = std::string("Missing or invalid packaged resource: ") + resource->id;
        }
        return false;
      }
#endif
      continue;
    }
    if (!session->load(*resource)) {
      if (error) {
        *error = session->error();
      }
      return false;
    }
  }
  return true;
}
}  // namespace donner

extern "C" DONNER_RESOURCE_EXPORT const char* DonnerPackagedResourceManifest() {
  static const std::string manifest = [] {
    std::string result = "{\"schemaVersion\":1,\"assets\":[";
    bool first = true;
    auto resources = donner::PackagedResources();
    std::sort(resources.begin(), resources.end(), [](const auto* a, const auto* b) {
      return std::string_view(a->id) < std::string_view(b->id);
    });
    for (const auto* r : resources) {
      if (!first) {
        result += ',';
      }
      first = false;
      result += "{\"id\":" + donner::JsonString(r->id) +
                ",\"sha256\":" + donner::JsonString(r->sha256) +
                ",\"encodedSize\":" + std::to_string(r->encodedSize) +
                ",\"decodedSize\":" + std::to_string(r->decodedSize) +
                ",\"mediaType\":" + donner::JsonString(r->mediaType) +
                ",\"group\":" + donner::JsonString(r->group) +
                ",\"path\":" + donner::JsonString(r->path) + "}";
    }
    return result + "]}";
  }();
  return manifest.c_str();
}
extern "C" DONNER_RESOURCE_EXPORT int DonnerInstallPackagedResource(const char* sha256,
                                                                    const uint8_t* bytes,
                                                                    size_t size) {
  if (!sha256 || !bytes || std::string_view(sha256).size() != 64) {
    return 0;
  }
  for (const auto* resource : donner::PackagedResources()) {
    if (std::string_view(resource->sha256) == sha256) {
      return donner::GetPackagedResourceSession()->install(*resource, {bytes, size}) ? 1 : 0;
    }
  }
  return 0;
}

extern "C" DONNER_RESOURCE_EXPORT void DonnerFailPackagedResource(const char* sha256) {
  if (!sha256) {
    return;
  }
  for (const auto* resource : donner::PackagedResources()) {
    if (std::string_view(resource->sha256) == sha256) {
      donner::GetPackagedResourceSession()->fail(*resource);
      return;
    }
  }
}
