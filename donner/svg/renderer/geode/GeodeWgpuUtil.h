#pragma once
/// @file
/// Small utility shims on top of the `wgpu::` C++ wrapper (from
/// `eliemichel/WebGPU-distribution`'s `webgpu.hpp`) used throughout the
/// Geode renderer.
///
/// The vendored `webgpu.hpp` wraps wgpu-native's C API directly — its
/// `wgpu::StringView` only has a `std::string_view` constructor, which
/// makes the otherwise-common `descriptor.label = "…"` assignment
/// verbose. The helpers in this header exist so the rest of the
/// Geode code can stay readable:
///
///     desc.label = wgpuLabel("GeodePipeline");
///
/// rather than spelling out the std::string_view wrap at every site. This file
/// also provides a small RAII owner for WebGPU handles returned from create /
/// acquire calls. The vendored `webgpu.hpp` objects are typed raw handles, not
/// owning smart handles.

#include <string_view>
#include <utility>
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

  /// Release the current handle and optionally take ownership of a replacement.
  void reset(Handle handle = Handle()) noexcept {
    if (handle_) {
      handle_.release();
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
  Handle handle_;
};

}  // namespace donner::geode
