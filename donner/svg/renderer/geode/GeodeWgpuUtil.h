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
/// rather than spelling out the std::string_view wrap at every site.
/// These are the only `wgpu::`-related helpers in this file for now;
/// if we add more (e.g., chained-struct builders, buffer upload
/// shortcuts) they should live here too.

#include <string_view>

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

}  // namespace donner::geode
