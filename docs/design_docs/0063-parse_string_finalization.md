# Design: Source-Backed XML Parse Strings

**Status:** Implemented. Source-backed strings and copy-on-write storage are retained; compact
finalization was measured and rejected.
**Author:** GPT-5
**Created:** 2026-08-22

## Summary

XML parsing now constructs eligible persistent strings as `RcString` substrings of the document's
owned source snapshot. This removes one allocation for each ordinary long XML name or value without
depending on the caller's input buffer.

`XMLSourceStore` owns that snapshot through shared storage. Before a structured edit mutates shared
source bytes, it copies the current source once. Existing DOM strings continue to reference the
immutable prior snapshot, while anchors, diagnostics, and subsequent edits use the new source.

Short persistent results are copied into `RcString` inline storage. The parser cursor itself remains
reference-backed, so this detaches common short XML names and values without copying the entire
parse input or changing source-offset calculations.

An alternative compact/intern finalization pass was implemented and benchmarked. It reduced
retained memory by about 7%, but increased allocation work and peak memory and remained about 6%
slower. The compact API and traversal were therefore removed; their allocation and timing results
remain as evidence for the decision.

## Goals

- Avoid allocating string bytes for direct long XML source substrings during parsing.
- Remove all dependence on the caller-owned input buffer.
- Preserve source retention, structured editing, source anchors, and diagnostic byte offsets.
- Measure complete-parse allocation calls, requested bytes, retained bytes, and peak live bytes on
  a repeated-attribute fixture and `donner_splash.svg`.
- Keep the implementation linear and bounded for hostile input.

## Non-Goals

- Removing `XMLSourceStore`; structured editing requires it.
- Returning public non-owning views or changing public DOM/CSS value APIs.
- Shipping compact/intern finalization when its measured cost exceeds its benefit.
- Interning strings created by runtime mutations or standalone CSS parsing.

## Architecture

```text
caller bytes
    |
    v
XMLSourceStore-owned shared snapshot
    |
    +--> XMLParser cursor and direct substrings
    |       long persistent result -> shared RcString slice
    |       short persistent result -> RcString inline copy
    |       entity/normalized result -> existing owned flattening path
    |
    +--> returned editable document
            source store retains current snapshot
            first mutation copies if DOM strings still share it
            anchors and diagnostics continue against current source
```

`RcString` long storage has an internal type-erased shared owner. A private friend factory allows
`XMLSourceStore` to create an owning full-source value whose substrings preserve the shared owner,
including short intermediate slices. `ChunkedString::toSingleRcString()` then copies a short
persistent result into inline storage, while a long direct substring keeps the snapshot owner.

`XMLSourceStore::replace()` validates the edit and its resource limits before calling
`ensureUniqueSource()`. If parsed strings still share the current source, the store clones it and
then applies the mutation. This is ordinary copy-on-write: the parse result is safe, the caller's
input may disappear immediately, and unchanged DOM strings remain valid after edits.

## Invariants

- `XMLParser::Parse` returns only strings whose bytes outlive the returned document.
- Source-backed `RcString` values never refer to caller-owned memory.
- `XMLSourceStore` never mutates a source buffer while parsed strings share it.
- Parser cursor slices remain reference-backed so source locations retain their original offsets.
- Short persistent strings use inline storage and do not keep an otherwise unneeded snapshot alive.
- Entity expansion and other multi-piece values retain the existing owned flattening behavior.
- The optimization does not alter node locations, anchors, diagnostic ranges, or public string APIs.

## Measured Results

Complete `SVGParser::ParseSVG` allocation results on the measured macOS system, with the returned
document alive at the sample point:

| Workload            | Version       |  Calls | Requested bytes | Retained bytes | Peak live bytes |
| ------------------- | ------------- | -----: | --------------: | -------------: | --------------: |
| Repeated attributes | Parent        | 14,358 |       6,411,849 |      6,066,487 |       6,066,487 |
| Repeated attributes | Source-backed | 13,847 |       6,388,081 |      6,066,527 |       6,066,527 |
| Donner Splash       | Parent        | 23,000 |       9,042,703 |      8,243,148 |       8,243,268 |
| Donner Splash       | Source-backed | 22,759 |       9,006,570 |      8,237,725 |       8,237,845 |

The source-backed path removes 511 complete-parse allocations from the repeated-attribute fixture
and 241 from the splash. Total retained memory changes little because source bytes were already
retained for editing and SVG geometry, styles, and ECS containers dominate the full parse.

### Rejected Compact/Intern Experiment

The final compact experiment kept short values inline and interned only long values:

| Workload            |  Calls | Requested bytes | Retained bytes | Peak live bytes |
| ------------------- | -----: | --------------: | -------------: | --------------: |
| Repeated attributes | 13,852 |       6,396,574 |      5,643,255 |       6,066,687 |
| Donner Splash       | 22,764 |       9,043,238 |      7,689,194 |       8,265,306 |

Compared with source retention, compact mode saved about 7.0% retained memory on repeated
attributes and 6.7% on the splash. It added five allocation calls in each workload, increased
requested and peak bytes slightly, and changed the alternating splash parse mean from about
2.579 ms to 2.733 ms, a 5.95% regression. The traversal, public mode switch, and map rebuilding were
not justified by that tradeoff, so the production implementation was removed.

## Tests and Benchmarks

- `SvgParseAllocation_tests.cc` gates allocation calls, requested bytes, retained live bytes, and
  peak live bytes for repeated attributes and the canonical splash.
- `SvgParsePerfBench.cpp` accepts the splash as a Bazel runfile and reports repeatable median parse
  timing without renderer work.
- XML tests prove a shared long attribute survives source copy-on-write and a short persistent
  attribute uses inline storage without retaining the snapshot.
- `XMLSourceStore` tests prove an owning source reference survives replacement.
- Existing XML/SVG parser, node-location, structured-editing, and diagnostic tests remain the
  compatibility gates; structured XML and SVG fuzzers remain the hostile-input gates.

## Security and Resource Behavior

The change introduces no new parsing loops or attacker-controlled recursion. Input, tree, depth,
attribute, and entity limits remain authoritative. Source replacement still validates range,
UTF-8, source-size, and anchor work limits before copying or mutating storage. Copy-on-write is at
most one full-source allocation for a particular shared snapshot; later edits use the store's
unique current source unless new parsed values share it.

## Alternatives Considered

- **Compact and intern at parse end:** Rejected after the measured experiment above.
- **Copy every string at parse end:** Lifetime-safe but retains the allocation traffic this change
  is intended to avoid.
- **Mutate the shared source in place:** Rejected because it invalidates or changes DOM strings.
- **Permanent document interner:** Rejected because it retains dead values and adds lifetime policy
  to all later mutations.
- **Non-owning public views:** Rejected because callers could outlive the parser or source buffer.

## Future Work

- Revisit compacting only if a non-editing workload demonstrates a materially larger retained-memory
  win or a finalization strategy meets the 5% CPU budget.
- Evaluate the same owned-substring technique for standalone CSS only after defining its source
  lifetime boundary.
