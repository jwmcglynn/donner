#pragma once
/// @file
/// Small utility shims on top of the `wgpu::` C++ wrapper (from
/// `eliemichel/WebGPU-distribution`'s `webgpu.hpp`) used throughout the
/// Geode renderer.
///
/// The vendored `webgpu.hpp` wraps wgpu-native's C API directly - its
/// `wgpu::StringView` only has a `std::string_view` constructor, which
/// makes the otherwise-common `descriptor.label = "..."` assignment
/// verbose. The helpers in this header exist so the rest of the
/// Geode code can stay readable:
///
///     desc.label = wgpuLabel("GeodePipeline");
///
/// rather than spelling out the std::string_view wrap at every site. This file
/// also provides a small RAII owner for WebGPU handles returned from create /
/// acquire calls. The vendored `webgpu.hpp` objects are typed raw handles, not
/// owning smart handles.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>
#include <webgpu/webgpu.hpp>

namespace donner::geode {

/// Wrap a NUL-terminated C string literal in a `wgpu::StringView`
/// suitable for direct assignment to a descriptor's `label` field.
/// `std::string_view{}` handles the `strlen()` at compile time when the
/// argument is a literal.
[[nodiscard]] inline wgpu::StringView wgpuLabel(const char* text) noexcept {
  return wgpu::StringView{std::string_view{text}};
}

/// Wrap an arbitrary `std::string_view` as a `wgpu::StringView`. Kept
/// separate from `wgpuLabel(const char*)` so the common label-literal
/// call path stays trivially copyable without pulling `string_view`
/// into callers that don't already need it.
[[nodiscard]] inline wgpu::StringView wgpuLabel(std::string_view text) noexcept {
  return wgpu::StringView{text};
}

/// Whether headless/test WebGPU creation should request a software fallback
/// adapter. Bazel's Linux test configuration sets this to keep independent
/// test processes from contending for one physical Vulkan device. Product
/// processes leave it unset and retain the platform-default adapter policy.
[[nodiscard]] inline bool wgpuForceFallbackAdapterRequested() noexcept {
  const char* fallbackEnv = std::getenv("DONNER_GEODE_FORCE_FALLBACK_ADAPTER");
  if (fallbackEnv == nullptr || fallbackEnv[0] == '\0') {
    return false;
  }

  const std::string_view value(fallbackEnv);
  if (value == "1") {
    return true;
  }
  if (value == "0") {
    return false;
  }

  std::fprintf(stderr,
               "[Geode/wgpu-native] Ignoring unsupported "
               "DONNER_GEODE_FORCE_FALLBACK_ADAPTER=%.*s; expected 0 or 1.\n",
               static_cast<int>(value.size()), value.data());
  return false;
}

/**
 * Release a raw WebGPU handle and reset the wrapper to null.
 *
 * @tparam Handle A `wgpu::` handle type with `operator bool()` and `release()`.
 * @param handle Owned raw handle to release.
 */
template <typename Handle>
void ReleaseWgpuHandle(Handle& handle) {
  if (handle) {
    handle.release();
    handle = Handle();
  }
}

/**
 * Move-only RAII owner for a single WebGPU handle.
 *
 * @tparam Handle A `wgpu::` handle type with `operator bool()` and `release()`.
 *
 * The vendored `webgpu.hpp` wrapper is a thin typed wrapper over raw C handles:
 * copying it does not retain, and destroying it does not release. Use this type
 * when Donner owns a +1 reference from a WebGPU create/acquire call. Borrowed
 * handles should remain plain `wgpu::` values.
 */
template <typename Handle>
class ScopedWgpuHandle {
public:
  /// Construct an empty scoped handle.
  ScopedWgpuHandle() = default;

  /// Take ownership of an existing +1 handle.
  explicit ScopedWgpuHandle(Handle handle) : handle_(std::move(handle)) {}

  /// Release the owned handle, if any.
  ~ScopedWgpuHandle() { reset(); }

  ScopedWgpuHandle(const ScopedWgpuHandle&) = delete;
  ScopedWgpuHandle& operator=(const ScopedWgpuHandle&) = delete;

  /// Move ownership from another scoped handle.
  ScopedWgpuHandle(ScopedWgpuHandle&& other) noexcept : handle_(std::move(other.handle_)) {
    other.handle_ = Handle();
  }

  /// Release the current handle and move ownership from another scoped handle.
  ScopedWgpuHandle& operator=(ScopedWgpuHandle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::move(other.handle_);
      other.handle_ = Handle();
    }
    return *this;
  }

  /// Return the owned handle.
  [[nodiscard]] const Handle& get() const noexcept { return handle_; }

  /// Return the owned handle.
  [[nodiscard]] Handle& get() noexcept { return handle_; }

  /// True when a non-null handle is owned.
  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handle_); }

  /// Number of non-null handles released by this scoped-handle specialization.
  [[nodiscard]] static uint64_t releaseCountForTesting() noexcept {
    return releaseCount_.load(std::memory_order_relaxed);
  }

  /// Release the current handle and optionally take ownership of a replacement.
  void reset(Handle handle = Handle()) noexcept {
    if (handle_) {  // Keep the release counter scoped to RAII-owned resources.
      ReleaseWgpuHandle(handle_);
      releaseCount_.fetch_add(1, std::memory_order_relaxed);
    }
    handle_ = std::move(handle);
  }

  /// Return the owned handle without releasing it.
  [[nodiscard]] Handle take() noexcept {
    Handle result = std::move(handle_);
    handle_ = Handle();
    return result;
  }

private:
  static inline std::atomic<uint64_t> releaseCount_{0};
  Handle handle_;
};

/**
 * Scoped owner for short-lived WebGPU resources used while recording a command
 * encoder.
 *
 * The returned `wgpu::` values are borrowed aliases that remain valid while the
 * arena is alive. This is useful for render-pass helpers that need handles to
 * survive until the enclosing encoder is finished, but do not need bespoke
 * ownership fields for every per-draw texture view / bind group / buffer.
 */
class ScopedWgpuResourceArena {
public:
  /// Retain a buffer handle and return a borrowed alias.
  [[nodiscard]] wgpu::Buffer retain(wgpu::Buffer handle) {
    return retainImpl(buffers_, std::move(handle));
  }

  /// Retain a texture handle and return a borrowed alias.
  [[nodiscard]] wgpu::Texture retain(wgpu::Texture handle) {
    return retainImpl(textures_, std::move(handle));
  }

  /// Retain a texture-view handle and return a borrowed alias.
  [[nodiscard]] wgpu::TextureView retain(wgpu::TextureView handle) {
    return retainImpl(textureViews_, std::move(handle));
  }

  /// Retain a sampler handle and return a borrowed alias.
  [[nodiscard]] wgpu::Sampler retain(wgpu::Sampler handle) {
    return retainImpl(samplers_, std::move(handle));
  }

  /// Retain a bind-group handle and return a borrowed alias.
  [[nodiscard]] wgpu::BindGroup retain(wgpu::BindGroup handle) {
    return retainImpl(bindGroups_, std::move(handle));
  }

  /// Destroy backing storage for retained buffers/textures before release.
  void destroyBackings() {
    destroyBackingsImpl(buffers_);
    destroyBackingsImpl(textures_);
  }

private:
  template <typename Handle>
  static Handle retainImpl(std::vector<ScopedWgpuHandle<Handle>>& storage, Handle handle) {
    if (!handle) {
      return Handle();
    }
    Handle borrowed = handle;
    storage.push_back(ScopedWgpuHandle<Handle>(std::move(handle)));
    return borrowed;
  }

  template <typename Handle>
  static void destroyBackingsImpl(std::vector<ScopedWgpuHandle<Handle>>& storage) {
    for (ScopedWgpuHandle<Handle>& handle : storage) {
      if (handle) {
        handle.get().destroy();
        handle.reset();
      }
    }
    storage.clear();
  }

  std::vector<ScopedWgpuHandle<wgpu::Buffer>> buffers_;
  std::vector<ScopedWgpuHandle<wgpu::Texture>> textures_;
  std::vector<ScopedWgpuHandle<wgpu::TextureView>> textureViews_;
  std::vector<ScopedWgpuHandle<wgpu::Sampler>> samplers_;
  std::vector<ScopedWgpuHandle<wgpu::BindGroup>> bindGroups_;
};

}  // namespace donner::geode
