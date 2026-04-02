//! C FFI bindings for the Rust tiny-skia library.
//!
//! Exposes opaque handle types and C-compatible functions so that C++ tests can
//! render scenes through the *original* Rust implementation and compare the
//! resulting pixels with the C++ port.

use std::slice;

use tiny_skia::{
    BlendMode, Color, FillRule, LineCap, LineJoin, Paint, Path, PathBuilder, Pixmap, Rect, Stroke,
    StrokeDash, Transform,
};

#[repr(C)]
pub struct TsFfiPaint {
    paint: Paint<'static>,
}

#[repr(C)]
pub struct TsFfiRect {
    rect: Rect,
}

#[repr(C)]
pub struct TsFfiTransform {
    transform: Transform,
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn blend_mode_from_u8(v: u8) -> BlendMode {
    match v {
        0 => BlendMode::Clear,
        1 => BlendMode::Source,
        2 => BlendMode::Destination,
        3 => BlendMode::SourceOver,
        4 => BlendMode::DestinationOver,
        5 => BlendMode::SourceIn,
        6 => BlendMode::DestinationIn,
        7 => BlendMode::SourceOut,
        8 => BlendMode::DestinationOut,
        9 => BlendMode::SourceAtop,
        10 => BlendMode::DestinationAtop,
        11 => BlendMode::Xor,
        12 => BlendMode::Plus,
        13 => BlendMode::Modulate,
        14 => BlendMode::Screen,
        15 => BlendMode::Overlay,
        16 => BlendMode::Darken,
        17 => BlendMode::Lighten,
        18 => BlendMode::ColorDodge,
        19 => BlendMode::ColorBurn,
        20 => BlendMode::HardLight,
        21 => BlendMode::SoftLight,
        22 => BlendMode::Difference,
        23 => BlendMode::Exclusion,
        24 => BlendMode::Multiply,
        25 => BlendMode::Hue,
        26 => BlendMode::Saturation,
        27 => BlendMode::Color,
        28 => BlendMode::Luminosity,
        _ => BlendMode::SourceOver,
    }
}

fn line_cap_from_u8(v: u8) -> LineCap {
    match v {
        0 => LineCap::Butt,
        1 => LineCap::Round,
        2 => LineCap::Square,
        _ => LineCap::Butt,
    }
}

fn line_join_from_u8(v: u8) -> LineJoin {
    match v {
        0 => LineJoin::Miter,
        1 => LineJoin::MiterClip,
        2 => LineJoin::Round,
        3 => LineJoin::Bevel,
        _ => LineJoin::Miter,
    }
}

// ---------------------------------------------------------------------------
// Pixmap
// ---------------------------------------------------------------------------

/// Creates a new RGBA pixmap filled with transparent black.
/// Returns null on failure.
#[no_mangle]
pub extern "C" fn ts_ffi_pixmap_new(width: u32, height: u32) -> *mut Pixmap {
    match Pixmap::new(width, height) {
        Some(pm) => Box::into_raw(Box::new(pm)),
        None => std::ptr::null_mut(),
    }
}

/// Frees a pixmap created by `ts_ffi_pixmap_new`.
#[no_mangle]
pub unsafe extern "C" fn ts_ffi_pixmap_free(pixmap: *mut Pixmap) {
    if !pixmap.is_null() {
        drop(Box::from_raw(pixmap));
    }
}

/// Returns a pointer to the raw RGBA8 pixel data (premultiplied alpha).
#[no_mangle]
pub unsafe extern "C" fn ts_ffi_pixmap_data(pixmap: *const Pixmap) -> *const u8 {
    (*pixmap).data().as_ptr()
}

/// Returns the byte length of the pixel data buffer.
#[no_mangle]
pub unsafe extern "C" fn ts_ffi_pixmap_data_len(pixmap: *const Pixmap) -> usize {
    (*pixmap).data().len()
}

/// Returns the pixmap width.
#[no_mangle]
pub unsafe extern "C" fn ts_ffi_pixmap_width(pixmap: *const Pixmap) -> u32 {
    (*pixmap).width()
}

/// Returns the pixmap height.
#[no_mangle]
pub unsafe extern "C" fn ts_ffi_pixmap_height(pixmap: *const Pixmap) -> u32 {
    (*pixmap).height()
}

/// Fills the entire pixmap with a solid color (straight alpha, will be
/// premultiplied internally by tiny-skia).
#[no_mangle]
pub unsafe extern "C" fn ts_ffi_pixmap_fill_color(
    pixmap: *mut Pixmap,
    r: u8,
    g: u8,
    b: u8,
    a: u8,
) {
    let color = Color::from_rgba8(r, g, b, a);
    (*pixmap).fill(color);
}

// ---------------------------------------------------------------------------
// PathBuilder
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn ts_ffi_path_builder_new() -> *mut PathBuilder {
    Box::into_raw(Box::new(PathBuilder::new()))
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_path_builder_free(builder: *mut PathBuilder) {
    if !builder.is_null() {
        drop(Box::from_raw(builder));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_path_builder_move_to(builder: *mut PathBuilder, x: f32, y: f32) {
    (*builder).move_to(x, y);
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_path_builder_line_to(builder: *mut PathBuilder, x: f32, y: f32) {
    (*builder).line_to(x, y);
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_path_builder_quad_to(
    builder: *mut PathBuilder,
    x1: f32,
    y1: f32,
    x: f32,
    y: f32,
) {
    (*builder).quad_to(x1, y1, x, y);
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_path_builder_cubic_to(
    builder: *mut PathBuilder,
    x1: f32,
    y1: f32,
    x2: f32,
    y2: f32,
    x: f32,
    y: f32,
) {
    (*builder).cubic_to(x1, y1, x2, y2, x, y);
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_path_builder_close(builder: *mut PathBuilder) {
    (*builder).close();
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_path_builder_push_rect(
    builder: *mut PathBuilder,
    left: f32,
    top: f32,
    right: f32,
    bottom: f32,
) {
    if let Some(rect) = Rect::from_ltrb(left, top, right, bottom) {
        (*builder).push_rect(rect);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_path_builder_push_circle(
    builder: *mut PathBuilder,
    cx: f32,
    cy: f32,
    r: f32,
) {
    (*builder).push_circle(cx, cy, r);
}

/// Consumes the builder and returns a `Path`, or null if the path is invalid.
/// After this call the builder pointer is **invalid** – the caller must not
/// use or free it.
#[no_mangle]
pub unsafe extern "C" fn ts_ffi_path_builder_finish(builder: *mut PathBuilder) -> *mut Path {
    let b = Box::from_raw(builder);
    match b.finish() {
        Some(path) => Box::into_raw(Box::new(path)),
        None => std::ptr::null_mut(),
    }
}

// ---------------------------------------------------------------------------
// Path
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_path_free(path: *mut Path) {
    if !path.is_null() {
        drop(Box::from_raw(path));
    }
}

// ---------------------------------------------------------------------------
// Fill path
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn ts_ffi_paint_new_solid_rgba8(
    r: u8,
    g: u8,
    b: u8,
    a: u8,
    anti_alias: bool,
    blend_mode: u8,
) -> *mut TsFfiPaint {
    let mut paint: Paint<'static> = Paint::default();
    paint.set_color_rgba8(r, g, b, a);
    paint.anti_alias = anti_alias;
    paint.blend_mode = blend_mode_from_u8(blend_mode);
    Box::into_raw(Box::new(TsFfiPaint { paint }))
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_paint_free(paint: *mut TsFfiPaint) {
    if !paint.is_null() {
        drop(Box::from_raw(paint));
    }
}

#[no_mangle]
pub extern "C" fn ts_ffi_rect_from_ltrb(
    left: f32,
    top: f32,
    right: f32,
    bottom: f32,
) -> *mut TsFfiRect {
    match Rect::from_ltrb(left, top, right, bottom) {
        Some(rect) => Box::into_raw(Box::new(TsFfiRect { rect })),
        None => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_rect_free(rect: *mut TsFfiRect) {
    if !rect.is_null() {
        drop(Box::from_raw(rect));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_transform_from_row(transform: *const f32) -> *mut TsFfiTransform {
    if transform.is_null() {
        return std::ptr::null_mut();
    }

    Box::into_raw(Box::new(TsFfiTransform {
        transform: read_transform(transform),
    }))
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_transform_free(transform: *mut TsFfiTransform) {
    if !transform.is_null() {
        drop(Box::from_raw(transform));
    }
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_fill_path_prepared(
    pixmap: *mut Pixmap,
    path: *const Path,
    paint: *const TsFfiPaint,
    fill_rule: u8,
    transform: *const TsFfiTransform,
) -> bool {
    if pixmap.is_null() || path.is_null() || paint.is_null() || transform.is_null() {
        return false;
    }

    let pm = &mut *pixmap;
    let p = &*path;
    let paint = &(*paint).paint;
    let ts = (*transform).transform;
    let rule = if fill_rule == 1 {
        FillRule::EvenOdd
    } else {
        FillRule::Winding
    };

    pm.fill_path(p, paint, rule, ts, None);
    true
}

#[no_mangle]
pub unsafe extern "C" fn ts_ffi_fill_rect_prepared(
    pixmap: *mut Pixmap,
    rect: *const TsFfiRect,
    paint: *const TsFfiPaint,
    transform: *const TsFfiTransform,
) -> bool {
    if pixmap.is_null() || rect.is_null() || paint.is_null() || transform.is_null() {
        return false;
    }

    let pm = &mut *pixmap;
    let rect = (*rect).rect;
    let paint = &(*paint).paint;
    let ts = (*transform).transform;
    pm.fill_rect(rect, paint, ts, None);
    true
}

/// Fills `path` onto `pixmap` using a solid color paint.
///
/// `transform` must point to 6 floats: [sx, kx, tx, ky, sy, ty].
/// Returns `true` on success.
#[no_mangle]
pub unsafe extern "C" fn ts_ffi_fill_path(
    pixmap: *mut Pixmap,
    path: *const Path,
    r: u8,
    g: u8,
    b: u8,
    a: u8,
    fill_rule: u8,
    anti_alias: bool,
    blend_mode: u8,
    transform: *const f32,
) -> bool {
    let pm = &mut *pixmap;
    let p = &*path;

    let mut paint = Paint::default();
    paint.set_color_rgba8(r, g, b, a);
    paint.anti_alias = anti_alias;
    paint.blend_mode = blend_mode_from_u8(blend_mode);

    let rule = if fill_rule == 1 {
        FillRule::EvenOdd
    } else {
        FillRule::Winding
    };

    let ts = read_transform(transform);
    pm.fill_path(p, &paint, rule, ts, None);
    true
}

// ---------------------------------------------------------------------------
// Fill rect
// ---------------------------------------------------------------------------

/// Fills an axis-aligned rectangle onto `pixmap`.
///
/// `transform` must point to 6 floats: [sx, kx, tx, ky, sy, ty].
#[no_mangle]
pub unsafe extern "C" fn ts_ffi_fill_rect(
    pixmap: *mut Pixmap,
    left: f32,
    top: f32,
    right: f32,
    bottom: f32,
    r: u8,
    g: u8,
    b: u8,
    a: u8,
    anti_alias: bool,
    blend_mode: u8,
    transform: *const f32,
) -> bool {
    let pm = &mut *pixmap;

    let rect = match Rect::from_ltrb(left, top, right, bottom) {
        Some(r) => r,
        None => return false,
    };

    let mut paint = Paint::default();
    paint.set_color_rgba8(r, g, b, a);
    paint.anti_alias = anti_alias;
    paint.blend_mode = blend_mode_from_u8(blend_mode);

    let ts = read_transform(transform);
    pm.fill_rect(rect, &paint, ts, None);
    true
}

// ---------------------------------------------------------------------------
// Stroke path
// ---------------------------------------------------------------------------

/// Strokes `path` onto `pixmap` using a solid color paint.
///
/// `transform` must point to 6 floats: [sx, kx, tx, ky, sy, ty].
/// `dash_array` / `dash_count` are optional (pass null / 0 for no dash).
#[no_mangle]
#[allow(non_snake_case)]
pub unsafe extern "C" fn ts_ffi_stroke_path(
    pixmap: *mut Pixmap,
    path: *const Path,
    r: u8,
    g: u8,
    b: u8,
    a: u8,
    width: f32,
    miterLimit: f32,
    lineCap: u8,
    lineJoin: u8,
    anti_alias: bool,
    blend_mode: u8,
    transform: *const f32,
    dash_array: *const f32,
    dash_count: u32,
    dash_offset: f32,
) -> bool {
    let pm = &mut *pixmap;
    let p = &*path;

    let mut paint = Paint::default();
    paint.set_color_rgba8(r, g, b, a);
    paint.anti_alias = anti_alias;
    paint.blend_mode = blend_mode_from_u8(blend_mode);

    let mut stroke = Stroke {
        width,
        miter_limit: miterLimit,
        line_cap: line_cap_from_u8(lineCap),
        line_join: line_join_from_u8(lineJoin),
        dash: None,
    };

    if !dash_array.is_null() && dash_count > 0 {
        let arr = slice::from_raw_parts(dash_array, dash_count as usize);
        stroke.dash = StrokeDash::new(arr.to_vec(), dash_offset);
    }

    let ts = read_transform(transform);
    pm.stroke_path(p, &paint, &stroke, ts, None);
    true
}

// ---------------------------------------------------------------------------
// Helpers (private)
// ---------------------------------------------------------------------------

unsafe fn read_transform(ptr: *const f32) -> Transform {
    let t = slice::from_raw_parts(ptr, 6);
    Transform::from_row(t[0], t[1], t[2], t[3], t[4], t[5])
}
