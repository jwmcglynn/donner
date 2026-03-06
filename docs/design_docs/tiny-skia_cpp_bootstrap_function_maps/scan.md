# Function Mapping Tables

Legend: `☐` Not started, `🧩` Stub only, `🟡` Implemented/tested (Rust completeness not yet vetted), `🟢` Rust-completeness vetted (line-by-line against Rust), `✅` Verified parity sign-off (user-requested audit complete), `⏸` Blocked.

### `third_party/tiny-skia/src/scan/path.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `fill_path` | `scan::fillPath` | 🟢 | Line-by-line audited: Empty-path no-op and rectangle fill span smoke tests |
| `fill_path_impl` | `scan::fillPathImpl` | 🟢 | Line-by-line audited: Clipping-disabled and culling paths |
| `conservative_round_to_int` | `conservativeRoundToInt` | 🟢 | Line-by-line audited: Conservative rounding bounds checks |
| `round_down_to_int` | `roundDownToInt` | 🟢 | Line-by-line audited: Biased down-round behavior |
| `round_up_to_int` | `roundUpToInt` | 🟢 | Line-by-line audited: Biased up-round behavior |
| `walk_edges` | `walkEdges` | 🟢 | Line-by-line audited: Horizontal span sequencing checks |
| `remove_edge` | `removeEdge` | 🟢 | Line-by-line audited: Linked-list unlink behavior |
| `backward_insert_edge_based_on_x` | `backwardInsertEdgeBasedOnX` | 🟢 | Line-by-line audited: Ordered insertion with backward scan |
| `insert_edge_after` | `insertEdgeAfter` | 🟢 | Line-by-line audited: Doubly-linked splice behavior |
| `backward_insert_start` | `backwardInsertStart` | 🟢 | Line-by-line audited: Backward start probe invariants |
| `insert_new_edges` | `insertNewEdges` | 🟢 | Line-by-line audited: Y-gated new-edge insertion behavior |

### `third_party/tiny-skia/src/scan/path_aa.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `fill_path` | `scan::path_aa::fillPath` | 🟢 | Line-by-line audited: Empty path no-op and rectangle AA span smoke checks |
| `fill_path_impl` | `scan::path_aa::fillPathImpl` | 🟢 | Line-by-line audited: Fallback-to-non-AA and clipping bounds coverage |
| `rect_overflows_short_shift` | `rectOverflowsShortShift` | 🟢 | Line-by-line audited: Overflow and clamp behavior on large bounds |
| `coverage_to_partial_alpha` | `coverageToPartialAlpha` | 🟢 | Line-by-line audited: Alpha quantization checks at boundaries |

### `third_party/tiny-skia/src/scan/hairline.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `stroke_path` | `scan::strokePath` | 🟢 | Line-by-line audited: Clip handling and span sequence smoke checks |
| `stroke_path_impl` | `scan::strokePathImpl` | 🟢 | Line-by-line audited: Segment dispatch smoke checks |
| `LineCap::Butt` | `LineCap::Butt` | 🟢 | Line-by-line audited: Enum value parity |
| `LineCap::Round` | `LineCap::Round` | 🟢 | Line-by-line audited: Enum value parity |
| `LineCap::Square` | `LineCap::Square` | 🟢 | Line-by-line audited: Enum value parity |
| `hair_line_rgn` | `hairLineRgn` | 🟢 | Line-by-line audited: Horizontal/vertical clipping and traversal |
| `extend_pts` | `extendPts` | 🟢 | Line-by-line audited: Segment-end cap extension coverage |
| `hair_quad` | `hairQuad` | 🟢 | Line-by-line audited: Colinear control points reduce to line-equivalent spans |
| `hair_quad2` | `hairQuad2` | 🟢 | Line-by-line audited: Colinear control-point path uses direct line span sequence |
| `hair_cubic` | `hairCubic` | 🟢 | Line-by-line audited: Colinear cubic reduces to no-split line-equivalent spans |
| `hair_cubic2` | `hairCubic2` | 🟢 | Line-by-line audited: Colinear cubic no-split spans and path-level regression checks |

### `third_party/tiny-skia/src/scan/hairline_aa.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `fill_rect` | `fillRect` | 🟢 | Line-by-line audited: Rectangle intersection fallback and fixed-rect dispatch |
| `fill_fixed_rect` | `fillFixedRect` | 🟢 | Line-by-line audited: Fixed-point conversion to 8-bit span coordinates |
| `fill_dot8` | `fillDot8` | 🟢 | Line-by-line audited: Boundary and fractional-edge coverage |
| `do_scanline` | `doScanline` | 🟢 | Line-by-line audited: 1-pixel, single-edge, and fractional scanline checks |
| `call_hline_blitter` | `callHlineBlitter` | 🟢 | Line-by-line audited: Chunked call sequence and width clamping checks |
| `stroke_path` | `scan::hairline_aa::strokePath` | 🟢 | Line-by-line audited: Clipping/no-clip anti-span behavior |
| `anti_hair_line_rgn` | `antiHairLineRgn` | 🟢 | Line-by-line audited: Line pre-clip and partial-clip sub-rect dispatch checks |
| `do_anti_hairline` | `doAntiHairline` | 🟢 | Line-by-line audited: Dominant-axis and clipping-overlap branch checks |
| `bad_int` | `badInt` | 🟢 | Line-by-line audited: Integer edge sentinel parity |
| `any_bad_ints` | `anyBadInts` | 🟢 | Line-by-line audited: High-bit-flag checks |
| `contribution_64` | `contribution64` | 🟢 | Line-by-line audited: Fractional contribution extraction |
| `HLineAntiHairBlitter` | `HLineAntiHairBlitter` | 🟢 | Line-by-line audited: Lower/upper blend split checks |
| `HorishAntiHairBlitter` | `HorishAntiHairBlitter` | 🟢 | Line-by-line audited: Vertical pair blend split checks |
| `VLineAntiHairBlitter` | `VLineAntiHairBlitter` | 🟢 | Line-by-line audited: Horizontal pair blend split checks |
| `VertishAntiHairBlitter` | `VertishAntiHairBlitter` | 🟢 | Line-by-line audited: Horizontal pair blend split checks |
| `RectClipBlitter` | `RectClipBlitter` | 🟢 | Line-by-line audited: Rect clipping boundaries and anti-run clipping checks |

### `third_party/tiny-skia/src/scan/mod.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `fill_rect` | `scan::fillRect` | 🟢 | Line-by-line audited: Integer-rounding and clip intersection behavior |
| `fill_rect_aa` | `scan::fillRectAa` | 🟢 | Line-by-line audited: Fractional-rect antialias path coverage |

