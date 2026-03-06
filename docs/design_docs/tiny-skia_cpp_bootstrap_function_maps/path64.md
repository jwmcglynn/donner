# Function Mapping Tables

Legend: `☐` Not started, `🧩` Stub only, `🟡` Implemented/tested (Rust completeness not yet vetted), `🟢` Rust-completeness vetted (line-by-line against Rust), `✅` Verified parity sign-off (user-requested audit complete), `⏸` Blocked.

### `third_party/tiny-skia/src/path64/cubic64.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `Cubic64Pair` | `Cubic64Pair` | 🟢 | Line-by-line audited: Struct layout coverage |
| `Cubic64::new` | `Cubic64::create` | 🟢 | Line-by-line audited: Point copy semantics |
| `Cubic64::as_f64_slice` | `Cubic64::asF64Slice` | 🟢 | Line-by-line audited: Flattened coordinate order |
| `Cubic64::point_at_t` | `Cubic64::pointAtT` | 🟢 | Line-by-line audited: Endpoint fast-path and midpoint checks; matcher diagnostics upgraded in `Cubic64Test.PointAtTEvaluatesEndpointsAndMidpoint` |
| `Cubic64::search_roots` | `Cubic64::searchRoots` | 🟢 | Line-by-line audited: Segmented binary-search behavior |
| `find_inflections` | `Cubic64::findInflections` | 🟢 | Line-by-line audited: Subdivision invariants |
| `Cubic64::chop_at` | `Cubic64::chopAt` | 🟢 | Line-by-line audited: Midpoint split control points; matcher diagnostics upgraded in `Cubic64Test.ChopAtUsesSpecialCaseAtMidpoint` |
| `coefficients` | `coefficients` | 🟢 | Line-by-line audited: Coefficient transform parity |
| `roots_valid_t` | `rootsValidT` | 🟢 | Line-by-line audited: Endpoint clamp and dedupe behavior |
| `roots_real` | `rootsReal` | 🟢 | Line-by-line audited: Real-root regime parity |
| `find_extrema` | `findExtrema` | 🟢 | Line-by-line audited: Derivative-division parity |
| `interp_cubic_coords_x` | `interpCubicCoordsX` | 🟢 | Line-by-line audited: Coord decomposition identity |
| `interp_cubic_coords_y` | `interpCubicCoordsY` | 🟢 | Line-by-line audited: Coord decomposition identity |

### `third_party/tiny-skia/src/path64/line_cubic_intersections.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `horizontal_intersect` | `horizontalIntersect` | 🟢 | Line-by-line audited: Root set and fallback behavior; matcher diagnostics upgraded in `LineCubicIntersectionsTest.HorizontalIntersectFindsExpectedSingleRoot` |
| `vertical_intersect` | `verticalIntersect` | 🟢 | Line-by-line audited: Root set and fallback behavior |

### `third_party/tiny-skia/src/path64/mod.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `DBL_EPSILON_ERR` | `kDblEpsilonErr` | 🟢 | Line-by-line audited: Constant value equivalence |
| `FLT_EPSILON_HALF` | `kFloatEpsilonHalf` | 🟢 | Line-by-line audited: Constant value equivalence |
| `FLT_EPSILON_CUBED` | `kFloatEpsilonCubed` | 🟢 | Line-by-line audited: Constant value equivalence |
| `FLT_EPSILON_INVERSE` | `kFloatEpsilonInverse` | 🟢 | Line-by-line audited: Constant value equivalence |
| `Scalar64::bound` | `bound` | 🟢 | Line-by-line audited: Clamp boundary behavior and NaN/inf expectations |
| `Scalar64::between` | `between` | 🟢 | Line-by-line audited: Range test ordering invariants |
| `Scalar64::precisely_zero` | `preciselyZero` | 🟢 | Line-by-line audited: Zero threshold parity |
| `Scalar64::approximately_zero_or_more` | `approximatelyZeroOrMore` | 🟢 | Line-by-line audited: Boundary threshold parity |
| `Scalar64::approximately_one_or_less` | `approximatelyOneOrLess` | 🟢 | Line-by-line audited: Boundary threshold parity |
| `Scalar64::approximately_zero` | `approximatelyZero` | 🟢 | Line-by-line audited: Magnitude threshold parity |
| `Scalar64::approximately_zero_inverse` | `approximatelyZeroInverse` | 🟢 | Line-by-line audited: Magnitude threshold parity |
| `Scalar64::approximately_zero_cubed` | `approximatelyZeroCubed` | 🟢 | Line-by-line audited: Magnitude threshold parity |
| `Scalar64::approximately_zero_half` | `approximatelyZeroHalf` | 🟢 | Line-by-line audited: Magnitude threshold parity |
| `Scalar64::approximately_zero_when_compared_to` | `approximatelyZeroWhenComparedTo` | 🟢 | Line-by-line audited: Relative threshold parity |
| `Scalar64::approximately_equal` | `approximatelyEqual` | 🟢 | Line-by-line audited: Signed and mirrored comparisons |
| `Scalar64::approximately_equal_half` | `approximatelyEqualHalf` | 🟢 | Signed threshold parity |
| `Scalar64::almost_dequal_ulps` | `almostDequalUlps` | 🟢 | Line-by-line audited: ULP-like bound parity |
| `cube_root` | `cubeRoot` | 🟢 | Line-by-line audited: Positive/negative/zero parity |
| `cbrt_5d` | `cbrt5d` | 🟢 | Line-by-line audited: Bit-level seed parity smoke |
| `halley_cbrt3d` | `halleyCbrt3d` | 🟢 | Line-by-line audited: Root convergence parity |
| `cbrta_halleyd` | `cbrtaHalleyd` | 🟢 | Line-by-line audited: Iteration formula parity |
| `interp` | `interp` | 🟢 | Line-by-line audited: Linear interpolation identity |

### `third_party/tiny-skia/src/path64/point64.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `Point64::from_xy` | `Point64::fromXy` | 🟢 | `x/y` initialization identity checks |
| `Point64::from_point` | `Point64::fromPoint` | 🟢 | Line-by-line audited: Float round-trip with `Point`; matcher diagnostics upgraded in `Point64Test.FromPointAndToPointRoundTrip` |
| `Point64::zero` | `Point64::zero` | 🟢 | Line-by-line audited: Zero initialization checks |
| `Point64::to_point` | `Point64::toPoint` | 🟢 | Line-by-line audited: Float conversion identity checks |
| `Point64::axis_coord` | `Point64::axisCoord` | 🟢 | Line-by-line audited: X/Y axis extraction checks |

### `third_party/tiny-skia/src/path64/quad64.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `push_valid_ts` | `pushValidTs` | 🟢 | Line-by-line audited: Dedupe, range filtering, and clamping assertions; matcher diagnostics upgraded in `Quad64Test.PushValidTsFiltersAndDedups` |
| `roots_valid_t` | `rootsValidT` | 🟢 | Line-by-line audited: Unit interval root filtering checks |
| `roots_real` | `rootsReal` | 🟢 | Line-by-line audited: Monic and mirrored root sets checks; matcher diagnostics upgraded in `Quad64Test.RootsRealFromMonicQuadratic` |

Add one section per file as soon as implementation begins.
