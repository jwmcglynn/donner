# PathSpline Boolean Operations Developer Guide {#PathBooleanOperations}

This guide describes the developer-facing design of the PathSpline Boolean stack now that the
feature is implemented. It focuses on how the pieces fit together, what guarantees the
implementations provide, and where to extend or swap components.

## Overview
PathSpline Boolean operations expose union, intersection, difference (A−B), reverse difference
(B−A), and xor between two path operands while honoring even-odd and non-zero fill rules. The
pipeline preserves curves whenever possible, avoiding full flattening except at locally subdivided
intersection spans. All components are backend-agnostic and avoid dependencies on Skia or other
renderers.

### Key Guarantees
- Curve-first behavior: cubic/quadratic/arc segments stay intact unless subdivision is required to
  resolve intersections within the configured tolerance.
- Deterministic output for identical inputs, using tolerance-driven predicates and consistent
  segment ordering.
- Testability: the Boolean engine is injected through a mock-friendly interface, and segmentation and
  reconstruction helpers have direct unit coverage.
- Thread safety: helpers are const-correct, allocate per-call state, and avoid shared globals.

## Architecture Snapshot
1. **Normalization:** `PathBooleanOps` normalizes inputs via `PathSpline::maybeAutoReopen`, enforcing
   explicit closures for fillable contours.
2. **Segmentation:** `PathBooleanSegmenter` converts each path into ordered curve spans, performing
   tolerance-bound subdivision only when curvature or intersection detection requires it. Spans keep
   their originating curve types and parameter ranges.
3. **Engine Boundary:** `PathBooleanEngine` consumes segmented subpaths plus effective fill rules and
   tolerance. The default `PathBooleanCustomEngine` currently combines spans while preserving curve
   metadata; alternative engines can be supplied by the caller for specialized kernels.
4. **Reconstruction:** `PathBooleanReconstructor` rebuilds `PathSpline` output from engine-emitted
   spans, reusing untouched curves directly and emitting polylines only for spans that were forced to
   linearize during subdivision.
5. **API Surface:** `PathSpline::BooleanOp` and the convenience wrappers (union/intersection/
   difference/reverse difference/xor) orchestrate the above stages and return a fillable
   `PathSpline` result.

## Engine Integration Guidance
- Use the provided `PathBooleanCustomEngine` for baseline behavior or inject your own
  `PathBooleanEngine` implementation when embedding alternative kernels. The adapter only requires
  segmented subpaths and preserves fill-rule context.
- Tolerance defaults to `PathBooleanOps::kDefaultTolerance`; callers may override when they need
  tighter or looser subdivision for intersection resolution.
- The engine should treat empty operands with the short-circuit semantics encoded in
  `PathBooleanOps` tests: unions/xors propagate the non-empty path, while intersections and
  differences collapse to empty.

## Testing Hooks
- Unit tests live under `donner/svg/core/tests/` and cover segmentation, reconstruction, adapter
  behavior, and the custom engine. Mock-based tests rely on `PathBooleanEngine` to assert wiring
  without invoking real kernels.
- Integration tests exercise `PathSpline::BooleanOp` end-to-end with the custom engine and verify
  geometry preservation across operations and fill rules.

## Limitations and Future Extensions
- The current custom engine is intended as a correctness-preserving baseline; more robust
  intersection handling or polyline-only modes can be added behind new `PathBooleanEngine`
  implementations without changing the public API.
- Stroke booleans remain out of scope; callers should stroke to fills before applying Boolean
  operations.
