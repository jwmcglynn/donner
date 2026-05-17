# Design: Projected DOM Editing (Bidirectional Source ↔ Canvas)

**Status:** Draft — revised 2026-05-16 after structured-editing postmortem
**Author:** Claude Opus 4.6 (1M context)
**Created:** 2026-04-11
**Reviewed by:** DesignReviewBot, DuckBot, ParserBot, CSSBot, PerfBot, SecurityBot

> **Revision history.** This doc was resurrected from the prototype
> `jwmcglynn/donner-editor` repo (commit `da076ec`, "Structured text
> editor with bidirectional sync") and initially adapted to the
> in-tree M2/M3 mutation seam. The six-bot specialist review surfaced
> a framing gap (the text↔canvas peer model is wrong; XML is the
> spine), six in-tree APIs the initial draft proposed re-inventing,
> one load-bearing in-tree bug (`StyleComponent::setStyle` is
> additive-only by accident), a TOCTOU race between patch build and
> patch apply, and several hot-path allocations that contradicted
> the stated "<1ms keystroke" goal. This revision reframes the whole
> design around **tree-identity preservation** and promotes syntax
> highlighting + save to earlier milestones so the design ships
> visible user value before the hard incremental work.
>
> **2026-05-16 postmortem revision.** The `tier3-editor-improvements`
> branch exposed that the implemented structured-editing path drifted
> away from the original intent. The desired model was **incremental
> reparsing**: when a character is typed, locate that byte range in the
> live XML tree and reparse only the relevant attribute, text node,
> opening tag, or smallest enclosing subtree. The branch instead built
> correctness around editor-side `TextPatch` byte splices,
> `ChangeClassifier` whole-buffer diffs, and preserving full reparses to
> refresh stale absolute source offsets. This revision supersedes that
> approach with an XML-owned source-store design: all manipulations land
> on the XML DOM, source text is updated inside `donner/base/xml`, and
> XML source ranges move with edits automatically.

## Summary

`//donner/editor` lands the source pane and the canvas as two views of the
same document, but the current branch still keeps those views synchronized by
side effect. Text edits are diffed after the fact by `ChangeClassifier`; some
are converted to `SetAttribute`, and the rest become full
`ReplaceDocumentCommand`s. Canvas edits mutate the live DOM/ECS, then
`DocumentSyncController` builds manual `TextPatch` byte splices against the
source pane and queues a preserving reparse so XML source offsets are fresh
again.

That architecture failed the design's premise. It made source text, XML
source ranges, and SVG/ECS state three stores that had to be manually kept in
sync. The revised design has one owner: **the XML document owns both the tree
and the source projection**. The editor submits text or canvas intent to the
XML DOM. The XML library performs the source edit, updates source anchors,
incrementally reparses the smallest safe XML scope, and emits XML mutation
events. The SVG layer consumes those events to update its derived ECS
projection.

**The framing under this design is not "two views of one document."** The
framing is: **the XML tree is the document. The source text is a projection
of the tree with formatting metadata attached. The ECS is a derived index
over the tree.** Not peers — one spine, two projections. The XML layer owns
source bytes, source anchors, nodes, attributes, and incremental reparsing.
Every edit, text or canvas, lands on the XML DOM first. The source view is
updated from XML-owned `XMLSourceDelta`s, and the SVG/ECS view is updated
from XML-owned `XMLMutation`s.

This reframing collapses several previously-hard problems: manual
editor-side source splicing disappears, whole-buffer diff classification is
not load-bearing, source ranges move through XML-owned anchors, and the
cascade's own dirty-flag machinery (from `incremental_invalidation.md`) drives
invalidation instead of a hand-maintained switch statement in the editor.

**The HARD RULE** that pins the revised design:
**all manipulations land at the XML DOM level first.** A text keystroke is
an `EditIntent` applied to `XMLDocument::source()`, mapped to a live
`XMLNode`/attribute by source anchors, and handled by incremental reparsing.
A canvas drag is an `XMLNode::setAttribute("transform", ...)` call; the XML
library serializes the value into the owned source store and emits an
attribute mutation. No editor component manually splices source bytes. No
editor component owns source-range fixup.

Scoped into this plan:

- **Bidirectional sync** at XML-node/attribute granularity. The editor
  calls XML DOM APIs; `donner/base/xml` mutates tree + source atomically;
  `donner/svg` updates the derived SVG/ECS projection from XML mutation
  events.
- **SVG-aware syntax highlighting** using a new `XMLParser` token callback
  plus donner's own CSS tokenizer (already reusable from outside
  `details/`).
- **Context-aware autocomplete** sourced from `kSVGElementNames`,
  `kSVGPresentationAttributeNames`, and `PropertyRegistry::kProperties`
  — no hardcoded SVG vocabulary in the editor.
- **Save / Save As** with dirty tracking, native file dialogs, and
  symlink-safe file writes.
- **Graceful intermediate states**: typing a partial SVG value such as
  `fill="re"` updates XML source text immediately, records a scoped SVG
  diagnostic, and leaves the previous valid SVG semantic projection in
  place until the value parses. Typing malformed XML such as a deleted
  quote marks a scoped XML dirty region and defers semantic projection
  changes until the local scope parses again or falls back structurally.

This design is **post-M3 work**: M1–M3 (in `docs/design_docs/editor.md`)
explicitly excluded `SourcePatch` and structured editing. This doc picks
up where M3 leaves off, and is gated behind
`EditorApp::setStructuredEditingEnabled(bool)` through the risky milestones
so it can be reverted in a single flag flip if it misbehaves.

## Postmortem: What Failed In This Branch

The branch shipped useful primitives, but the composed design was wrong:

- **The implementation made the editor own source surgery.**
  `TextPatch`, `AttributeWriteback`, and `DocumentSyncController` build and
  apply byte-level replacements outside the XML library. This duplicated XML
  serialization rules in editor code and forced every canvas mutation to also
  remember how to patch text.
- **`ChangeClassifier` became correctness-critical.** It computes one
  whole-buffer diff, scans for quoted values with local heuristics, and guesses
  whether the edit is an attribute modification, attribute insertion, or
  structural change. The follow-up fixes for attribute insertion and quote
  detection show the problem: the classifier was not using the live parsed tree
  as the authority.
- **Absolute source offsets became stale after successful writebacks.**
  `SourceSync.h` documents the failure directly: after a self-generated
  writeback, re-classifying the text back onto the live DOM leaves every
  `XMLNode` source range at pre-patch offsets, so later edits can map onto the
  wrong sibling. The branch fixed this by queuing a preserving full reparse
  after editor-owned source writes. That is a workaround, not structured
  editing.
- **Manual source splicing leaked into rendering behavior.** Drag-end
  writebacks and delete writebacks often become `ReplaceDocumentCommand`
  reparses. That forced the compositor to preserve or remap caches across
  reparsed entity spaces, which led to the filter-drag snapback, delete flash,
  and layer-remap validation work on this branch. Those compositor fixes are
  valid, but structured editing should not make ordinary attribute updates look
  like document replacement.
- **The implementation missed incremental reparsing.** The original intent was
  to identify where the character was typed in the live tree and reparse only
  the relevant attribute/node/subtree. The branch did not add an XML source
  store, source anchors, node-at-offset lookup, or local XML reparsers, so it
  had no place to implement that intent correctly.

The core lesson: `donner/editor` should not be a source synchronization engine.
It should submit edit intent. `donner/base/xml` should own source mutation,
source-range maintenance, and incremental reparsing. `donner/svg` should own
the XML-to-SVG semantic projection.

## Premortem: Six Months After Shipping

Assume this design shipped, the flag defaulted on, and six months of real
editing passed. The expected outcome is mixed: the architecture probably fixes
the original stale-offset and source/canvas drift class, but several second-
order problems become obvious only after users combine typing, dragging,
malformed XML, filters, and undo in one session.

### What Worked

- **XML-owned source mutation paid off.** Moving canvas writeback into
  `donner/base/xml` removed the stale absolute-offset class that forced
  preserving full reparses on this branch. The source pane, XML tree, and SVG
  projection usually agreed after ordinary drag/type/delete actions.
- **Incremental reparsing preserved interaction state.** Attribute-value,
  opening-tag, and text-node edits stopped replacing the document, so selection,
  compositor caches, and renderer warm state survived most edits.
- **The stress suite found real bugs early.** Mixed click/drag/zoom/text/delete
  scripts caught failures that unit tests missed: stale hit-test entities after
  source deletion, filtered groups snapping to old transforms, and source
  anchors retargeting after repeated insertions.
- **Scoped diagnostics were better than all-or-nothing parse errors.** Users
  could type partial values without losing the canvas, and developers could
  inspect which XML scope was dirty instead of chasing a whole-document parse
  failure.

### What Fell Over

- **Dirty-region state became its own document model.** A malformed quote or
  partial entity inside one attribute was manageable. A user then dragging the
  same element, deleting a sibling, or saving while that region was dirty
  exposed unclear rules: is the dirty source authoritative, is the last-valid
  XML tree authoritative, or is the GUI mutation allowed to edit around it?
- **Anchor bias bugs were subtle.** Insertions exactly at attribute boundaries,
  adjacent empty text nodes, and remove-then-insert sequences produced off-by-
  one anchor movements that looked like semantic parser bugs. These failures
  were hard to debug without a source-anchor trace.
- **SVG projection coverage lagged XML correctness.** XML mutations were
  accurate, but some SVG components still had parser-era assumptions:
  removed list attributes left stale vectors, filter primitives missed dirty
  flags, inline `style=` invalidation was too broad, and `<style>` blocks still
  fell back to full restyle.
- **Formatting policy became user-visible product behavior.** Users noticed
  where new attributes were inserted, which quote style was chosen, and whether
  inline style declarations were rewritten wholesale. "Preserve formatting"
  was not a boolean; it needed explicit local policies and tests.
- **The stress suite risked flakiness.** Async renderer timing, backend bitmap
  differences, and DPI-dependent viewport math made some end-to-end failures
  difficult to classify. The suite was valuable, but only after it emitted
  source/action/bitmap artifacts and separated invariant failures from
  wall-clock performance checks.
- **Undo felt inconsistent.** XML mutations, GUI transforms, source text undo,
  and dirty-region recovery had different histories. Users expected one undo to
  revert the thing they just saw, not the internal subsystem that produced it.
- **"Last-known-good" could hide stale semantics.** If invalid SVG values kept
  the old render alive without strong UI diagnostics, users thought their edit
  had applied when it had only changed source text.
- **Document fallback was overused at first.** The easiest safe escalation was
  full document parse. Without hard counters and failing tests, fallback slowly
  crept into cases that should have stayed `OpeningTag` or `ElementSubtree`.

### Obvious In Retrospect

- **`SourceStore` should ship alone first.** Before any SVG projection work,
  M3 needs exhaustive property tests for anchors, bias, deletion, fragment
  install, source-version rejection, and debug dumps. If source anchors are not
  boring, every later milestone looks haunted.
- **Dirty XML must be a first-class state.** The design cannot treat malformed
  XML as "old tree plus an error marker" and move on. It needs explicit dirty
  regions, ownership rules for GUI mutations touching dirty scopes, save
  behavior, and recovery transitions.
- **Every local edit needs a declared scope.** Tests should fail when a
  supposedly local operation silently falls back to `Document`. The scope ratio
  is a product health metric, not just a performance metric.
- **Projection coverage needs a removal matrix.** Attribute set is the easy
  path. Attribute removal, invalid value recovery, component default restore,
  and dirty-flag emission need table-driven coverage for every SVG component
  family before the flag defaults on.
- **Formatting needs policy objects, not scattered serializer choices.** Each
  element should have an explicit local formatting policy: quote style,
  attribute insertion point, whitespace around `=`, self-closing behavior, and
  inline style rewrite granularity.
- **Stress tests must produce replayable artifacts.** A failing mixed
  GUI/source run must emit the action log, seed, source before/after, anchor
  trace, chosen reparse scopes, final/reference/diff bitmaps, and whether a
  document fallback occurred. Otherwise failures are too expensive to root-
  cause.
- **Undo should be designed before broad rollout.** The flag can ship to
  developers without unified undo, but default-on structured editing needs XML
  mutation records that can compose with transform undo and source-pane undo.

### Design Changes From The Premortem

- M3 exit criteria include `SourceStore` property tests and a human-readable
  anchor trace format.
- M4 treats dirty regions as explicit XML document state with save and GUI-
  mutation rules, not just diagnostics.
- M5 adds a projection removal/defaulting matrix for SVG components.
- M5.5 requires local formatting-policy tests for inserted attributes, removed
  nodes, and rewritten `style=` values.
- M5.75 stress artifacts are required, and performance assertions are kept out
  of correctness tests unless the target is explicitly a perf test.
- M8 cannot flip the default until document-fallback counts stay near zero on
  stress seeds and real editing traces.

## Goals

- **Tree identity is preserved across all localized edits.** Attribute
  edits, text-node edits, inline-style edits, and canvas-driven attribute
  mutations keep the same `XMLDocument`, `SVGDocument`, and existing
  unaffected `XMLNode` / `SVGElement` identities. Only structural fallback
  is allowed to replace a subtree or document.
  *Verified by:* `//donner/editor/tests:editor_sync_tests` gains cases that
  assert entity/root identity across attribute typing, canvas drag writeback,
  text-node edits, and inline `style=` edits.
- **Text edits are handled by incremental reparsing of the live XML tree.**
  The editor sends an `EditIntent {range, replacement}` to
  `XMLDocument::applySourceEdit`. XML maps the edit range to the live tree,
  reparses the smallest safe scope (`AttributeValue`, `OpeningTag`,
  `TextNode`, `ElementSubtree`, then `Document` fallback), and emits XML
  mutation events. `ChangeClassifier` whole-buffer diffing is not part of
  the correctness path.
  *Verified by:* `//donner/base/xml:xml_incremental_reparse_tests`
  covers edit-to-scope classification and `//donner/editor/tests:
  editor_sync_tests` asserts no `ReplaceDocumentCommand` for localized
  attribute/text edits.
- **Canvas → source writeback is a DOM mutation, not an editor text patch.**
  A drag calls `XMLNode::setAttribute("transform", serialized)` or the SVG
  wrapper equivalent. The XML library updates the owned source projection and
  source anchors, then the editor mirrors the XML-owned source delta into the
  visible `TextEditor`. `TextPatch` and `AttributeWriteback` are retired from
  the editor-owned path.
  *Verified by:* a path-scoped banned-patterns lint forbids
  `donner/editor/**` from calling `std::string::replace` or constructing
  source-splice structs for SVG/XML writeback outside test helpers.
- **Source formatting is preserved on round-trip.** Indentation,
  comments, attribute order, attribute quoting style, and text-node
  whitespace are unchanged after a sequence of canvas edits.
  *Verified by:* a byte-level diff golden — `load → drag every rect
  by (1,1) → serialize → diff` must touch only `transform` spans,
  not a single whitespace byte elsewhere.
- **Graceful intermediate states.** Typing partial SVG values
  (e.g. `fill="re`) keeps the XML source current and the last-known-good
  SVG semantic projection alive with a scoped diagnostic. Typing malformed
  XML marks the smallest dirty XML region and avoids touching SVG semantics
  until local incremental reparsing succeeds or document fallback is chosen.
  *Verified by:* integration tests type partial SVG values and deleted XML
  delimiters character by character, asserting stable document identity,
  scoped diagnostics, and no stale source-range retargeting.
- **Zero hardcoded SVG vocabulary in the editor.** Every element name,
  attribute name, and CSS property comes from a donner registry
  (`kSVGElementNames`, `kSVGPresentationAttributeNames`,
  `PropertyRegistry::kProperties`).
  *Verified by:* a path-scoped banned-patterns rule forbidding
  hardcoded SVG element/attribute string literals under
  `donner/editor/text/**`.
- **`Save` is byte-equivalent to the XML-owned source store.** The
  `TextEditor` is a view of `XMLDocument::source()`, so `Cmd+S` writes
  exactly the bytes the XML source store owns. Reloading produces an
  equivalent `SVGDocument`.
  *Verified by:* a fuzzer that drives random tool + text-edit
  sequences and asserts `save → load → assert equivalent` on every
  step. Added to `continuous_fuzzing.md`.

## Non-Goals

- **Unified undo across text and canvas.** Incremental XML source edits
  and canvas DOM mutations both become XML mutations, but this design does
  not define a full cross-view undo stack. `UndoTimeline` remains scoped to
  existing transform operations until a separate undo design stores XML
  mutation records.
- **`SVGParser::ParseSVG` on the localized keystroke path.** Banned by
  the HARD RULE except as a document-level structural fallback. Local
  keystrokes must route through XML incremental reparsing plus SVG semantic
  projection updates.
- **Tree-sitter or LSP integration.** Tokenization is XML-parser-
  callback-driven, not grammar-driven.
- **Multi-cursor editing, collaborative editing, real-time conflict
  resolution.**
- **CSS validation.** Highlighting and tokenization, yes; semantic
  property validation, no. Donner's `PropertyRegistry` does that at
  cascade time.
- **Schema-based SVG validation.** No XSD, no SVG2 schema, no "this
  attribute isn't valid on `<rect>`" error squiggles.
- **Editing non-SVG XML.** The token callback and `SourceStore` are
  generic XML, but autocomplete and SVG semantic projection are SVG-only.
- **`File → New` from a blank canvas.** Elements created by canvas
  tools (path tool, shape tool — not yet landed) without a source
  location need a separate serialization path. Deferred to their
  tool design docs.
- **Undo for `DeleteElement`.** Inherited from M3.
- **Per-element attribute filtering** (only suggest `cx`/`cy`/`r` for
  `<circle>`) — `PropertyRegistry` doesn't carry this metadata today.
  Listed under Future Work. Autocomplete suggests any presentation
  attribute regardless of the containing element.
- **CSS value validation in autocomplete.** Property *names* come
  from the registry; legal *values* for a property are encoded in
  the per-property parser function, not as reflectable metadata.
  Building a uniform value-vocabulary reflection is a separate
  project.

## Next Steps

1. **Stop extending editor-owned source splicing.** Treat
   `TextPatch`, `AttributeWriteback`, and `ChangeClassifier` as
   compatibility code only. New structured-editing work starts in
   `donner/base/xml`.
2. **Build `XMLDocument::SourceStore` and source anchors first.** This is
   the missing primitive that lets source ranges update automatically and
   lets text edits map to the live tree without whole-buffer diffing.
3. **Implement incremental reparsing scopes in XML.** Start with
   `AttributeValue` and `OpeningTag`, because they cover typed attribute
   edits and canvas transform writeback. Then add `TextNode` and
   `ElementSubtree`.
4. **Wire SVG as a semantic projection of XML mutations.** Once XML emits
   `XMLMutation`s, update `SVGDocument`/ECS from those events and keep full
   `ParseSVG` as document fallback only.
5. **Measure real editing sessions** after XML-owned incremental reparsing
   exists. Report the `AttributeValue : OpeningTag : TextNode :
   ElementSubtree : Document` ratio. If `Document` is common, the local
   scope selection is too conservative.

## Implementation Plan

The plan is split into **prerequisites** (M−1), **donner core** (M0–M1),
**visible user value** (M2, M7), and **the XML-owned bidirectional hot path**
(M3–M5.75). **M3–M5.75 shipped behind
`EditorApp::setStructuredEditingEnabled(false)` by default**; M8 flips the
default to true while keeping the runtime opt-out. M3–M5.75 supersede the
previous `TextPatch` / `ChangeClassifier` design.

#### M1-M7 Status Audit (2026-05-17)

- [x] M1 complete: the lexer-only tokenizer, token enum, error recovery,
      representative tokenizer tests, token fuzzer, and empty-sink benchmark
      have shipped. `ParseWithSink` is explicitly removed from M1 scope until a
      real single-pass DOM-plus-token use case appears.
- [x] M2 release scope: XML-aware syntax highlighting has shipped. The
      unchecked M2 items below are follow-up refinements, not current release
      blockers.
- [x] M3 complete: `XMLSourceStore`, source anchors, per-node source metadata,
      source interval lookup, and the anchor/source-location tests have shipped.
- [x] M4 complete: edit-intent forwarding, scoped source edit application,
      scoped dirty-region diagnostics, dedicated incremental parser entry
      points, and the listed tests have shipped.
- [x] M5 complete: attribute/text/style projection paths, granular subtree
      mutation records, source diagnostics, invalid-value preservation, and
      SVG projection/removal coverage have shipped with tests.
- [x] M5.5 complete: source-backed canvas drag/delete/insert/move paths mutate
      through XML/SVG DOM APIs, mirror XML-owned source deltas into the editor,
      and assert localized canvas edits do not dispatch `ReplaceDocumentCommand`.
- [x] M5.75 complete: the structured-editing stress suite now covers
      per-action invariants, mixed GUI/source edits, source-side deletion,
      busy-render deletion queueing, deterministic seeded actions, and
      replayable failure artifacts.
- [x] M7 save scope complete: Save/Save As writes the XML-owned source
      store through a symlink-safe file writer, updates dirty/path state,
      and has bitmap round-trip tests for canvas and text edits. The UI
      uses the editor's existing path-modal pattern; native file dialogs
      remain platform polish rather than a structured-editing invariant.
- [x] M8 complete: `EditorApp` defaults structured editing on, the editor
      shell relies on that default, and tests cover both the default-on
      incremental path and the runtime opt-out fallback to full-document
      reparse.

### M−1: Prerequisites (blocks everything)

These are fixes to in-tree code that the rest of this design assumes are
in place. They are useful independently of structured editing and should
land as standalone PRs in this order.

- [x] **`StyleComponent::setStyle` replaces its own contribution
      without clobbering presentation attributes.** The original
      plan was to uncomment an absent `clearStyle()` and wipe the
      registry. That's wrong: `PropertyRegistry` mixes presentation-
      attribute properties (`fill="red"`, specificity `0,0,0`) and
      style-attribute properties (`style="stroke:green"`, specificity
      `StyleAttribute()`) into the same bucket, so a naive reset
      breaks the parse path — `std::map`-ordered attribute iteration
      writes `fill` into the registry *before* `style=` is seen.
      The real fix uses the specificity tag that's already stored
      on every `Property<T>`:
      `PropertyRegistry::clearStyleAttributeProperties()` walks the
      registry via `forEachProperty` and clears only properties
      whose `specificity == Specificity::StyleAttribute()`, plus
      erases matching entries from `unparsedProperties`.
      `StyleComponent::setStyle` then calls it before `parseStyle`,
      which gives correct replace-only-my-contribution semantics
      for both the parse path and the editor's text-edit rewrite
      path. Regression test in `SVGElement_tests.cc` verifies that
      `fill="red"` survives a subsequent `setStyle("stroke: blue")`
      while `opacity` from a prior style attribute gets cleared.
- [x] **`xml::XMLParser::Options` DoS caps.** Added `maxElements`
      (100k default), `maxAttributesPerElement` (1k), and
      `maxNestingDepth` (256) alongside the existing `maxEntityDepth`
      + `maxEntitySubstitutions`. The element cap is enforced in a
      new `countTreeNode()` helper called from every tree-node
      creator (`parseElement`, `parseXMLDeclaration`, `parseComment`,
      `parseDoctype`, `parseProcessingInstructions`, `parseCData`)
      so an attacker can't amplify with non-element nodes. Nesting
      depth is tracked via an explicit counter bumped around
      `parseNodeContents` rather than the recursive call stack so
      the error message fires cleanly before the recursion.
      Seven new tests in `XMLParser_tests.cc` cover boundary cases:
      cap exceeded, realistic documents under default caps, per-
      element attribute-cap scoping, and wide-shallow documents
      passing a tight nesting cap.
- [ ] **`AttributeParser` re-entrancy audit.** `ParseAndSetAttribute`
      was designed for "called once per attribute during a single
      parse" and the editor's reuse will violate several of its
      invariants:
      - Additive list fields that `.clear()` only when their owning
        attribute re-parses (`values=` on `feColorMatrix`,
        `tableValues=` on `feFuncX`, `kernelMatrix=` on
        `feConvolveMatrix`). Deleting such an attribute from the
        source text leaves the stale vector in the ECS.
      - `SVGParserContext` accumulates warnings unboundedly if the
        editor reuses one context across edits. Editor must construct
        a fresh context per edit.
      - Element-specific setters (`setDxList`, `setDyList`, etc.) are
        called directly from `AttributeParser.cc` — audit each to
        confirm the underlying setter marks dirty flags.
      Output: documented below.

      **Audit results (ParserBot, 2026-04-11):**

      - **Safe for re-parse (overwrite semantics, marks dirty):**
        All CSS presentation attributes via `trySetPresentationAttribute`
        (pure assignment, not accumulation). All typed element setters
        (`setX`, `setCx`, `setPoints`, `setDxList`, `setHref`, etc. —
        ~30 call sites). `id`/`class`/`style` via `setAttribute`
        dispatch (remove-and-replace).
      - **Unsafe — stale on attribute deletion, no dirty flags:**
        Three vector fields cleared only on re-parse of the *same*
        attribute: `FEFuncComponent::tableValues` (line 726),
        `FEColorMatrixComponent::values` (line 815),
        `FEConvolveMatrixComponent::kernelMatrix` (line 1252).
        ~80 direct `comp.field = value` writes across all filter
        primitives + `TextPathComponent` (lines 596–1360, 1998–2014)
        — none mark dirty flags, and `removeAttribute`'s fallback
        `trySetPresentationAttribute(name, "initial")` doesn't reach
        these because they aren't presentation attributes.
      - **`SVGParserContext`:** accumulates warnings via
        `addWarning()`/`addSubparserWarning()`. Editor must use a
        fresh `ParseWarningSink` per edit.
      - **`ClearAttribute` API:** needs a per-element-type dispatch
        that resets the relevant ECS component field to its default
        and marks `DirtyFlagsComponent::Shape`. Minimum viable:
        `comp = ComponentType{}` for the owning component. Longer
        term: route all ~80 direct `comp.field` writes through
        typed setters that mark dirty, matching the pattern used
        by the geometry-element setters.
- [x] **`xml::XMLParser::GetAttributeLocation` error recovery.**
      The three `UTILS_RELEASE_ASSERT`s in `getElementAttributeLocation`
      are now graceful `return std::nullopt` branches — under
      `-fno-exceptions` they would have terminated the editor if a
      stale offset pointed at a now-malformed element. The outer
      `GetAttributeLocation` also gained an explicit offset bounds
      check so an offset past `str.size()` returns `nullopt` instead
      of hitting `std::string_view::substr`'s `out_of_range` (which
      calls `std::abort` in the no-exceptions build). Regression
      tests cover: offset past end-of-string, offset at
      `str.size()`, offset pointing at a malformed element name,
      offset pointing at text content, and offset pointing at a
      partially-typed unterminated attribute. The XML parser fuzzer
      gained a dedicated arm that drives `GetAttributeLocation`
      with random `(offset, name)` inputs (including out-of-range
      offsets) to lock in the no-crash contract — Linux-CI only per
      the existing fuzzer pattern.
- [x] **`css::Declaration` source range.** Added `SourceRange
      sourceRange` to `Declaration`, populated by
      `consumeDeclarationGeneric`. `sourceRange.start` is the name
      offset (as before for the deprecated `sourceOffset` field);
      `sourceRange.end` points at the *start* of the last consumed
      non-whitespace value token — a best-effort approximation, not
      a byte-perfect "past the last byte" range, so editor callers
      that need byte-perfect bounds must scan forward from
      `sourceRange.end` through the source to the `;` or `}`.
      `!important` markers and trailing whitespace do not pull
      `sourceRange.end` past the last real value (covered by tests).
      Also added `SourceRange::operator==` so `Declaration`'s
      defaulted equality operator keeps compiling.
- [x] **Baseline benchmark.** Landed at
      `//donner/benchmarks:structured_editing_perf_bench` (follows
      the existing pattern in `donner/benchmarks/`). 12 benchmarks
      covering full-reparse (trivial / 50-element / 500-element),
      XML-only parse, `GetAttributeLocation`, all three M0
      serializers (`Lengthd::toRcString`, `toSVGTransformString`,
      `Path::toSVGPathData`), and `SVGGraphicsElement::setTransform`.

      **Baseline numbers (M4 Mac Mini, -c opt, 2026-04-12):**

      | Operation | Time |
      |-----------|------|
      | Full SVG reparse — trivial (3 elements) | 52 µs |
      | Full SVG reparse — medium (50 elements) | 459 µs |
      | Full SVG reparse — large (500 elements) | 3.96 ms |
      | XML-only parse — trivial | 14 µs |
      | XML-only parse — large (500 elements) | 1.18 ms |
      | GetAttributeLocation — trivial | 2.6 µs |
      | Lengthd::toRcString | 93 ns |
      | toSVGTransformString (translate) | 81 ns |
      | toSVGTransformString (general matrix) | 461 ns |
      | Path::toSVGPathData (3 commands) | 180 ns |
      | Path::toSVGPathData (~20 commands) | 1.1 µs |
      | setTransform | 27 ns |

      The headline insight: **full reparse of a 50-element SVG costs
      ~460 µs, well within the 16.67 ms frame budget** — the fast
      path doesn't need to be fast for *latency*, it needs to be
      fast for *not destroying the document*. The 500-element case
      at 4 ms starts to matter for frame budget. CI gating deferred
      to M5 (needs benchmark runner infrastructure).

### M0: Donner-side serialization (shrunk — most of this already exists)

- [x] `Path::toSVGPathData() → RcString` (was `PathSpline`; class is `Path`
      in the codebase). Implemented in `donner/base/Path.cc`; declaration
      in `donner/base/Path.h`. Uses `detail::FormatNumberForSVG` from
      `donner/base/FormatNumber.h` for the integer fast-path (`int64_t` cast)
      and shortest round-trippable `{}` form for fractional values.
      Emits uppercase absolute commands: `M x y`, `L x y`,
      `Q cx cy x y`, `C c1x c1y c2x c2y x y`, `Z`.
      Arc commands are not in the Path data model — `PathBuilder::arcTo`
      decomposes them to cubic Bézier curves before storage, so they
      appear as `C` segments in the output.
      Unit tests in `donner/base/tests/Path_tests.cc` cover: empty path,
      MoveTo-only, LineTo, multi-LineTo, ClosePath, QuadTo, CurveTo,
      fractional coordinates, negative values, multiple subpaths,
      all-verb-types, integer-no-decimal, and zero coordinates (14 tests).
      Round-trip tests in `donner/svg/parser/tests/PathParser_tests.cc`
      (12 tests) verify `PathParser::Parse(path.toSVGPathData())` yields
      an equivalent Path for every verb type including arc input.
- [x] `Lengthd::toRcString() → RcString`. Integer values omit the
      decimal (via `int64_t` cast to avoid `{:g}`'s scientific
      notation for large integer-valued doubles); non-integer values
      print via `{:g}` for shortest round-trippable form. All 17
      CSS unit identifiers handled. Round-trip test in
      `LengthParser_tests.cc` covers 17 units × 9 representative
      values = 153 pairs.
- [x] Free function `toSVGTransformString(const Transform2d&)` in
      `donner/base/Transform.h`. Decomposes to simplest form:
      identity → empty string, pure translate → `translate(x)` or
      `translate(x, y)`, pure uniform scale → `scale(s)`, pure
      non-uniform scale → `scale(sx, sy)`, pure rotation around
      origin → `rotate(deg)`, otherwise `matrix(a, b, c, d, e, f)`.
      Rotation is checked *before* scale so `rotate(180°)` →
      `[-1, 0, 0, -1, 0, 0]` serializes as `rotate(180)` instead of
      `scale(-1)` (both are valid SVG but rotation is the canonical
      form). Number formatting uses `std::format("{}", value)` for
      shortest round-trippable double output, with an `int64_t`
      cast for integer-valued doubles. Round-trip test in
      `TransformParser_tests.cc` covers identity, all six translate
      cases, three scales including reflection, three rotations
      (including the 180° scale-collision case), skewX/skewY
      (which fall through to `matrix(...)`), and an explicit
      general-matrix case with exact expected string output.
- [x] `xml::XMLNode::serializeToString(int indentLevel = 0) →
      RcString`. Self-closing empty elements, escaped attribute values
      (via `EscapeAttributeValue` below), `<!-- -->` + `<![CDATA[ ]]>`.
      **Not responsible for preserving author whitespace** — used only
      for elements created by canvas tools without source location.
      Implemented in `XMLNode.cc` with a local `EscapeTextContent` helper
      (escapes `<`/`>`/`&` for Data nodes). Block indentation applied when
      an Element has child Element nodes; inline otherwise (mixed text).
      Round-trip test verifies `XMLParser::Parse(node.serializeToString())`
      reconstructs the original structure including unescaped Data values.
- [x] `xml::EscapeAttributeValue(std::string_view value,
      char quoteChar) → std::optional<RcString>`. Quote-aware
      (escapes `"` only under `"` delimiter, `'` only under `'`,
      passes the other through). Escapes the five XML predefined
      entities (`<`, `>`, `&`, `"`, `'`) plus `\t`/`\n`/`\r` as
      numeric character references (so XML attribute-value
      whitespace normalization doesn't collapse them on reparse).
      Rejects `\0`, C0 control chars except `\t`/`\n`/`\r`, lone
      UTF-16 surrogates, non-characters `U+FFFE`/`U+FFFF`,
      overlong sequences, and truncated multi-byte starts — every
      rejection path returns `std::nullopt`. Valid multi-byte
      UTF-8 passes through unchanged. Lives in a new
      `donner/base/xml/XMLEscape.{h,cc}` to keep the escape
      concern isolated from the parser. 16 unit tests cover ASCII
      passthrough, every entity, whitespace escaping, all C0
      rejects, every UTF-8 invalidity class, and a round-trip
      through `XMLParser::Parse` for 10 representative inputs
      (plain, metacharacter-heavy, tabs/newlines, multi-byte
      UTF-8, emoji, empty) in both double- and single-quote
      delimited forms.
- [ ] **Already exists — drop from this plan:**
      - `Declaration::toCssText` (`donner/css/Declaration.cc:16`)
      - `ComponentValue::toCssText` (variants at
        `donner/css/ComponentValue.cc:46, 85, 263`)
      - `StyleSystem::updateStyle` (`donner/svg/components/style/
        StyleSystem.cc:142`) — use this, don't swap `PropertyRegistry`
      - `XMLParser::GetAttributeLocation` (`donner/base/xml/XMLParser.h:118`)
      - `XMLNode::getAttributeLocation` (`donner/base/xml/XMLNode.h:281`)
      - `donner/css/parser/details/Tokenizer.h` is a clean public
        surface (`Tokenizer(string_view)`, `Token next()`,
        `bool isEOF()`) — just lift into a forwarding header
        `donner/css/Tokenizer.h` and widen Bazel visibility.

### M1: XML token callback API + lexer-only mode

- [x] **`xml::XMLTokenType` enum** in `donner/base/xml/XMLTokenType.h`:
      15 token types (`TagOpen`, `TagName`, `TagClose`, `TagSelfClose`,
      `AttributeName`, `AttributeValue`, `Comment`, `CData`,
      `TextContent`, `XmlDeclaration`, `Doctype`, `EntityRef`,
      `ProcessingInstruction`, `Whitespace`, `ErrorRecovery`).
      `AttributeValue` includes delimiters; `=` is emitted as
      `Whitespace` (gap-free stream, no dedicated `AttributeEquals`).
      `XMLToken` struct carries `type` + `SourceRange` + a `text()`
      convenience accessor.
- [x] **Standalone lexer-only tokenizer** in
      `donner/base/xml/XMLTokenizer.h`: free function template
      `Tokenize<TokenSink>(string_view, sink)`. Implemented via an
      internal `XMLTokenizerImpl` class that splits out of the
      template so we don't carry lambdas/gotos across scopes. The
      tokenizer does NOT build `XMLNode`s, does NOT expand entities,
      and does NOT touch `entt::registry`. It's a pure lexer that
      segments the byte stream into tokens with source ranges.
- [x] **Error recovery:** on malformed input the tokenizer emits
      `ErrorRecovery` tokens and synchronizes to the next `<` or
      end-of-input. Unterminated comments, CDATA, attributes, and
      invalid element names all recover rather than aborting. The
      token stream below the error continues normally — a
      highlighter built on this path stays lit below the cursor.
- [x] **`XMLParser::ParseWithSink` removed from M1 scope.** The lexer-only
      `Tokenize` entry point is what M2 (highlighting) consumes. Incremental
      reparsing should use XML tree anchors and scoped parser entry points, not
      a separate token-stream classifier. A tree-building variant should only
      be added when a concrete use case needs both tokens and a DOM tree in a
      single pass.
- [x] **Token-callback fuzzer.** Dedicated harness
      `XMLTokenizer_fuzzer.cc`: splits the input into
      source bytes + a bitstream; the sink consumes bits to decide
      whether to (a) record the token, (b) call
      `GetAttributeLocation` on a prior element (exercises re-entry),
      or (c) no-op. Asserts tokens are monotonic in offset and
      non-overlapping. Runs on Linux CI per the continuous-fuzzing budget.
- [x] Tests: token offsets reconstruct the input byte-for-byte for
      a representative corpus; malformed-input tokens match the well-
      formed prefix plus an `ErrorRecovery` marker; empty-sink
      instantiation is zero-overhead (compile-time check via
      `static_assert(sizeof(NoTokenSink) == 1)` and a benchmark).

### M2: SVG-aware syntax highlighting (moved up from original M7)

Ships user-visible value at M1.5, exercises the token callback on
real traffic before M3+ depends on it (DuckBot §5).

- [x] **Custom XML-aware `tokenize` callback** replaces the regex-
      based `LanguageDefinition::SVG()`. The callback runs per-line
      (matching the existing `colorizeRange` plugin interface) and
      recognizes: tag delimiters (`<`, `</`, `>`, `/>` → Punctuation),
      quoted strings (attribute values → String), entity references
      (`&amp;` → Number), numeric values with optional CSS units
      (→ Number), identifiers (element names, attribute names →
      Identifier, promoted to Keyword or KnownIdentifier by the
      existing `colorizeRange` post-processing against `def.keywords`
      and `def.identifiers`), and `=` (→ Punctuation).
- [x] **Extended keyword + identifier sets.** Keywords now include
      all ~60 SVG element names (expanded from the prior 22). Known
      identifiers include ~50 common SVG/CSS attributes (`id`,
      `class`, `style`, `viewBox`, `fill`, `stroke`, `transform`,
      `d`, geometry attributes, font properties, etc.) so attribute
      names highlight as `KnownIdentifier` instead of plain
      `Identifier`.
- [x] **XML comment delimiters.** `commentStart`/`commentEnd` set
      to `<!--`/`-->` (were C-style `/*`/`*/`). The existing multi-
      line comment detection pass handles `<!-- -->` spanning across
      lines. `singleLineComment` set to empty (XML has no `//`-style
      single-line comments).
- [ ] New dedicated `ColorIndex` values (`XmlTagName`,
      `XmlAttributeName`, etc.) and palette cleanup of unused C++
      color indices — deferred to a follow-up; the current mapping
      to existing `ColorIndex` values (Keyword, KnownIdentifier,
      String, Number, Punctuation) provides good visual
      differentiation without touching `TextBuffer.h`.
- [ ] CSS regions: `<style>` / inline `style="..."` CSS sub-
      tokenization — deferred to a follow-up.
- [ ] Per-line tokenizer cache with line-start state — deferred to
      a follow-up.

### M3: XML-Owned Source Store + Anchors

This replaces the old `TextPatch` / `AttributeWriteback` milestone. Those
types can remain temporarily as compatibility shims, but they are not part of
the target architecture.

- [x] **`xml::XMLSourceStore`** owns the current XML bytes. It exposes
      anchored spans, `SourceAnchorBias`, and `replace(offset, length,
      replacement)`, returning an `XMLSourceDelta` describing the changed byte
      range and text.
      The editor no longer mutates an independent source string for XML/SVG
      writeback. The initial source/anchor store exists, parsed
      `XMLDocument`s now own one, and parsed whole-node locations are backed by
      source anchors.
- [x] **Source positions become anchors, not raw absolute offsets.**
      Add `SourceAnchorId` and `SourceSpan {start, end}`. Anchors live in the
      `SourceStore`, carry insertion bias, and are updated by every source
      edit. `XMLNode::getNodeLocation()` continues returning resolved
      absolute `SourceRange` for callers, but mutable tree internals store
      anchor ids. The first implementation keeps original `FileOffset`
      line-info for unedited parses and resolves source anchors after the
      source store version changes.
- [x] **Per-node source metadata.** Each parsed node stores anchor spans for:
      opening tag, closing tag (if any), text value span, and each attribute's
      full span + value span + quote style. Whole-node spans, node subspans,
      and attribute source anchors are implemented.
- [x] **Offset lookup index.** `XMLDocument::nodeAtSourceOffset(offset)` and
      `XMLDocument::attributeAtSourceOffset(offset)` use a source interval
      index over resolved anchors. This is the authoritative replacement for
      `ChangeClassifier` scanning. `nodeAtSourceOffset` builds a resolved
      source interval index, and `attributeAtSourceOffset` uses long-lived
      attribute source anchors with a compatibility fallback for source-less
      attributes.
- [x] **Absolute vs. relative span decision.** Do **not** switch the public
      model to relative-only spans. Relative child spans reduce some ancestor
      updates but do not solve edits before siblings or repeated insertions in
      earlier text. The primary mutable representation should be source
      anchors in a central store. Resolved absolute ranges are cached/indexed
      for lookup and invalidated by `SourceStore` deltas. Reparsed fragments
      may use parent-relative offsets internally while being installed, then
      convert them to anchors.
- [x] Tests:
      - [x] source edit before a node moves that node's resolved `SourceRange`;
      - [x] insertion at a span boundary respects anchor bias;
      - [x] repeated edits before unidentified siblings do not retarget a later
        sibling;
      - [x] removed nodes invalidate their anchors;
      - [x] resolved ranges reconstruct the current `SourceStore` bytes;
      - [x] opening-tag, closing-tag, text-value, and attribute source spans
        resolve to the current `SourceStore` bytes.

### M4: XML Incremental Reparsing

- [x] **Edit-intent capture, not post-hoc whole-buffer diff.** `TextEditor`
      emits `EditIntent {range, replacement, kind, bufferVersion}` for each
      source-pane mutation. `DocumentSyncController` forwards that intent to
      `XMLDocument::applySourceEdit`; it does not diff old/new full strings.
      The XML-side `XMLEditIntent` / `applySourceEdit` transaction exists;
      `TextEditorCore` now captures `SourceEditIntent`s for core insert,
      replace, delete, undo, and redo paths, and `DocumentSyncController`
      forwards buffered intent batches to `AsyncSVGDocument::applySourceEdit`
      when structured editing is enabled. Shell-composed cut, paste,
      autocomplete, and find/replace operations now route through the same
      core edit primitive, so they emit the same intents. The whole-buffer
      diff remains as a compatibility fallback for programmatic `setText`.
- [x] **Local scope selection.** `XMLDocument::applySourceEdit` maps the edit
      to a live source anchor and chooses the smallest safe reparse scope:
      - `AttributeValue`: edit stays inside one quoted value; reparse XML
        attribute value/entity decoding, then emit one attribute mutation.
        Implemented for XML DOM attributes.
      - `OpeningTag`: edit adds/removes/renames attributes without changing
        element nesting; reparse only the opening tag and emit added/removed/
        changed attribute mutations. Implemented for well-formed opening-tag
        attribute diffs; malformed opening tags preserve the last valid tree
        and return a scoped diagnostic.
      - `TextNode`: edit stays inside a data/CDATA/comment/PI value; reparse
        that node's value and emit one value mutation. Implemented for parsed
        PCDATA, CDATA, comment, and processing-instruction nodes.
      - `ElementSubtree`: edit changes child structure inside one element;
        reparse the element's source span, then structurally remap children
        by tag/id/path where possible.
      - `Document`: fallback for edits that cross multiple top-level scopes,
        malformed recovery beyond a bounded dirty region, or resource-limit
        failures.
- [x] **Dirty scoped regions for malformed XML.** If local reparsing fails
      because the user is mid-edit (deleted quote, partial tag, unfinished
      entity), keep the source bytes, preserve the last valid XML/SVG
      projection for that scope, attach a diagnostic to the dirty region, and
      retry local reparsing on subsequent edits before escalating to document
      fallback. Implemented for local opening-tag, attribute-value, text-node,
      raw text-like node, and element-subtree reparses; parse failures are
      reported against the bounded source dirty region while the prior DOM
      projection remains intact.
- [x] **Incremental reparse APIs in `donner/base/xml`.** Add local parser
      entry points for attribute values, opening tags, text-like nodes, and
      element fragments. These parsers produce source anchors relative to the
      replacement fragment and install them through `SourceStore`.
      `XMLIncrementalParser` now exposes dedicated entry points for attribute,
      opening-tag, PCDATA, raw text-like node, and element-subtree fragments,
      and `XMLDocument::applySourceEdit` uses those entry points for every
      local source-edit scope.
- [x] Tests:
      - [x] typing one character in `fill="red"` reparses only the attribute;
      - [x] inserting ` transform="..."` before `/>` reparses only the opening tag;
      - [x] deleting a quote creates a scoped dirty region and keeps tree identity;
      - [x] deleting an opening-tag close creates a bounded dirty region;
      - [x] unfinished attribute/text entities keep tree identity and recover locally;
      - [x] restoring the quote clears the diagnostic without full document parse;
      - [x] editing inside text content updates only that text node;
      - [x] editing inside CDATA/comment/PI values updates only that text-like node;
      - [x] structural edit inside one group reparses only that group.

### M5: XML → SVG Semantic Projection

- [x] **XML mutation event stream.** `XMLDocument::applySourceEdit` and DOM
      APIs emit `XMLMutation` records: `AttributeSet`, `AttributeRemoved`,
      `NodeValueChanged`, `NodeInserted`, `NodeRemoved`, `SubtreeReplaced`,
      and `SourceDiagnosticChanged`. Source-side subtree reparses keep a
      coarse `SubtreeReplaced` record for parent invalidation and also emit
      granular descendant `AttributeSet`, `AttributeRemoved`,
      `NodeValueChanged`, `NodeInserted`, and `NodeRemoved` records so SVG
      projection can clear removed semantics without a document fallback.
      Scoped XML dirty-region diagnostics emit `SourceDiagnosticChanged`
      mutations on enter and clear, with the diagnostic payload attached to the
      mutation.
- [x] **`SVGDocument` consumes XML mutations.** The SVG layer maps the
      mutated `XMLNode` identity to the existing `SVGElement`/entity and calls
      the existing attribute/value parsers with a fresh `SVGParserContext`.
      Inline `style=` goes through `StyleSystem::updateStyle`; presentation
      attributes go through the existing property registry path; geometry and
      filter attributes reset stale component fields on removal. Current
      implementation wires `SVGDocument::applySourceEdit` through the XML
      source-edit layer and projects `AttributeSet` / `AttributeRemoved`
      mutations through `SVGElement::setAttribute` / `removeAttribute`.
      `SubtreeReplaced` projection now initializes simple shape, text/tspan,
      and style elements from the updated XML subtree. DOM-originated
      source-backed element insertion/removal now consumes `NodeInserted` /
      `NodeRemoved` mutations; source-side subtree reparses additionally
      project granular descendant mutations before returning to the editor.
      Callers keep parent-context render invalidation in SVG systems.
- [x] **Invalid SVG values do not roll back XML source.** If XML is
      well-formed but an SVG value is temporarily invalid (`fill="re"`),
      the XML attribute value is current, the SVG semantic component keeps the
      last valid value, and a scoped SVG diagnostic is surfaced to the editor.
      Current implementation covers presentation-attribute value edits
      projected through `SVGDocument::applySourceEdit`, including recovery that
      updates semantics and clears the diagnostic.
- [x] **Renderer invalidation stays in SVG systems.** XML mutation handling
      sets the same dirty flags as parser-originated mutations. The editor
      does not own an invalidation switch.
- [x] Tests:
      - [x] edit `fill` updates style and dirty flags without `ParseSVG`;
      - [x] delete `fill` clears the presentation attribute and dirty flags
        without `ParseSVG`;
      - [x] edit `d` updates path geometry and dirty flags without `ParseSVG`;
      - [x] source-side subtree removal of `d` clears stale path geometry
        through granular projection mutations;
      - [x] delete `values=` from `feColorMatrix` clears the vector component;
      - [x] edit simple text content updates `TextComponent` and dirty flags
        without `ParseSVG`;
      - [x] insert `<style>` in a local subtree and project its stylesheet into
        SVG semantics without `ParseSVG`;
      - [x] edit `<style>` text content and update computed styles without
        `ParseSVG`;
      - [x] invalid value records a diagnostic and preserves the last valid
        semantic value;
      - [x] recovered valid value updates semantics and clears the diagnostic.
      - [x] malformed XML source diagnostics emit through XML mutations and
        clear through SVG projection without document replacement.

### M5.5: Canvas DOM Mutations Update XML Source

- [x] **Canvas tools call DOM APIs only.** Select drag, delete, and
      source-backed create/move/replace helpers call `XMLNode` / `SVGElement`
      APIs. Future path tools must use the same DOM path instead of building
      source replacements. Production transform/delete writeback now routes
      source-backed documents through XML DOM mutation; legacy `TextPatch`
      helpers remain for fallback/tests.
- [x] **`XMLNode::setAttribute` serializes through `SourceStore`.** If the
      attribute has source metadata, replace its value span preserving quote
      style and attribute order. If absent, insert a new serialized attribute
      using the element's local formatting policy. If the node has no source
      span, serialize the node or nearest source-less subtree when inserted.
      `XMLDocument::setAttribute` implements source-backed replace/insert;
      the lower-level `XMLNode` convenience setter remains a direct DOM setter.
- [x] **`XMLNode::remove` updates source and tree together.** Element delete
      removes the node span from `SourceStore`, invalidates anchors for the
      subtree, detaches the DOM node, and emits one `NodeRemoved` mutation.
      `XMLDocument::removeNode`, source-backed `SVGElement::remove`, and
      source-backed `SVGElement::removeChild` cover the first version,
      including recursive source-location invalidation for removed subtrees.
- [x] **`XMLNode::insertBefore` / `appendChild` update source and tree
      together.** Source-backed insertion serializes a source-less XML/SVG
      subtree through the XML serializer, inserts it through `SourceStore`,
      installs parsed source anchors onto the inserted subtree, attaches it to
      the live tree, and emits one `NodeInserted` mutation. The first version
      supports unparented element subtrees inserted before an existing child or
      before a parent element's closing tag, and expands self-closing parents
      when appending a child. Source-backed `SVGElement::replaceChild` composes
      insert/remove for source-less replacements. Moving existing source-backed
      element nodes now preserves their source text, including moves into
      self-closing destination parents that must first expand to an explicit
      open/close tag pair.
- [x] **Editor mirrors XML source deltas.** `DocumentSyncController` applies
      `XMLSourceDelta`s from the XML document to `TextEditor` with change
      suppression so source-pane echo does not become a user edit. This is a
      view update, not a separate source-of-truth splice. The current
      implementation applies XML source delta sequences directly for
      source-backed transform, delete, and existing-node move writebacks,
      records command-flush delete deltas in `AsyncSVGDocument`, and keeps
      full-source mirroring as the fallback if replaying a delta batch does
      not reconstruct the XML-owned source.
- [x] Tests:
      - [x] drag inserts/replaces `transform` via XML DOM and source store;
      - [x] delete removes the XML node span and selection remaps/clears;
      - [x] canvas mutation and visible source update share one frame;
      - [x] no `ReplaceDocumentCommand` is dispatched for localized canvas edits.

### M5.75: Structured Editing Stress Suite

This is the structured-editing counterpart to
`//donner/editor/tests:editor_layer_stress_tests`. It should exercise the real
editor interaction seams with a deterministic script runner, not just isolated
unit APIs. The suite starts as a direct harness over `EditorApp`, `SelectTool`,
`ViewportState`, `TextEditor`, `DocumentSyncController`, and `AsyncRenderer`;
once [`0029-ui_input_repro.md`](0029-ui_input_repro.md) Stage 2 exists, the
same scenarios should also run through headless `.donner-repro` playback.

- [x] **Add `//donner/editor/tests:structured_editing_stress_tests`.** The
      harness owns an SVG source string, `TextEditor`, XML/SVG document,
      viewport, select tool, and async renderer. It exposes high-level actions:
      `click(id/docPoint)`, `drag(screenDelta)`, `zoomAround(cursor, factor)`,
      `pan(delta)`, `typeSource(offset, text)`, `replaceSource(range, text)`,
      `deleteSelection()`, and `settleFrame()`. Initial target landed with
      `EditorApp`, `SelectTool`, `ViewportState`, `TextEditor`, and
      `DocumentSyncController`; `AsyncRenderer` is now wired into every
      invariant checkpoint. Artifact capture is covered by a smoke test.
- [x] **Assert after every action, not only at the end.** Required invariants:
      - `TextEditor::getText()` equals `XMLDocument::source()`;
      - all touched node/attribute source anchors resolve inside the current
        source and reconstruct the edited spans;
      - localized source edits report the expected `ReparseScope`;
      - localized GUI edits emit `XMLSourceDelta` + `XMLMutation` without
        `ReplaceDocumentCommand`;
      - selected SVG element identity either remains stable or is explicitly
        cleared/remapped after deletion;
      - the async renderer reaches idle within the timeout and the next click
        can still select the expected element;
      - settled bitmaps match a reference render from the current XML source.
      Current coverage asserts text/source equality, command queue emptiness,
      stable document generation, live node/attribute source-range
      reconstruction for touched elements, delete `XMLSourceDelta`s, and
      live plus non-interaction async render equality against reload after
      every scripted action.
- [x] **Canvas → source scenarios.**
      - Drag an unfiltered rect through zoom and pan; assert `transform=`
        changes in source, anchors move, and final render matches reload.
      - Drag a filtered group and a clipped/opacity group through repeated
        zooms; assert no tile drift, no compositor reset on localized
        writeback, and text source updates in the same frame.
      - Delete a selected element while a render is busy; assert source removes
        exactly that node span, selection clears, clicks on nearby elements
        still work, and reload matches.
      - Current coverage drags plain, filtered, and clipped/opacity elements,
        verifies a plain drag changes source text, queues deletion while an
        async render is in flight, mirrors the XML source delta, and verifies
        the post-delete scene by reload.
- [x] **Source → canvas scenarios.**
      - Type inside `fill="red"`, `transform="..."`, `d="..."`, and inline
        `style="..."`; assert the scoped XML edit updates SVG semantics and the
        canvas without replacing the document.
      - Insert a new attribute in an opening tag; assert only `OpeningTag`
        reparses and canvas selection/drag remains responsive afterward.
      - Delete and restore an attribute quote; assert a bounded dirty region
        appears, last-known-good render stays visible, and restoration clears
        the diagnostic without a document fallback.
      - Current coverage edits `fill`, `transform`, path `d`, inline `style`,
        and an inserted `stroke` attribute, then verifies selection still works
        after recovery from a malformed quote.
- [x] **Mixed GUI + text scenarios.**
      - Click → drag → zoom → type a source attribute edit on the selected
        element → drag again. Assert selection identity, source anchors, and
        rendered position all agree.
      - Move a filtered element by editing `transform=` in source, then drag it
        in the GUI. Assert the drag begins from the source-edited position and
        does not snap back.
      - Delete an element in source, then click/drag around its old screen
        location. Assert hit testing targets the new visual contents, not stale
        entity/source metadata.
      - Current coverage includes the deterministic mixed scenario plus a
        source-side delete that verifies the old screen location hits the
        background and a nearby click selects the clipped element, not stale
        deleted metadata.
- [x] **Deterministic randomized pass.** Run a short seeded sequence that
      mixes clicks, drags, zooms, pans, text replacements, deletes, and
      malformed-then-restored edits. Store the seed in the failure message and
      cap the run time for normal CI. Longer seeds belong in a perf/manual
      target, not the default test lane. Current coverage adds seed
      `0x5EED575`, records the seed in the action log, mixes selection,
      zoom/pan, GUI drags, source `fill` and `transform` edits, a
      malformed-then-restored quote, filtered-element movement, deletion, and
      a post-delete hit-test checkpoint.
- [x] **Failure artifacts.** On invariant failure, write the current source,
      action log, last settled bitmap, reference bitmap, and diff bitmap to
      `$TEST_UNDECLARED_OUTPUTS_DIR`. These artifacts are mandatory because
      most failures will be state-divergence bugs, not simple crashes.
      Current coverage records the scripted action log, manifest, current
      XML source, editor text, live/reference/diff/side-by-side bitmaps, and
      last-settled live/reference/diff/side-by-side bitmaps. A stress-harness
      smoke test verifies the artifact set without intentionally failing the
      test target.

### M6: Autocomplete from registries

- [ ] `detectXmlContext(source, cursorOffset) → XmlContext` with
      variants `ElementName`, `AttributeName`, `StyleValue`,
      `TextContent`, `Unknown`. **Pull, not push** — only invoked
      when the user triggers autocomplete, not on every keystroke.
      Implemented via the existing `XMLParser::Tokenize` token
      stream, not regex.
- [ ] Suggestion sources:
      - Element names: `kSVGElementNames` (iterate via a new
        compile-time key-array helper next to `kProperties`)
      - Attribute names: `kSVGPresentationAttributeNames` + static
        structural list
      - CSS property names (in `style="…"` and `<style>`):
        `kProperties`, with a flag "also a presentation attribute"
        derived from `kValidPresentationAttributes`. Trailing `: `
        auto-inserted on selection.
- [ ] Tests: cursor right after `<` → element suggestions; cursor
      inside an open tag → attribute suggestions; cursor inside
      `style="…"` → CSS property suggestions; cursor in text →
      nothing.

### M7: Save / Save As (moved up from original M9)

Moved up because the bidirectional invariant is only interesting if
the user can save the result. Save against an M4 world (canvas→text
works; text→canvas is still full-reparse) is already a real product.

- [x] `EditorApp` gains `currentFilePath_: std::optional<std::string>`
      and `isDirty_: bool`.
- [x] `Cmd+S` → save to `currentFilePath_` if present, else prompt.
      `Cmd+Shift+S` → always prompt. The current implementation uses
      an in-app path modal matching File → Open; native dialogs remain
      a follow-up platform integration.
- [x] **Symlink-safe writes.** `open(path, O_CREAT | O_EXCL, ...)`
      for new files, `open(path, O_WRONLY | O_NOFOLLOW, ...)` for
      overwrites. No pre-open `stat` (TOCTOU). Fail loudly on
      `ELOOP` — do not silently chase symlinks. macOS sandboxing is not
      enabled for the current editor build; if the app is sandboxed later,
      the native dialog follow-up must add scoped-resource handling.
- [x] Window title shows `currentFilePath_` + `●` when dirty.
- [x] Tests: load → drag → save → reload → assert equivalent. Load →
      text edit → save → reload → assert equivalent. Symlink chase
      rejected. The equivalence assertions use
      `donner/editor/tests:bitmap_golden_compare` to compare live and
      reloaded renders.

### M8: Flip the kill-switch default to `true`

- [x] After a week of continuous fuzzing on the token callback,
      source-store, incremental-reparse, and SVG-projection fuzzers with no
      new crashes, plus a clean run of the structured-editing stress suite
      seed corpus, flip `setStructuredEditingEnabled`'s default to `true`.
      The flag stays in the API for a release cycle so users can opt out
      while we collect field feedback.

## Background

### Current branch state after the failed structured-editing attempt

```
TextEditor                  Editor sync code                 XML/SVG/ECS
    │                              │                              │
    │── whole-buffer text ────────>│ ChangeClassifier             │
    │                              │  heuristic diff              │
    │                              │── SetAttribute or ──────────>│
    │                              │   ReplaceDocument            │
    │                              │                              │
    │<── setText(source) ─────────│ TextPatch /                  │<── canvas drag/delete
    │                              │ AttributeWriteback           │    mutates DOM/ECS
    │                              │ manual byte splices          │
    │                              │                              │
    │                              │ QueueSourceWritebackReparse  │
    │                              │ to refresh stale XML ranges  │
```

This state is explicitly **not** the target architecture. The target removes
`ChangeClassifier` from the correctness path, retires editor-owned source
splicing, and moves source mutation + source range maintenance into
`donner/base/xml`.

### Existing infrastructure (with file:line, all verified)

**Source-location tracking (donner core):**
- `xml::XMLNode::getNodeLocation() → std::optional<SourceRange>`
  — opening-tag-to-closing-tag span
- `xml::XMLNode::getAttributeLocation(xmlInput, name)` —
  `XMLNode.h:281`
- `xml::XMLParser::GetAttributeLocation(str, offset, name)` —
  `XMLParser.h:118`, implementation `XMLParser.cc:1526`
  (**M−1 must fix the three release-asserts**)
- `FileOffset` / `SourceRange` — `donner/base/FileOffset.h`

**Mutation seam (donner editor, M2/M3):**
- `EditorApp::applyMutation(EditorCommand)`
- `EditorCommand` variants: `SetTransform`, `ReplaceDocument`,
  `DeleteElement`. Per-entity coalescing on `SetTransform`.
- `AsyncSVGDocument::lastParseError()` keeps prior document on
  `ReplaceDocument` parse failure and returns the diagnostic.

**Text editor (donner editor):**
- `TextEditor`, `TextBuffer`, `TextEditor::ErrorMarkers` — ported
  widget from the prototype.
- Autocomplete infra (`addAutocompleteEntry`, `buildSuggestions`,
  snippet system, keyboard nav) is fully implemented but not
  wired to a real vocabulary source.

**Parsers / invalidation (donner core):**
- `xml::XMLParser::Parse` — produces `XMLDocument` with source ranges
  on every node. **No element/attribute/depth caps today** (M−1
  blocker).
- `svg::parser::SVGParser::ParseSVG`
- `svg::parser::AttributeParser::ParseAndSetAttribute` —
  `AttributeParser.h:22`. **Has re-entrancy hazards** (M−1 audit).
- `svg::components::style::StyleSystem::updateStyle` —
  `StyleSystem.cc:142`. **The right API for inline `style=` edits**,
  not swapping `PropertyRegistry` from the outside.
- `svg::components::style::StyleComponent::setStyle` —
  `StyleComponent.h:28`. **Additive-only bug** (M−1 fix).
- `css::CSS::ParseStyleAttribute`, `CSS::ParseStylesheet`
- `css::parser::details::Tokenizer` — reusable as a public API;
  just needs a forwarding header (M0).
- `css::Declaration::toCssText` — `Declaration.cc:16`. Handles
  `Ident`, `Function`, `Number`, `Dimension`, `Percentage`, `String`,
  `Hash`, `Url`, `Delim`, `AtKeyword`, `CDO`/`CDC`, brackets,
  `!important`. **Does not evaluate `calc()` or `var()`** — they
  round-trip as `Function` tokens, which is the desired behavior.
- `kSVGElementNames`, `kSVGPresentationAttributeNames`,
  `PropertyRegistry::kProperties` (`PropertyRegistry.cc:1208`),
  `kValidPresentationAttributes` (`PropertyRegistry.cc:1129`).
  Presentation-vs-CSS-only split is `kProperties ⊇
  kValidPresentationAttributes`.
- `DirtyFlagsComponent` + systems from
  [`incremental_invalidation.md`](incremental_invalidation.md).

## Proposed Architecture

### The XML tree and source store are the spine

```
                 ┌─────────────────────────┐
                 │ XMLDocument             │
                 │  XML tree + SourceStore │
                 └────────────┬────────────┘
                              │
               XMLNode with SourceAnchor spans
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
 ┌─────────────┐       ┌─────────────┐       ┌─────────────┐
 │ TextEditor  │       │ SVGDocument │       │ XML children│
 │ source view │       │ ECS view    │       │ / metadata  │
 └──────┬──────┘       └──────┬──────┘       └─────────────┘
        │                     │
        │ EditIntent          │ XMLMutation
        ▼                     ▼
 XMLDocument::applySourceEdit  SVG semantic projection
```

Canvas edits and text edits both land on `XMLDocument`. Text edits arrive as
source `EditIntent`s and are localized by source anchors. Canvas edits call
DOM APIs such as `XMLNode::setAttribute` / `XMLNode::remove`. In both cases,
the XML layer mutates the tree and source store atomically, then emits:

- an `XMLSourceDelta` for views that display source text;
- an `XMLMutation` for derived semantic projections such as SVG/ECS;
- scoped diagnostics for XML or SVG errors that should not destroy the last
  valid projection.

The editor is not a participant in source serialization. It mirrors
`XMLSourceDelta`s into `TextEditor`, forwards user `EditIntent`s back to
`XMLDocument`, and renders diagnostics.

### Identity preservation is the invariant

The HARD RULE, precisely: **localized edits preserve the identity of the
`XMLDocument`, `SVGDocument`, and unaffected nodes/entities**. Attribute
edits, text-node edits, and canvas-driven attribute mutations preserve the
edited node identity too. Structural edits may replace the smallest enclosing
subtree, but the replacement path must try to remap child identity by tag/id/
path before falling back to a larger scope.

### Source anchors replace manual offset fixup

Current `XMLNode` stores optional absolute `FileOffset`s. That is fine for a
parse result, but it is the wrong mutable representation: one insertion near
the top of the file makes every downstream absolute offset stale.

The revised model stores `SourceAnchorId`s in node metadata. Anchors live in
`XMLDocument::SourceStore`, carry insertion bias, and are updated centrally
when source bytes change. Public APIs keep returning resolved absolute
`SourceRange`s because they are useful for diagnostics, highlighting, and
tests. Internally, source truth is anchor-based.

This answers the relative-span question: **relative spans alone are not the
fix**. A child-relative offset still needs updates when an earlier sibling
changes length, and a parent-relative model makes global lookup harder.
Fragment-relative offsets are useful while installing a freshly reparsed
fragment, but the long-lived representation should be anchors plus an interval
index that resolves current absolute positions on demand.

### Incremental reparsing scopes

`XMLDocument::applySourceEdit` owns the localization decision:

1. Resolve the edit range through the source interval index.
2. Pick the smallest safe scope.
3. Apply the source edit in `SourceStore`.
4. Reparse that scope against the new bytes.
5. Install changed XML tree/value/attribute metadata and emit mutations.

Scopes are ordered by cost and blast radius: `AttributeValue`, `OpeningTag`,
`TextNode`, `ElementSubtree`, `Document`. A scope can decline and escalate if
the edit crosses boundaries, violates XML syntax in a way that cannot be kept
as a dirty region, or exceeds parser resource limits.

### DOM mutations update source inside XML

Canvas tools should call DOM APIs. For example:

```cpp
rect.xmlNode().setAttribute("transform", "translate(10 0)");
group.xmlNode().remove();
```

Those calls update the XML tree and source projection inside
`donner/base/xml`. If an attribute already exists, the source store replaces
the value span preserving quote style. If it does not exist, the XML layer
inserts a serialized attribute using the node's local formatting policy. If a
node is removed, the XML layer removes the node span and invalidates anchors
for the subtree.

### Concurrent-edit coherence

The old TOCTOU problem moves out of patch application. `TextEditor` sends an
`EditIntent` with the source version it observed. `SourceStore` applies it
only if the version and anchor mapping still match; otherwise the editor asks
for the current source and replays or drops the intent according to normal text
editor conflict rules. Canvas DOM mutations run through the same `SourceStore`
transaction mechanism, so source view updates and semantic mutations share one
ordered log.

### Feature gate

`EditorApp::setStructuredEditingEnabled(bool)` is a runtime flag, not
a compile-time switch. M3–M8 code paths compile and link always; the flag
now defaults to `true` and gates whether `DocumentSyncController` sends source-pane edits to
`XMLDocument::applySourceEdit` or falls back to current full-document reparse.
Canvas DOM APIs may use the XML source store once it is reliable, but the
editor can still opt out of source-pane incremental reparsing during rollout.

## API / Interfaces

```cpp
// donner/base/xml/XMLParser.h additions (M1)
namespace donner::xml {

enum class XMLTokenType : std::uint8_t {
  TagOpen,          // "<"
  TagName,          // "rect"
  TagClose,         // ">"
  TagSelfClose,     // "/>"
  AttributeName,    // "fill"
  AttributeValue,   // "\"red\"" — INCLUDES delimiters
  Comment,          // "<!-- ... -->"
  CData,            // "<![CDATA[ ... ]]>"
  TextContent,      // raw text between tags
  XmlDeclaration,   // "<?xml ... ?>"
  Doctype,          // "<!DOCTYPE ... >"
  EntityRef,        // "&amp;", "&#x20;"
  ProcessingInstruction,
  Whitespace,       // spans inside tags; stream is gap-free
  ErrorRecovery,    // emitted by Tokenize() on malformed input
};

struct NoTokenSink {
  void operator()(XMLTokenType, SourceRange) const noexcept {}
};

// Caps added in M−1 as an upstream prerequisite.
struct XMLParser::Options {
  int maxEntityDepth = 64;
  int maxEntitySubstitutions = 1024;
  std::size_t maxElements = 100'000;
  std::size_t maxAttributesPerElement = 1'000;
  std::size_t maxNestingDepth = 256;
};

// Existing API — keep stable.
static ParseResult<XMLDocument> Parse(
    std::string_view source, const Options& options = {}) noexcept;

// New — template on sink, zero-overhead for NoTokenSink (M1).
template <typename TokenSink>
  requires std::invocable<TokenSink&, XMLTokenType, SourceRange>
static ParseResult<XMLDocument> ParseWithSink(
    std::string_view source, const Options& options, TokenSink&& sink) noexcept;

// Lexer-only mode — no tree construction (M1). Error-recovers and
// emits ErrorRecovery tokens rather than aborting.
template <typename TokenSink>
  requires std::invocable<TokenSink&, XMLTokenType, SourceRange>
static void Tokenize(std::string_view source, TokenSink&& sink) noexcept;

// Existing — GetAttributeLocation at XMLParser.h:118, fix release-
// asserts in M−1 to return nullopt on malformed input.
static std::optional<SourceRange> GetAttributeLocation(
    std::string_view source, FileOffset elementStartOffset,
    const XMLQualifiedNameRef& attributeName);

// New in M0 — total, quote-aware escape.
std::optional<RcString> EscapeAttributeValue(std::string_view value,
                                              char quoteChar = '"') noexcept;

}  // namespace donner::xml

// donner/base/xml/XMLDocument.h additions (M3-M4)
namespace donner::xml {

using SourceAnchorId = std::uint64_t;

enum class EditBias : std::uint8_t {
  StickBeforeInsertedText,
  StickAfterInsertedText,
};

struct SourceSpan {
  SourceAnchorId start = 0;
  SourceAnchorId end = 0;
};

struct XMLSourceDelta {
  SourceRange oldRange;
  SourceRange newRange;
  std::string_view replacement;  // view into XMLDocument::source().
};

struct EditIntent {
  SourceRange range;
  std::string_view replacement;
  std::uint64_t sourceVersion = 0;
};

enum class ReparseScope : std::uint8_t {
  AttributeValue,
  OpeningTag,
  TextNode,
  ElementSubtree,
  Document,
};

struct XMLMutation {
  enum class Kind : std::uint8_t {
    AttributeSet,
    AttributeRemoved,
    NodeValueChanged,
    NodeInserted,
    NodeRemoved,
    SubtreeReplaced,
    SourceDiagnosticChanged,
  };

  Kind kind;
  XMLNode node;
  XMLQualifiedName attributeName;
  std::optional<RcString> value;
  ReparseScope scope = ReparseScope::Document;
};

struct ApplySourceEditResult {
  bool applied = false;
  ReparseScope scope = ReparseScope::Document;
  std::vector<XMLSourceDelta> sourceDeltas;
  std::vector<XMLMutation> mutations;
  std::optional<ParseDiagnostic> diagnostic;
};

class XMLDocument {
public:
  std::string_view source() const;
  std::uint64_t sourceVersion() const;

  std::optional<XMLNode> nodeAtSourceOffset(std::size_t offset) const;
  std::optional<std::pair<XMLNode, XMLQualifiedName>>
  attributeAtSourceOffset(std::size_t offset) const;

  ApplySourceEditResult applySourceEdit(const EditIntent& intent);
};

class XMLNode {
public:
  // Existing DOM APIs gain XML-owned source updates when the node belongs
  // to a document with a SourceStore.
  void setAttribute(const XMLQualifiedNameRef& name, std::string_view value);
  void removeAttribute(const XMLQualifiedNameRef& name);
  void setValue(const RcStringOrRef& value);
  void remove();
};

}  // namespace donner::xml

// donner/svg additions (M5)
namespace donner::svg {

class SVGDocument {
public:
  void applyXMLMutations(std::span<const xml::XMLMutation> mutations);
};

}  // namespace donner::svg
```

## Error Handling

- **Attribute-value XML parse fails**: keep the source bytes, mark the
  attribute value span dirty, preserve the prior XML attribute value and SVG
  semantic projection for that attribute, and attach a scoped diagnostic.
- **Attribute XML parses but SVG semantic parse fails**: update the XML
  attribute string and source anchors, keep the previous valid SVG component
  value, and attach an SVG diagnostic to the attribute span.
- **Opening-tag reparse fails**: keep the source bytes, preserve prior
  attributes/tree for that element, and mark the opening tag span dirty. Retry
  on the next edit in that span.
- **Element-subtree reparse fails**: keep the source bytes and preserve the
  previous valid subtree projection. Escalate to document fallback only when
  the dirty region crosses the enclosing element boundary or the user pauses
  and requests validation.
- **Source version mismatch**: reject the stale `EditIntent`, ask the
  `TextEditor` to reconcile against `XMLDocument::source()`, and do not mutate
  XML/SVG state.
- **Replacement violates resource limits**: reject the edit before mutating
  the source store and surface a diagnostic. This is the XML equivalent of the
  existing parser DoS caps.
- **Concurrent text + canvas edit (TOCTOU)**: `SourceStore` version and
  anchor checks reject the stale transaction; the editor reconciles the source
  view from `XMLDocument::source()` before replaying any user intent.
- **`EscapeAttributeValue` called with NUL/surrogate halves**:
  returns `std::nullopt`; writeback rejects the mutation rather than
  producing invalid XML. Surfaced to the user as "cannot encode this
  value."
- **`AttributeParser` re-entrancy on element-specific list attributes**
  (`feColorMatrix values=`, etc.): the new `ClearAttribute` API
  (M−1) is called from the SVG XML-mutation handler when XML emits
  `AttributeRemoved`.
- **Save fails** (permissions, disk full, `ELOOP`): dialog shown,
  dirty flag stays set, buffer unchanged.
- **XML parser caps exceeded** (`maxElements`, etc.): returns
  `ParseDiagnostic` like any other parse error; existing
  `lastParseError()` path surfaces it.

## Performance

> **Note:** every number in this section is a **target**, not a
> measurement. The first deliverable of M−1 is
> `//donner/editor/benchmarks:structured_editing_bench` which
> measures the full-reparse baseline on representative corpora.
> Targets here are refined in a follow-up revision after the first
> benchmark run.

Frame budget is 16.67ms (60fps). The structured-editing path must
coexist with the renderer, ImGui, and the cascade.

| Operation | Target p99 | Status |
|-----------|-----------|--------|
| `SourceStore::replace` + anchor update | 20 µs | needs measurement |
| `nodeAtSourceOffset` / attribute lookup | 5 µs | needs measurement |
| Attribute-value incremental reparse | 20 µs | needs measurement |
| Opening-tag incremental reparse | 50 µs | needs measurement |
| Text-node incremental reparse | 20 µs | needs measurement |
| `AttributeParser::ParseAndSetAttribute` — scalar (`fill`, `cx`) | 50 µs | needs measurement |
| `AttributeParser::ParseAndSetAttribute` — short path (<100 cmds) | 500 µs | needs measurement |
| `AttributeParser::ParseAndSetAttribute` — large path (500+ cmds) | **3 ms — may exceed frame budget alone** | needs measurement |
| Targeted invalidation (one entity) | 50 µs | needs measurement |
| Canvas `XMLNode::setAttribute` source update | 50 µs | needs measurement |
| Token callback re-tokenize (per line, cached) | 100 µs | needs measurement — lexer-only mode required |
| Per-line re-tokenize on `<!--` at top of 10k-line file | 5 ms | needs measurement; fall back to async if over |
| Cascade recompute for inline `style=` edit on deeply-nested element | needs measurement | |
| Renderer re-raster after targeted invalidation (per backend) | needs measurement | TinySkiaBot / GeodeBot bot-handoff |
| Structural fallback keystroke roundtrip (500KB file) | **20–100 ms — off-main-thread** | needs measurement; gate on user-visible lag budget |
| **Full incremental keystroke roundtrip — scalar attr** | **<1 ms** | headline goal |

Anti-targets:
- Walking the entire `XMLNode` tree per edit. Source offset lookup uses the
  source interval index; fragment install touches the edited scope only.
- Re-running `SVGParser::ParseSVG` on the localized incremental path (HARD
  RULE).
- Making `donner/editor` responsible for XML serialization or source-range
  fixup.
- Allocating on the hot path:
  - `EditIntent::replacement` is a view into `TextEditor` / event storage
    for user input or into XML serializer storage for DOM-originated edits.
  - `XMLSourceDelta::replacement` is a view into `XMLDocument::source()`.
  - `tokenCallback` is a template parameter, not `std::function`.
  - `AttributeParser::ParseAndSetAttribute` already takes
    `std::string_view` — no copy on the way in.
- Building full `XMLNode` trees per re-tokenize. Lexer-only mode is
  required — tree-building mode costs orders of magnitude more due to
  `entt::registry` `get_or_emplace` churn.

Two-tier debounce:
- **Tier 1 (next frame):** `AttributeValue`, `OpeningTag`, and `TextNode`
  edits whose XML and SVG parse costs are bounded.
- **Tier 2 (typing idle, ~150ms):** large `d=`/`points=` values, `<style>`
  block edits, `ElementSubtree`, and document fallback. Source text still
  updates immediately; only semantic projection is delayed.

Benchmark gate (PerfBot §7):
- **10% variance** on headline keystroke-roundtrip.
- **5% variance** on sub-metrics (`SourceStore::replace`,
  `nodeAtSourceOffset`, `AttributeValue` reparse).
- **Absolute 1ms p99 wall** — any run above that fails regardless of
  relative change. Catches baseline drift.
- **N=1000 runs, reporting p99**, not mean.
- Pinned CI runner (perf runner class), not shared CI.

Downstream costs not in this table but must be measured:
- ImGui re-render cost on a large buffer (source pane is immediate-
  mode; big file = per-frame cost).
- GPU texture upload for the rendered canvas (hidden in Skia/TinySkia,
  exposed in Geode).
- `RcString` intern table lookups in `AttributeParser`.

## Security / Privacy

**New attack surfaces introduced by this design, beyond the existing
`editor.md` threat model:**

1. **`SourceStore` transactions.** Every source mutation, whether from
   text input or DOM writeback, must pass through one XML-owned transaction:
   - **Integer-overflow-safe range check:**
     `offset <= source.size() && length <= source.size() - offset`
     (no `offset + length` add). CWE-190.
   - **UTF-8 boundary check:** both `offset` and `offset + length`
     land on codepoint boundaries. Corrupting mid-codepoint enables
     downstream XML injection and is classic CWE-176.
   - **`sourceVersion` + anchor validation:** reject stale
     `EditIntent`s whose observed source version or resolved anchor spans no
     longer match the current source. This is the TOCTOU guard that replaces
     patch fingerprints. CWE-362.
   - **`kMaxReplacementSize` check** (1 MB default). Prevents an adversarial
     tool or pasted input from queueing gigabyte edits.
   - **Dirty-region bounds:** malformed XML can create a scoped dirty region,
     but the region must stay bounded by the chosen reparse scope or escalate
     to document fallback. It must not silently expand without limit.
   All rejections are counted and logged.

2. **`EscapeAttributeValue` totality.** Quote-aware, escapes the five
   XML predefined entities, escapes C0/C1 control characters per
   XML 1.0, **rejects `\0` and surrogate halves** (returns
   `nullopt`). Property testing alone is insufficient — the gating test is
   the **DOM source-serialization fuzzer** (see Testing §) that exercises
   canvas DOM mutation → `SourceStore` update → XML reload → assert
   round-trip. CWE-91, CWE-117.

3. **`XMLParser::tokenCallback` DoS / re-entrancy.** The token
   callback fires per token. A 10M-attribute SVG would fire 200M+
   times today. **Blocked on M−1 DoS caps** (`maxElements`,
   `maxAttributesPerElement`, `maxNestingDepth`). The callback
   signature must be `noexcept`-typed (enforced via the template's
   `requires`-clause on `std::invocable<sink, ..., noexcept>`) —
   `-fno-exceptions` + callback-that-throws is UB today. Re-entry
   via `XMLParser::GetAttributeLocation` from inside the callback
   is explicitly supported and fuzzed. CWE-400, CWE-674.

4. **`AttributeParser::ParseAndSetAttribute` re-entrancy.** The M−1
   audit is the gating deliverable. Known hazards:
   - Additive list fields (see M−1 bullet). Fix: add
     `ClearAttribute(name)` and call it from the SVG XML-mutation handler
     when XML emits `AttributeRemoved`.
   - `SVGParserContext` warning accumulator. Fix: construct fresh
     per edit or drain after each call. Unbounded growth is
     CWE-401.
   - Element-specific direct setters (`setDxList`, etc.) must mark
     dirty flags. Fix: audit and add where missing.
   - `style=` attribute must keep replace-only-my-contribution semantics in
     `StyleComponent::setStyle` so repeated inline style edits do not
     accumulate stale declarations.

5. **File save.** Symlink-safe: `O_NOFOLLOW` + `O_CREAT | O_EXCL`.
   No pre-open `stat`. `nfd_extended` sandbox story is an explicit
   decision in M7 — either unsandboxed build or direct `NSSavePanel`
   with `-startAccessingSecurityScopedResource`. CWE-59, CWE-367.

6. **Display-side sanitization.** User-typed source rendered in the
   ImGui widget: cap per-line render length (~1M chars → truncate
   with ellipsis, full text stays in the buffer). Error-marker
   strings passed to ImGui tooltips: truncate to ~120 chars, replace
   bidi-override control chars (U+202A–U+202E, U+2066–U+2069) with
   U+FFFD. Trojan Source (CWE-1007) is a real vector for editors
   that render untrusted source; donner doesn't display in a browser
   but does render diagnostics that embed user values.

**Fuzzers required** (added to
[`continuous_fuzzing.md`](continuous_fuzzing.md)):
- `XMLParser_token_callback_fuzzer.cc` (M1): exercises the callback
  path, re-entry via `GetAttributeLocation`, malformed inputs.
- `XMLSourceStore_fuzzer.cc` (M3): random source edits, anchor spans, and
  DOM-originated source deltas; asserts anchors stay ordered, resolve inside
  the source, and never retarget removed nodes.
- `XMLIncrementalReparse_fuzzer.cc` (M4): random localized edits inside
  attributes, opening tags, text nodes, and element fragments; asserts scoped
  reparsing either installs a valid local mutation or records a bounded dirty
  region without corrupting the previous projection.
- `SVGXMLProjection_fuzzer.cc` (M5): random XML mutation streams over SVG
  elements; asserts SVG components update, clear stale removed-attribute
  state, and preserve last-known-good semantics on invalid SVG values.
- `StructuredEditingDriver_fuzzer.cc` (M5.5): random interleaving of tool
  DOM actions and text edits, asserts `save → load → equivalent` on every
  step.
- `StructuredEditingStress_seed_corpus` (M5.75): minimized action logs from
  the deterministic stress suite, replayed as regression inputs when a mixed
  GUI/text scenario fails.
- Extended `XMLParser_fuzzer` with the new DoS caps exercised.
- Extended `XMLParser_structured_fuzzer` with
  `bool enable_token_callback`.

All must run in continuous fuzzing, not just once at merge.

## Testing and Validation

- **Unit tests** per milestone, same package as the module under test.
- **Round-trip golden tests:**
  - M0 serializers: `parse → serialize → parse → assert equivalence`.
  - M5.5 canvas DOM writeback:
    `load → drag → XMLSourceDelta → diff touches only transform spans`.
  - M5.75 mixed GUI/text stress:
    `script → settle → reference render from XMLDocument::source()`.
  - M7 save: `load → drag → save → reload → assert equivalent`.
- **Tree-identity assertions:** the M4/M5 fixtures check pointer equality
  of the `XMLDocument`, `SVGDocument`, edited node, and unaffected ECS
  entities across localized edit sequences. Any undeclared replacement fails
  the test.
- **Scope assertions:** every incremental-reparse test asserts the chosen
  scope (`AttributeValue`, `OpeningTag`, `TextNode`, `ElementSubtree`, or
  `Document`). Localized tests fail if they fall back to `Document`.
- **`ReplaceDocumentCommand` counter:** per-test upper bound = initial loads,
  explicit opens, and declared document fallbacks only.
- **Fuzzers** — listed in Security §.
- **Benchmarks:** `//donner/editor/benchmarks:structured_editing_bench`
  gates CI on 10%/5% variance + 1ms p99 absolute wall.
- **Banned-patterns lint:** forbids hardcoded SVG element/attribute
  literals under `donner/editor/text/**` and editor-side XML source splicing
  helpers under `donner/editor/**`.
- **Real-editing-session measurement:** before M5 ships, instrument
  the editor on a corpus of actual editing sessions and report the
  `AttributeValue : OpeningTag : TextNode : ElementSubtree : Document` ratio.
  If `Document` is common on realistic input, the scope-selection design is
  too conservative.
- **Target CI entry points:** `//donner/base/xml:xml_source_store_tests`,
  `//donner/base/xml:xml_incremental_reparse_tests`,
  `//donner/svg/tests:svg_xml_projection_tests`, and
  `//donner/editor/tests:editor_sync_tests` own the invariant coverage.
  `//donner/editor/tests:structured_editing_stress_tests` owns the end-to-end
  mixed GUI/text regression matrix.

## Reversibility

Blast radius is large; reversibility is load-bearing.

- **M−1 fixes** are narrow, useful independently, and ship as standalone
  PRs. Each is revertible on its own.
- **M0 core additions** (serializers, `XMLTokenType`,
  `EscapeAttributeValue`) are subtractive-safe and have no dependent in-tree
  consumers besides the editor.
- **M1 token callback** lands as a new API without changing existing
  `XMLParser::Parse` behavior. Existing callers instantiate `NoTokenSink`
  (the default) and get dead-code-stripped to the current path.
- **M2 syntax highlighting** is purely a display change. Reverting
  restores the regex-based highlighter.
- **M3–M8 are flag-gated.** `EditorApp::setStructuredEditingEnabled(false)`
  keeps source-pane edits on the full-document reparse path while the XML
  source store, incremental reparsers, SVG projection APIs, and
  structured-editing stress suite continue to compile. M8 flips the default
  to `true`; the opt-out remains for one release cycle as an escape hatch.
- **M7 save** is flag-orthogonal. Reverting removes the menu items.

If the design is abandoned, M−1–M2 and M7 stay (they're useful on their
own). M3–M6 must not leave behind a parallel editor-owned writeback
implementation. The rollback removes the new XML source-store entry points,
their editor integration, the stress-suite structured-editing mode, and any
compatibility shims that no live caller uses.

## Alternatives Considered

### Tree-sitter incremental parsing
Rejected. 1.5MB grammar artifact, parallel parse tree to keep in sync
with `xml::XMLParser`, and the attribute-level fast path is sufficient
for SVG editing without adding a dependency.

### Full re-parse with DOM diffing
Rejected as the localized path. The more important failure is identity:
even when a full parse is fast enough, diffing a replacement tree back onto
the live document makes ordinary keystrokes look like structural churn.
Full parse remains the document-level fallback for edits that cannot be kept
inside a bounded scope.

### Source text as source of truth (canvas-only editing)
Rejected. Loses formatting, comments, attribute order, and authorial
intent. Also rules out typing SVG directly.

### Text and canvas as peers (the prototype's design)
Rejected in favor of the framing flip (DuckBot §1). The peer model
forces a delta accumulator, a classifier that has to be correct rather
than merely fast, and a sync pump that can drift. The tree-spine model makes
XML the single writer for source bytes and semantic mutations.

### Relative-only source spans
Rejected as the primary representation. Relative spans are useful while
installing a freshly parsed fragment, but they do not solve edits before
earlier siblings and make source-offset lookup harder. The target
representation is a central `SourceStore` with source anchors and a resolved
interval index.

### Rope-backed source storage
Deferred. A rope could reduce large-buffer copy costs, but it does not by
itself solve source-to-node identity or SVG semantic projection. Start with a
piece-table-like `SourceStore` plus anchors; revisit rope/gap-buffer storage
only if benchmarks show source replacement cost dominates.

### `std::function<void(XMLTokenType, SourceRange)>` for the token
callback
Rejected (ParserBot §1). Heap-capture risk, per-token indirect call,
`-fno-exceptions` footgun. Template-on-sink with `NoTokenSink` default
is the shape.

### Recomputing attribute ranges on demand with `GetAttributeLocation`
Rejected for the structured-editing hot path. It is fine for read-only tools
and compatibility code, but incremental reparsing needs reliable node/attribute
lookup after every source edit. That requires source metadata and anchors that
move with the source, not repeated scans over stale absolute offsets.

### `CSS::ParseStylesheet` full re-parse on every `<style>` edit
Rejected for the fast path but **accepted as the structural
fallback**. Per CSSBot §6, donner's cascade today doesn't incrementally
track per-stylesheet contributions — any `<style>` edit today triggers
`needsFullStyleRecompute = true` which is O(document). The right
incremental path belongs in
[`incremental_invalidation.md`](incremental_invalidation.md), not here.
M5 accepts the full restyle cost on `<style>` edits as a
known deficiency and documents it.

## Open Questions

1. **What fraction of real keystrokes hit `Document` fallback?** The
   HARD RULE only works if fallback is rare. Must be measured on a real
   editing corpus after M4 has scope instrumentation. If the measured
   document-fallback rate is high, the incremental reparse scopes are too
   coarse or too conservative.

2. **SourceStore representation.** Piece table, gap buffer, and rope can all
   host anchors. Initial recommendation: piece-table-like storage with anchor
   ids and a resolved interval index, because it minimizes copying without
   forcing parser-wide rope plumbing. Revisit only after benchmark data.

3. **Dirty-region escalation policy.** When malformed XML persists, should
   the editor keep retrying the local dirty scope indefinitely, escalate after
   an idle timeout, or escalate only when the user requests validation/save?
   Initial answer: retry locally while the cursor stays in the dirty scope;
   escalate only when edits cross the enclosing scope or resource caps trip.

4. **Entity-reference edits.** Editing inside `&amp;quot;` in an attribute
   value is still an `AttributeValue` scope if the resulting entity sequence
   can be kept as a dirty value region. XML owns this decision through the
   attribute-value parser, not a text classifier. Need explicit tests for
   partial, restored, and invalid entity references.

5. **Inline `style` DOM mutation granularity.** When a tool changes one CSS
   property, do we rewrite the whole `style="..."` value or preserve and
   rewrite only the declaration span? Initial answer: the XML layer still
   updates one attribute value span, but the SVG/CSS layer may provide a
   formatting-preserving style-value serializer once `Declaration::sourceRange`
   is trustworthy.

6. **Attribute order on writeback.** Preserve original source order
   (author-friendly, more code) or normalize to canonical order
   (simpler, surprises authors)? **Initial answer:** preserve order;
   new attributes append at end.

7. **Latent bugs to file separately** (surfaced by CSSBot):
   `mergeStyleDeclarations` does case-insensitive name matching,
   which is **incorrect for custom properties** (`--foo`). Custom
   property names are case-sensitive per spec. Not this doc's to
   fix, but track as a bug if the editor starts writing custom
   properties.

8. **macOS sandbox story for `nfd_extended`.** Does it preserve
   security-scoped URLs or return paths the sandbox refuses? Test
   in M7; commit to sandboxed-with-scope or unsandboxed-with-doc.

9. **ImGui widget cost on large buffers.** Is the source pane
   itself (immediate-mode, per-frame render) a dominant cost on
   10k-line files? Must be measured before we optimize anything
   under it.

## Future Work

- [ ] Cross-boundary undo: one undo reverts both the canvas-visible semantic
      mutation and the XML source delta through XML mutation records.
- [ ] Per-element attribute filtering for autocomplete
      (requires new registry metadata — separate project).
- [ ] CSS value vocabulary reflection for value autocomplete
      (separate project).
- [ ] Named SVG color suggestions inside color attribute values.
- [ ] Snippet expansion (`rect` → full element template).
- [ ] `Format Document` command (normalize indentation, attribute
      spacing, quote style).
- [ ] Color picker integration.
- [ ] Rope-backed or gap-buffer `SourceStore` storage if benchmarks show the
      initial piece-table-like source store is not enough.
- [ ] Tree-sitter as an alternative tokenizer behind a compile-time
      flag, if profiling shows the XML callback path is a bottleneck
      on very large files.
- [ ] Real-time collaborative editing with operational transform.
