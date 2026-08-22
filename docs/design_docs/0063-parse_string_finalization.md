# Design: Parse-Lifetime String References and Finalization

**Status:** In Progress. Allocation and peak-live-memory baselines are gated; source-backed string
storage is the active milestone.
**Author:** GPT-5
**Created:** 2026-08-22

## Summary

SVG parsing currently converts XML names, attribute values, and text slices into owning
`RcString` values as the XML tree is built. This is lifetime-safe, but it spreads allocation work
through the latency-sensitive parse and repeatedly allocates identical names such as `path`,
`fill`, and `transform`.

This design introduces a bounded XML parse-string session with two result modes:

- **Source-retained:** Direct substrings remain `RcString` slices into an owned immutable source
  snapshot. `XMLSourceStore` uses copy-on-write before the first source mutation, so existing DOM
  strings remain valid while structured editing advances the mutable source.
- **Compact:** A mandatory finalization pass deduplicates retained strings into compact owned pages,
  then the parse backing and source store are released when the caller does not need source editing.

XML attributes are the first proven slice because their names and values dominate ordinary XML
string traffic and their ordered-map storage makes the lifetime rules testable. Both modes remove
dependence on the caller's input buffer. Only source-retained mode keeps a source projection.

## Goals

- Move ordinary XML string-byte allocation out of the parser loop and into one explicit
  finalization phase.
- Intern identical tag names, attribute names, attribute values, and XML node values.
- Let source-retained documents share immutable source storage safely across structured edits.
- Ensure compact documents retain neither mutable source bytes nor parse backing.
- Preserve structured editing, incremental reparsing, source anchors, and diagnostic byte offsets.
- Reduce XML/SVG parse allocation calls by at least 30% on the retained corpus without regressing
  parse CPU by more than 5%.
- Keep finalization time and memory linear and bounded for hostile input.

## Non-Goals

- Removing `XMLSourceStore` from source-retained documents. Structured editing requires it.
- Returning public non-owning string views or changing public DOM/CSS APIs.
- Interning arbitrary runtime strings created after parsing.
- Compacting renderer, font, image, or geometry payloads.
- Applying the first milestone to standalone CSS parsing. CSS can adopt the same session only after
  the XML ownership boundary is proven.
- Repacking source-retained documents or every prior compact page after each incremental edit.

## Next Steps

- Confirm the two result modes, copy-on-write boundary, and performance rubric.
- Prototype source-backed `RcString` slices for source-retained parses.
- Preserve the baseline gates while adding copy-on-write source storage.

## Implementation Plan

- [x] Milestone 1: Establish allocation, retained-memory, and lifetime baselines.
  - [x] Add deterministic C++ allocation calls, requested bytes, retained live bytes, and peak live
        requested bytes to the allocation tracker.
  - [x] Add repeated-attribute and canonical `donner_splash.svg` parse gates.
  - [x] Record parent allocation, requested-byte, retained-byte, and peak-live-byte results.
- [ ] Milestone 2: Add source-backed parse strings and copy-on-write source retention.
  - [ ] Add shared external backing support to `RcString` without changing its public value API.
  - [ ] Make `XMLSourceStore` source storage copy-on-write.
  - [ ] Route direct XML attribute names and values through source-backed slices.
  - [ ] Prove edited and unchanged attributes remain correct across the first and later edits.
- [ ] Milestone 3: Add compact/intern finalization for non-source-retained parses.
- [ ] Milestone 4: Prove structured-editing and diagnostic parity.
- [ ] Milestone 5: Evaluate SVG/CSS adoption and decide whether to extend or stop.

## Requirements and Constraints

- `XMLParser::Parse` must return only strings that remain valid after the caller's input is gone.
- `XMLSourceStore::replace` must copy source storage before mutating bytes referenced by a DOM
  string.
- Fatal diagnostics remain valid without a returned document.
- Incrementally parsed fragments use compact mode before their values enter a source-retained live
  document, preventing a whole-source clone on every later edit.
- Ordered map/set keys must never be mutated in place. Attribute, namespace, and entity maps are
  rebuilt transactionally during finalization.
- Finalization failure must not return a partially remapped document.
- No exceptions, new external dependencies, or public ECS/string-pool APIs.

## Proposed Architecture

```text
caller SVG bytes
      |
      v
XMLSourceStore shared source snapshot and anchors
      |
      +--> XMLParser uses source-backed RcString slices
               direct source text: zero string-byte allocation
               entities/normalization: owned decoded buffers
      |
      +--> source-retained result
      |        keep immutable snapshot slices
      |        copy source storage before first mutation
      |        compact every incremental fragment before merge
      |
      +--> compact result
               collect bounded persistent string fields
               deduplicate exact byte strings
               pack long strings into bounded NUL-terminated pages
               rebuild map/set-backed components transactionally
               release source store and parse backing
```

### Source-Backed `RcString`

`RcString` long storage gains an internal type-erased shared owner so a slice can keep either the
existing vector storage or an `XMLSourceStore` snapshot alive. `XMLSourceStore` owns its current
source through `std::shared_ptr<std::string>` and exposes an internal full-source `RcString`
reference.

`XMLParserImpl` initializes `ChunkedString` from that owning reference instead of from the caller's
`std::string_view`. Single-chunk `ChunkedString::toSingleRcString()` then transfers the shared slice
without copying. Entity-expanded and normalized multi-chunk strings continue to allocate owned
decoded bytes.

Source-retained results may keep these `RcString` slices because the referenced snapshot becomes
immutable. Before `XMLSourceStore::replace` mutates shared storage, it clones the complete current
source and switches the store to the new unique copy. Compact results replace every source-backed
slice and release the store before return.

Short source substrings continue to use `RcString` inline storage and need neither snapshot
retention nor finalization. Long XML attribute values provide the first benchmarked shared-slice
path.

### Compact-Mode Finalization

The finalizer runs only for compact results, after the complete XML tree is parsed and before
`XMLParser::Parse` returns. It collects mutable string fields from:

- element `TreeComponent` qualified names;
- `AttributesComponent` names and values;
- `XMLValueComponent` values;
- namespace declarations and caches;
- retained entity declaration names and values.

Short strings remain in `RcString` inline storage. Unique long strings are packed into bounded
pages with a NUL byte after every value so `RcString::data()` keeps its null-termination contract.
Each finalized long `RcString` is a slice sharing its compact page.

Components whose ordering depends on string bytes are rebuilt rather than mutated. The finalizer
constructs replacement attribute/entity/namespace containers, validates them, and swaps only after
the replacement is complete.

### Result Boundaries

Source-retained SVG parsing becomes:

1. create `XMLSourceStore` and parse against its shared source snapshot;
2. return the XML tree with eligible `RcString` slices still sharing that immutable snapshot;
3. project the XML tree into SVG components, transferring shared `RcString` values where possible;
4. retain the source store and anchors for structured editing;
5. on the first source mutation, copy shared current source storage before editing it.

Compact SVG parsing becomes:

1. create a parse-lifetime source store and parse against its shared snapshot;
2. finalize and detach all XML strings;
3. use the parse source through SVG projection for offsets and line information;
4. release the source store before returning the SVG document.

Incremental reparsing of a source-retained document always uses compact mode for the fragment. Its
tree and values therefore enter the live registry with no reference to the current mutable source,
and later edits do not repeatedly clone the whole document.

## Data and State

In source-retained mode, the source store and long direct XML strings share one snapshot after
parse. The first structured edit clones that snapshot only when it is still shared. Unchanged DOM
strings continue to reference the immutable prior snapshot; edited DOM values are replaced with
new owning strings. Because incremental fragments are compacted, the current source remains unique
after the first clone instead of cloning once per keystroke.

The source resource budget accounts for both the current source and immutable snapshots still held
by DOM strings. An edit is rejected transactionally if copy-on-write would exceed the retained
source-snapshot budget.

The compact pages are owned only through finalized `RcString` values. The document context does not
need a permanent global interner, and a page is released when its last string is released. Later
runtime mutations create ordinary owning `RcString` values; compact documents have no source store
and source-retained fragment parses never add references to the mutable source.

The finalizer uses a bounded list of field handles and exact byte comparisons. Deduplication work
is charged against a new parse-finalization work budget. If the work budget would be exceeded, the
parser rejects the document with a resource-limit diagnostic rather than returning source-backed
strings or silently falling back to unbounded work.

## Diagnostics and Structured Editing

`ParseDiagnostic` stores owning reason text plus `FileOffset`/`SourceRange` byte positions.
Finalization does not change source bytes, offsets, anchors, or line tables. SVG subparser warnings
continue to remap through `SVGParserContext` using `XMLSourceStore::source()`.

Structured edits start only after the source-retained parse result is complete. A CI test forces
copy-on-write reallocation of the complete source and verifies that long tag names, attribute
names/values, and text values remain unchanged. Compact-mode tests verify `hasSourceStore()` is
false while diagnostic ranges produced during parsing remain identical. Existing source-anchor and
incremental-edit suites remain required gates.

## Performance

The benchmark reports both result modes and separate phases so the optimization cannot hide work:

- XML parse CPU and C++ allocation calls/requested bytes;
- string-finalization CPU, transient bytes, and retained compact-page bytes;
- SVG projection CPU and allocations;
- complete `SVGParser::ParseSVG` CPU and allocations.
- first structured-edit copy-on-write CPU and retained snapshot bytes for source-retained mode.

Parent allocation baselines on the measured macOS system:

| Workload            |  Calls | Requested bytes | Retained bytes | Peak live bytes |
| ------------------- | -----: | --------------: | -------------: | --------------: |
| Repeated attributes | 14,358 |       6,411,849 |      6,066,487 |       6,066,487 |
| Donner Splash       | 23,000 |       9,042,703 |      8,243,148 |       8,243,268 |

The CI ceilings include portability headroom for standard-library layout differences. Source
loading is outside the measured scope, and the parsed document remains alive when retained and peak
bytes are sampled.

Acceptance requires:

- at least 30% fewer complete-parse C++ allocation calls on the repeated-name and representative
  SVG corpora;
- no more than 5% complete-parse CPU regression;
- no increase in retained string bytes on the representative corpus;
- no more than 10% peak requested-byte regression during finalization;
- no more than one full-source copy across a sequence of incremental edits;
- source-backed strings observable only in source-retained results.

If finalization merely moves the same allocation cost or increases peak memory materially, stop and
retain the current owning parse path.

## Security / Privacy

SVG/XML input is hostile. Source size, element count, nesting depth, attribute count, entity
expansion, and parsed payload budgets remain authoritative. Finalization adds checked arithmetic for
field counts, page bytes, separators, and offsets.

Compact pages have a fixed maximum size. A value larger than one page receives its own bounded
storage. Deduplication has an explicit work budget, avoiding attacker-controlled unbounded hash or
comparison work. Source-retained snapshot bytes have an aggregate budget, and copy-on-write checks
that budget before cloning. Compact results cannot retain source-backed strings; source-retained
results cannot mutate shared storage. These invariants are enforced by
`//donner/base/xml:xml_tests` and the structured XML/SVG fuzzers.

No sensitive data leaves the process, and the change adds no logging, network, or persistence
surface beyond the existing document source store.

## Testing and Validation

- Unit tests for shared-source `RcString` slices, SSO behavior, NUL termination, and page lifetime.
- XML parser equality tests for every node type, namespaces, entities, Unicode, empty strings, and
  repeated names/values.
- A source-detachment test that forces `XMLSourceStore` reallocation after parse.
- A source-retained attribute test proving unchanged long values survive copy-on-write while the
  edited value changes.
- A multi-edit test proving only the first edit clones the full source and compact fragments do not
  re-share it.
- Compact-mode tests proving the returned document has no source store or parse-backing references.
- Structured editing tests for attribute/text insert, replace, remove, subtree reparse, diagnostics,
  undo/redo projection, and source-anchor resolution.
- Exact diagnostic range/message comparisons on parent and candidate.
- Allocation/timing benchmarks for direct slices and decoded fallbacks.
- `//donner/base/xml:xml_parser_structured_fuzzer` and
  `//donner/svg/parser:svg_parser_structured_fuzzer` under ASan/libFuzzer.
- Full `bazel test //...`, lint, and generated CMake validation before publication.

## Rollout Plan

Land the benchmark first. Add an explicit parser result-mode option, preserving source-retained
behavior for editor/structured-editing callers and selecting compact mode for secure static
subdocuments and callers that do not need source projection. Do not infer string lifetime solely
from `ProcessingMode`; source retention is an orthogonal API choice.

Keep source-backed storage internal and replace the owning path in one XML parser change only after
copy-on-write and compact detachment tests pass. If either mode misses its performance or memory
gates, retain the benchmark and revert that mode's representation.

## Alternatives Considered

- **Mutate shared source storage in place:** Rejected. `XMLSourceStore` must copy shared storage
  before mutation so source-retained DOM slices stay valid.
- **Always compact, including editor documents:** Safe but gives up the largest allocation win when
  the source snapshot is already retained for structured editing.
- **Copy every string individually at parse end:** Lifetime-safe but does not intern duplicates or
  compact long-string storage.
- **Permanent global document interner:** Simplifies later insertions but retains dead strings and
  adds synchronization/lifetime policy to every mutation.
- **Handle-indirected mutable strings:** Allows remapping without a traversal but adds an extra
  pointer chase to every `RcString::data()` call and changes a foundational value type.
- **Finalize after SVG projection:** Requires finding and rewriting every `RcString` across all SVG
  components. Finalizing XML first gives SVG projection only stable owned strings.

## Open Questions

- Should the first implementation compact entity declaration storage, or remove that parse-only
  context before return when later incremental parsing cannot use it?
- Should `XMLParser::Parse` default to source-retained mode for compatibility while
  `SVGParser::Options` exposes an explicit compact choice, or should XML callers opt into retention?
- What retained-source-snapshot limit preserves editor usability for a maximum-size document while
  preventing repeated snapshot accumulation?
- What exact finalization work budget preserves the current accepted-input envelope without opening
  a comparison-work amplification path?
- Should compact pages be 32 KiB or 64 KiB after measuring allocator and locality effects?
- Does the shared-owner `RcString` factory belong in the public base library with an internal tag,
  or should XML storage be a friend-only construction path?

## Future Work

- [ ] Reuse the proven parse-string session in standalone CSS parsing.
- [ ] Evaluate periodic reinterning after many structured edits using explicit operator action or a
      bounded maintenance threshold.
