# Function Mapping Tables

Legend: `☐` Not started, `🧩` Stub only, `🟡` Implemented/tested (Rust completeness not yet vetted), `🟢` Rust-completeness vetted (line-by-line against Rust), `✅` Verified parity sign-off (user-requested audit complete), `⏸` Blocked.

### `third_party/tiny-skia/src/painter.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `FillRule` enum | `FillRule` (Path.h) | 🟢 | Line-by-line audited: Winding, EvenOdd enum values match Rust. Validated in audit 2026-03-02. |
| `Paint` struct | `Paint` (Painter.h) | 🟢 | Line-by-line audited: shader, blend_mode, anti_alias, colorspace, force_hq_pipeline fields match Rust. Validated in audit 2026-03-02. |
| `Paint::default` | `Paint` default initialization | 🟢 | Line-by-line audited: Default values (Black, SourceOver, anti_alias=true, Linear, force_hq=false) match Rust. Validated in audit 2026-03-02. |
| `Paint::set_color` | `Paint::setColor()` | 🟢 | Line-by-line audited: Color setter matches Rust. Validated in audit 2026-03-02. |
| `Paint::set_color_rgba8` | `Paint::setColorRgba8()` | 🟢 | Line-by-line audited: RGBA8 color setter matches Rust. Validated in audit 2026-03-02. |
| `Paint::is_solid_color` | `Paint::isSolidColor()` | 🟢 | Line-by-line audited: Variant type check matches Rust. Validated in audit 2026-03-02. |
| `DrawTiler` struct | `DrawTiler` (Painter.h) | 🟢 | Line-by-line audited: kMaxDimensions=8191, required(), create(), next() match Rust. Validated in audit 2026-03-02. |
| `DrawTiler::MAX_DIMENSIONS` | `DrawTiler::kMaxDimensions` | 🟢 | Line-by-line audited: `8192 - 1` matches Rust. Validated in audit 2026-03-02. |
| `DrawTiler::required` | `DrawTiler::required()` | 🟢 | Line-by-line audited: Tiling requirement check matches Rust. Validated in audit 2026-03-02. |
| `DrawTiler::new` | `DrawTiler::create()` | 🟢 | Line-by-line audited: Optional factory matches Rust. Validated in audit 2026-03-02. |
| `DrawTiler::Iterator::next` | `DrawTiler::next()` | 🟢 | Line-by-line audited: Row-major tile iteration matches Rust. Validated in audit 2026-03-02. |
| `is_too_big_for_math` | `isTooBigForMath()` | 🟢 | Line-by-line audited: SCALAR_MAX * 0.25 threshold with NaN-safe comparison matches Rust. Validated in audit 2026-03-02. |
| `treat_as_hairline` | `treatAsHairline()` | 🟢 | Line-by-line audited: Zero-width, non-AA, fastLen + ave logic matches Rust. Validated in audit 2026-03-02. |
| `PixmapMut::fill_rect` | `fillRect()` | 🟢 | Line-by-line audited: Identity fast path and transform delegation match Rust. Validated in audit 2026-03-02. |
| `PixmapMut::fill_path` | `fillPath()` | 🟢 | Line-by-line audited: Identity path with tiling and transform path match Rust. Validated in audit 2026-03-02. |
| `PixmapMut::stroke_path` | `strokePath()` | 🟢 | Line-by-line audited: Dash, hairline detect, thick stroke via fill match Rust. Validated in audit 2026-03-02. |
| `PixmapMut::stroke_hairline` | `strokeHairline()` | 🟢 | Line-by-line audited: Dispatch to scan::hairline{_aa}::strokePath matches Rust. Validated in audit 2026-03-02. |
| `PixmapMut::draw_pixmap` | `drawPixmap()` | 🟢 | Line-by-line audited: Pattern shader + fillRect composition matches Rust. Validated in audit 2026-03-02. |
| `PixmapMut::apply_mask` | `applyMask()` | 🟢 | Line-by-line audited: LoadMaskU8 → LoadDestination → DestinationIn → Store pipeline matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/src/pipeline/blitter.rs` — Paint-aware factory
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `RasterPipelineBlitter::new(paint, mask, pixmap)` | `RasterPipelineBlitter::create(Paint, mask, pixmap)` | 🟢 | Line-by-line audited: Full shader pipeline construction with blend mode optimizations and pattern pixmap cloning matches Rust. Validated in audit 2026-03-02. |

### Infrastructure additions
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `Rect::width` | `Rect::width()` (Geom.h) | 🟢 | Line-by-line audited: Width accessor matches Rust. Validated in audit 2026-03-02. |
| `Rect::height` | `Rect::height()` (Geom.h) | 🟢 | Line-by-line audited: Height accessor matches Rust. Validated in audit 2026-03-02. |
| `IntSize::to_int_rect` | `IntSize::toIntRect()` (Geom.h) | 🟢 | Line-by-line audited: IntRect conversion matches Rust. Validated in audit 2026-03-02. |
| `IntSize::to_rect` | `IntSize::toRect()` (Geom.h) | 🟢 | Line-by-line audited: Rect conversion matches Rust. Validated in audit 2026-03-02. |
| `Transform::map_points` | `Transform::mapPoints()` (pipeline/Mod.h) | 🟢 | Line-by-line audited: Affine point mapping matches Rust. Validated in audit 2026-03-02. |
| `Path::transform` | `Path::transform()` (Path.h) | 🟢 | Line-by-line audited: Returns new Path with transformed points matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::from_rect` | `pathFromRect()` (Path.h) | 🟢 | Line-by-line audited: Move-Line-Line-Line-Close path creation matches Rust. Validated in audit 2026-03-02. |
