# Function Mapping Tables

Legend: `☐` Not started, `🧩` Stub only, `🟡` Implemented/tested (Rust completeness not yet vetted), `🟢` Rust-completeness vetted (line-by-line against Rust), `✅` Verified parity sign-off (user-requested audit complete), `⏸` Blocked.

## `third_party/tiny-skia/src/wide/mod.rs` -> `src/tiny_skia/wide/Mod.cpp` / `src/tiny_skia/wide/Mod.h`

| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `generic_bit_blend` | `tiny_skia::wide::genericBitBlend` | 🟢 | Line-by-line audited: Bit-expression ported directly matches Rust. Validated in audit 2026-03-02. |
| `FasterMinMax::faster_min` | `tiny_skia::wide::fasterMin` | 🟢 | Line-by-line audited: Branch ordering mirrors Rust trait impl. Validated in audit 2026-03-02. |
| `FasterMinMax::faster_max` | `tiny_skia::wide::fasterMax` | 🟢 | Line-by-line audited: Branch ordering mirrors Rust trait impl. Validated in audit 2026-03-02. |

## `third_party/tiny-skia/src/wide/f32x4_t.rs` -> `src/tiny_skia/wide/F32x4T.cpp` / `src/tiny_skia/wide/F32x4T.h`

| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `f32x4::splat` | `tiny_skia::wide::F32x4T::splat` | 🟢 | Line-by-line audited: Direct lane broadcast port matches Rust. Validated in audit 2026-03-02. |
| `f32x4::abs` | `tiny_skia::wide::F32x4T::abs` | 🟢 | Line-by-line audited: Per-lane absolute value matches Rust scalar fallback. Validated in audit 2026-03-02. |
| `f32x4::max` | `tiny_skia::wide::F32x4T::max` | 🟢 | Line-by-line audited: Per-lane `fasterMax` matches Rust scalar fallback. Validated in audit 2026-03-02. |
| `f32x4::min` | `tiny_skia::wide::F32x4T::min` | 🟢 | Line-by-line audited: Per-lane `fasterMin` matches Rust scalar fallback. Validated in audit 2026-03-02. |
| `f32x4::{cmp_eq,cmp_ne,cmp_ge,cmp_gt,cmp_le,cmp_lt}` | `tiny_skia::wide::F32x4T::{cmpEq,cmpNe,cmpGe,cmpGt,cmpLe,cmpLt}` | 🟢 | Line-by-line audited: Scalar mask encoding (`0xFFFFFFFF`/`0`) matches Rust. Validated in audit 2026-03-02. |
| `f32x4::blend` | `tiny_skia::wide::F32x4T::blend` | 🟢 | Line-by-line audited: Bit-blend via `genericBitBlend` matches Rust scalar fallback. Validated in audit 2026-03-02. |

## `third_party/tiny-skia/src/wide/f32x8_t.rs` -> `src/tiny_skia/wide/F32x8T.cpp` / `src/tiny_skia/wide/F32x8T.h`

| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `f32x8::splat` | `tiny_skia::wide::F32x8T::splat` | 🟢 | Line-by-line audited: Direct eight-lane broadcast constructor matches Rust. Validated in audit 2026-03-02. |
| `f32x8::{floor,fract,normalize}` | `tiny_skia::wide::F32x8T::{floor,fract,normalize}` | 🟢 | Line-by-line audited: Scalar fallback lane math matches Rust. Validated in audit 2026-03-02. |
| `f32x8::{to_i32x8_bitcast,to_u32x8_bitcast}` | `tiny_skia::wide::F32x8T::{toI32x8Bitcast,toU32x8Bitcast}` | 🟢 | Line-by-line audited: Per-lane bitcast conversion paths match Rust cast intent. Validated in audit 2026-03-02. |
| `f32x8::{cmp_eq,cmp_ne,cmp_ge,cmp_gt,cmp_le,cmp_lt}` | `tiny_skia::wide::F32x8T::{cmpEq,cmpNe,cmpGe,cmpGt,cmpLe,cmpLt}` | 🟢 | Line-by-line audited: Scalar mask encoding (`0xFFFFFFFF`/`0`) matches Rust. Validated in audit 2026-03-02. |
| `f32x8::blend` | `tiny_skia::wide::F32x8T::blend` | 🟢 | Line-by-line audited: Bit-select fallback via `genericBitBlend` matches Rust. Validated in audit 2026-03-02. |
| `f32x8::{abs,max,min,is_finite}` | `tiny_skia::wide::F32x8T::{abs,max,min,isFinite}` | 🟢 | Line-by-line audited: Per-lane scalar fallback matches Rust. Validated in audit 2026-03-02. |
| `f32x8::{round,round_int,trunc_int}` | `tiny_skia::wide::F32x8T::{round,roundInt,truncInt}` | 🟢 | Line-by-line audited: Scalar rounding/truncation fallback and integer conversion paths match Rust. Validated in audit 2026-03-02. |
| `impl {Add,Sub,Mul,Div,BitAnd,BitOr,BitXor,Neg,Not,PartialEq} for f32x8` | `tiny_skia::wide::F32x8T operators` | 🟢 | Line-by-line audited: Per-lane arithmetic and bitwise operators match Rust scalar fallback form. Validated in audit 2026-03-02. |
| `f32x8::{recip_fast,recip_sqrt,sqrt,powf}` | `tiny_skia::wide::F32x8T::{recipFast,recipSqrt,sqrt,powf}` | 🟢 | Line-by-line audited: Added in WI-01; recipFast, recipSqrt, sqrt, powf match Rust per-lane scalar fallback. Validated in audit 2026-03-02. |
| `impl AddAssign for f32x8` | `tiny_skia::wide::F32x8T::operator+=` | 🟢 | Added in WI-01: Per-lane add-assign operator matches Rust AddAssign impl. Validated in audit 2026-03-02. |

## `third_party/tiny-skia/src/wide/i32x4_t.rs` -> `src/tiny_skia/wide/I32x4T.cpp` / `src/tiny_skia/wide/I32x4T.h`

| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `i32x4::splat` | `tiny_skia::wide::I32x4T::splat` | 🟢 | Line-by-line audited: Direct lane broadcast constructor matches Rust. Validated in audit 2026-03-02. |
| `i32x4::blend` | `tiny_skia::wide::I32x4T::blend` | 🟢 | Line-by-line audited: Bit-select path via `genericBitBlend` matches Rust. Validated in audit 2026-03-02. |
| `i32x4::{cmp_eq,cmp_gt,cmp_lt}` | `tiny_skia::wide::I32x4T::{cmpEq,cmpGt,cmpLt}` | 🟢 | Line-by-line audited: Scalar fallback mask encoding (`-1` / `0`) matches Rust. Validated in audit 2026-03-02. |
| `i32x4::{to_f32x4,to_f32x4_bitcast}` | `tiny_skia::wide::I32x4T::{toF32x4,toF32x4Bitcast}` | 🟢 | Line-by-line audited: Numeric cast and bitcast conversion paths match Rust. Validated in audit 2026-03-02. |
| `impl Add for i32x4` | `tiny_skia::wide::I32x4T::operator+` | 🟢 | Line-by-line audited: Wrapping lane-add semantics via unsigned bit math matches Rust. Validated in audit 2026-03-02. |
| `impl Mul for i32x4` | `tiny_skia::wide::I32x4T::operator*` | 🟢 | Line-by-line audited: Wrapping lane-mul semantics via unsigned bit math matches Rust. Validated in audit 2026-03-02. |
| `impl {BitAnd,BitOr,BitXor} for i32x4` | `tiny_skia::wide::I32x4T::{operator&,operator|,operator^}` | 🟢 | Line-by-line audited: Per-lane bitwise ops match Rust. Validated in audit 2026-03-02. |

## `third_party/tiny-skia/src/wide/i32x8_t.rs` -> `src/tiny_skia/wide/I32x8T.cpp` / `src/tiny_skia/wide/I32x8T.h`

| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `i32x8::splat` | `tiny_skia::wide::I32x8T::splat` | 🟢 | Line-by-line audited: Direct eight-lane broadcast constructor matches Rust. Validated in audit 2026-03-02. |
| `i32x8::blend` | `tiny_skia::wide::I32x8T::blend` | 🟢 | Line-by-line audited: Bit-select path via `genericBitBlend` matches Rust. Validated in audit 2026-03-02. |
| `i32x8::{cmp_eq,cmp_gt,cmp_lt}` | `tiny_skia::wide::I32x8T::{cmpEq,cmpGt,cmpLt}` | 🟢 | Line-by-line audited: Scalar fallback mask encoding (`-1` / `0`) matches Rust. Validated in audit 2026-03-02. |
| `i32x8::{to_f32x8,to_u32x8_bitcast,to_f32x8_bitcast}` | `tiny_skia::wide::I32x8T::{toF32x8,toU32x8Bitcast,toF32x8Bitcast}` | 🟢 | Line-by-line audited: Numeric cast and bitcast conversion paths match Rust. Validated in audit 2026-03-02. |
| `impl Add for i32x8` | `tiny_skia::wide::I32x8T::operator+` | 🟢 | Line-by-line audited: Wrapping lane-add semantics via unsigned bit math matches Rust. Validated in audit 2026-03-02. |
| `impl Mul for i32x8` | `tiny_skia::wide::I32x8T::operator*` | 🟢 | Line-by-line audited: Wrapping lane-mul semantics via unsigned bit math matches Rust. Validated in audit 2026-03-02. |
| `impl {BitAnd,BitOr,BitXor} for i32x8` | `tiny_skia::wide::I32x8T::{operator&,operator|,operator^}` | 🟢 | Line-by-line audited: Per-lane bitwise ops match Rust. Validated in audit 2026-03-02. |

## `third_party/tiny-skia/src/wide/u32x4_t.rs` -> `src/tiny_skia/wide/U32x4T.cpp` / `src/tiny_skia/wide/U32x4T.h`

| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `u32x4::splat` | `tiny_skia::wide::U32x4T::splat` | 🟢 | Line-by-line audited: Direct lane broadcast constructor matches Rust. Validated in audit 2026-03-02. |
| `u32x4::cmp_eq` | `tiny_skia::wide::U32x4T::cmpEq` | 🟢 | Line-by-line audited: Scalar fallback equality mask (`u32::MAX`/`0`) matches Rust. Validated in audit 2026-03-02. |
| `u32x4::{cmp_ne,cmp_lt,cmp_le,cmp_gt,cmp_ge}` | `tiny_skia::wide::U32x4T::{cmpNe,cmpLt,cmpLe,cmpGt,cmpGe}` | 🟢 | Added in WI-02: Per-lane comparison operators match Rust scalar fallback. Validated in audit 2026-03-02. |
| `u32x4::shl` | `tiny_skia::wide::U32x4T::shl<Rhs>` | 🟢 | Line-by-line audited: Template shift-left mirrors per-lane scalar fallback matches Rust. Validated in audit 2026-03-02. |
| `u32x4::shr` | `tiny_skia::wide::U32x4T::shr<Rhs>` | 🟢 | Line-by-line audited: Template shift-right mirrors per-lane scalar fallback matches Rust. Validated in audit 2026-03-02. |
| `impl Not for u32x4` | `tiny_skia::wide::U32x4T::operator~` | 🟢 | Line-by-line audited: Per-lane bitwise NOT matches Rust fallback. Validated in audit 2026-03-02. |
| `impl Add for u32x4` | `tiny_skia::wide::U32x4T::operator+` | 🟢 | Line-by-line audited: Unsigned wrap add matches Rust scalar fallback. Validated in audit 2026-03-02. |
| `impl {BitAnd,BitOr} for u32x4` | `tiny_skia::wide::U32x4T::{operator&,operator|}` | 🟢 | Line-by-line audited: Per-lane bitwise ops match Rust. Validated in audit 2026-03-02. |
| `impl BitXor for u32x4` | `tiny_skia::wide::U32x4T::operator^` | 🟢 | Added in WI-02: Per-lane bitwise XOR matches Rust. Validated in audit 2026-03-02. |

## `third_party/tiny-skia/src/wide/u32x8_t.rs` -> `src/tiny_skia/wide/U32x8T.cpp` / `src/tiny_skia/wide/U32x8T.h`

| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `u32x8::splat` | `tiny_skia::wide::U32x8T::splat` | 🟢 | Line-by-line audited: Direct eight-lane broadcast constructor matches Rust. Validated in audit 2026-03-02. |
| `u32x8::{to_i32x8_bitcast,to_f32x8_bitcast}` | `tiny_skia::wide::U32x8T::{toI32x8Bitcast,toF32x8Bitcast}` | 🟢 | Line-by-line audited: Per-lane bitcast conversion paths match Rust cross-vector reinterpret. Validated in audit 2026-03-02. |
| `u32x8::cmp_eq` | `tiny_skia::wide::U32x8T::cmpEq` | 🟢 | Line-by-line audited: Scalar fallback equality mask (`u32::MAX`/`0`) matches Rust. Validated in audit 2026-03-02. |
| `u32x8::{cmp_ne,cmp_lt,cmp_le,cmp_gt,cmp_ge}` | `tiny_skia::wide::U32x8T::{cmpNe,cmpLt,cmpLe,cmpGt,cmpGe}` | 🟢 | Added in WI-02: Per-lane comparison operators match Rust scalar fallback. Validated in audit 2026-03-02. |
| `u32x8::shl` | `tiny_skia::wide::U32x8T::shl<Rhs>` | 🟢 | Line-by-line audited: Template shift-left mirrors per-lane scalar fallback matches Rust. Validated in audit 2026-03-02. |
| `u32x8::shr` | `tiny_skia::wide::U32x8T::shr<Rhs>` | 🟢 | Line-by-line audited: Template shift-right mirrors per-lane scalar fallback matches Rust. Validated in audit 2026-03-02. |
| `impl Not for u32x8` | `tiny_skia::wide::U32x8T::operator~` | 🟢 | Line-by-line audited: Per-lane bitwise NOT matches Rust fallback. Validated in audit 2026-03-02. |
| `impl Add for u32x8` | `tiny_skia::wide::U32x8T::operator+` | 🟢 | Line-by-line audited: Unsigned wrap add matches Rust scalar fallback. Validated in audit 2026-03-02. |
| `impl {BitAnd,BitOr} for u32x8` | `tiny_skia::wide::U32x8T::{operator&,operator|}` | 🟢 | Line-by-line audited: Per-lane bitwise ops match Rust. Validated in audit 2026-03-02. |
| `impl BitXor for u32x8` | `tiny_skia::wide::U32x8T::operator^` | 🟢 | Added in WI-02: Per-lane bitwise XOR matches Rust. Validated in audit 2026-03-02. |
