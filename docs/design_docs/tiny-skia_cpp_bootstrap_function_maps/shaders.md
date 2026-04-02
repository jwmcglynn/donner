# Function Mapping Tables

Legend: `☐` Not started, `🧩` Stub only, `🟡` Implemented/tested (Rust completeness not yet vetted), `🟢` Rust-completeness vetted (line-by-line against Rust), `✅` Verified parity sign-off (user-requested audit complete), `⏸` Blocked.

### `third_party/tiny-skia/src/shaders/mod.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `SpreadMode` | `SpreadMode` (in pipeline/Mod.h) | 🟢 | Line-by-line audited: Enum with Pad, Reflect, Repeat variants matches Rust. Validated in audit 2026-03-02. |
| `Shader` enum | `Shader` (`std::variant<Color, LinearGradient>`) | 🟢 | Line-by-line audited: Variant type dispatches match Rust enum. Validated in audit 2026-03-02. |
| `Shader::is_opaque` | `isShaderOpaque()` | 🟢 | Line-by-line audited: Free function dispatching via std::visit matches Rust. Validated in audit 2026-03-02. |
| `Shader::push_stages` | `pushShaderStages()` | 🟢 | Line-by-line audited: SolidColor→pushUniformColor, LinearGradient→pushStages dispatch matches Rust. Validated in audit 2026-03-02. |
| `Shader::transform` | `transformShader()` | 🟢 | Line-by-line audited: SolidColor no-op, gradient postConcat matches Rust. Validated in audit 2026-03-02. |
| `Shader::apply_opacity` | `applyShaderOpacity()` | 🟢 | Line-by-line audited: Delegates to Color::applyOpacity or Gradient::applyOpacity matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/src/shaders/gradient.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `DEGENERATE_THRESHOLD` | `kDegenerateThreshold` | 🟢 | Line-by-line audited: `1.0f / (1 << 15)` matches Rust constant. Validated in audit 2026-03-02. |
| `GradientStop` | `GradientStop` | 🟢 | Line-by-line audited: Struct with NormalizedF32 position and Color matches Rust. Validated in audit 2026-03-02. |
| `Gradient::new` | `Gradient::Gradient` (constructor) | 🟢 | Line-by-line audited: Dummy endpoint insertion, monotonic position enforcement, uniform-stop detection, opacity tracking match Rust. Validated in audit 2026-03-02. |
| `Gradient::push_stages` | `Gradient::pushStages()` | 🟢 | Line-by-line audited: SeedShader → Transform → TileMode → (2-stop \| multi-stop) → Premultiply matches Rust. Validated in audit 2026-03-02. |
| `Gradient::apply_opacity` | `Gradient::applyOpacity()` | 🟢 | Line-by-line audited: Iterates stops, applies opacity, rechecks colors_are_opaque matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/src/shaders/linear_gradient.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `LinearGradient::new` | `LinearGradient::create()` | 🟢 | Line-by-line audited: Factory with empty/single/degenerate/non-invertible/infinite handling matches Rust. Validated in audit 2026-03-02. |
| `LinearGradient::push_stages` | `LinearGradient::pushStages()` | 🟢 | Line-by-line audited: Delegates to base Gradient::pushStages with no pre/post callbacks matches Rust. Validated in audit 2026-03-02. |
| `points_to_unit_ts` | `pointsToUnitTs()` (anonymous namespace) | 🟢 | Line-by-line audited: Gradient start/end to unit-space transform matches Rust. Validated in audit 2026-03-02. |
| `average_gradient_color` | `averageGradientColor()` (anonymous namespace) | 🟢 | Line-by-line audited: Weighted color average for degenerate gradients matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/path/src/transform.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `Transform` struct | `Transform` class (pipeline/Mod.h) | 🟢 | Line-by-line audited: 6-field affine matrix (sx, kx, ky, sy, tx, ty) matches Rust. Validated in audit 2026-03-02. |
| `Transform::identity` | `Transform::identity()` | 🟢 | Line-by-line audited: Identity construction matches Rust. Validated in audit 2026-03-02. |
| `Transform::from_row` | `Transform::fromRow()` | 🟢 | Line-by-line audited: Row-wise construction matches Rust. Validated in audit 2026-03-02. |
| `Transform::from_translate` | `Transform::fromTranslate()` | 🟢 | Line-by-line audited: Translate-only construction matches Rust. Validated in audit 2026-03-02. |
| `Transform::from_scale` | `Transform::fromScale()` | 🟢 | Line-by-line audited: Scale-only construction matches Rust. Validated in audit 2026-03-02. |
| `Transform::is_identity` | `Transform::isIdentity()` | 🟢 | Line-by-line audited: Identity classification matches Rust. Validated in audit 2026-03-02. |
| `Transform::is_translate` | `Transform::isTranslate()` | 🟢 | Line-by-line audited: Translate classification matches Rust. Validated in audit 2026-03-02. |
| `Transform::is_scale_translate` | `Transform::isScaleTranslate()` | 🟢 | Line-by-line audited: Scale-translate classification matches Rust. Validated in audit 2026-03-02. |
| `Transform::has_scale` | `Transform::hasScale()` | 🟢 | Line-by-line audited: Scale detection matches Rust. Validated in audit 2026-03-02. |
| `Transform::has_skew` | `Transform::hasSkew()` | 🟢 | Line-by-line audited: Skew detection matches Rust. Validated in audit 2026-03-02. |
| `Transform::has_translate` | `Transform::hasTranslate()` | 🟢 | Line-by-line audited: Translate detection matches Rust. Validated in audit 2026-03-02. |
| `Transform::invert` | `Transform::invert()` | 🟢 | Line-by-line audited: f64-precision determinant, identity/scale-translate fast paths, singular detection match Rust. Validated in audit 2026-03-02. |
| `Transform::pre_concat` | `Transform::preConcat()` | 🟢 | Line-by-line audited: Pre-concatenation matches Rust. Validated in audit 2026-03-02. |
| `Transform::post_concat` | `Transform::postConcat()` | 🟢 | Line-by-line audited: Post-concatenation matches Rust. Validated in audit 2026-03-02. |
| `Transform::pre_scale` | `Transform::preScale()` | 🟢 | Line-by-line audited: Pre-scale matches Rust. Validated in audit 2026-03-02. |
| `Transform::post_scale` | `Transform::postScale()` | 🟢 | Line-by-line audited: Post-scale matches Rust. Validated in audit 2026-03-02. |
| `Transform::pre_translate` | `Transform::preTranslate()` | 🟢 | Line-by-line audited: Pre-translate matches Rust. Validated in audit 2026-03-02. |
| `Transform::post_translate` | `Transform::postTranslate()` | 🟢 | Line-by-line audited: Post-translate matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/src/shaders/sweep_gradient.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `SweepGradient` struct | `SweepGradient` class | 🟢 | Line-by-line audited: Gradient base + t0/t1 angle parameters match Rust. Validated in audit 2026-03-02. |
| `SweepGradient::new` | `SweepGradient::create()` | 🟢 | Line-by-line audited: Factory with empty/single/inverted/degenerate/full-circle handling matches Rust. Validated in audit 2026-03-02. |
| `SweepGradient::is_opaque` | `SweepGradient::isOpaque()` | 🟢 | Line-by-line audited: Delegates to base Gradient matches Rust. Validated in audit 2026-03-02. |
| `SweepGradient::push_stages` | `SweepGradient::pushStages()` | 🟢 | Line-by-line audited: XYToUnitAngle + optional ApplyConcentricScaleBias matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/src/shaders/radial_gradient.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `FocalData` struct | `FocalData` struct | 🟢 | Line-by-line audited: r1, focalX, isSwapped fields and methods match Rust. Validated in audit 2026-03-02. |
| `FocalData::set` | `FocalData::set()` | 🟢 | Line-by-line audited: Focal point mapping with tsFromPolyToPoly and focalX=1 edge case match Rust. Validated in audit 2026-03-02. |
| `GradientType` enum | `GradientType` (`variant<RadialType, StripType, FocalData>`) | 🟢 | Line-by-line audited: Radial/Strip/Focal variant dispatch matches Rust enum. Validated in audit 2026-03-02. |
| `RadialGradient` struct | `RadialGradient` class | 🟢 | Line-by-line audited: Gradient base + GradientType layout matches Rust. Validated in audit 2026-03-02. |
| `RadialGradient::new` | `RadialGradient::create()` | 🟢 | Line-by-line audited: Factory with all degenerate/edge-case handling matches Rust. Validated in audit 2026-03-02. |
| `RadialGradient::new_radial_unchecked` | `RadialGradient::createRadialUnchecked()` | 🟢 | Line-by-line audited: Simple radial optimized path matches Rust. Validated in audit 2026-03-02. |
| `create` (module fn) | `RadialGradient::createTwoPoint()` | 🟢 | Line-by-line audited: Two-point conical logic (concentric, strip, focal) matches Rust. Validated in audit 2026-03-02. |
| `RadialGradient::push_stages` | `RadialGradient::pushStages()` | 🟢 | Line-by-line audited: GradientType dispatch with pre/post closures matches Rust. Validated in audit 2026-03-02. |
| `map_to_unit_x` | `mapToUnitX()` (anon namespace) | 🟢 | Line-by-line audited: Unit X mapping helper matches Rust. Validated in audit 2026-03-02. |
| `ts_from_poly_to_poly` | `tsFromPolyToPoly()` | 🟢 | Line-by-line audited: Transform mapping between two point pairs matches Rust. Validated in audit 2026-03-02. |
| `from_poly2` | `fromPoly2()` (anon namespace) | 🟢 | Line-by-line audited: Affine transform from two points matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/src/shaders/pattern.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `FilterQuality` enum | `FilterQuality` enum | 🟢 | Line-by-line audited: Nearest, Bilinear, Bicubic variants match Rust. Validated in audit 2026-03-02. |
| `PixmapPaint` struct | `PixmapPaint` struct | 🟢 | Line-by-line audited: opacity, blend_mode, quality fields with defaults match Rust. Validated in audit 2026-03-02. |
| `Pattern` struct | `Pattern` class | 🟢 | Line-by-line audited: pixmap, quality, spread_mode, opacity, transform layout matches Rust. Validated in audit 2026-03-02. |
| `Pattern::new` | `Pattern()` constructor | 🟢 | Line-by-line audited: Constructor parameters match Rust. Validated in audit 2026-03-02. |
| `Pattern::push_stages` | `Pattern::pushStages()` | 🟢 | Line-by-line audited: Full stage pipeline with quality dispatch and downgrade logic matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/path/src/scalar.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `SCALAR_NEARLY_ZERO` | `kScalarNearlyZero` (Math.h) | 🟢 | Line-by-line audited: `1.0 / 4096.0` matches Rust constant. Validated in audit 2026-03-02. |
| `Scalar::is_nearly_zero` | `isNearlyZero()` (Math.h) | 🟢 | Line-by-line audited: Inline function matches Rust. Validated in audit 2026-03-02. |
| `Scalar::is_nearly_zero_within_tolerance` | `isNearlyZeroWithinTolerance()` (Math.h) | 🟢 | Line-by-line audited: Tolerance-based zero check matches Rust. Validated in audit 2026-03-02. |
| `Scalar::is_nearly_equal` | `isNearlyEqual()` (Math.h) | 🟢 | Line-by-line audited: Near-equality check matches Rust. Validated in audit 2026-03-02. |
| `Scalar::invert` | `invert()` (Math.h) | 🟢 | Line-by-line audited: `1.0f / value` matches Rust. Validated in audit 2026-03-02. |
| `Scalar::bound` | `bound()` (Math.h) | 🟢 | Line-by-line audited: Templated clamp matches Rust. Validated in audit 2026-03-02. |
