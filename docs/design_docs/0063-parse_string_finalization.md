# Design: Parse-Lifetime String References and Finalization

**Status:** Draft
**Author:** GPT-5
**Created:** 2026-08-22

## Summary

SVG parsing currently converts XML names, attribute values, and text slices into owning
`RcString` values as the XML tree is built. This is lifetime-safe, but it spreads allocation work
through the latency-sensitive parse and repeatedly allocates identical names such as `path`,
`fill`, and `transform`.

This design introduces a bounded XML parse-string session. Direct source substrings share the
source storage during XML parsing. After the XML tree is complete, a mandatory finalization pass
deduplicates retained strings and moves their bytes into compact owned pages before SVG projection
or any structured edit can occur.

The structured-editing source cannot be discarded: `XMLSourceStore` must continue to own the
canonical serialized SVG and its anchors. The guarantee is narrower: no DOM or SVG string value
returned from parsing aliases that mutable source storage.

## Goals

- Move ordinary XML string-byte allocation out of the parser loop and into one explicit
  finalization phase.
- Intern identical tag names, attribute names, attribute values, and XML node values.
- Ensure every returned XML/SVG string is independent of mutable source bytes.
- Preserve structured editing, incremental reparsing, source anchors, and diagnostic byte offsets.
- Reduce XML/SVG parse allocation calls by at least 30% on the retained corpus without regressing
  parse CPU by more than 5%.
- Keep finalization time and memory linear and bounded for hostile input.

## Non-Goals

- Removing `XMLSourceStore` from parsed documents. Structured editing requires it.
- Returning public non-owning string views or changing public DOM/CSS APIs.
- Interning arbitrary runtime strings created after parsing.
- Compacting renderer, font, image, or geometry payloads.
- Applying the first milestone to standalone CSS parsing. CSS can adopt the same session only after
  the XML ownership boundary is proven.
- Repacking every prior compact page after each incremental edit.

## Next Steps

- Confirm this ownership boundary and performance rubric.
- Add phase-separated XML/SVG allocation benchmarks before changing representation.
- Prototype source-backed `RcString` slices and finalization on the XML tree only.

## Implementation Plan

- [ ] Milestone 1: Establish allocation, retained-memory, and lifetime baselines.
  - [ ] Extend the SVG parse benchmark with C++ allocation calls/requested bytes split into XML
        parse, string finalization, and SVG projection phases.
  - [ ] Add representative repeated-name, long-value, namespace, entity-expansion, and structured
        editing inputs.
  - [ ] Record parent CPU, allocation, requested-byte, and retained-byte results in this design.
- [ ] Milestone 2: Add source-backed parse strings.
- [ ] Milestone 3: Finalize XML strings into compact owned pages.
- [ ] Milestone 4: Prove structured-editing and diagnostic parity.
- [ ] Milestone 5: Evaluate SVG/CSS adoption and decide whether to extend or stop.

## Requirements and Constraints

- `XMLParser::Parse` must return only strings that remain valid after the caller's input is gone.
- `XMLSourceStore::replace` must never invalidate a DOM string.
- Fatal diagnostics remain valid without a returned document.
- Incrementally parsed fragments must finalize before their values enter the live document.
- Ordered map/set keys must never be mutated in place. Attribute, namespace, and entity maps are
  rebuilt transactionally during finalization.
- Finalization failure must not return a partially remapped document.
- No exceptions, new external dependencies, or public ECS/string-pool APIs.

## Proposed Architecture

```text
caller SVG bytes
      |
      v
XMLSourceStore owned source and anchors
      |
      +--> XMLParser uses source-backed RcString slices
      |        direct source text: zero string-byte allocation
      |        entities/normalization: owned decoded buffers
      |
      v
XML string finalizer
      |        collect bounded persistent string fields
      |        deduplicate exact byte strings
      |        pack long strings into bounded NUL-terminated pages
      |        copy short strings into RcString inline storage
      |        rebuild map/set-backed components transactionally
      v
finalized XML tree independent of source bytes
      |
      v
SVG projection and structured editing
```

### Source-Backed `RcString`

`RcString` long storage gains an internal type-erased shared owner so a slice can keep either the
existing vector storage or `XMLSourceStore` string storage alive. `XMLSourceStore` owns its source
through `std::shared_ptr<std::string>` and exposes an internal full-source `RcString` reference.

`XMLParserImpl` initializes `ChunkedString` from that owning reference instead of from the caller's
`std::string_view`. Single-chunk `ChunkedString::toSingleRcString()` then transfers the shared slice
without copying. Entity-expanded and normalized multi-chunk strings continue to allocate owned
decoded bytes.

This representation is parse-internal. A source-backed `RcString` must not escape
`XMLParser::Parse` because later source edits may reallocate the mutable `std::string`.

### Finalization

The finalizer runs after the complete XML tree is parsed and before `XMLParser::Parse` returns.
It collects mutable string fields from:

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

### Finalization Boundary

Initial SVG parsing becomes:

1. create `XMLSourceStore` and parse against its stable source;
2. finalize and detach all XML strings;
3. construct `SVGParserContext` from `XMLSourceStore::source()` for offsets and line information;
4. project the finalized XML tree into SVG components;
5. return the document with the mutable source store retained for structured editing.

Incremental reparsing uses the same rule: a fragment XML document finalizes before its tree or
values are merged into the live registry.

## Data and State

The compact pages are owned only through finalized `RcString` values. The document context does not
need a permanent global interner, and a page is released when its last string is released. Later
structured edits create ordinary owning `RcString` values and may add new compact pages through
fragment parsing; they never rewrite existing pages.

The finalizer uses a bounded list of field handles and exact byte comparisons. Deduplication work
is charged against a new parse-finalization work budget. If the work budget would be exceeded, the
parser rejects the document with a resource-limit diagnostic rather than returning source-backed
strings or silently falling back to unbounded work.

## Diagnostics and Structured Editing

`ParseDiagnostic` stores owning reason text plus `FileOffset`/`SourceRange` byte positions.
Finalization does not change source bytes, offsets, anchors, or line tables. SVG subparser warnings
continue to remap through `SVGParserContext` using `XMLSourceStore::source()`.

Structured edits start only after finalization. A CI test mutates/reallocates the complete source
buffer after parsing and verifies that long tag names, attribute names/values, and text values remain
unchanged. Existing source-anchor and incremental-edit suites remain required gates.

## Performance

The benchmark reports separate phases so the optimization cannot hide work:

- XML parse CPU and C++ allocation calls/requested bytes;
- string-finalization CPU, transient bytes, and retained compact-page bytes;
- SVG projection CPU and allocations;
- complete `SVGParser::ParseSVG` CPU and allocations.

Acceptance requires:

- at least 30% fewer complete-parse C++ allocation calls on the repeated-name and representative
  SVG corpora;
- no more than 5% complete-parse CPU regression;
- no increase in retained string bytes on the representative corpus;
- no more than 10% peak requested-byte regression during finalization;
- source-backed strings observable only inside the parse session.

If finalization merely moves the same allocation cost or increases peak memory materially, stop and
retain the current owning parse path.

## Security / Privacy

SVG/XML input is hostile. Source size, element count, nesting depth, attribute count, entity
expansion, and parsed payload budgets remain authoritative. Finalization adds checked arithmetic for
field counts, page bytes, separators, and offsets.

Compact pages have a fixed maximum size. A value larger than one page receives its own bounded
storage. Deduplication has an explicit work budget, avoiding attacker-controlled unbounded hash or
comparison work. No source-backed string can cross the finalization boundary; the invariant is
enforced by `//donner/base/xml:xml_tests` and the structured XML/SVG fuzzers.

No sensitive data leaves the process, and the change adds no logging, network, or persistence
surface beyond the existing document source store.

## Testing and Validation

- Unit tests for shared-source `RcString` slices, SSO behavior, NUL termination, and page lifetime.
- XML parser equality tests for every node type, namespaces, entities, Unicode, empty strings, and
  repeated names/values.
- A source-detachment test that forces `XMLSourceStore` reallocation after parse.
- Structured editing tests for attribute/text insert, replace, remove, subtree reparse, diagnostics,
  undo/redo projection, and source-anchor resolution.
- Exact diagnostic range/message comparisons on parent and candidate.
- Allocation/timing benchmarks for direct slices and decoded fallbacks.
- `//donner/base/xml:xml_parser_structured_fuzzer` and
  `//donner/svg/parser:svg_parser_structured_fuzzer` under ASan/libFuzzer.
- Full `bazel test //...`, lint, and generated CMake validation before publication.

## Rollout Plan

Land the benchmark first. Keep source-backed storage internal and replace the owning path in one XML
parser change only after detachment tests pass. Do not add a runtime flag: if the candidate misses
the performance or memory gates, revert the representation and retain the benchmark.

## Alternatives Considered

- **Keep source-backed strings for the document lifetime:** Rejected. `XMLSourceStore` is mutable,
  so edits can invalidate slice pointers and silently corrupt DOM values.
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
- What exact finalization work budget preserves the current accepted-input envelope without opening
  a comparison-work amplification path?
- Should compact pages be 32 KiB or 64 KiB after measuring allocator and locality effects?
- Does the shared-owner `RcString` factory belong in the public base library with an internal tag,
  or should XML storage be a friend-only construction path?

## Future Work

- [ ] Reuse the proven parse-string session in standalone CSS parsing.
- [ ] Evaluate periodic reinterning after many structured edits using explicit operator action or a
      bounded maintenance threshold.
