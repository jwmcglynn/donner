# Function Mapping Tables

Legend: `☐` Not started, `🧩` Stub only, `🟡` Implemented/tested (Rust completeness not yet vetted), `🟢` Rust-completeness vetted (line-by-line against Rust), `✅` Verified parity sign-off (user-requested audit complete), `⏸` Blocked.

### `third_party/tiny-skia/src/lib.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| _none tracked_ | _none_ | ⏸ | No function-level symbols tracked for this Rust file in current C++ scope |

### `third_party/tiny-skia/src/alpha_runs.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `AlphaRuns::new` | `AlphaRuns::AlphaRuns` | 🟢 | Line-by-line audited: allocates `width+1` runs/alpha and calls `reset(width)` like Rust |
| `AlphaRuns::catch_overflow` | `AlphaRuns::catchOverflow` | 🟢 | Line-by-line audited: identical `alpha - (alpha >> 8)` overflow catch and `alpha <= 256` guard |
| `AlphaRuns::is_empty` | `AlphaRuns::isEmpty` | 🟢 | Line-by-line audited: same `runs[0]`/`alpha[0]` and terminal-run-empty logic |
| `AlphaRuns::reset` | `AlphaRuns::reset` | 🟢 | Line-by-line audited: sets `runs[0]`, `runs[width]=None/nullopt`, and `alpha[0]=0` equivalently |
| `AlphaRuns::add` | `AlphaRuns::add` | 🟢 | Line-by-line audited: start/middle/stop branches, offset updates, and overflow handling match Rust flow |
| `AlphaRuns::break_run` | `AlphaRuns::breakRun` | 🟢 | Line-by-line audited: two-phase run splitting logic (`x`, then `count`) matches Rust |
| `AlphaRuns::break_at` | `AlphaRuns::breakAt` | 🟢 | Line-by-line audited: same run-walk and split-at-x behavior used by clipping path |

### `third_party/tiny-skia/src/blend_mode.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `BlendMode::should_pre_scale_coverage` | `shouldPreScaleCoverage` | 🟢 | Line-by-line audited vs Rust `matches!` set; includes `Destination`, `DestinationOver`, `Plus`, `DestinationOut`, `SourceAtop`, `SourceOver`, `Xor`; matcher diagnostics upgraded in `BlendModeTest.ShouldPreScaleCoverage` |
| `BlendMode::to_stage` | `toStage` | 🟢 | Line-by-line audited vs Rust `match`; all 29 blend-mode mappings + `Source -> nullopt` parity confirmed; matcher diagnostics upgraded with `Optional`/`nullopt` assertions in `BlendModeTest.ToStageMapping` |

### `third_party/tiny-skia/src/edge_builder.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `ShiftedIntRect::new` | `ShiftedIntRect::create` | 🟢 | Line-by-line audited: shifted-rect construction and recoverable roundtrip semantics match Rust |
| `BasicEdgeBuilder::build_edges` | `BasicEdgeBuilder::buildEdges` | 🟢 | Line-by-line audited: fixed `can_cull_to_the_right=false`, build failure handling, and `<2 edges` reject match Rust |
| `BasicEdgeBuilder::build` | `BasicEdgeBuilder::build` | 🟢 | Line-by-line audited: clip/no-clip paths, finite-point guards, and line/quad/cubic push flow match Rust structure |
| `combine_vertical` | `combineVertical` | 🟢 | Line-by-line audited: vertical merge/split/cancel branch logic and winding handling match Rust |
| `edge_iter` | `pathIter` | 🟢 | Line-by-line audited: iterator state initialization and auto-close policy match Rust |
| `PathEdgeIter::next` | `PathEdgeIter::next` | 🟢 | Line-by-line audited: Move/Close handling and Line/Quad/Cubic emission sequencing match Rust |
| `PathEdgeIter::close_line` | `PathEdgeIter::closeLine` | 🟢 | Line-by-line audited: close-edge emission from last point to move point matches Rust |

### `third_party/tiny-skia/src/edge_clipper.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `EdgeClipper::new` | `EdgeClipper::EdgeClipper` | 🟢 | Line-by-line audited: clip/cull state initialization and empty clipped-edge storage match Rust |
| `EdgeClipper::clip_line` | `EdgeClipper::clipLine` | 🟢 | Line-by-line audited: delegates to line clipper, emits segment pairs, and returns `None`/`nullopt` on empty exactly as Rust |
| `EdgeClipper::push_line` | `ClippedEdges::pushLine` | 🟢 | Line-by-line audited: edge emission into clipped-edge container matches Rust push semantics |
| `EdgeClipper::push_vline` | `EdgeClipper::pushVerticalLine` | 🟢 | Line-by-line audited: reverse swap + vertical segment emission matches Rust |
| `EdgeClipper::clip_quad` | `EdgeClipper::clipQuad` | 🟢 | Line-by-line audited: bounds reject, Y-extrema then X-extrema chopping, and mono-quad dispatch match Rust flow |
| `EdgeClipper::clip_mono_quad` | `EdgeClipper::clipMonoQuad` | 🟢 | Line-by-line audited: Y-clipping, X-left/X-right branch handling, vertical-edge fallback, and inside-case push match Rust |
| `EdgeClipper::push_quad` | `EdgeClipper::pushQuad` | 🟢 | Line-by-line audited: reverse/forward control-point ordering matches Rust |
| `EdgeClipper::clip_cubic` | `EdgeClipper::clipCubic` | 🟢 | Line-by-line audited: vertical reject, large-bounds line fallback, extrema chopping, and mono-cubic dispatch match Rust |
| `EdgeClipper::clip_mono_cubic` | `EdgeClipper::clipMonoCubic` | 🟢 | Line-by-line audited: Y-clipping, X-left/X-right clipping with fallback chop, and vertical-edge emission match Rust |
| `EdgeClipper::push_cubic` | `EdgeClipper::pushCubic` | 🟢 | Line-by-line audited: reverse/forward cubic control-point ordering matches Rust |
| `EdgeClipperIter::new` | `EdgeClipperIter::EdgeClipperIter` | 🟢 | Line-by-line audited: path iterator/clip/cull initialization matches Rust constructor |
| `EdgeClipperIter::next` | `EdgeClipperIter::next` | 🟢 | Line-by-line audited: per-edge clipper creation and line/quad/cubic dispatch with first non-empty return match Rust |
| `quick_reject` | `quickReject` | 🟢 | Line-by-line audited: identical top/bottom non-overlap predicate |
| `sort_increasing_y` | `sortIncreasingY` | 🟢 | Line-by-line audited: reverse-copy vs direct-copy behavior and boolean reverse flag match Rust |
| `chop_quad_in_y` | `chopQuadInY` | 🟢 | Line-by-line audited: top/bottom clipping branches, chop fallback, and clamp corrections match Rust |
| `chop_mono_quad_at_x` | `path_geometry::chopMonoQuadAtX` | 🟢 | Line-by-line audited: mono-quad root-at-X clipping path matches Rust usage and fallback behavior |
| `chop_mono_quad_at_y` | `path_geometry::chopMonoQuadAtY` | 🟢 | Line-by-line audited: mono-quad root-at-Y clipping path matches Rust usage and fallback behavior |
| `too_big_for_reliable_float_math` | `tooBigForReliableFloatMath` | 🟢 | Line-by-line audited: same ±(1<<22) limit-based bounds reject |
| `chop_cubic_in_y` | `chopCubicInY` | 🟢 | Line-by-line audited: top/bottom clipping, re-chop guard, and post-chop clamping logic match Rust |
| `chop_mono_cubic_at_x` | `chopMonoCubicAtXFallback` | 🟢 | Line-by-line audited: try exact chop then closest-`t` fallback flow matches Rust intent |
| `chop_mono_cubic_at_y` | `chopMonoCubicAtYFallback` | 🟢 | Line-by-line audited: try exact chop then closest-`t` fallback flow matches Rust intent |
| `mono_cubic_closest_t` | `monoCubicClosestT` | 🟢 | Line-by-line audited: iterative closest-`t` search and convergence guard match Rust algorithm |

### `third_party/tiny-skia/path/src/path.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `FillRule::Winding` | `FillRule::Winding` | 🟢 | Line-by-line audited: enum discriminant parity matches Rust `painter.rs` (`Winding = 0`) |
| `FillRule::EvenOdd` | `FillRule::EvenOdd` | 🟢 | Line-by-line audited: enum discriminant parity matches Rust `painter.rs` (`EvenOdd = 1`) |
| `Path::bounds` | `Path::bounds` | 🟢 | Line-by-line audited: C++ now returns cached precomputed bounds (`bounds_`) instead of recomputing per call |

### `third_party/tiny-skia/src/path_geometry.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `chop_quad_at` | `chopQuadAt` | 🟢 | Line-by-line audited: Covered by `PathGeometryTest.ChopQuadAtInterpolatesPoints` |
| `chop_quad_at_x_extrema` | `chopQuadAtXExtrema` | 🟢 | Line-by-line audited: Covered by `PathGeometryTest.ChopQuadAtXExtremaMonotonicLeavesInputIntact` |
| `chop_quad_at_y_extrema` | `chopQuadAtYExtrema` | 🟢 | Line-by-line audited: Covered by `PathGeometryTest.ChopQuadAtYExtremaMonotonicLeavesInputIntact` and `ChopQuadAtYExtremaFlattensPeak` |
| `find_cubic_extrema` | `findCubicExtrema` | 🟢 | Line-by-line audited: Correct t-values in simple and dual-extrema curves |
| `chop_cubic_at` | `chopCubicAt` | 🟢 | Line-by-line audited: Covered by `PathGeometryTest.ChopCubicAtReturnsOriginalForEmptyTValues` and `ChopCubicAtSplitsAtOneCut` |
| `chop_cubic_at_x_extrema` | `chopCubicAtXExtrema` | 🟢 | Line-by-line audited: X-monotone output flattening checks |
| `chop_cubic_at_y_extrema` | `chopCubicAtYExtrema` | 🟢 | Line-by-line audited: Covered by `PathGeometryTest.ChopCubicAtYExtremaFlattensMonotonicYForCurve` |
| `chop_cubic_at_max_curvature` | `chopCubicAtMaxCurvature` | 🟢 | Line-by-line audited: Covered by `PathGeometryTest.ChopCubicAtMaxCurvatureFiltersEndpointsAndSplits` and `ChopCubicAtMaxCurvatureNoInteriorRootsReturnsOriginalCurve` |
| `chop_mono_cubic_at_x` | `chopMonoCubicAtX` | 🟢 | Line-by-line audited: Covered by `PathGeometryTest.ChopMonoCubicAtXFindsAndChopsAtIntercept` |
| `chop_mono_cubic_at_y` | `chopMonoCubicAtY` | 🟢 | Line-by-line audited: Covered by `PathGeometryTest.ChopMonoCubicAtYReturnsFalseWithoutRoots` |
| `chop_mono_quad_at_x` | `chopMonoQuadAtX` | 🟢 | Line-by-line audited: Covered by `PathGeometryTest.ChopMonoQuadAtXReturnsFalseWhenNoIntersection` |
| `chop_mono_quad_at_y` | `chopMonoQuadAtY` | 🟢 | Line-by-line audited: Covered by `PathGeometryTest.ChopMonoQuadAtYReportsTAndLeavesBounds` |

### `third_party/tiny-skia/src/fixed_point.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `fdot6::from_i32` | `fdot6::fromI32` | 🟢 | Line-by-line audited: same `i16`-fit assertion and `<< 6` conversion as Rust |
| `fdot6::from_f32` | `fdot6::fromF32` | 🟢 | Line-by-line audited: same `n * 64` truncating conversion behavior |
| `fdot6::floor` | `fdot6::floor` | 🟢 | Line-by-line audited: identical `>> 6` floor behavior |
| `fdot6::ceil` | `fdot6::ceil` | 🟢 | Line-by-line audited: identical `(n + 63) >> 6` |
| `fdot6::round` | `fdot6::round` | 🟢 | Line-by-line audited: identical `(n + 32) >> 6` |
| `fdot6::to_fdot16` | `fdot6::toFdot16` | 🟢 | Line-by-line audited: same overflow guard and `<< 10` shift |
| `fdot6::div` | `fdot6::div` | 🟢 | Line-by-line audited: same small-operand fast path and `fdot16::div/divide` fallback |
| `fdot6::can_convert_to_fdot16` | `fdot6::canConvertToFdot16` | 🟢 | Line-by-line audited: equivalent max-dot6 bound check |
| `fdot6::small_scale` | `fdot6::smallScale` | 🟢 | Line-by-line audited: identical `((value * dot6) >> 6)` scaling |
| `fdot8::from_fdot16` | `fdot8::fromFdot16` | 🟢 | Line-by-line audited: identical `(x + 0x80) >> 8` conversion |
| `fdot16::from_f32` | `fdot16::fromF32` | 🟢 | Line-by-line audited: saturating cast intent and range behavior match Rust `saturate_from` |
| `fdot16::floor_to_i32` | `fdot16::floorToI32` | 🟢 | Line-by-line audited: identical `>> 16` |
| `fdot16::ceil_to_i32` | `fdot16::ceilToI32` | 🟢 | Line-by-line audited: identical `(x + ONE - 1) >> 16` |
| `fdot16::round_to_i32` | `fdot16::roundToI32` | 🟢 | Line-by-line audited: identical `(x + HALF) >> 16` |
| `fdot16::mul` | `fdot16::mul` | 🟢 | Line-by-line audited: identical widened multiply then `>> 16` |
| `fdot16::div` | `fdot16::divide` | 🟢 | Line-by-line audited: identical widened divide with i32 bound clamp |
| `fdot16::fast_div` | `fdot16::fastDiv` | 🟢 | Line-by-line audited: identical fast `left_shift(a,16)/b` path with guards |

### `third_party/tiny-skia/src/color.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `ColorU8::from_rgba` | `ColorU8::fromRgba` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` and `ColorFromRgbaEdgeCases` |
| `ColorU8::red` | `ColorU8::red` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` component checks |
| `ColorU8::green` | `ColorU8::green` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` component checks |
| `ColorU8::blue` | `ColorU8::blue` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` component checks |
| `ColorU8::alpha` | `ColorU8::alpha` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` component checks |
| `ColorU8::is_opaque` | `ColorU8::isOpaque` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaEdgeCases` |
| `ColorU8::premultiply` | `ColorU8::premultiply` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorUPremultiplyPreservesAlphaAndClamp` |
| `PremultipliedColorU8::from_rgba` | `PremultipliedColorU8::fromRgba` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorOptionValidation`; matcher diagnostics upgraded with `Optional`/`nullopt` assertions |
| `PremultipliedColorU8::from_rgba_unchecked` | `PremultipliedColorU8::fromRgbaUnchecked` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorOptionValidation` unchecked path |
| `PremultipliedColorU8::red` | `PremultipliedColorU8::red` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorOptionValidation` component checks |
| `PremultipliedColorU8::green` | `PremultipliedColorU8::green` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorOptionValidation` component checks |
| `PremultipliedColorU8::blue` | `PremultipliedColorU8::blue` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorOptionValidation` component checks |
| `PremultipliedColorU8::alpha` | `PremultipliedColorU8::alpha` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorOptionValidation` component checks |
| `PremultipliedColorU8::is_opaque` | `PremultipliedColorU8::isOpaque` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorOptionValidation` |
| `PremultipliedColorU8::demultiply` | `PremultipliedColorU8::demultiply` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorDemultiply` |
| `Color::from_rgba_unchecked` | `Color::fromRgbaUnchecked` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` |
| `Color::from_rgba` | `Color::fromRgba` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` and `ColorFromRgbaEdgeCases`; matcher diagnostics upgraded with `Optional`/`nullopt` assertions (including invalid-input case) |
| `Color::from_rgba8` | `Color::fromRgba8` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorConversionToU8AndPremultiplyRoundTrip` |
| `Color::red` | `Color::red` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` component checks |
| `Color::green` | `Color::green` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` component checks |
| `Color::blue` | `Color::blue` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` component checks |
| `Color::alpha` | `Color::alpha` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` component checks |
| `Color::set_red` | `Color::setRed` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorSettersClampToRange` |
| `Color::set_green` | `Color::setGreen` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorSettersClampToRange` |
| `Color::set_blue` | `Color::setBlue` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorSettersClampToRange` |
| `Color::set_alpha` | `Color::setAlpha` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorSettersClampToRange` |
| `Color::apply_opacity` | `Color::applyOpacity` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaAndOpacity` |
| `Color::is_opaque` | `Color::isOpaque` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorFromRgbaEdgeCases` |
| `Color::premultiply` | `Color::premultiply` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorConversionToU8AndPremultiplyRoundTrip` |
| `Color::to_color_u8` | `Color::toColorU8` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorConversionToU8AndPremultiplyRoundTrip` |
| `PremultipliedColor::red` | `PremultipliedColor::red` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorDemultiply` component checks |
| `PremultipliedColor::green` | `PremultipliedColor::green` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorDemultiply` component checks |
| `PremultipliedColor::blue` | `PremultipliedColor::blue` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorDemultiply` component checks |
| `PremultipliedColor::alpha` | `PremultipliedColor::alpha` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorDemultiply` component checks |
| `PremultipliedColor::demultiply` | `PremultipliedColor::demultiply` | 🟢 | Line-by-line audited: Covered by `ColorTest.PremultipliedColorDemultiply` |
| `PremultipliedColor::to_color_u8` | `PremultipliedColor::toColorU8` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorConversionToU8AndPremultiplyRoundTrip` |
| `premultiply_u8` | `premultiplyU8` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorUPremultiplyU8UsesFixedPointMultiply` |
| `color_f32_to_u8` | `colorF32ToU8` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorConversionToU8AndPremultiplyRoundTrip` |
| `ColorSpace::expand_channel` | `expandChannel` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorSpaceTransforms` |
| `ColorSpace::expand_color` | `expandColor` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorSpaceTransforms` |
| `ColorSpace::compress_channel` | `compressChannel` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorSpaceTransforms` |
| `ColorSpace::expand_stage` | `expandStage` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorSpaceTransforms` stage mapping assertions; linear-path `nullopt` check now uses matcher assertion |
| `ColorSpace::expand_dest_stage` | `expandDestStage` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorSpaceTransforms` stage mapping assertions; linear-path `nullopt` check now uses matcher assertion |
| `ColorSpace::compress_stage` | `compressStage` | 🟢 | Line-by-line audited: Covered by `ColorTest.ColorSpaceTransforms` stage mapping assertions; linear-path `nullopt` check now uses matcher assertion |
| `pipeline::Stage::*` | `tiny_skia::pipeline::Stage` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineStageOrderingMatchesRustReference` |

### `third_party/tiny-skia/src/math.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `bound` | `bound` | 🟢 | Line-by-line audited: Rust `max.min(value).max(min)` matches C++ clamp ordering exactly |
| `left_shift` | `leftShift` | 🟢 | Line-by-line audited: Rust `((value as u32) << shift) as i32` matches C++ unsigned-shift then cast |
| `left_shift64` | `leftShift64` | 🟢 | Line-by-line audited: Rust `((value as u64) << shift) as i64` matches C++ unsigned-shift then cast |
| `approx_powf` | `approxPowf` | 🟢 | Line-by-line audited: constants, bit-casts, floor/round branches, and infinity/zero guards match Rust formula; matcher diagnostics upgraded in `MathTest.LeftShiftAndApproxPowf` |

### `third_party/tiny-skia/src/geom.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `ScreenIntRect::from_xywh` | `ScreenIntRect::fromXYWH` | 🟢 | Line-by-line audited: Covered by `GeomTest.ScreenIntRectFromXYWHRejectsInvalidDimensions` and `ScreenIntRectFromXYWHRejectsOverflowAndBounds`; invalid/overflow cases now use matcher-based `nullopt` assertions |
| `ScreenIntRect::from_xywh_safe` | `ScreenIntRect::fromXYWHSafe` | 🟢 | Line-by-line audited: Covered by `GeomTest.ScreenIntRectOperations` and constructor safety paths |
| `ScreenIntRect::x` | `ScreenIntRect::x` | 🟢 | Line-by-line audited: Coordinate round-trip checks |
| `ScreenIntRect::y` | `ScreenIntRect::y` | 🟢 | Line-by-line audited: Coordinate round-trip checks |
| `ScreenIntRect::width` | `ScreenIntRect::width` | 🟢 | Line-by-line audited: Width read/write boundary tests |
| `ScreenIntRect::height` | `ScreenIntRect::height` | 🟢 | Line-by-line audited: Height read/write boundary tests |
| `ScreenIntRect::width_safe` | `ScreenIntRect::widthSafe` | 🟢 | Line-by-line audited: Width safety checks |
| `ScreenIntRect::left` | `ScreenIntRect::left` | 🟢 | Line-by-line audited: Coordinate round-trip checks |
| `ScreenIntRect::top` | `ScreenIntRect::top` | 🟢 | Line-by-line audited: Coordinate round-trip checks |
| `ScreenIntRect::right` | `ScreenIntRect::right` | 🟢 | Line-by-line audited: Overflow guard checks |
| `ScreenIntRect::bottom` | `ScreenIntRect::bottom` | 🟢 | Line-by-line audited: Overflow guard checks |
| `ScreenIntRect::size` | `ScreenIntRect::size` | 🟢 | Line-by-line audited: Size extraction invariants |
| `ScreenIntRect::contains` | `ScreenIntRect::contains` | 🟢 | Line-by-line audited: Containment property checks |
| `ScreenIntRect::to_int_rect` | `ScreenIntRect::toIntRect` | 🟢 | Line-by-line audited: Struct conversion invariants |
| `ScreenIntRect::to_rect` | `ScreenIntRect::toRect` | 🟢 | Line-by-line audited: Float conversion parity |
| `IntSizeExt::to_screen_int_rect` | `IntSize::toScreenIntRect` | 🟢 | Line-by-line audited: Positioned rectangle smoke tests |
| `IntSize::from_wh` | `IntSize::fromWh` | 🟢 | Line-by-line audited: Covered by `GeomTest.IntSizeFromWhRejectsZero`; reject-path now uses matcher-based `nullopt` assertions |
| `IntRect::from_xywh` | `IntRect::fromXYWH` | 🟢 | Line-by-line audited: Covered by `GeomTest.IntRectFromXYWHRejectsInvalidInputs`; reject-path now uses matcher-based `nullopt` assertions |
| `IntRect::width` | `IntRect::width` | 🟢 | Width read/write checks |
| `IntRect::height` | `IntRect::height` | 🟢 | Height read/write checks |
| `IntRectExt::to_screen_int_rect` | `IntRect::toScreenIntRect` | 🟢 | Line-by-line audited: Conversion validity checks |
| `Rect::from_ltrb` | `Rect::fromLtrb` | 🟢 | Line-by-line audited: Covered by `GeomTest.RectFromLtrbRejectsInvalidBounds`; reject-path now uses matcher-based `nullopt` assertions |
| `int_rect_to_screen` | `intRectToScreen` | 🟢 | Line-by-line audited: Cross-type conversion checks |

### `third_party/tiny-skia/src/blitter.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `Mask::image` | `Mask::image` | 🟢 | Line-by-line audited: Rust `[u8;2] image` maps directly to C++ `std::array<uint8_t,2> image` |
| `Mask::bounds` | `Mask::bounds` | 🟢 | Line-by-line audited: Rust `ScreenIntRect bounds` maps directly to C++ `ScreenIntRect bounds` |
| `Mask::row_bytes` | `Mask::rowBytes` | 🟢 | Line-by-line audited: Rust `u32 row_bytes` maps to C++ `uint32_t rowBytes` (name-only casing change) |
| `Blitter::blit_h` | `Blitter::blitH` | 🟢 | Line-by-line audited: default implementation is unreachable/abort in both Rust and C++; call-sequence diagnostics upgraded in `BlitterTest.OverridableMethodsReceiveCalls` |
| `Blitter::blit_anti_h` | `Blitter::blitAntiH` | 🟢 | Line-by-line audited: default implementation is unreachable/abort in both Rust and C++ |
| `Blitter::blit_v` | `Blitter::blitV` | 🟢 | Line-by-line audited: default implementation is unreachable/abort in both Rust and C++ |
| `Blitter::blit_anti_h2` | `Blitter::blitAntiH2` | 🟢 | Line-by-line audited: default implementation is unreachable/abort in both Rust and C++ |
| `Blitter::blit_anti_v2` | `Blitter::blitAntiV2` | 🟢 | Line-by-line audited: default implementation is unreachable/abort in both Rust and C++ |
| `Blitter::blit_rect` | `Blitter::blitRect` | 🟢 | Line-by-line audited: default implementation is unreachable/abort in both Rust and C++ |
| `Blitter::blit_mask` | `Blitter::blitMask` | 🟢 | Line-by-line audited: default implementation is unreachable/abort in both Rust and C++ |

### `third_party/tiny-skia/src/edge.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `Edge::as_line` | `Edge::asLine` | 🟢 | Line-by-line audited: enum/variant dispatch to embedded `LineEdge` matches Rust `match` branches |
| `Edge::as_line_mut` | `Edge::asLine` | 🟢 | Line-by-line audited: mutable delegate dispatch matches Rust `match` branches |
| `LineEdge::new` | `LineEdge::create` | 🟢 | Line-by-line audited: scale conversion, winding swap, zero-height reject, slope/dy setup, and field writes match Rust; matcher diagnostics upgraded in `EdgeLineTest.LineEdgeCreateAssignsWindingAndBounds` and remaining optional assertions in `EdgeLineTest` |
| `LineEdge::is_vertical` | `LineEdge::isVertical` | 🟢 | Line-by-line audited: `dx == 0` parity |
| `LineEdge::update` | `LineEdge::update` | 🟢 | Line-by-line audited: fixed-point downshift, zero-height reject, slope recompute, and edge state updates match Rust |
| `QuadraticEdge::new` | `QuadraticEdge::create` | 🟢 | Line-by-line audited: constructor delegates to internal setup + first `update()` gate, same as Rust; matcher diagnostics upgraded in `EdgeQuadraticTest.QuadraticEdgeCreateBasic` |
| `QuadraticEdge::new2` | `QuadraticEdge::create` | 🟢 | Line-by-line audited via internal `makeQuadraticEdge`: coefficient/shift derivation and state initialization match Rust `new2` |
| `QuadraticEdge::update` | `QuadraticEdge::update` | 🟢 | Line-by-line audited: segment stepping loop, success break conditions, and persisted state updates match Rust |
| `CubicEdge::new` | `CubicEdge::create` | 🟢 | Line-by-line audited: constructor delegates to internal setup + first `update()` gate, same as Rust; matcher diagnostics upgraded in `EdgeCubicTest.CubicEdgeCreateBasic` |
| `CubicEdge::new2` | `CubicEdge::create` | 🟢 | Line-by-line audited via internal `makeCubicEdge`: delta/shift math, coefficient setup, and initial state match Rust `new2` |
| `CubicEdge::update` | `CubicEdge::update` | 🟢 | Line-by-line audited: forward-difference stepping, `newY` monotonic clamp, and loop termination semantics match Rust |

### `third_party/tiny-skia/src/line_clipper.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `MAX_POINTS` | `kLineClipperMaxPoints` | 🟢 | Line-by-line audited: Rust `MAX_POINTS: usize = 4` matches C++ `kLineClipperMaxPoints = 4` |
| `clip` | `clip` | 🟢 | Line-by-line audited: Y-intersection math now uses original `src` for `sectWithHorizontal`, and winding orientation follows `src` X-order like Rust; matcher diagnostics upgraded in `LineClipperTest.ClipClampsBothSidesOnSkewLine` |
| `intersect` | `intersect` | 🟢 | Line-by-line audited: clipped endpoint Y values now use `sectWithVertical(src, ...)` parity with Rust; matcher diagnostics upgraded in `LineClipperTest.IntersectClipsAndReturnsTrueForPartiallyOverlapping` |

### `third_party/tiny-skia/src/mask.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `MaskType::Alpha` | `MaskType::Alpha` | 🟢 | Line-by-line audited: enum discriminant parity (`Alpha = 0`) |
| `MaskType::Luminance` | `MaskType::Luminance` | 🟢 | Line-by-line audited: enum discriminant parity (`Luminance = 1`) |
| `Mask::new` | `Mask::fromSize` | 🟢 | Line-by-line audited: size validation + zero-initialized allocation match Rust semantics; matcher diagnostics upgraded in `MaskTest.FromSizeRejectsZeroDimensions` |
| `Mask::from_vec` | `Mask::fromVec` | 🟢 | Line-by-line audited: exact size check (`width * height`) and ownership transfer match; matcher diagnostics upgraded in `MaskTest.FromVecRequiresExactSize` |
| `Mask::width` | `Mask::width` | 🟢 | Line-by-line audited: returns stored `IntSize::width()` |
| `Mask::height` | `Mask::height` | 🟢 | Line-by-line audited: returns stored `IntSize::height()` |
| `Mask::size` | `Mask::size` | 🟢 | Line-by-line audited: stored size passthrough |
| `Mask::data` | `Mask::data` | 🟢 | Line-by-line audited: immutable backing buffer view |
| `Mask::data_mut` | `Mask::dataMut` | 🟢 | Line-by-line audited: mutable backing buffer view |
| `Mask::take` | `Mask::take` | 🟢 | Line-by-line audited: ownership move-out; C++ resets stored size to default-empty state |
| `Mask::from_pixmap` | `Mask::fromPixmap` | 🟢 | Line-by-line audited: alpha copy and luminance path (demultiply + luma + alpha + ceil clamp) match Rust |
| `Mask::as_submask` | `Mask::asSubmask` | 🟢 | Line-by-line audited: full-size borrowed mask view with real-width parity |
| `Mask::submask` | `Mask::submask` | 🟢 | Line-by-line audited: intersects requested rect with mask bounds and returns offset view; matcher diagnostics upgraded in `MaskTest.SubmaskComputesIntersectedViewAndOffset` |
| `Mask::as_subpixmap` | `Mask::asSubpixmap` | 🟢 | Line-by-line audited: mutable full-size mask view parity |
| `Mask::subpixmap` | `Mask::subpixmap` | 🟢 | Line-by-line audited: mutable intersected subview + row-offset parity; matcher diagnostics upgraded in `MaskTest.SubpixmapComputesIntersectedMutableView` |
| `Mask::fill_path` | `Mask::fillPath` | 🟢 | Line-by-line audited: implementation in `MaskOps.cpp` matches Rust fill flow with transform handling and tiled fallback; covered by `MaskTest.FillPathDrawsOntoMask` and `MaskTest.FillPathWithTransformOffsetsPath` |
| `Mask::intersect_path` | `Mask::intersectPath` | 🟢 | Line-by-line audited: builds temporary mask via `fillPath` then multiplies into destination mask like Rust; covered by `MaskTest.IntersectPathMultipliesMasks` |

### `third_party/tiny-skia/src/pixmap.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `BYTES_PER_PIXEL` | `kBytesPerPixel` | 🟢 | Line-by-line audited: `4` bytes per RGBA pixel |
| `Pixmap::new` | `Pixmap::fromSize` | 🟢 | Line-by-line audited: size validation + zero-initialized allocation parity; width cap matches Rust; matcher diagnostics upgraded in `PixmapTest.FromSizeRejectsZeroAndTooWideInputs` |
| `Pixmap::from_vec` | `Pixmap::fromVec` | 🟢 | Line-by-line audited: exact byte-length validation and ownership transfer parity; matcher diagnostics upgraded in `PixmapTest.FromVecValidatesExactByteLength` |
| `Pixmap::as_ref` | `Pixmap::asRef` | 🟢 | Line-by-line audited: immutable borrowed view over same storage |
| `Pixmap::as_mut` | `Pixmap::asMut` | 🟢 | Line-by-line audited: mutable borrowed view over same storage |
| `Pixmap::width` | `Pixmap::width` | 🟢 | Line-by-line audited: stored size passthrough |
| `Pixmap::height` | `Pixmap::height` | 🟢 | Line-by-line audited: stored size passthrough |
| `Pixmap::size` | `Pixmap::size` | 🟢 | Line-by-line audited: stored size passthrough |
| `Pixmap::data` | `Pixmap::data` | 🟢 | Line-by-line audited: immutable RGBA byte span |
| `Pixmap::data_mut` | `Pixmap::dataMut` | 🟢 | Line-by-line audited: mutable RGBA byte span |
| `Pixmap::pixel` | `Pixmap::pixel` | 🟢 | Line-by-line audited: checked coordinate-to-index and optional return parity |
| `Pixmap::pixels` | `Pixmap::pixels` | 🟢 | Line-by-line audited: byte storage reinterpreted as premultiplied RGBA pixels |
| `Pixmap::pixels_mut` | `Pixmap::pixelsMut` | 🟢 | Line-by-line audited: mutable pixel reinterpret view parity |
| `Pixmap::take` | `Pixmap::take` | 🟢 | Line-by-line audited: ownership move-out; C++ resets stored size to default-empty state |
| `PixmapRef::from_bytes` | `PixmapRef::fromBytes` | 🟢 | Line-by-line audited: validates non-zero size + minimum data length and width cap; matcher diagnostics upgraded in `PixmapTest.PixmapRefFromBytesAndPixelAccessMatchRgbaPacking` |
| `PixmapRef::pixel` | `PixmapRef::pixel` | 🟢 | Line-by-line audited: checked index computation parity |
| `PixmapRef::pixels` | `PixmapRef::pixels` | 🟢 | Line-by-line audited: immutable pixel reinterpret view |
| `PixmapMut` APIs | `PixmapMut` APIs | 🟢 | Line-by-line audited: mutable borrowed view shape mapped for Rust parity |
| `SubPixmapMut` struct | `SubPixmapMut` struct | 🟢 | Line-by-line audited: stride + data pointer scaffold mapped for pipeline integration |
| `Pixmap::fill` | `Pixmap::fill` | 🟢 | Line-by-line audited: premultiply once and broadcast fill across full pixmap storage |
| `Pixmap::take_demultiplied` | `Pixmap::takeDemultiplied` | 🟢 | Line-by-line audited: per-pixel demultiply conversion before ownership move-out |
| `Pixmap::clone_rect` | `Pixmap::cloneRect` | 🟢 | Line-by-line audited: contained-rect check + row-wise copy into new pixmap |
| `PixmapRef::clone_rect` | `PixmapRef::cloneRect` | 🟢 | Line-by-line audited: borrowed-view rect clone path mirrors Rust ownership semantics |
| `PixmapRef::from_bytes_mut` | `PixmapMut::fromBytes` | 🟢 | Line-by-line audited: mutable-byte constructor validates size and minimum byte length; matcher diagnostics upgraded in `PixmapTest.PixmapMutFromBytesAndSubpixmapProvideMutableSubview` |
| `PixmapMut::as_subpixmap` | `PixmapMut::asSubpixmap` | 🟢 | Line-by-line audited: full mutable subview with real-width stride parity |
| `PixmapMut::subpixmap` | `PixmapMut::subpixmap` | 🟢 | Line-by-line audited: intersected mutable subview + byte-offset parity |

### `third_party/tiny-skia/path/src/stroker.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `PathStroker::new` | `PathStroker::PathStroker` | 🟢 | Line-by-line audited: Constructor initializes stroke parameters, cap/join procs matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::stroke` | `PathStroker::stroke` | 🟢 | Line-by-line audited: Main entry iterates path segments, dispatches to line/quad/cubic/close matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::stroke_line` | `PathStroker::strokeLine` | 🟢 | Line-by-line audited: Degenerate + normal line handling matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::stroke_quad` | `PathStroker::strokeQuad` | 🟢 | Line-by-line audited: Recursive quad subdivision with linearity check matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::stroke_cubic` | `PathStroker::strokeCubic` | 🟢 | Line-by-line audited: Recursive cubic subdivision, cusp/inflection handling matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::stroke_close` | `PathStroker::strokeClose` | 🟢 | Line-by-line audited: Close contour with join to start matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::finish` | `PathStroker::finish` | 🟢 | Line-by-line audited: Finalizes inner/outer paths, returns finished Path matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::cap` | `PathStroker::cap` | 🟢 | Line-by-line audited: Delegates to CapProc (butt/round/square) matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::join` | `PathStroker::join` | 🟢 | Line-by-line audited: Delegates to JoinProc (miter/round/bevel) matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::pre_join_to` | `PathStroker::preJoinTo` | 🟢 | Line-by-line audited: Unit normal computation for next segment matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::post_join_to` | `PathStroker::postJoinTo` | 🟢 | Line-by-line audited: Join application at segment boundary matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::init_quad` | `PathStroker::initQuad` | 🟢 | Line-by-line audited: QuadConstruct setup for subdivision matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::init_cubic` | `PathStroker::initCubic` | 🟢 | Line-by-line audited: QuadConstruct setup for cubic subdivision matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::quad_stroke` | `PathStroker::quadStroke` | 🟢 | Line-by-line audited: Perpendicular rays for quad segment matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::cubic_stroke` | `PathStroker::cubicStroke` | 🟢 | Line-by-line audited: Perpendicular rays for cubic segment matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::check_quad_linear` | `checkQuadLinear` | 🟢 | Line-by-line audited: Quad stroke linearity test matches Rust. Validated in audit 2026-03-02. |
| `PathStroker::check_cubic_linear` | `checkCubicLinear` | 🟢 | Line-by-line audited: Cubic stroke linearity test matches Rust. Validated in audit 2026-03-02. |
| `butt_capper` | `buttCapper` | 🟢 | Line-by-line audited: Flat cap matches Rust. Validated in audit 2026-03-02. |
| `round_capper` | `roundCapper` | 🟢 | Line-by-line audited: Semicircle cap via conic arcs matches Rust. Validated in audit 2026-03-02. |
| `square_capper` | `squareCapper` | 🟢 | Line-by-line audited: Endpoint extension by half stroke width matches Rust. Validated in audit 2026-03-02. |
| `bevel_joiner` | `bevelJoiner` | 🟢 | Line-by-line audited: Straight line bevel between offset points matches Rust. Validated in audit 2026-03-02. |
| `round_joiner` | `roundJoiner` | 🟢 | Line-by-line audited: Circular arc join via conic matches Rust. Validated in audit 2026-03-02. |
| `miter_joiner` | `miterJoiner` | 🟢 | Line-by-line audited: Miter join with limit fallback to bevel matches Rust. Validated in audit 2026-03-02. |
| `miter_clip_joiner` | `miterClipJoiner` | 🟢 | Line-by-line audited: Miter join with clip at miter limit matches Rust. Validated in audit 2026-03-02. |
| `Path::stroke` | `Path::stroke` | 🟢 | Line-by-line audited: Entry point creating PathStroker matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/path/src/dash.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `StrokeDash` struct | `StrokeDash` (Stroke.h) | 🟢 | Line-by-line audited: Dash array + offset struct matches Rust. Validated in audit 2026-03-02. |
| `StrokeDash::new` | `StrokeDash::create` | 🟢 | Line-by-line audited: Even entry count, positive, finite validation matches Rust. Validated in audit 2026-03-02. |
| `ContourMeasure` struct | `ContourMeasure` (Dash.h) | 🟢 | Line-by-line audited: Segment storage with distance/t-value pairs matches Rust. Validated in audit 2026-03-02. |
| `ContourMeasureIter` struct | `ContourMeasureIter` (Dash.h) | 🟢 | Line-by-line audited: Contour iteration with cumulative lengths matches Rust. Validated in audit 2026-03-02. |
| `ContourMeasureIter::next` | `ContourMeasureIter::next` | 🟢 | Line-by-line audited: Next contour ContourMeasure return matches Rust. Validated in audit 2026-03-02. |
| `ContourMeasure::length` | `ContourMeasure::length` | 🟢 | Line-by-line audited: Total arc length accessor matches Rust. Validated in audit 2026-03-02. |
| `ContourMeasure::segment_to` | `ContourMeasure::segmentTo` | 🟢 | Line-by-line audited: Sub-path extraction between distance values matches Rust. Validated in audit 2026-03-02. |
| `Path::dash` | `Path::dash` | 🟢 | Line-by-line audited: Dash pattern application via ContourMeasureIter matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/path/src/path_builder.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `PathBuilder::new` | `PathBuilder::PathBuilder` | 🟢 | Line-by-line audited: Default constructor matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::with_capacity` | `PathBuilder::PathBuilder(verbs, points)` | 🟢 | Line-by-line audited: Reserve capacity variant matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::move_to` | `PathBuilder::moveTo` | 🟢 | Line-by-line audited: New contour start matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::line_to` | `PathBuilder::lineTo` | 🟢 | Line-by-line audited: Line segment append matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::quad_to` | `PathBuilder::quadTo` | 🟢 | Line-by-line audited: Quadratic bezier append matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::cubic_to` | `PathBuilder::cubicTo` | 🟢 | Line-by-line audited: Cubic bezier append matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::conic_to` | `PathBuilder::conicTo` | 🟢 | Line-by-line audited: Conic-to-quad conversion via autoConicToQuads matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::close` | `PathBuilder::close` | 🟢 | Line-by-line audited: Contour close matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::push_rect` | `PathBuilder::pushRect` | 🟢 | Line-by-line audited: Rectangle contour matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::push_oval` | `PathBuilder::pushOval` | 🟢 | Line-by-line audited: Oval contour via conics matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::push_circle` | `PathBuilder::pushCircle` | 🟢 | Line-by-line audited: Circle via pushOval matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::push_path` | `PathBuilder::pushPath` | 🟢 | Line-by-line audited: Existing Path append matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::reverse_path_to` | `PathBuilder::reversePathTo` | 🟢 | Line-by-line audited: Reverse path append matches Rust. Validated in audit 2026-03-02. |
| `PathBuilder::finish` | `PathBuilder::finish` | 🟢 | Line-by-line audited: Optional Path return with builder reset matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/path/src/scalar.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `NormalizedF32` | `NormalizedF32` (Color.h) | 🟢 | Line-by-line audited: Float in [0,1] with ZERO/ONE/create() matches Rust. Validated in audit 2026-03-02. |
| `NormalizedF32Exclusive` | `NormalizedF32Exclusive` (Scalar.h) | 🟢 | Line-by-line audited: Float in (0,1) exclusive with HALF constant matches Rust. Validated in audit 2026-03-02. |
| `NonZeroPositiveF32` | `NonZeroPositiveF32` (Scalar.h) | 🟢 | Line-by-line audited: Positive finite float matches Rust. Validated in audit 2026-03-02. |
| `FiniteF32` | `FiniteF32` (Scalar.h) | 🟢 | Line-by-line audited: Finite float matches Rust. Validated in audit 2026-03-02. |

### `third_party/tiny-skia/path/src/path_geometry.rs` (additions)
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `chop_quad_at` (NormalizedF32Exclusive) | `chopQuadAtT` | 🟢 | Line-by-line audited: Single-t quad chop variant matches Rust. Validated in audit 2026-03-02. |
| `chop_cubic_at2` (NormalizedF32Exclusive) | `chopCubicAt2` | 🟢 | Line-by-line audited: Two-t cubic chop variant matches Rust. Validated in audit 2026-03-02. |
| `eval_quad_at` | `evalQuadAt` | 🟢 | Line-by-line audited: Quad evaluation at parameter t matches Rust. Validated in audit 2026-03-02. |
| `eval_quad_tangent_at` | `evalQuadTangentAt` | 🟢 | Line-by-line audited: Quad tangent evaluation at t matches Rust. Validated in audit 2026-03-02. |
| `eval_cubic_pos_at` | `evalCubicPosAt` | 🟢 | Line-by-line audited: Cubic position evaluation at t matches Rust. Validated in audit 2026-03-02. |
| `eval_cubic_tangent_at` | `evalCubicTangentAt` | 🟢 | Line-by-line audited: Cubic tangent evaluation at t matches Rust. Validated in audit 2026-03-02. |
| `find_quad_max_curvature` | `findQuadMaxCurvature` | 🟢 | Line-by-line audited: Maximum curvature t computation matches Rust. Validated in audit 2026-03-02. |
| `find_quad_extrema` | `findQuadExtrema` | 🟢 | Line-by-line audited: Quad extremum t computation matches Rust. Validated in audit 2026-03-02. |
| `find_cubic_inflections` | `findCubicInflections` | 🟢 | Line-by-line audited: Inflection t-values computation matches Rust. Validated in audit 2026-03-02. |
| `find_cubic_max_curvature_ts` | `findCubicMaxCurvatureTs` | 🟢 | Line-by-line audited: Max curvature t-values computation matches Rust. Validated in audit 2026-03-02. |
| `find_cubic_cusp` | `findCubicCusp` | 🟢 | Line-by-line audited: Cusp t-value detection matches Rust. Validated in audit 2026-03-02. |
| `find_unit_quad_roots` | `findUnitQuadRoots` | 🟢 | Line-by-line audited: Quadratic roots in [0,1] matches Rust. Validated in audit 2026-03-02. |
| `Conic::build_unit_arc` | `Conic::buildUnitArc` | 🟢 | Line-by-line audited: Unit arc conic construction matches Rust. Validated in audit 2026-03-02. |
| `Conic::chop_into_quads_pow2` | `Conic::chopIntoQuadsPow2` | 🟢 | Line-by-line audited: Conic-to-quadratic approximation matches Rust. Validated in audit 2026-03-02. |
| `auto_conic_to_quads` | `autoConicToQuads` | 🟢 | Line-by-line audited: Power-of-2 subdivision for conic-to-quad matches Rust. Validated in audit 2026-03-02. |
