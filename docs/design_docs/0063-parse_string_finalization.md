# Retrospective: XML Parse String Storage Experiment

**Status:** Retrospective
**Type:** Retrospective
**Author:** GPT-5.6 Sol
**Created:** 2026-08-22

## Summary

- We prototyped direct source-backed XML strings and a post-parse compact/intern mode.
- Source slices reduced allocation calls but violated string-boundary and retained-memory safety;
  compacting saved about 7% of tracked live requested bytes but remained about 6% slower.
- Neither representation ships. Permanent allocation, peak-live-requested-byte, and splash timing
  benchmarks remain so a simpler future approach can be evaluated from a stable baseline.

## Scope

- Reviewed XML parse strings, `RcString` boundary semantics, `XMLSourceStore`, incremental structured
  edits, allocation tracking, the repeated-attribute fixture, and `donner_splash.svg`.
- Compared the current owned-string baseline with source-slice, compact/intern, and short-string
  detachment prototypes.
- This retrospective does not change production string/value APIs, ship either result mode,
  redesign standalone CSS ownership, remove editable source storage, or treat requested C++ payload
  bytes as allocator-resident memory or RSS.

## Outcome

- Production parser, `RcString`, `XMLSourceStore`, structured-editing, and diagnostic behavior match
  the parent implementation.
- `//donner/benchmarks:svg_parse_allocation_tests` monitors allocation calls, total requested bytes,
  live requested bytes, and peak live requested bytes for repeated attributes and Donner Splash.
- Peak-live tracking is isolated to that dedicated untimed test binary. Existing timing and renderer
  comparison binaries retain their prior allocator, so the richer tracker cannot distort their
  wall-clock workloads.
- `//donner/benchmarks:svg_parse_perf_bench` carries the splash as a Bazel runfile.

The final owned-string baseline on arm64 macOS 26.5.2, Bazel 8.7.0, and Apple clang 21.0.0 is:

| Workload            |  Calls | Requested bytes | Live requested bytes | Peak live requested bytes |
| ------------------- | -----: | --------------: | -------------------: | ------------------------: |
| Repeated attributes | 14,358 |       6,411,849 |            6,066,487 |                 6,066,487 |
| Donner Splash       | 23,000 |       9,042,703 |            8,243,148 |                 8,243,268 |

The tracker overrides C++ `operator new` in the dedicated test process. These figures exclude its
headers/alignment padding, allocator metadata, direct `malloc`, other runtimes, and process RSS. CI
assertions are broad cross-platform resource ceilings, not proof of an optimization.

## Code Review Findings

1. **Source-backed public values were not NUL-terminated - fixed by removal.** A direct long
   attribute slice ends before an XML quote or delimiter, so `value.data()[value.size()]` is not
   NUL. Ordinary parsed long values previously used terminated owning storage. The candidate could
   expose adjacent XML bytes to C-string consumers, so it was removed before publication.
2. **Incremental edits could retain quadratic fragment history - fixed by removal.** Opening-tag and
   subtree edits reparse growing fragments but import only changed values. Direct slices could retain
   one historical fragment per edit, outside the current-source limit. Copy-on-write protected
   mutation but did not bound aggregate retained snapshots.
3. **Small public values could retain whole source snapshots - fixed by removal.** Copying one
   attribute or node value could preserve unrelated input bytes after the document died, changing
   memory and data-retention expectations.
4. **Compact finalization missed its tradeoff gate - accepted as a rejected experiment.** The final
   candidate used 13,852 calls / 5,643,255 live requested bytes on repeated attributes and 22,764
   calls / 7,689,194 live requested bytes on the splash. Compared with source slices, live requested
   bytes fell about 7.0% and 6.7%, but allocation and peak work rose and alternating splash parse
   means changed from about 2.579 ms to 2.733 ms, a 5.95% regression.
5. **The first live-byte tracker protocol was generation-racy - fixed.** Independent review found
   that separate atomic counter and generation operations could split one allocation across scope
   transitions. The dedicated tracker now serializes scope, allocation, and free accounting and
   ignores frees from untracked or older generations.
6. **Putting the stronger tracker in a timing binary changed the workload - fixed.** Header
   allocation and lock serialization apply even outside an active measurement scope. The permanent
   tracker therefore lives only in `svg_parse_allocation_tests`; existing timed binaries are
   unchanged.

## Fragility and Refactoring Opportunities

- `RcString::substr()` can intentionally share a non-terminal interior range. A future source-slice
  design needs an explicit non-C-string value type or a NUL-terminated arena, rather than silently
  broadening that sharp edge to routine DOM parse results.
- A NUL-terminated parse arena could copy persistent values into one bounded page without interning,
  but it duplicates bytes beside editor source and still needs explicit incremental-import policy.
- Aggregate retained-snapshot budgeting would bound amplification but would not solve termination or
  hidden whole-source retention at public value boundaries.
- The test-only peak tracker and older timing-benchmark tracker deliberately remain separate to keep
  timed workloads stable. If they are unified later, allocator isolation must be preserved by
  separate binaries rather than a disabled runtime flag.

## Testing Review

- Parser, XML source-store, SVG parser, structured-editing, and sanitizer tests were green on the
  unsafe source candidate. They checked value equality and mutation behavior, but not NUL at a
  parsed long value boundary or retained bytes across adversarial edit sequences.
- Independent code and security reviews supplied the missing lifetime and amplification analysis;
  the production representation was then removed.
- The permanent allocation target tests exact basic accounting, prior/untracked generation frees,
  aligned allocation, zero-sized/nothrow allocation, concurrent allocation/free accounting, and
  rejection of overlapping scopes.
- Focused final validation:

```sh
bazel test //donner/benchmarks:svg_parse_allocation_tests \
  //donner/base/xml:xml_tests //donner/svg/parser:parser_tests \
  //donner/editor/tests:structured_editing_stress_tests --test_output=errors
```

- Allocation evidence:

```sh
bazel test //donner/benchmarks:svg_parse_allocation_tests --test_output=all
```

- Compact timing used optimized builds, 25 timed parses after three warmups, and six alternated
  source/compact runs. `--compact` exists only at the recorded experimental checkpoint:

```sh
bazel run -c opt //donner/benchmarks:svg_parse_perf_bench -- \
  --iterations=25 --warmup=3 --repeat=1 donner_splash.svg
bazel run -c opt //donner/benchmarks:svg_parse_perf_bench -- \
  --iterations=25 --warmup=3 --repeat=1 --compact donner_splash.svg
```

## Process Review

- Measurement-first discipline worked for compacting: the mode was removed when its retained-byte
  benefit did not justify CPU and implementation complexity.
- Equality and structured-editing tests created false confidence in source slices because they did
  not encode the complete public value and aggregate-retention contracts. Independent review before
  publication was the effective gate.
- Review also caught measurement observer effects in the shared timing binary. Isolating the strong
  allocator in an untimed test keeps the retained benchmark evidence honest.
- Experimental commits preserve the prototypes and measurements, while the final review diff
  deletes dead production paths and exposes only the benchmark infrastructure and retrospective.

## Actions

- [x] Remove source-slice, copy-on-write, and compact/intern production paths.
- [x] Retain repeated-attribute and Donner Splash allocation/peak-live-requested-byte coverage in
      `//donner/benchmarks:svg_parse_allocation_tests`.
- [x] Isolate the stronger allocator from timed benchmark binaries.
- [x] Add cross-generation, aligned, zero-sized, concurrent, and overlapping-scope tracker tests.
- [ ] Run the repository-wide `//...` gate on the exact final candidate before publication.
- [ ] If source slices are revisited, first design explicit termination semantics, public value
      retention, incremental-import detachment, and aggregate snapshot budgets, with CI tests for
      each invariant.
