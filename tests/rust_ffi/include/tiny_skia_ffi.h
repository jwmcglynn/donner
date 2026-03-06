#pragma once

/// @file tiny_skia_ffi.h
/// C-compatible FFI bindings to the Rust tiny-skia library.
///
/// Provides opaque handle types and rendering primitives so that C++ tests can
/// drive the *original* Rust implementation and compare pixel output with the
/// C++ port.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Opaque handle types
// ---------------------------------------------------------------------------

/// Opaque Rust `tiny_skia::Pixmap`.
typedef struct TsFfiPixmap TsFfiPixmap;

/// Opaque Rust `tiny_skia::Path`.
typedef struct TsFfiPath TsFfiPath;

/// Opaque Rust `tiny_skia::PathBuilder`.
typedef struct TsFfiPathBuilder TsFfiPathBuilder;

/// Opaque Rust render-state wrappers for prepared benchmark calls.
typedef struct TsFfiPaint TsFfiPaint;
typedef struct TsFfiRect TsFfiRect;
typedef struct TsFfiTransform TsFfiTransform;

// ---------------------------------------------------------------------------
// Pixmap
// ---------------------------------------------------------------------------

/// Creates a new RGBA pixmap filled with transparent black.
/// Returns NULL on failure (e.g. zero dimensions, allocation failure).
TsFfiPixmap* ts_ffi_pixmap_new(uint32_t width, uint32_t height);

/// Frees a pixmap. Safe to call with NULL.
void ts_ffi_pixmap_free(TsFfiPixmap* pixmap);

/// Returns a pointer to the raw premultiplied RGBA8 pixel data.
const uint8_t* ts_ffi_pixmap_data(const TsFfiPixmap* pixmap);

/// Returns the byte length of the pixel data buffer.
size_t ts_ffi_pixmap_data_len(const TsFfiPixmap* pixmap);

/// Returns the pixmap width in pixels.
uint32_t ts_ffi_pixmap_width(const TsFfiPixmap* pixmap);

/// Returns the pixmap height in pixels.
uint32_t ts_ffi_pixmap_height(const TsFfiPixmap* pixmap);

/// Fills the entire pixmap with a solid color (straight-alpha RGBA8).
void ts_ffi_pixmap_fill_color(TsFfiPixmap* pixmap, uint8_t r, uint8_t g,
                              uint8_t b, uint8_t a);

// ---------------------------------------------------------------------------
// PathBuilder
// ---------------------------------------------------------------------------

/// Creates a new path builder.
TsFfiPathBuilder* ts_ffi_path_builder_new(void);

/// Frees a path builder. Safe to call with NULL.
void ts_ffi_path_builder_free(TsFfiPathBuilder* builder);

void ts_ffi_path_builder_move_to(TsFfiPathBuilder* builder, float x, float y);
void ts_ffi_path_builder_line_to(TsFfiPathBuilder* builder, float x, float y);
void ts_ffi_path_builder_quad_to(TsFfiPathBuilder* builder, float x1, float y1,
                                 float x, float y);
void ts_ffi_path_builder_cubic_to(TsFfiPathBuilder* builder, float x1,
                                  float y1, float x2, float y2, float x,
                                  float y);
void ts_ffi_path_builder_close(TsFfiPathBuilder* builder);

/// Pushes a rectangle sub-path (left, top, right, bottom).
void ts_ffi_path_builder_push_rect(TsFfiPathBuilder* builder, float left,
                                   float top, float right, float bottom);

/// Pushes a circle sub-path.
void ts_ffi_path_builder_push_circle(TsFfiPathBuilder* builder, float cx,
                                     float cy, float r);

/// Consumes the builder and returns a finished Path.
/// Returns NULL if the path is invalid/empty.
/// After this call the builder pointer is INVALID – do not use or free it.
TsFfiPath* ts_ffi_path_builder_finish(TsFfiPathBuilder* builder);

// ---------------------------------------------------------------------------
// Path
// ---------------------------------------------------------------------------

/// Frees a path. Safe to call with NULL.
void ts_ffi_path_free(TsFfiPath* path);

// ---------------------------------------------------------------------------
// Rendering – fill
// ---------------------------------------------------------------------------

/// Creates a solid-color paint object to reuse across rendering calls.
TsFfiPaint* ts_ffi_paint_new_solid_rgba8(uint8_t r, uint8_t g, uint8_t b,
                                         uint8_t a, bool anti_alias,
                                         uint8_t blend_mode);

/// Frees a paint handle. Safe to call with NULL.
void ts_ffi_paint_free(TsFfiPaint* paint);

/// Creates a validated rectangle from [left, top, right, bottom].
/// Returns NULL if the rectangle is invalid.
TsFfiRect* ts_ffi_rect_from_ltrb(float left, float top, float right, float bottom);

/// Frees a rectangle handle. Safe to call with NULL.
void ts_ffi_rect_free(TsFfiRect* rect);

/// Creates a transform handle from [sx, kx, tx, ky, sy, ty].
/// Returns NULL when `transform` is NULL.
TsFfiTransform* ts_ffi_transform_from_row(const float transform[6]);

/// Frees a transform handle. Safe to call with NULL.
void ts_ffi_transform_free(TsFfiTransform* transform);

/// Prepared-state fill path call for engine-core comparisons.
bool ts_ffi_fill_path_prepared(TsFfiPixmap* pixmap, const TsFfiPath* path,
                               const TsFfiPaint* paint, uint8_t fill_rule,
                               const TsFfiTransform* transform);

/// Prepared-state fill rect call for engine-core comparisons.
bool ts_ffi_fill_rect_prepared(TsFfiPixmap* pixmap, const TsFfiRect* rect,
                               const TsFfiPaint* paint,
                               const TsFfiTransform* transform);

/// Fills `path` onto `pixmap` using a solid-color paint.
///
/// @param fill_rule  0 = Winding, 1 = EvenOdd.
/// @param blend_mode See BlendMode enum (0–28); default SourceOver = 3.
/// @param transform  Pointer to 6 floats: [sx, kx, tx, ky, sy, ty].
///                   Pass an identity array {1,0,0, 0,1,0} for no transform.
/// @return true on success.
bool ts_ffi_fill_path(TsFfiPixmap* pixmap, const TsFfiPath* path, uint8_t r,
                      uint8_t g, uint8_t b, uint8_t a, uint8_t fill_rule,
                      bool anti_alias, uint8_t blend_mode,
                      const float transform[6]);

/// Fills an axis-aligned rectangle onto `pixmap`.
bool ts_ffi_fill_rect(TsFfiPixmap* pixmap, float left, float top, float right,
                      float bottom, uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                      bool anti_alias, uint8_t blend_mode,
                      const float transform[6]);

// ---------------------------------------------------------------------------
// Rendering – stroke
// ---------------------------------------------------------------------------

/// Strokes `path` onto `pixmap` using a solid-color paint.
///
/// @param lineCap    0 = Butt, 1 = Round, 2 = Square.
/// @param lineJoin   0 = Miter, 1 = MiterClip, 2 = Round, 3 = Bevel.
/// @param dash_array Pointer to dash values (may be NULL for no dash).
/// @param dash_count Number of elements in dash_array.
/// @param dash_offset Offset into the dash pattern.
bool ts_ffi_stroke_path(TsFfiPixmap* pixmap, const TsFfiPath* path, uint8_t r,
                        uint8_t g, uint8_t b, uint8_t a, float width,
                        float miterLimit, uint8_t lineCap, uint8_t lineJoin,
                        bool anti_alias, uint8_t blend_mode,
                        const float transform[6], const float* dash_array,
                        uint32_t dash_count, float dash_offset);

#ifdef __cplusplus
}  // extern "C"
#endif
