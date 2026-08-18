# Design: Optional Multithreaded Rendering

**Status:** Draft
**Author:** Claude Opus 5
**Drafted by:** Claude Opus 5
**Created:** 2026-08-18

## Summary

Donner renders on one thread. That is the right default and this design does not change it. But a
filtered document is not shape-bound, it is filter-bound: on a reference multi-core x86-64 Linux host
at `-c opt`, a single 900x900 CPU filter node costs 31 to 36 ms, and a two-node
blur-plus-diffuse-lighting graph on the same buffer costs 112 ms. One node of a filtered frame
therefore costs about as much as the entire Ghostscript Tiger frame that
[0060](0060-tinyskia_retained_raster_cache.md) is built to make cheap. No amount of caching helps
here, because the work is not repeated, it is genuinely new pixel math over a large buffer.

This design adds **optional, config-gated multithreading to the CPU filter path**, and nothing else.
The single unit of concurrency is a **row band of one filter node's raster**: a large filter
primitive's output rows are split across workers, each worker computes the same per-output-pixel
function it computes today, and the filter graph's node order and semantics are untouched. A working
prototype of this decomposition over the unmodified `convolveMatrix` primitive measured a **4.41x
speedup at 8 threads with zero bit-differing floats** against the whole-buffer result (see
"Measurements").

Two supporting decisions make the feature safe to own:

- **One worker pool for the whole renderer.** Not one per feature. It follows the
  structured-concurrency contract [0060](0060-tinyskia_retained_raster_cache.md) establishes:
  joinable units, joins expressed as dependencies, ordered writes, a zero-worker inline mode that is
  the same code path, and no detached threads. 0060's prepare stage becomes a consumer of this pool
  rather than introducing a second one.
- **Determinism by construction, not by testing luck.** Every worker writes a disjoint output
  region and reads only immutable input, so the output does not depend on how rows were assigned.
  Byte-identical output at any worker count is a corpus gate, not an aspiration.

Two things this design explicitly **does not** build, with the reasoning recorded so it is not
re-litigated: Geode command-encoding parallelism, and render-tree branch parallelism. Both are in
"Recorded Do-Not-Build Decisions".

## Goals

- A large CPU filter node's raster scales with worker count: at least 4x on eight workers for the
  gather-shaped primitives (convolve, lighting, morphology, component transfer, composite), measured
  by a filter scaling benchmark at `-c opt`.
- **Byte-identical output at every worker count**, including the zero-worker inline mode, over the
  renderer golden corpus and the resvg suite. This is the primary correctness property and it is a
  CI gate, not a review promise.
- The default build and the default runtime are unchanged: with the feature flag off, no threading
  primitives are linked and no thread is created; with the flag on but the worker count left at its
  default of zero, execution is inline on the caller thread through the same code path.
- Exactly one worker pool exists in the renderer, with an explicit owner and lifetime. No feature
  creates its own threads, and no thread is detached.
- A Wasm build without thread support runs the existing single-threaded path unchanged, with no new
  cross-origin-isolation requirement.
- The threaded configuration is exercised under ThreadSanitizer in CI, not only in the default
  build.

## Non-Goals

- **No compositor-layer parallelism.** Compose is not currently independent per layer; see
  "Preconditions and Sequencing". This design does not attempt it and does not pretend the
  precondition is met.
- **No compositor tiling.** Splitting the composited frame into screen tiles is a separate problem
  with its own ordering and damage-tracking constraints, and it is deferred to a follow-up design
  doc.
- **No Geode command-encoding parallelism** and **no render-tree branch parallelism**. See
  "Recorded Do-Not-Build Decisions".
- **No change to filter graph semantics.** Nodes still execute in graph order, buffers are still
  produced and consumed exactly as they are today, and per-node color-space tagging is untouched.
  This design parallelizes the inside of a node, not the schedule of nodes.
- **No relaxed determinism.** There is no "visually identical" tolerance and no fast-math or
  reassociation of any accumulation. If a decomposition cannot be shown byte-identical, it is not
  used.
- **No GPU filter changes.** `GeodeFilterEngine` is untouched.
- **No new concurrency in the SVG DOM.** The document access model from
  [0033](0033-multithreading_and_dom_lifetime.md) is unchanged; filter work runs inside a single
  frame's existing document access, on data the frame already owns.

## Next Steps

- Add the explicit output-row-range parameter and the band-identity tests to the CPU filter
  primitives, single-threaded, before any worker pool exists. That change is independently valuable
  (it makes the tiling seam checkable) and carries no concurrency risk.

## Implementation Plan

- [ ] Milestone 1: The tiling seam, single-threaded
  - [ ] Add an explicit output row range to the gather-shaped float primitives
        (`convolveMatrix`, `diffuseLighting`, `specularLighting`, `componentTransfer`,
        `colorMatrix`, `composite`, `blend`, `merge`, `displacementMap`, `offset`, `tile`,
        `turbulence`, `flood`, and the `FloatPixmap` color-space conversions), defaulting to the
        whole buffer so every existing call site is unchanged.
  - [ ] Band-identity tests in the filter test target: for each primitive, executing the buffer as
        N bands must be byte-identical to executing it whole, for several band heights including
        heights that do not divide the buffer.
  - [ ] Move the multi-pass primitives (`gaussianBlur`, `morphology`) to an internal band loop per
        pass, still sequential, with per-band scratch instead of per-call scratch.
- [ ] Milestone 2: The shared worker pool
  - [ ] `RenderWorkerPool` with joinable work items, an idempotent join, dynamic band assignment,
        and a zero-worker mode that runs the identical code path inline.
  - [ ] Bazel `bool_flag` + `config_setting` gating, default off; no threading headers included
        when off.
  - [ ] Runtime worker-count configuration plumbed from the renderer's options, default zero.
- [ ] Milestone 3: Filter tiling on the pool
  - [ ] Drive the Milestone 1 row ranges from the pool inside `ApplyFilterGraphToPixmap`'s
        execution, with a join at every pass boundary.
  - [ ] Thread-count identity gate over the renderer golden corpus and the resvg suite at worker
        counts {0, 1, 2, 8}.
  - [ ] ThreadSanitizer CI lane building the flag on and running the identity gate.
- [ ] Milestone 4: Performance validation and acceptance
  - [ ] Filter scaling benchmark reporting per-primitive and whole-graph times against worker count.
  - [ ] Acceptance gate on at least two filter-heavy corpus documents.

## Background

### Where a filtered frame spends its time

`RendererTinySkia` builds a filter layer, hands the layer's pixmap to
`ApplyFilterGraphToPixmap` (`donner/svg/renderer/FilterGraphExecutor.cc`), which converts Donner's
`FilterGraph` into the pixel-space graph the CPU filter library executes, and then calls
`tiny_skia::filter::executeFilterGraph`. That function walks `graph.nodes` in order. Each node
allocates a full-size `FloatPixmap`, runs its primitive over the whole buffer, applies subregion
clipping, and publishes the result as the next node's input.

The important structural fact is that **every node's cost is proportional to the full buffer area**,
not to the node's subregion. `createTransparentFloat(w, h)` sizes every intermediate at the layer's
full extent, and the primitives loop `for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)`.
A filtered document at editor resolution therefore pays several full-buffer passes per filter, and
those passes are the frame.

Measurements on a reference multi-core x86-64 Linux host at `-c opt`, median of nine, 900x900 float
buffers, taken with a scratch harness built against the in-tree filter library:

| Operation                                   | Time     |
| ------------------------------------------- | -------- |
| `gaussianBlur` sigma 2.0                    | 33.0 ms  |
| `gaussianBlur` sigma 8.0                    | 31.8 ms  |
| `gaussianBlur` sigma 24.0                   | 31.4 ms  |
| `diffuseLighting`, point light              | 34.8 ms  |
| `convolveMatrix` 3x3                        | 36.1 ms  |
| `morphology` dilate radius 6                | 34.6 ms  |
| `executeFilterGraph`, `feDropShadow`        | 94.4 ms  |
| `executeFilterGraph`, blur + diffuse light  | 112.2 ms |

Three conclusions follow directly, and each one shapes the design.

**Blur cost is independent of sigma.** 33.0, 31.8, and 31.4 ms across a 12x sigma range. The running-
sum box blur is O(1) per pixel per pass, so blur cost is a function of pixel count alone. Filter cost
scales with area, which is exactly the quantity a row split divides.

**One node is the frame.** A single node at 31 to 36 ms is comparable to the whole Ghostscript Tiger
frame 0060 measures at about 41 ms. Making the second frame of an unchanged filtered document cheap
does not help, because a filter node's output is not reusable across a zoom or a parameter change;
the work is new each time. This is why the centerpiece is intra-node parallelism and not another
cache.

**Node kernels are the majority but not all of the graph.** The two-node chain costs 112.2 ms while
its two kernels cost about 67 ms. The residual is buffer allocation, the uint8-to-float conversion,
the sRGB-to-linear entry conversion and the linear-to-sRGB exit conversion, and subregion clipping.
Those are all per-pixel passes over the same buffers, so they tile under exactly the same rules, and
the design includes them rather than leaving 40 percent of the graph serial.

### Why the node, and not the graph

Parallelism could in principle be extracted at three levels: across filter graphs, across nodes
within a graph, or within a node.

Across graphs is out of scope: filter layers are pushed and popped inside a single ordered draw
traversal, and lifting them out is a rendering-order change, not a threading change.

Across nodes is nearly always unavailable. `executeFilterGraph` maintains `previousOutput` and a
`namedBuffers` map; the overwhelmingly common graph is a chain where node N+1 consumes node N.
Independent branches do exist (a `feMerge` of two chains, an `feComposite` of two inputs), but they
are the minority, the branches are usually unequal in cost so the join wastes a worker, and the win
is capped at the branch factor. Intra-node tiling, by contrast, applies to every node of every
graph and its parallelism is bounded by buffer height, not by graph shape.

Within a node is therefore the seam, and it is also the safest one: it changes no data flow at all.
The node's inputs are the same buffers, its output is the same buffer, and the only thing that
changes is which thread computes which output rows.

## Proposed Architecture

### The tiling seam is a row range, not a sliced buffer

Every CPU filter primitive today has the shape `f(const Src& src, Dst& dst, params)` and writes
every row of `dst`. The change is to add an explicit **output row range** `[yBegin, yEnd)` while
still passing the **whole** source buffer:

```cpp
void convolveMatrix(const FloatPixmap& src, FloatPixmap& dst, const ConvolveParams& params,
                    RowRange rows = RowRange::All());
```

Passing the whole source and restricting only the output is the load-bearing decision, and it is
what makes the apron a non-problem:

- **No apron copy exists.** A primitive reading a neighbourhood simply indexes into the shared,
  immutable source. There is no per-band staging buffer, no halo exchange, and no boundary
  reconciliation step. The apron is purely a description of which source rows a band touches, which
  matters for cost accounting and for cache behaviour, not for correctness.
- **Absolute coordinates keep working.** The lighting primitives compute the light direction from
  the pixel's absolute position (`params.light.x/y/z`, and `params.pixelToUser` mapping pixel
  coordinates to user space). A band handed a rebased sub-buffer would silently light the wrong
  geometry; a band handed a row range into the original buffer cannot. The same argument covers
  `tile`, whose tile origin is in buffer coordinates, and `turbulence`, whose noise is a function of
  absolute position.
- **Edge modes keep working.** `resolveEdge` clamps or wraps against the real buffer extent. A band
  is not an edge, so `BlurEdgeMode::Duplicate` at a band boundary does not fire, and at the real
  image edge it fires exactly as it does today.

The prototype in "Measurements" deliberately used the harder variant, staging each band into its own
buffer with an apron, precisely to prove the apron reasoning is sound; that variant still produced
zero bit differences, and its band-copy overhead (a 12 percent regression at one thread) is the cost
the row-range API removes.

### Tile decomposition by primitive class

Bands are always horizontal, spanning the full buffer width. The split axis is chosen per primitive
so that it is an axis along which the algorithm carries no state. This is the only per-primitive
judgement the design requires.

| Class | Primitives | Split axis | Source rows a band reads | Carried state |
| ----- | ---------- | ---------- | ------------------------ | ------------- |
| Per-pixel pure | `flood`, `colorMatrix`, `componentTransfer`, `composite`, `blend`, `merge`, `offset`, `tile`, `turbulence`, subregion clipping, `srgbToLinear` / `linearToSrgb` on `FloatPixmap` | output rows | the same rows (`offset` and `tile` read a fixed translation of them) | none |
| Bounded gather | `convolveMatrix`, `diffuseLighting`, `specularLighting` | output rows | `[yBegin - up, yEnd + down)`; convolve uses `up = targetY`, `down = orderY - 1 - targetY`; lighting uses 1 and 1 for its 3x3 normal | none |
| Unbounded gather | `displacementMap` | output rows | up to the whole source, bounded by `scale` | none |
| Separable multi-pass | `gaussianBlur`, `morphology` | rows of the pass currently executing | the whole row (both are row-wise passes) | a running sum or a van Herk block accumulator **along x**, within one row |
| Reduction | `computeNonTransparentBounds` | output rows | the same rows | a per-band min/max, combined in fixed band order |

The separable multi-pass row deserves the most care, because it is the one place a naive tiling would
be wrong. `boxBlurHorizontalFloat` carries a `Vec4f32 sum` across the x loop, adding the entering
pixel and subtracting the leaving one. Restarting that accumulator part-way along a row would
produce a different sequence of float additions and therefore, legitimately, different bytes.
**So a row is never split.** Each band owns whole rows, and the accumulator is re-initialized per row
exactly as it is today. The vertical pass is implemented as transpose, horizontal pass, transpose
back, so banding the transposed pass by transposed rows is banding the original buffer by columns,
which is again an axis with no carried state. The transposes themselves are a blocked copy with
disjoint reads and writes and band trivially.

`morphology`'s van Herk pass allocates its `fwd` / `bwd` scratch once outside the row loop and
reuses it per row. That scratch becomes per band. Blur's `buffer` and `scratch` are whole-buffer and
are written disjointly by rows, so they stay shared.

Passes are separated by a **join**, not by a lock. Pass N+1 reads the whole output of pass N, so the
pool joins all of pass N's bands before submitting pass N+1. That is the dependency edge; there is
no other synchronization inside a primitive.

### Why the output is thread-count invariant

The determinism argument has four parts and each one is checkable in the tree:

1. **Disjoint writes.** Bands partition the output rows. Two bands never write the same byte, so no
   write is ordered against another write and no atomic or lock mediates pixel stores.
2. **Immutable reads.** Every primitive reads a source buffer it does not write. The one apparent
   exception, the in-place `FloatPixmap` color-space conversions, reads and writes only the pixel it
   is converting, so a band's reads and writes stay inside its own rows. `gaussianBlur` and
   `morphology` copy their input into a private buffer before the first pass, so their passes are
   also strictly read-one-buffer, write-another.
3. **Per-pixel purity.** Each output pixel is a pure function of the source buffer and the node
   parameters. That function does not depend on which rows were computed first, or on how many
   bands there were, so the partition itself does not appear in the result. This is why identity
   holds across *different* worker counts and not merely across runs at one worker count.
4. **Deterministic reduction.** The one place a filter needs cross-band state is a whole-buffer
   reduction, `computeNonTransparentBounds`, used for paint-input subregions. Each band computes its
   own min/max and the bands are combined in **fixed band-index order**, never in completion order.
   Min and max happen to be associative and exact, so this is belt and braces today; the rule is
   stated as a contract so that a future reduction over a non-associative operation (a float sum, a
   weighted average) cannot be added without honouring it.

The band grid is a pure function of the buffer height and a fixed band height, **not** of the worker
count. Worker assignment is dynamic, so a busy machine and an idle machine produce different
schedules and identical pixels. Fixing the grid independently of worker count also keeps a debugger
session reproducible and makes per-band scratch sizing predictable.

### Interaction with the sRGB LUT layer

The color-space conversions are the largest non-kernel cost in the graph, and they are already
structured for this. `ColorSpace.cpp` builds its four transfer tables in function-local statics
(`srgbToLinearFloatLut()` and friends) and the file already documents that C++11 function-local
static initialization is thread-safe, so a concurrent first caller is fine. After initialization the
tables are `const` and are only read. `lookupUnit` clamps its input before indexing, including
mapping NaN to zero, so there is no input-dependent control flow that could differ per thread.

The conversions therefore need no change beyond the row range: they are per-pixel pure over a
read-only table. Concretely, `srgbToLinear(FloatPixmap&)` and `linearToSrgb(FloatPixmap&)` gain a
row range and are driven by the same band loop as any other pass.

One rule is worth writing down because it is easy to break later: **any future quantized ramp,
cached conversion, or memoized table must be immutable after construction, or built before the first
band is submitted.** A lazily-populated per-call cache mutated from a worker would be both a race and
a determinism hazard, and it would not necessarily fail a single-threaded test.

### Interaction with SIMD

`SimdVec.h` selects NEON, Wasm SIMD128, SSE2, or a scalar fallback at compile time, and `Vec4u32` /
`Vec4f32` are value types over local registers with no global or thread-local state. Banding
therefore does not interact with vectorization at all: each band runs the same instruction mix on
its own rows.

Two consequences are worth stating:

- **Bands must be whole rows** (already required by the blur accumulator), which keeps every band's
  inner loop aligned exactly as it is today, so the vectorized tail handling is unchanged.
- **The band height must not be so small that the per-band prologue dominates.** The default band
  height is a tuning parameter with a floor; see "Open Questions".

The scalar fallback build is also the ThreadSanitizer build's likely configuration on some lanes,
which is fine: identity is asserted within a configuration, never across ISA branches. Cross-ISA
byte differences already exist by design (`FloatPixmap::fromPixmap` documents one such deliberate
NEON/scalar divergence) and are outside this design's scope.

### The shared worker pool

The pool is a renderer-scoped service with one owner, following the structured-concurrency contract
0060 sets out for its prepare stage:

- **One pool, one owner.** The pool is owned by the renderer for the lifetime of the renderer.
  Nothing else in the tree creates threads for rendering work. When 0060's prepare stage lands, it
  submits to this pool rather than introducing a second one and a second flag.
- **Joinable units, idempotent joins.** A submitted band is a joinable unit with an idempotent wait.
  The submitter joins; nothing else observes a band's completion.
- **Joins are dependencies, not timing.** Every consumer of parallel output joins it explicitly:
  the pass boundary inside a separable primitive, and the node boundary inside the graph executor.
  There is no sleep, no polling, and no "should be done by now".
- **No detached threads.** Workers are created when the pool is constructed and joined when it is
  destroyed. A band never outlives the call that submitted it, so a cancelled or failed frame cannot
  leave work running against freed buffers.
- **Zero-worker mode is the same code path.** With zero workers, submission executes the band inline
  on the caller thread through the same joinable-unit structure. There is no second single-threaded
  implementation that could drift from the threaded one, which is what makes the identity gate
  meaningful rather than tautological.
- **Workers touch nothing else.** A band reads its node's input buffers and writes its own output
  rows. It does not touch the ECS registry, the document, the renderer's paint state, the compositor,
  or any Geode object. This is the property that keeps the ThreadSanitizer lane quiet for reasons
  rather than by luck.

Failure behaviour is fail-safe rather than fail-closed: if the pool cannot be created (no thread
support, a platform limit, a configuration error), the renderer runs the zero-worker inline path and
renders correctly, more slowly. A rendering feature must not refuse to render because a performance
optimization is unavailable.

### Configuration gating

A `bool_flag` plus `config_setting` in `donner/svg/renderer/BUILD.bazel`, following the existing
`:filters` / `:text` / `:renderer_backend` pattern, for example
`--//donner/svg/renderer:render_worker_pool`. With the flag off, which is the default, the pool
compiles to the inline path and no threading header is included and no threading primitive is
linked.

Worker count is a **runtime** setting on the renderer's options, defaulting to zero. Compile-time
gating and runtime count are separate on purpose: the flag decides whether the machinery exists, the
count decides whether it is used, and CI can build the flag on while most targets still run at zero
workers.

### Wasm

The plain `wasm` configuration does not pass `-pthread`, and the design keeps the worker pool
compiled out there: a browser build without cross-origin isolation must keep working, and the CPU
filter path must not become a second reason to require it. The `editor-wasm` configuration does pass
`-pthread`, so it *can* host the pool, but the runtime worker count still defaults to zero and the
renderer must probe for usable concurrency rather than assume it: a build with pthread support served
without cross-origin isolation cannot actually spawn workers, and that case must land on the inline
path, not on a failed render.

The cooperative structure (joinable bands, idempotent joins, inline submission) compiles and runs
unchanged in every Wasm configuration, because the zero-worker mode is the same code path.

## API / Interfaces

```cpp
/// A half-open range of output rows a primitive should produce. The default is every row, so
/// existing call sites are unchanged.
struct RowRange {
  int begin = 0;
  int end = -1;  ///< -1 means "to the end of the destination".
  static RowRange All() { return RowRange{}; }
};

/// Renderer-scoped worker pool. Zero workers executes inline through the same path.
class RenderWorkerPool {
 public:
  explicit RenderWorkerPool(int workerCount);
  ~RenderWorkerPool();  ///< Joins every worker. Never detaches.

  /// Runs `body(RowRange)` over the band decomposition of `[0, rows)` and joins before returning.
  /// The band grid is a pure function of `rows` and the fixed band height, not of worker count.
  void forEachBand(int rows, const std::function<void(RowRange)>& body);
};
```

`ApplyFilterGraphToPixmap` gains an optional pool pointer; a null pool is the current behaviour.
The primitives gain their `RowRange` parameter. Nothing in the public SVG API changes.

## Performance

### Measurements

All numbers are `-c opt` on a reference multi-core x86-64 Linux host, median of nine repetitions, taken
with a scratch harness linked against the in-tree filter library. They are reproduced in this doc as
the motivation and the acceptance baseline; the benchmark target in Milestone 4 makes them
re-runnable.

**Concurrency headroom**, running the same primitive on N disjoint 900x900 buffers from N threads.
This bounds what banding one buffer can reach on this machine, because it is the same instruction mix
against the same memory system with no shared state:

| Workers | `diffuseLighting` wall | per buffer | `gaussianBlur` wall | per buffer |
| ------- | ---------------------- | ---------- | ------------------- | ---------- |
| 1       | 36.8 ms                | 36.8 ms    | 22.5 ms             | 22.5 ms    |
| 2       | 37.2 ms                | 18.6 ms    | 23.9 ms             | 12.0 ms    |
| 4       | 37.4 ms                | 9.3 ms     | 27.9 ms             | 7.0 ms     |
| 8       | 37.8 ms                | 4.7 ms     | 43.6 ms             | 5.5 ms     |

Lighting is compute-bound and holds 7.8x throughput at eight threads. Blur is memory-bandwidth-bound
(streaming loads and stores plus two transposes) and reaches about 4.1x. That asymmetry is expected
and it is the honest ceiling: **blur will scale worse than the gather primitives, and the goal
targets the gather-shaped ones.**

**End-to-end banding prototype**, `convolveMatrix` 3x3 over a 900x900 float buffer, bands staged with
an apron and executed on plain threads, compared byte-for-byte against the whole-buffer result:

| Configuration | Time | Speedup | Bit-differing floats |
| ------------- | ---- | ------- | -------------------- |
| serial, whole buffer | 35.0 ms | 1.00x | reference |
| banded, 1 thread     | 39.9 ms | 0.88x | 0 |
| banded, 2 threads    | 21.6 ms | 1.62x | 0 |
| banded, 4 threads    | 12.3 ms | 2.85x | 0 |
| banded, 8 threads    | 7.9 ms  | 4.41x | 0 |
| banded, 16 threads   | 6.4 ms  | 5.43x | 0 |

Zero bit differences at every thread count is the result that matters. The 0.88x at one thread is the
prototype's band staging and copy-back, which the row-range API removes, so the shipped speedups
should be modestly better than the table.

### Targets and acceptance

- **Scaling target**: at least 4x at eight workers on the gather-shaped primitives, at least 2.5x at
  eight workers on `gaussianBlur`, measured by the Milestone 4 benchmark.
- **Zero-worker regression bar**: the inline path must be within noise of today's timings on the same
  benchmark. The row-range parameter must not cost anything when it is the whole range.
- **End-to-end acceptance**: whole-frame time on at least two filter-heavy corpus documents must
  improve by at least 2x at eight workers, and must not regress at zero workers. See "Testing and
  Validation" for the documents.

## Security / Privacy

SVG input is untrusted, and filter parameters are attacker-influenced (sigma, kernel order, radius,
buffer extent). This design adds no parsing surface and no new allocation that scales with input,
but it does add a thread pool, so the boundaries are:

- **Bounded worker count.** The worker count is a build/embedder setting, never derived from document
  content. No document can cause thread creation, and no filter parameter influences the band grid
  beyond the buffer height that already bounds the serial loop.
- **No new memory scaling.** Bands write into the buffers the serial path already allocates. The only
  new per-band allocation is the small van Herk scratch in `morphology`, which is O(width) per band
  and therefore O(width * workers), bounded by a constant worker count.
- **Existing caps still apply, unchanged.** `FloatPixmap::fromSize` and `gaussianBlur`'s 1 GiB
  allocation ceiling are untouched, and they are evaluated before any band is submitted, so an
  oversized buffer is rejected exactly as it is today rather than being rejected concurrently.
- **Fail-safe, not fail-open.** A pool that cannot start degrades to inline execution. There is no
  path where a threading failure produces partial or uninitialized pixels, because a band that was
  never submitted was never joined and the frame does not proceed past the join.
- **Fuzzing.** The existing SVG render fuzzer runs with the pool enabled at a small non-zero worker
  count on the sanitizer lanes, so hostile filter graphs exercise the band decomposition and the
  reduction combine.

## Testing and Validation

Every invariant below names the CI target that fails when it breaks, per this directory's rule.

- **Band identity, single-threaded** (`@tiny-skia-cpp//src/tiny_skia/filter/tests:tiny_skia_filter_tests`):
  for each primitive, executing a buffer as N row bands is byte-identical to executing it whole.
  Parameterized over band heights that do and do not divide the buffer height, over both edge-mode
  families, and over buffers smaller than one band. This is the invariant that makes everything
  downstream safe, and it is enforceable with no threads at all.
- **Thread-count identity** (new `//donner/svg/renderer/tests:filter_thread_identity_tests`): render
  each corpus document at worker counts {0, 1, 2, 8} and assert byte-equal snapshots across all four.
  Runs over the renderer golden corpus and, on lanes with
  `--//donner/svg/renderer:resvg_test_suite_available=True`, the resvg suite. This is the A/B
  compare-mode pattern the retained-span work uses in
  [0060](0060-tinyskia_retained_raster_cache.md): one corpus driver, two or more modes, byte
  comparison, with the diagnostic naming the first differing pixel.
- **Golden pinning under threads**: a threaded configuration of the existing
  `//donner/svg/renderer/tests:renderer_tests` and `//donner/svg/renderer/tests:resvg_test_suite`,
  so threaded output is pinned to the checked-in goldens and not merely to itself.
- **ThreadSanitizer lane**: `bazel test --config=tsan` over
  `//donner/svg/renderer/tests:filter_thread_identity_tests` and the filter library tests, built with
  `--//donner/svg/renderer:render_worker_pool=true` and run at a non-zero worker count. A green
  default build does not verify flag-gated code, so the lane must build the flag on. This lane is
  required, not advisory.
- **Zero-worker equivalence** (`//donner/svg/renderer/tests:filter_thread_identity_tests`): output at
  worker count zero with the flag compiled **on** must equal output with the flag compiled **off**,
  which is what makes "the default is unchanged" a checked claim.
- **Reduction determinism** (`//donner/svg/renderer/tests:filter_graph_executor_tests`): a paint-input
  subregion computed from banded bounds must equal the whole-buffer bounds, including for content
  whose non-transparent pixels straddle band boundaries.
- **Performance acceptance** (Milestone 4 benchmark): per-primitive and whole-graph timings against
  worker count, plus whole-frame timings on at least two filter-heavy corpus documents. Candidates
  are `donner/svg/renderer/tests/svg_document_render_corpus/filters.svg` and
  `donner/svg/renderer/testdata/filter_spot_light.svg`, with `donner_splash.svg` as a realistic
  mixed-content third; the final pair is chosen in Milestone 4 by which documents actually spend the
  majority of their frame inside `executeFilterGraph`, and that measurement is recorded in this doc.
  Numbers land here only as measured output, never as estimates.

## Preconditions and Sequencing

**The per-layer compositor compose fix is a named precondition for any compositor-layer
parallelism, and it is not met.** `CompositorController::composeLayers` walks segments and layers in
paint order and blits each payload through one renderer whose paint state is **carried across
draws**. `drawPayload` has to call `setPaint(PaintParams{})` before every tile blit specifically
because a preceding direct-render can leave a group's opacity on the renderer, which then dims an
already-composited tile; that is a real bug the reset exists to prevent. Per-layer opacity and blend
mode are not parameters of the compose blit at all today: subtrees that need them are either kept
out of promotion or direct-rendered.

Two layers composed concurrently would therefore share mutable renderer paint state, and the
sequential ordering that makes the reset correct would no longer exist. The fix is to consolidate
compose into a single explicit blit operation carrying its own payload, transform, opacity, and blend
mode, with no dependence on carried renderer state. **Until that lands, compositor-layer parallelism
is not on the table**, and this design does not touch the compositor.

**Compositor tiling is deferred to a follow-up design doc.** Splitting the composed frame into screen
tiles interacts with layer promotion, damage tracking, the split background/foreground fast path, and
the ordering rules above. It is a larger problem than intra-node filter tiling and it should not ride
along on this doc's determinism argument, which is specific to pure per-pixel functions over
immutable inputs.

**Scope of this doc: CPU filter tiling only.** That is the whole of it.

## Recorded Do-Not-Build Decisions

These are conclusions, recorded with their reasoning so they do not have to be rediscovered.

### Do not parallelize Geode command encoding

**Decision: no.** The encoder is not the bottleneck, and cross-thread encoding would break a
documented correctness invariant.

*Not the bottleneck.* Since path residency and cross-entity batching landed, a steady-state Geode
frame is served from resident state rather than from encoding. `GeodeCounters` documents the targets
directly: `pathEncodes` is "`== 0` on an unchanged-geometry frame (the `GeodePathCacheComponent`
serves all paths)", `bufferCreates` is "`== 0` on an unchanged-geometry frame", `textureCreates` is
"`== 0` on repeat-render at the same size", and `submits` is "`== 1` per frame regardless of
layer/filter/mask push depth". `GeodePerf_tests.cc` asserts against those ceilings. Parallelizing a
stage whose steady-state work is already near zero buys nothing, and the first frame that does
encode is dominated by upload and pipeline creation rather than by CPU encoding.

*It would break the write-after-record invariant.* Geode publishes per-draw parameters by writing
into buffers and then recording draws into one command encoder submitted once per frame. Buffer
writes are queue-ordered ahead of every draw in that submit, so **the last write to a slot wins for
every draw in the frame**, regardless of where the draw was recorded. The encoder states this
explicitly at the point where the solo resident path decides whether to write a slot's uniform: a
scene-form write issued after a recorded solo draw "would retroactively change that draw's uniform
(last write wins at submit time)". The solo path avoids the hazard by binding a shared identity
record and never writing a slot a same-frame repeat could overwrite; the batch path avoids it by
giving each instance its own record slot.

That invariant is a property of a **single, ordered** encode stream. Two threads encoding into the
same frame would interleave writes and draws with no defined order between them, and the failure
mode is not a crash or a race a sanitizer reports: it is a draw silently rendering with another
draw's parameters. On top of that, the supporting structures are documented single-threaded by
design (`GeodeBufferPool`: "Not thread-safe; all use is on the renderer's thread";
`GeodeResidentSlab`: "Not thread-safe: allocate/free/beginFrame mutate the free list and bump
allocator ... a document's slab is only touched from one thread"). Making them thread-safe would add
synchronization to the exact path the residency work made cheap.

### Do not parallelize render-tree branches

**Decision: no.** The ECS registry does not tolerate structural mutation concurrent with view
iteration, and the measured win did not exist.

*The precise constraint.* Component storage is paged, so **inserting** a component never relocates
existing ones. **Erasing** one is swap-and-pop: the storage moves its last element into the freed
slot. The renderer relies on this today, and the tree documents why it is dangerous. `PathShape` is a
borrowed view, and `RendererInterface.h` records the consequence: a pointer into a component erased
while borrowed "does not dangle into freed memory, it silently starts naming a different entity's
geometry. That renders the wrong shape with no crash, and neither ASAN nor a sanitizer build will
flag it." The stated precondition is that no component of that type is erased between the borrow and
its last use, and it holds only because every removal site runs outside draw traversal.

Draw traversal is not free of structural mutation either. `RendererDriver` snapshots the entity slice
into a `std::vector<Entity>` **before** traversing precisely because `preRenderFeImageFragments`
emplaces into and sorts `RenderingInstanceComponent` storage, "which would invalidate a
live-iterating view". So the current single-threaded design already has to sequence structural
mutation against iteration by hand.

Branch-level tasks would put concurrent iteration and concurrent structural mutation into the same
registry, against exactly this storage model. The failure mode is the worst kind: correct-looking
pixels from the wrong entity, invisible to ThreadSanitizer because the swap-and-pop is a
well-synchronized write to memory the reader is legitimately allowed to read. The tree already
carries the conclusion this leads to: a `--config=tsan` build exists and is run against the document
concurrency and render-snapshot tests, and what
[0033](0033-multithreading_and_dom_lifetime.md) shipped in response to this class of problem was
scoped read/write access guards plus immutable render snapshots, not finer-grained locking inside the
registry. The ECS boundary is held by construction because instrumentation cannot see the failure.

*And the win was not there.* Branch-level task shapes produced no measurable improvement on the
corpus, because per-branch cost is small relative to task submission and join overhead: a typical
subtree is tens of microseconds of shape drawing, against a filter node's tens of milliseconds. The
parallelism worth having in a Donner frame is concentrated inside a few very large leaves, which is
what this design targets, and not spread across many small branches. This last point is a
measurement whose harness does not exist in-tree; it is recorded here as a design assumption and the
Milestone 4 benchmark is the place to falsify it cheaply if anyone wants to revisit.

## Alternatives Considered

- **Parallelize across filter graph nodes.** Rejected as the primary mechanism: the common graph is a
  chain, so there is nothing to overlap, and the branchy cases have unequal branch costs that waste
  workers. It composes with this design later if it is ever worth it, because tiling a node does not
  constrain how nodes are scheduled.
- **Parallelize across filter layers (several filtered elements at once).** Rejected: filter layers
  are pushed and popped inside an ordered draw traversal against renderer state, so this is a
  rendering-order change, not a threading change, and it would inherit exactly the carried-state
  problem described under "Preconditions and Sequencing".
- **Two-dimensional tiles instead of row bands.** Rejected for now. Column splitting is unavailable
  for the separable primitives without restarting the running-sum accumulator mid-row, which changes
  float accumulation order and therefore output bytes. Row bands give sufficient parallelism for
  realistic buffer heights (a 900-row buffer supports far more bands than workers), and they keep
  every inner loop and its vectorized tail exactly as they are today.
- **A pool per feature (one for filters, one for prepare).** Rejected: two pools oversubscribe the
  machine against each other, double the shutdown and failure surface, and make the "no detached
  threads, one owner" property twice as hard to hold. One pool with one flag is the contract.
- **Task-graph or coroutine scheduling for the filter graph.** Rejected as premature. The dependency
  structure inside a node is a straight sequence of passes with a join between them; expressing that
  as a general task graph adds a scheduler to debug without adding parallelism.
- **Accept small pixel differences across thread counts.** Rejected outright. Output that depends on
  the machine's core count is not a rendering engine; it is a source of unreproducible bug reports,
  and it would make every golden test flaky in a way that is unfixable after the fact.

## Open Questions

- **Band height.** The default is a tuning parameter with a floor, chosen so per-band prologue and
  scheduling cost stay well under the band's work. Milestone 4 measures the curve; a fixed 32 or 64
  rows and a "no more bands than 4x workers" cap are the starting candidates.
- **Which primitives are worth banding at all.** `flood` and `offset` are memcpy-shaped and may not
  repay a submission. The decomposition is uniform for correctness, but the pool can hold a per-
  primitive minimum-rows threshold below which submission is inline. That threshold must not change
  output, which the band-identity test already guarantees.
- **Blur's bandwidth ceiling.** At 4.1x on eight threads, `gaussianBlur` is bandwidth-bound. Whether
  fusing the transpose into the pass, or blocking the transposes differently, moves that number is an
  open optimization, not a blocker.
- **Interaction with 0060's prepare stage sharing the pool.** When both are enabled, prepare bands and
  filter bands contend for the same workers within a frame. The join structure makes this correct;
  whether it is *fast* needs measuring once both exist.
- **Editor async-render interaction.** The editor already renders on a worker. Nesting the pool inside
  that worker is correct (the pool's threads are its own), but the total thread budget across the
  editor's renderer worker and this pool needs an explicit policy before the editor turns it on.

## Assumptions To Validate

Stated as assumptions rather than facts, and each names where it gets settled:

- **Branch-level parallelism produced no measurable win on the corpus.** Recorded above as the
  rationale for a do-not-build decision; no in-tree harness reproduces it today. Milestone 4's
  benchmark is where it can be cheaply falsified if anyone wants to reopen it.
- **Whole-frame filter share.** The measurements above are of the filter library in isolation. The
  claim that filter execution is the majority of a filtered *frame* is strongly implied by the node
  costs but is not directly measured end-to-end; Milestone 4 measures it and picks the two acceptance
  documents from that measurement.
- **Scaling on other hosts.** All numbers are from one reference multi-core x86-64 Linux host. Scaling
  on a small-core-count machine, and on the other architectures the golden corpus runs on, is expected
  to be lower for the bandwidth-bound primitives and is measured in Milestone 4.
- **`morphology` and `gaussianBlur` band cleanly through their transposes.** The reasoning is solid
  (the transpose is a disjoint blocked copy and each pass is row-wise) but the prototype covered
  `convolveMatrix`, not these. Milestone 1's band-identity tests settle it before any thread exists,
  which is the point of doing Milestone 1 single-threaded.

## Future Work

- [ ] Compositor tiling, in its own design doc, once the compose blit is consolidated.
- [ ] Compositor-layer parallelism, gated on that same consolidation.
- [ ] Band the uint8 filter paths, which currently only matter for a few call sites.
- [ ] Evaluate sharing the pool with the editor's async-render worker rather than nesting pools.
- [ ] Revisit cross-node parallelism for genuinely branchy filter graphs, if the corpus ever shows
      them mattering.
