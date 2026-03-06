#pragma once

/// @file RustReference.h
/// C++ RAII wrappers around the Rust tiny-skia FFI.
///
/// Provides Pixmap, Path, and PathBuilder handles with automatic lifetime
/// management, plus free-standing rendering functions that mirror the C++ API.

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "tiny_skia/BlendMode.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Path.h"
#include "tiny_skia/Point.h"
#include "tiny_skia/Stroke.h"
#include "tiny_skia_ffi.h"

namespace tiny_skia::rustRef {

// ---------------------------------------------------------------------------
// Pixmap
// ---------------------------------------------------------------------------

class Pixmap {
 public:
  static std::optional<Pixmap> create(std::uint32_t width,
                                      std::uint32_t height) {
    auto* handle = ts_ffi_pixmap_new(width, height);
    if (!handle) return std::nullopt;
    return Pixmap(handle);
  }

  ~Pixmap() {
    ts_ffi_pixmap_free(handle_);
  }

  Pixmap(const Pixmap&) = delete;
  Pixmap& operator=(const Pixmap&) = delete;

  Pixmap(Pixmap&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}
  Pixmap& operator=(Pixmap&& o) noexcept {
    if (this != &o) {
      ts_ffi_pixmap_free(handle_);
      handle_ = std::exchange(o.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] std::uint32_t width() const {
    return ts_ffi_pixmap_width(handle_);
  }

  [[nodiscard]] std::uint32_t height() const {
    return ts_ffi_pixmap_height(handle_);
  }

  [[nodiscard]] std::span<const std::uint8_t> data() const {
    return {ts_ffi_pixmap_data(handle_), ts_ffi_pixmap_data_len(handle_)};
  }

  void fill(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    ts_ffi_pixmap_fill_color(handle_, r, g, b, a);
  }

  TsFfiPixmap* raw() { return handle_; }
  const TsFfiPixmap* raw() const { return handle_; }

 private:
  explicit Pixmap(TsFfiPixmap* handle) : handle_(handle) {}
  TsFfiPixmap* handle_ = nullptr;
};

// ---------------------------------------------------------------------------
// Path
// ---------------------------------------------------------------------------

class Path {
 public:
  ~Path() {
    ts_ffi_path_free(handle_);
  }

  Path(const Path&) = delete;
  Path& operator=(const Path&) = delete;

  Path(Path&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}
  Path& operator=(Path&& o) noexcept {
    if (this != &o) {
      ts_ffi_path_free(handle_);
      handle_ = std::exchange(o.handle_, nullptr);
    }
    return *this;
  }

  const TsFfiPath* raw() const { return handle_; }

 private:
  friend class PathBuilder;
  explicit Path(TsFfiPath* handle) : handle_(handle) {}
  TsFfiPath* handle_ = nullptr;
};

// ---------------------------------------------------------------------------
// PathBuilder
// ---------------------------------------------------------------------------

class PathBuilder {
 public:
  PathBuilder() : handle_(ts_ffi_path_builder_new()) {}

  ~PathBuilder() {
    ts_ffi_path_builder_free(handle_);
  }

  PathBuilder(const PathBuilder&) = delete;
  PathBuilder& operator=(const PathBuilder&) = delete;

  PathBuilder(PathBuilder&& o) noexcept
      : handle_(std::exchange(o.handle_, nullptr)) {}
  PathBuilder& operator=(PathBuilder&& o) noexcept {
    if (this != &o) {
      ts_ffi_path_builder_free(handle_);
      handle_ = std::exchange(o.handle_, nullptr);
    }
    return *this;
  }

  void moveTo(float x, float y) {
    ts_ffi_path_builder_move_to(handle_, x, y);
  }

  void lineTo(float x, float y) {
    ts_ffi_path_builder_line_to(handle_, x, y);
  }

  void quadTo(float x1, float y1, float x, float y) {
    ts_ffi_path_builder_quad_to(handle_, x1, y1, x, y);
  }

  void cubicTo(float x1, float y1, float x2, float y2, float x, float y) {
    ts_ffi_path_builder_cubic_to(handle_, x1, y1, x2, y2, x, y);
  }

  void close() {
    ts_ffi_path_builder_close(handle_);
  }

  void pushRect(float left, float top, float right, float bottom) {
    ts_ffi_path_builder_push_rect(handle_, left, top, right, bottom);
  }

  void pushCircle(float cx, float cy, float r) {
    ts_ffi_path_builder_push_circle(handle_, cx, cy, r);
  }

  /// Consumes the builder and returns a Path.
  /// After calling this the PathBuilder is in a moved-from state.
  [[nodiscard]] std::optional<Path> finish() {
    auto* raw = ts_ffi_path_builder_finish(handle_);
    handle_ = nullptr;  // consumed by finish()
    if (!raw) return std::nullopt;
    return Path(raw);
  }

 private:
  TsFfiPathBuilder* handle_ = nullptr;
};

// ---------------------------------------------------------------------------
// Transform helper
// ---------------------------------------------------------------------------

namespace detail {

inline void writeTransform(const Transform& ts, float out[6]) {
  // Transform::fromRow expects (sx, ky, kx, sy, tx, ty) order.
  out[0] = ts.sx;
  out[1] = ts.ky;
  out[2] = ts.kx;
  out[3] = ts.sy;
  out[4] = ts.tx;
  out[5] = ts.ty;
}

inline std::uint8_t blendModeToU8(BlendMode m) {
  return static_cast<std::uint8_t>(m);
}

inline std::uint8_t fillRuleToU8(FillRule r) {
  return static_cast<std::uint8_t>(r);
}

inline std::uint8_t lineCapToU8(LineCap c) {
  return static_cast<std::uint8_t>(c);
}

inline std::uint8_t lineJoinToU8(LineJoin j) {
  return static_cast<std::uint8_t>(j);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Fill path
// ---------------------------------------------------------------------------

inline bool fillPath(Pixmap& pixmap, const Path& path, std::uint8_t r,
                     std::uint8_t g, std::uint8_t b, std::uint8_t a,
                     FillRule fillRule, bool antiAlias, BlendMode blendMode,
                     const Transform& transform) {
  float ts[6];
  detail::writeTransform(transform, ts);
  return ts_ffi_fill_path(pixmap.raw(), path.raw(), r, g, b, a,
                          detail::fillRuleToU8(fillRule), antiAlias,
                          detail::blendModeToU8(blendMode), ts);
}

/// Convenience overload using the C++ Paint-like defaults.
inline bool fillPath(Pixmap& pixmap, const Path& path, std::uint8_t r,
                     std::uint8_t g, std::uint8_t b, std::uint8_t a,
                     FillRule fillRule, const Transform& transform,
                     bool antiAlias = true,
                     BlendMode blendMode = BlendMode::SourceOver) {
  return fillPath(pixmap, path, r, g, b, a, fillRule, antiAlias, blendMode,
                  transform);
}

// ---------------------------------------------------------------------------
// Fill rect
// ---------------------------------------------------------------------------

inline bool fillRect(Pixmap& pixmap, float left, float top, float right,
                     float bottom, std::uint8_t r, std::uint8_t g,
                     std::uint8_t b, std::uint8_t a, bool antiAlias,
                     BlendMode blendMode, const Transform& transform) {
  float ts[6];
  detail::writeTransform(transform, ts);
  return ts_ffi_fill_rect(pixmap.raw(), left, top, right, bottom, r, g, b, a,
                          antiAlias, detail::blendModeToU8(blendMode), ts);
}

// ---------------------------------------------------------------------------
// Stroke path
// ---------------------------------------------------------------------------

inline bool strokePath(Pixmap& pixmap, const Path& path, std::uint8_t r,
                       std::uint8_t g, std::uint8_t b, std::uint8_t a,
                       const Stroke& stroke, bool antiAlias,
                       BlendMode blendMode, const Transform& transform) {
  float ts[6];
  detail::writeTransform(transform, ts);

  const float* dashArray = nullptr;
  std::uint32_t dashCount = 0;
  float dashOffset = 0.0f;
  if (stroke.dash.has_value()) {
    dashArray = stroke.dash->array.data();
    dashCount = static_cast<std::uint32_t>(stroke.dash->array.size());
    dashOffset = stroke.dash->offset;
  }

  return ts_ffi_stroke_path(
      pixmap.raw(), path.raw(), r, g, b, a, stroke.width, stroke.miterLimit,
      detail::lineCapToU8(stroke.lineCap),
      detail::lineJoinToU8(stroke.lineJoin), antiAlias,
      detail::blendModeToU8(blendMode), ts, dashArray, dashCount, dashOffset);
}

/// Convenience overload with default paint settings.
inline bool strokePath(Pixmap& pixmap, const Path& path, std::uint8_t r,
                       std::uint8_t g, std::uint8_t b, std::uint8_t a,
                       const Stroke& stroke, const Transform& transform,
                       bool antiAlias = true,
                       BlendMode blendMode = BlendMode::SourceOver) {
  return strokePath(pixmap, path, r, g, b, a, stroke, antiAlias, blendMode,
                    transform);
}

}  // namespace tiny_skia::rustRef
