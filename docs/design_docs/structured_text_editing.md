# Design: Projected DOM Editing (Bidirectional Source ↔ Canvas)

**Status:** Draft — revised 2026-04-11 after specialist review
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

## Summary

`//donner/editor` lands the source pane and the canvas as two views of the
same document, but the relationship between them is **one-way and
destructive**. Keystrokes trigger a full `SVGParser::ParseSVG` of the entire
buffer via `EditorCommand::ReplaceDocumentCommand`, which throws the existing
`SVGDocument` away and rebuilds it from scratch. Canvas mutations
(`SelectTool` drags, future `NodeEditTool` edits) update the ECS in place but
the source pane is never written back. The text and canvas drift apart the
moment a tool fires.

**The framing under this design is not "two views of one document."** The
framing is: **the XML tree is the document. The source text is a projection
of the tree with formatting metadata attached. The ECS is a derived index
over the tree.** Not peers — one spine, two projections. The `XMLNode`
already has a source range, a parsed attribute value, and (via the
`XMLNode ↔ SVGElement` mapping) an ECS handle attached to it. Every edit,
text or canvas, lands on the node first, and two tiny projectors fan out
from the node: one rewrites the source span, the other calls
`AttributeParser::ParseAndSetAttribute` on the ECS component. The classifier,
the `nodeAtOffset` walk, and the `FileOffsetRange` delta bookkeeping that
the prototype needed stop being load-bearing and become *optimizations* on
top of a tree-identity model.

This reframing collapses several previously-hard problems: the delta
accumulator disappears (the tree is canonical, offsets regenerate lazily),
the classifier is a fast path for the common case rather than a correctness
requirement, and the cascade's own dirty-flag machinery (from
`incremental_invalidation.md`) drives invalidation instead of a hand-
maintained switch statement in the editor.

**The HARD RULE** that pins the whole design:
**tree-identity is preserved across edits.** The same `XMLDocument` and the
same `SVGDocument` survive every text edit that lands inside an existing
attribute, text node, or known-safe structural rearrangement. `ParseSVG`
only runs on initial load, File → Open, and — *as a bounded escape hatch* —
when a structural edit cannot be applied incrementally. The editor's
ambition is to keep the escape-hatch count per editing session near zero.

Scoped into this plan:

- **Bidirectional sync** at attribute-granularity, via the existing
  `EditorApp::applyMutation` mutation seam extended with `SetAttribute`
  and a `TextPatch` sideband.
- **SVG-aware syntax highlighting** using a new `XMLParser` token callback
  plus donner's own CSS tokenizer (already reusable from outside
  `details/`).
- **Context-aware autocomplete** sourced from `kSVGElementNames`,
  `kSVGPresentationAttributeNames`, and `PropertyRegistry::kProperties`
  — no hardcoded SVG vocabulary in the editor.
- **Save / Save As** with dirty tracking, native file dialogs, and
  symlink-safe file writes.
- **Graceful intermediate states**: typing a partial attribute value
  keeps the last-known-good document alive. This extends the
  `AsyncSVGDocument::lastParseError()` machinery from M3.

This design is **post-M3 work**: M1–M3 (in `docs/design_docs/editor.md`)
explicitly excluded `SourcePatch` and structured editing. This doc picks
up where M3 leaves off, and is gated behind
`EditorApp::setStructuredEditingEnabled(bool)` through the risky milestones
so it can be reverted in a single flag flip if it misbehaves.

## Goals

- **Tree identity is preserved across all supported edit kinds.** Every
  attribute edit, every inline-style edit, every canvas mutation leaves
  `SVGDocument::rootEntity()` stable and leaves every non-affected
  `XMLNode`'s stored `FileOffsetRange` byte-accurate after delta fixup.
  *Verified by:* a test fixture that counts `SVGParser::ParseSVG` calls
  per test and asserts `callCount == initialLoads`. Any call past that
  budget fails the test. The budget is explicit per test, not a moving
  average.
- **Attribute-value edits do not call `SVGParser::ParseSVG`.** Typing
  inside a recognized attribute dispatches to
  `AttributeParser::ParseAndSetAttribute` on the live ECS entity at
  attribute-level granularity. The keystroke roundtrip stays inside
  1ms p99 **for attributes whose parse cost is bounded** (scalars,
  colors, transforms, short paths). Large-`d` edits have their own
  budget (see Performance §).
  *Verified by:* `//donner/editor/benchmarks:structured_editing_bench`
  gates the <1ms p99 wall on a scalar-attribute corpus. A separate
  large-path row sets a 5ms target for `d=` with ≤100 commands.
- **Canvas → text writeback is same-frame and atomic.** Every
  `EditorCommand` that mutates an attribute also drops a `TextPatch`
  onto `pendingTextPatches_` inside `applyMutation`; the source pane
  drains the sideband at the end of the same `flushFrame()` before it
  renders. The user never sees canvas and source panes disagree
  across a frame boundary.
  *Verified by:* an integration test that instruments frame counters
  and asserts the canvas and source updates share a frame index.
- **Source formatting is preserved on round-trip.** Indentation,
  comments, attribute order, attribute quoting style, and text-node
  whitespace are unchanged after a sequence of canvas edits.
  *Verified by:* a byte-level diff golden — `load → drag every rect
  by (1,1) → serialize → diff` must touch only `transform` spans,
  not a single whitespace byte elsewhere.
- **Graceful intermediate states.** Typing partial values
  (e.g. `fill="re`) does not destroy the document. The canvas keeps
  rendering the last-known-good state and the source pane shows an
  error marker on the affected line.
  *Verified by:* an integration test that types `fill="re` character
  by character and asserts `app.hasDocument()` and a populated
  `TextEditor::ErrorMarkers` at each step.
- **Zero hardcoded SVG vocabulary in the editor.** Every element name,
  attribute name, and CSS property comes from a donner registry
  (`kSVGElementNames`, `kSVGPresentationAttributeNames`,
  `PropertyRegistry::kProperties`).
  *Verified by:* a path-scoped banned-patterns rule forbidding
  hardcoded SVG element/attribute string literals under
  `donner/editor/text/**`.
- **`Save` is byte-equivalent to the displayed source.** Because the
  writeback path keeps the buffer current, `Cmd+S` writes exactly what
  the user sees. Reloading produces an equivalent `SVGDocument`.
  *Verified by:* a fuzzer that drives random tool + text-edit
  sequences and asserts `save → load → assert equivalent` on every
  step. Added to `continuous_fuzzing.md`.

## Non-Goals

- **Unified undo across text and canvas.** Text edits routed through
  the incremental classifier are **un-undoable** through
  `UndoTimeline` during M3–M6 — the edit never flows through the
  transform-only timeline and `TextBuffer`'s widget-local undo sees
  none of the edits that were dispatched to the classifier. Crossing
  the text↔canvas boundary clears the opposing stack. This is a
  deliberate scope cut for ship speed; cross-boundary undo is Future
  Work. **Flagged loudly** because this is surprising and will bite.
- **`SVGParser::ParseSVG` on the keystroke path.** Banned by the HARD
  RULE except as a structural-fallback escape hatch; see Open
  Questions for the measurement we owe before turning the kill-switch
  off.
- **Tree-sitter or LSP integration.** Tokenization is XML-parser-
  callback-driven, not grammar-driven.
- **Multi-cursor editing, collaborative editing, real-time conflict
  resolution.**
- **CSS validation.** Highlighting and tokenization, yes; semantic
  property validation, no. Donner's `PropertyRegistry` does that at
  cascade time.
- **Schema-based SVG validation.** No XSD, no SVG2 schema, no "this
  attribute isn't valid on `<rect>`" error squiggles.
- **Editing non-SVG XML.** The token callback is generic XML but
  autocomplete, classification, and incremental dispatch are SVG-only.
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

1. **Land the prerequisites** (M−1, see Implementation Plan) — these
   are the load-bearing in-tree fixes that the rest of the design
   depends on. Most are one-line or one-file changes useful beyond
   the editor.
2. **Build the benchmark first** — `//donner/editor/benchmarks:
   structured_editing_bench` with representative SVGs (simple, large
   `d=`, deep cascade, 10k-line, 500KB). Baseline the current full-
   reparse path. Every number in the Performance table stays "needs
   measurement" until it's grounded in a benchmark number.
3. **Measure real editing sessions** — before M5 ships, instrument
   the editor on a corpus of real editing sessions and report the
   `AttributeValue : TextContent : Structural` ratio. If structural
   is >30%, the whole fast-path architecture is suspect and this
   doc needs another round.

## Implementation Plan

The plan is split into **prerequisites** (M−1), **donner core** (M0–M1),
**visible user value** (M2, M7), and **the bidirectional hot path** (M3–M6).
**M3–M6 ship behind `EditorApp::setStructuredEditingEnabled(false)` by
default**; M7 flips the default to true after a week of continuous fuzzing.
The milestone sequence is deliberately not the dependency order — it's the
user-value order, to ship earlier.

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
      Output: a documented list of safe vs unsafe attributes, and a
      new `AttributeParser::ClearAttribute(name)` API for the
      attribute-removal case.
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
- [ ] **Baseline benchmark.** Land `//donner/editor/benchmarks:
      structured_editing_bench` with representative SVG corpora and
      measure the current full-reparse keystroke-roundtrip on each.
      These are the numbers the M5/M6 targets are judged against.
      Tracked by `//donner/editor/benchmarks:structured_editing_bench`;
      gates CI with **10% variance on headline metrics, 5% on
      sub-metrics, absolute 1ms p99 wall** (Perf review §7).

### M0: Donner-side serialization (shrunk — most of this already exists)

- [ ] `PathSpline::toSVGPathData() → RcString`. Round-trip with
      `PathParser::Parse`. Covers empty/line/cubic/quadratic/arc/closed/
      fractional + round-trip-format equivalence.
- [x] `Lengthd::toRcString() → RcString`. Integer values omit the
      decimal (via `int64_t` cast to avoid `{:g}`'s scientific
      notation for large integer-valued doubles); non-integer values
      print via `{:g}` for shortest round-trippable form. All 17
      CSS unit identifiers handled. Round-trip test in
      `LengthParser_tests.cc` covers 17 units × 9 representative
      values = 153 pairs.
- [ ] Free function `toSVGTransformString(const Transformd&)` in
      `donner/base/`. Decomposes to simplest form: identity → empty,
      translate/scale/rotate → named forms, general → matrix. Round-
      trips with the SVG transform parser.
- [ ] `xml::XMLNode::serializeToString(int indentLevel = 0) →
      RcString`. Self-closing empty elements, escaped attribute values
      (via `EscapeAttributeValue` below), `<!-- -->` + `<![CDATA[ ]]>`.
      **Not responsible for preserving author whitespace** — used only
      for elements created by canvas tools without source location.
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

- [ ] Add `xml::XMLTokenType` enum to `donner/base/xml/`:
      `TagOpen`, `TagName`, `TagClose`, `TagSelfClose`, `AttributeName`,
      `AttributeValue`, `Comment`, `CData`, `TextContent`,
      `XmlDeclaration`, `Doctype`, `EntityRef`, `ProcessingInstruction`,
      `Whitespace`. **Drops `AttributeEquals`** (redundant — the `=` is
      always between `AttributeName.end` and `AttributeValue.start`).
      `AttributeValue` **includes its delimiters** (quote chars) so
      writeback can preserve quote style. `TagOpen` is just `<`,
      `TagName` is the name, `TagClose` is `>`, `TagSelfClose` is `/>`
      — zero overlap.
- [ ] **`XMLParser::Parse` becomes a template on the token sink:**
      ```cpp
      struct NoTokenSink {
        void operator()(XMLTokenType, SourceRange) const noexcept {}
      };
      template <typename TokenSink = NoTokenSink>
      static ParseResult<XMLDocument> Parse(std::string_view, const Options&,
                                            TokenSink&& sink = {}) noexcept;
      ```
      The no-sink path is zero-overhead (dead-stripped). **No
      `std::function` on the parse hot path** — it would add a
      per-token indirect call, heap-capture risk, and footgun
      `throw`-through-noexcept. The template compiles once per editor
      sink type. Non-template overload `Parse(str, options)` stays
      stable for existing callers.
- [ ] **Lexer-only mode.** Add a new entry point
      `XMLParser::Tokenize(std::string_view, TokenSink&&)` that runs
      the tokenizer without constructing `XMLNode`s. Re-tokenizing a
      single line for highlighting cannot afford to allocate
      `AttributesComponent`s or walk `entt::registry` — the cost
      dominates the per-line budget (see Perf §4). The lexer-only
      mode is what M2 (highlighting) actually consumes; the tree-
      building mode is what M5 (classifier) consumes.
- [ ] **Error recovery for the lexer-only mode.** Today `parseElement`
      is strict-fail — on first error it returns a diagnostic and
      stops, so in a 10k-element SVG where the user is typing a
      partial tag at line 500, tokens for lines 501+ never emit.
      A highlighter built on that path goes dark below the cursor.
      Lexer-only mode must synchronize on `<` and `>`, skip over
      the malformed region emitting an `ErrorRecovery` token, and
      continue. Tree-building mode keeps its current strict-fail
      semantics.
- [ ] **Token-callback fuzzer.** Dedicated harness
      `XMLParser_token_callback_fuzzer.cc`: splits the input into
      source bytes + a bitstream; the sink consumes bits to decide
      whether to (a) record the token, (b) call
      `GetAttributeLocation` on a prior element (exercises re-entry),
      or (c) no-op. Asserts tokens are monotonic in offset and
      non-overlapping. Extends the existing structured fuzzer with
      a `bool enable_token_callback` protobuf field. Runs on Linux
      CI per the continuous-fuzzing budget.
- [ ] Tests: token offsets reconstruct the input byte-for-byte for
      a representative corpus; malformed-input tokens match the well-
      formed prefix plus an `ErrorRecovery` marker; empty-sink
      instantiation is zero-overhead (compile-time check via
      `static_assert(sizeof(NoTokenSink) == 1)` and a benchmark).

### M2: SVG-aware syntax highlighting (moved up from original M7)

Ships user-visible value at M1.5, exercises the token callback on
real traffic before M3+ depends on it (DuckBot §5).

- [ ] Replace the editor's regex-based `LanguageDefinition::SVG()` with
      a callback-driven tokenizer that consumes `XMLParser::Tokenize`
      output. Known SVG element names (from `kSVGElementNames`) get
      `Keyword`; unknown elements get `XmlTagName`; known attributes
      (`kSVGPresentationAttributeNames` + the static list of
      structural attributes: `id`, `class`, `style`, `viewBox`,
      `xmlns`, `preserveAspectRatio`) get `XmlAttributeName`; unknown
      attributes get `Identifier`.
- [ ] Add `XmlTagName`, `XmlAttributeName`, `XmlAttributeValue`,
      `XmlComment`, `XmlPunctuation` to `TextEditor::ColorIndex`.
      Audit and remove unused ColorIndex values from the ImGui
      origins (`UserFunction`, `UserType`, `UniformVariable`,
      `GlobalVariable`, `LocalVariable`, `FunctionArgument`).
- [ ] CSS regions: `<style>` element content + inline `style="..."`
      values are passed through the CSS `Tokenizer` (lifted from
      `details/` in M0). Known CSS property names (checked against
      `kValidPresentationAttributes` vs `kProperties` — the presentation-
      attribute vs CSS-only split is already encoded in
      `PropertyRegistry.cc:1129–1200`, 1208) highlight distinctly.
- [ ] **Per-line tokenizer cache with line-start state.** State is
      one of `{Default, InComment, InCData, InTagOpen, InAttrValue}`.
      A line's cached tokens stay valid iff its start-state equals
      its prior start-state. Typing `<!--` at line 1 invalidates
      every downstream line's start-state (worst case: full re-
      tokenize). Add a benchmark case: type `<!--` at the top of a
      10k-line SVG, measure re-tokenize cost. If >5ms, re-tokenize
      async on a background thread with a placeholder palette until
      it catches up.
- [ ] Tests: golden token-stream snapshots on representative SVGs
      (`donner_splash.svg`, an inline-style file, a `<style>`-block
      file, a deliberately malformed file).
- [ ] Tests: a 10k-line SVG with a `<!--` typed at the top re-
      tokenizes under budget.

### M3: `TextPatch` + `SetAttribute` + `AttributeWriteback`

- [ ] **`EditorCommand::Kind::SetAttribute`** variant carrying
      `{element, attributeName, attributeValue}`. Coalesce key is
      `(entity, attributeName)` so successive edits to the same
      attribute collapse.
- [ ] **`donner/editor/TextPatch.{h,cc}`** with view-backed payload:
      ```cpp
      struct TextPatch {
        std::size_t offset;
        std::size_t length;
        std::string_view replacement;  // view into arena, not owned
        std::uint64_t bufferVersion;   // TextBuffer version at build time
        std::uint32_t rangeFingerprint;  // hash of bytes in [offset, offset+length)
      };
      ```
      **The `string_view` payload is anchored in a per-frame patch
      arena** (`PatchArena` — a scratch allocator owned by `EditorApp`,
      reset at the start of every `flushFrame()`). This keeps the
      zero-allocation-per-keystroke property.
- [ ] **`applyPatches(TextBuffer&, std::span<const TextPatch>) →
      ApplyPatchesResult`** sorts by **descending offset** before
      applying. Per-patch preconditions:
      - `offset <= buffer.size() && length <= buffer.size() - offset`
        (overflow-safe check, no `offset + length` add).
      - `isUtf8Boundary(buffer, offset)` and
        `isUtf8Boundary(buffer, offset + length)`.
      - `bufferVersion == buffer.version()` (fingerprint-augmented
        stale guard — see §5 of Security below).
      - `fingerprint(buffer, offset, length) == patch.rangeFingerprint`.
      - `replacement.size() <= kMaxReplacementSize` (1 MB default cap;
        configurable per-editor for paste of large inline-data URLs).
      - **Intra-batch rule:** patches must be non-overlapping. Reject
        the entire batch on overlap (caller bug, not data corruption).
      All rejections are counted in `ApplyPatchesResult` and logged.
- [ ] **`donner/editor/AttributeWriteback.{h,cc}`** builds a
      `TextPatch` from `(SVGElement, attrName, newValue, patchArena)`:
      1. Call `XMLNode::getAttributeLocation(source, attrName)` to
         find the existing span.
      2. Compute `rangeFingerprint` on the old span.
      3. If found, patch replaces the quoted-value span including
         the quote chars, with escaping via `EscapeAttributeValue`
         matching the existing quote char.
      4. If absent, patch inserts ` name="value"` before the closing
         `>` of the opening tag.
- [ ] Tests cover: update existing value, preserve other attributes,
      insert new attribute (self-closing element + with-children
      element), XML escaping of `<`/`&`/`"`/newline/C0, batch with
      multiple patches, overlapping batch rejected, stale-version
      rejected, fingerprint-mismatch rejected, UTF-8-boundary
      rejected, oversized-replacement rejected.
- [ ] **End-to-end writeback fuzzer** (`TextPatch_fuzzer.cc`): random
      `(SVGDocument, element, attrName, newValue)` tuples → build
      patch → apply → re-parse → assert the element's attribute
      round-trips to `newValue`. This is the gating test for XML
      injection escapes; property-testing `EscapeAttributeValue`
      alone does not exercise the round-trip through a real parser.

### M4: Canvas → text writeback

- [ ] **Synchronous patch emission inside `applyMutation`.** When
      `SelectTool::onMouseUp` fires an `EditorCommand::SetTransform`,
      `EditorApp::applyMutation` also:
      1. Calls `AttributeWriteback::build` with the arena.
      2. Pushes the resulting `TextPatch` onto `pendingTextPatches_`.
      The patch is built *at command emission time*, not *at flush
      time* — because the coalescing is already handled per-entity-
      per-attribute and only the *latest* patch for a given
      `(entity, attr)` pair needs to survive, the emission-time
      patches are safe to drop-in-place as coalescing happens.
      **Alternative considered and rejected:** building patches post-
      flushFrame from the coalesced state. That works too, but makes
      the frame pipeline harder to reason about and SecurityBot
      flagged the TOCTOU-race window it opens.
- [ ] **Same-frame drain.** The main loop calls
      `EditorApp::drainPendingTextPatches()` after `flushFrame()` and
      before rendering the source pane. The drain applies patches via
      `applyPatches`, calls `TextBuffer::incrementVersion`, and calls
      `textEditor.resetTextChanged()` so the source-pane write isn't
      misread as a user edit. **Patches and their triggering canvas
      updates share a frame index** — no one-frame visible lag.
      *Regression test:* drag produces a canvas update and a text
      update in the same `flushFrame()`; the frame counter does not
      advance between them.
- [ ] Tests: drag → patch produced; drag → patch applied → text
      contains updated `transform`; drag of element with no existing
      `transform` → patch *inserts* the attribute; drag of element
      with `transform` written across multiple lines → other
      attributes' line breaks preserved.

### M5: Text → canvas incremental parsing

**Gated behind `setStructuredEditingEnabled(false)`.**

- [ ] **Edit-intent capture, not post-hoc diff.** `TextBuffer`
      propagates the actual edit operation that produced a frame's
      change (`insert_char`, `delete_range`, `paste_blob`) rather
      than a line-diff that the classifier has to reconstruct. This
      is the DuckBot §7 "loud" correction — `changedLines_`-as-input
      is fragile and a line-diff loses the operation that produced
      it. The classifier input type becomes
      `EditIntent {Kind, byteRange, payload}`.
- [ ] **`enum class ChangeClass { AttributeValue, TextContent,
      Structural }`.** Classification is a pure function of the
      `EditIntent` plus the prior `XMLDocument`'s node at the edit
      offset.
- [ ] **`xml::XMLNode::nodeAtOffset(source, offset)`** walks the
      tree top-down using stored `FileOffsetRange`s and returns the
      deepest node whose range contains `offset`. O(depth) per call.
- [ ] `classifyChange(SVGDocument&, source, editIntent) → ChangeClass`.
      Pure logic, heavily tested; the fuzzer drives it with random
      `(document, intent)` pairs.
- [ ] **`EditorCommand::Kind::TextEdit`** carries
      `{sourceView, byteRange, classification, bufferVersion}`.
      `sourceView` is a `std::string_view` into `TextBuffer`'s stable
      storage, not an owned string. The command queue coalesces by
      accumulating byte-range union.
- [ ] **`AsyncSVGDocument::applyOne` for `TextEdit`** switches on
      classification:
      - **`AttributeValue`**: extract new value via `sourceView` +
        `byteRange`; look up target element via
        `XMLNode::nodeAtOffset` → `XMLNode ↔ SVGElement` mapping;
        call **`StyleSystem::updateStyle`** for `style=`, otherwise
        `AttributeParser::ParseAndSetAttribute` with a fresh
        `SVGParserContext`. Update the `XMLNode`'s stored attribute
        value so source-location tracking stays consistent.
      - **`TextContent`**: update the `XMLNode`'s text via
        `setValue`. `<style>`/`<script>`/`<title>`/`<desc>` each run
        their per-element post-update hook.
      - **`Structural`**: fall back to `ReplaceDocumentCommand`,
        **with the structural-subtree-identity optimization as a
        follow-up** (Open Question §1).
- [ ] **Tree-identity-preserving delta bookkeeping.** After a
      successful incremental edit changes an attribute value's length,
      shift subsequent `FileOffsetRange`s by the delta. Data structure:
      `std::vector<(offset, cumulativeDelta)>` with a cursor, not an
      order-statistics tree — the order-statistics tree allocates per
      insert (no-malloc rule) and the sequential-typing access
      pattern turns it into an O(1) amortized cursor-advance anyway.
      Delta type is `std::ptrdiff_t`; overflow triggers structural
      fallback.
- [ ] **Renderer-cache invalidation** (DesignReviewBot §5, PerfBot §8).
      Targeted invalidation for each attribute kind is a switch
      statement in the prototype. In donner, let
      `DirtyFlagsComponent` + the systems from
      `incremental_invalidation.md` handle it — the editor's M5 adds
      hooks, not a parallel invalidation graph. Cross-reference that
      doc; don't invent a parallel one.
- [ ] **Two-tier debounce.** 16ms (next-frame) for
      `AttributeValue` edits on known-fast attributes (scalars,
      colors, transforms); 150ms for large-`d` / `points` /
      structural. Gate on `classifyChange` output plus a per-
      attribute `isKnownFast` predicate. Injectable clock for tests.
- [ ] Tests: edit `fill` → only style component invalidated;
      edit `d` → only path geometry invalidated; delete `values=`
      from `feColorMatrix` → stale vector cleared via the new
      `ClearAttribute` API from M−1; type `<` mid-document → falls
      back to structural; edit inside an `&amp;` entity reference →
      classifier handles the entity-boundary case (see Open
      Questions §3).

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

- [ ] `EditorApp` gains `currentFilePath_: std::optional<std::string>`
      and `isDirty_: bool`.
- [ ] `Cmd+S` → save to `currentFilePath_` if present, else prompt.
      `Cmd+Shift+S` → always prompt. Native file dialogs via
      `nfd_extended` (already `dev_dependency = True` in the
      prototype; pull forward).
- [ ] **Symlink-safe writes.** `open(path, O_CREAT | O_EXCL, ...)`
      for new files, `open(path, O_WRONLY | O_NOFOLLOW, ...)` for
      overwrites. No pre-open `stat` (TOCTOU). Fail loudly on
      `ELOOP` — do not silently chase symlinks. **Must pick** a
      macOS sandbox story: either (a) unsandboxed build, or (b)
      `NSSavePanel` directly with
      `-startAccessingSecurityScopedResource` wrapping the write;
      `nfd_extended` needs a test to determine whether it preserves
      the security scope.
- [ ] Window title shows `currentFilePath_` + `●` when dirty.
- [ ] Tests: load → drag → save → reload → assert equivalent. Load →
      text edit → save → reload → assert equivalent. Symlink chase
      rejected.

### M8: Flip the kill-switch default to `true`

- [ ] After a week of continuous fuzzing on the token callback,
      writeback, and structured-editing driver fuzzers with no new
      crashes, flip `setStructuredEditingEnabled`'s default to
      `true`. The flag stays in the API for a release cycle so
      users can opt out while we collect field feedback.

## Background

### Current state in `donner/editor` (post-M3)

```
TextEditor (UI)            EditorApp + AsyncSVGDocument           Canvas
    │                                │                              │
    │── isTextChanged() ────────────>│                              │
    │   ReplaceDocument(source) ────>│  SVGParser::ParseSVG          │
    │   (full re-parse every         │  setDocument()                │
    │    keystroke)                  │── frameVersion++ ───────────>│
    │                                │                              │
    │                                │<── SetTransform(elt, t) ────│  drag
    │                                │    setTransform on entity    │
    │                                │    (text NOT updated)        │
    │                                │                              │
    │<── selectAndFocus() ──────────│  Source-range highlight       │
    │   (read-only)                  │                              │
    │<── setErrorMarkers() ─────────│  lastParseError() (M3)         │
```

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

### The XML tree is the spine

```
                      ┌─────────────────────┐
                      │   XMLDocument       │
                      │   (source of truth) │
                      └──────────┬──────────┘
                                 │
                     XMLNode with source range
                                 │
                 ┌───────────────┼───────────────┐
                 │               │               │
                 ▼               ▼               ▼
        ┌────────────┐    ┌────────────┐    ┌────────────┐
        │ TextBuffer │    │ ECS entity │    │  XMLNode   │
        │ projection │    │ projection │    │  children  │
        │ (source    │    │ (parsed    │    │ (tree      │
        │  text)     │    │  values)   │    │  recursion)│
        └────────────┘    └────────────┘    └────────────┘
             ▲                  ▲
             │                  │
         TextPatch        SetAttribute
        (splice)         (ECS update)
             │                  │
             └─── both fan out from XMLNode::setAttribute ───┘
```

Canvas edits and text edits both land on the `XMLNode` first. The node's
`setAttribute` (new in M3, built on `AttributeWriteback`) fans out to two
projectors: one that produces a `TextPatch` for the source view, one that
calls `AttributeParser::ParseAndSetAttribute` for the ECS view. Neither
projection is load-bearing on its own — the XMLNode is the source of
truth, and the two views are consistent because they both derive from it.

### Identity preservation is the invariant

The HARD RULE, precisely: **every edit preserves the identity of the
`XMLDocument` and the `SVGDocument` objects**; text↔canvas edits that
land inside an existing node also preserve that node's identity.
Structural fallback is the only case where subtree identity breaks, and
even then `SVGDocument` identity is preserved (we call
`ReplaceDocumentCommand` under the hood, which constructs a fresh
`SVGDocument` — the editor's accessor is still stable). Tree identity is
tested via pointer equality assertions across edit sequences, not a
`ParseSVG` call counter (which is a proxy for identity).

### Why the sideband is not another EditorCommand

A prior draft argued this on three grounds. The strongest argument,
refined:

1. **Coalescing shape is inverted.** Canvas `EditorCommand`s coalesce
   per-entity-per-attribute keeping the latest; `TextPatch`es must
   apply *all* intermediate writes in reverse-offset order. The
   queue's coalesce rule is the opposite of what text patches need.
2. **Storage target.** The queue mutates `SVGDocument`. Text patches
   mutate `TextBuffer`, which lives on the `TextEditor` widget. Putting
   them in the same queue conflates two backing stores.
3. **Apply timing.** Patches drain *after* `flushFrame` so they reflect
   the post-coalesce state. Putting them in the queue would require a
   second drain pass after the first — essentially the sideband wearing
   a different hat.

**Patches are built at `applyMutation` time**, not at flush time. This
pushes the per-entity-per-attribute coalescing responsibility onto the
patch sideband too — a second `SetTransformCommand` on the same entity
produces a second patch that supersedes the first in the arena. The
arena is drained at flush time in source order; duplicate-key patches
resolve via `(entity, attr) → latest patch` lookup before `applyPatches`
runs. Same coalesce rule, different target.

### Concurrent-edit coherence (TOCTOU fix)

`TextPatch` carries a `bufferVersion` stamped at build time and a
`rangeFingerprint` over the bytes it intends to replace. `TextBuffer`
increments its version on every user keystroke and on every
`applyPatches` call. At drain time, `applyPatches` rejects any patch
whose `bufferVersion` is older than `TextBuffer::version()` or whose
fingerprint doesn't match the current bytes at `[offset, offset+length)`.
This catches the SecurityBot §6 middle-case TOCTOU: the user types
inside a `transform="…"` value while a drag's writeback patch is
queued, the buffer version advances, the patch is dropped, and the
next drag emits a fresh patch against the updated buffer. The dropped
patch is logged but does not fail the frame — the canvas mutation
already landed in the ECS, and the next `SetTransformCommand` will
re-patch.

### No delta accumulator (DuckBot §7)

Under the framing flip the per-keystroke delta accumulator
disappears. Edits that land inside an `XMLNode` update **that node's
stored attribute value in place**; offsets of subsequent siblings and
descendants only need fixup when the XMLNode's own source range
changes length, which happens at the debounce drain, not per keystroke.
At drain time a single O(N-subtree) walk suffices because edits are
already batched. Sequential typing inside one attribute is O(1) at
drain because only one node changed length.

### Compile-time feature gate

`EditorApp::setStructuredEditingEnabled(bool)` is a runtime flag, not
a compile-time switch. M3–M6 code paths compile and link always; the
flag gates only the dispatch inside `AsyncSVGDocument::applyOne` for
`TextEdit` commands. When the flag is off, `TextEdit` is routed to
`ReplaceDocumentCommand` unconditionally — the current M3 behavior.
M0–M2 core additions (serializers, token callback, patch
infrastructure) are subtractive-safe and ship unconditionally. M7
(Save) is flag-orthogonal. Rollback is one line.

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

// donner/editor/EditorCommand.h additions (M3, M5)
namespace donner::editor {

enum class ChangeClass : std::uint8_t {
  AttributeValue,
  TextContent,
  Structural,
};

struct EditorCommand {
  enum class Kind : std::uint8_t {
    SetTransform,
    SetAttribute,     // M3
    TextEdit,         // M5
    ReplaceDocument,
    DeleteElement,
  };

  Kind kind = Kind::SetTransform;
  std::optional<svg::SVGElement> element;

  // Existing:
  Transform2d transform;
  std::string bytes;  // ReplaceDocument only

  // SetAttribute (M3). attributeName/Value are views into the
  // command queue's per-frame arena.
  std::string_view attributeName;
  std::string_view attributeValue;

  // TextEdit (M5). sourceView is into TextBuffer's stable storage;
  // byteRange is the union of coalesced edit ranges; classification
  // is derived from the owning EditIntent.
  std::string_view sourceView;
  std::size_t byteRangeStart = 0;
  std::size_t byteRangeEnd = 0;
  ChangeClass classification = ChangeClass::Structural;
  std::uint64_t bufferVersionAtBuild = 0;

  static EditorCommand SetAttributeCommand(
      svg::SVGElement element,
      std::string_view name, std::string_view value,
      PatchArena& arena);
  static EditorCommand TextEditCommand(
      std::string_view source, std::size_t start, std::size_t end,
      ChangeClass classification, std::uint64_t version);
};

}  // namespace donner::editor

// donner/editor/TextPatch.h (M3)
namespace donner::editor {

struct TextPatch {
  std::size_t offset = 0;
  std::size_t length = 0;
  std::string_view replacement;  // view into PatchArena, not owned
  std::uint64_t bufferVersion = 0;
  std::uint32_t rangeFingerprint = 0;
};

struct ApplyPatchesResult {
  std::size_t applied = 0;
  std::size_t rejectedStaleVersion = 0;
  std::size_t rejectedFingerprint = 0;
  std::size_t rejectedUtf8Boundary = 0;
  std::size_t rejectedOversized = 0;
  std::size_t rejectedOverlap = 0;  // fatal — returns immediately
};

ApplyPatchesResult applyPatches(TextBuffer& buffer,
                                std::span<const TextPatch> patches) noexcept;

// PatchArena is a scratch allocator owned by EditorApp; reset at the
// start of every flushFrame(). All std::string_view payloads in
// pending patches point into its backing storage.
class PatchArena;

}  // namespace donner::editor
```

## Error Handling

- **Attribute parse rejects the new value** (hot path): leave prior
  value in ECS, surface warning marker via
  `TextEditor::setErrorMarkers`, **do not fall back to full re-parse**
  — that would discard unrelated edits. The user keeps typing.
- **Structural fallback fails** (`SVGParser::ParseSVG` returns error
  on the whole buffer): existing `lastParseError()` (M3) keeps the
  prior document and surfaces the diagnostic.
- **Patch stale-version or fingerprint mismatch**: drop the patch,
  increment `ApplyPatchesResult` counter, log. The canvas mutation
  is already in the ECS; the next tool action will re-emit a fresh
  patch against the current buffer.
- **UTF-8 boundary split in patch offset/length**: reject the patch.
  Never corrupt the buffer. Count and log.
- **Oversized replacement** (>1 MB default): reject. Caller bug or
  adversarial input.
- **Overlapping patches in one batch**: fatal batch rejection.
  Indicates an editor bug — patches should be non-overlapping by
  construction (coalesced via the arena's `(entity, attr) → latest`
  lookup). Log and assert in debug.
- **Concurrent text + canvas edit (TOCTOU)**: fingerprint mismatch
  rejects the stale patch; user's in-attribute typing is preserved.
  Last-writer-wins, but the loser's write is dropped cleanly.
- **`EscapeAttributeValue` called with NUL/surrogate halves**:
  returns `std::nullopt`; writeback rejects the mutation rather than
  producing invalid XML. Surfaced to the user as "cannot encode this
  value."
- **`AttributeParser` re-entrancy on element-specific list attributes**
  (`feColorMatrix values=`, etc.): the new `ClearAttribute` API
  (M−1) is called before `ParseAndSetAttribute` when the classifier
  detects "attribute removed from source."
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
| `classifyChange` | 10 µs | needs measurement |
| `nodeAtOffset` walk | 5 µs | needs measurement |
| `AttributeParser::ParseAndSetAttribute` — scalar (`fill`, `cx`) | 50 µs | needs measurement |
| `AttributeParser::ParseAndSetAttribute` — short path (<100 cmds) | 500 µs | needs measurement |
| `AttributeParser::ParseAndSetAttribute` — large path (500+ cmds) | **3 ms — may exceed frame budget alone** | needs measurement |
| Targeted invalidation (one entity) | 50 µs | needs measurement |
| `applyPatches` (single drag patch) | 20 µs | needs measurement |
| Token callback re-tokenize (per line, cached) | 100 µs | needs measurement — lexer-only mode required |
| Per-line re-tokenize on `<!--` at top of 10k-line file | 5 ms | needs measurement; fall back to async if over |
| Cascade recompute for inline `style=` edit on deeply-nested element | needs measurement | |
| Renderer re-raster after targeted invalidation (per backend) | needs measurement | TinySkiaBot / SkiaBot / GeodeBot bot-handoff |
| Structural fallback keystroke roundtrip (500KB file) | **20–100 ms — off-main-thread** | needs measurement; gate on user-visible lag budget |
| **Full incremental keystroke roundtrip — scalar attr** | **<1 ms** | headline goal |

Anti-targets:
- Walking the entire `XMLNode` tree per edit (the offset-fixup walk
  is O(subtree), not O(document), and runs at debounce drain, not
  per keystroke).
- Re-running `SVGParser::ParseSVG` on the incremental path (HARD
  RULE).
- Allocating on the hot path:
  - `TextEdit::sourceView` and `TextPatch::replacement` are
    `std::string_view` into stable backing storage
    (`TextBuffer`, `PatchArena`).
  - `tokenCallback` is a template parameter, not `std::function`.
  - `EditorCommand::SetAttribute::attributeName/Value` are arena
    string_views, not owned strings.
  - `AttributeParser::ParseAndSetAttribute` already takes
    `std::string_view` — no copy on the way in.
- Building full `XMLNode` trees per re-tokenize. Lexer-only mode is
  required — tree-building mode costs orders of magnitude more due to
  `entt::registry` `get_or_emplace` churn.

Two-tier debounce:
- **Tier 1 (16ms, next-frame):** `AttributeValue` edits on known-fast
  attributes. Gate via `classifyChange` + an `isKnownFast` predicate.
  User-visible latency is one frame.
- **Tier 2 (150ms, typing-idle):** everything else, including
  `d=`/`points=`/`transform=` on large values, `<style>` block edits,
  and structural fallbacks. 150ms is on the high end of perceptible
  but the fast path covers the attributes users type into fastest.

Benchmark gate (PerfBot §7):
- **10% variance** on headline keystroke-roundtrip.
- **5% variance** on sub-metrics (`classifyChange`, `nodeAtOffset`).
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

1. **`TextPatch` apply.** Five mandatory checks before every splice:
   - **Integer-overflow-safe length check:**
     `offset <= buffer.size() && length <= buffer.size() - offset`
     (no `offset + length` add). CWE-190.
   - **UTF-8 boundary check:** both `offset` and `offset + length`
     land on codepoint boundaries. Corrupting mid-codepoint enables
     a downstream XML injection and is classic CWE-176.
   - **`bufferVersion` + `rangeFingerprint` check:** the TOCTOU fix
     described in Proposed Architecture. Without the fingerprint,
     the stale-length guard catches only tail-case overruns, not
     the middle case where the user typed inside the span. CWE-362.
   - **`kMaxReplacementSize` check** (1 MB default). Prevents an
     adversarial tool from queueing gigabyte patches.
   - **Non-overlapping intra-batch rule.** Batches with overlaps are
     fatally rejected. Indicates a caller bug.
   All rejections are counted and logged.

2. **`EscapeAttributeValue` totality.** Quote-aware, escapes the five
   XML predefined entities, escapes C0/C1 control characters per
   XML 1.0, **rejects `\0` and surrogate halves** (returns
   `nullopt`). Property testing alone is insufficient — the gating
   test is the **end-to-end writeback fuzzer** (see Testing §) that
   exercises the full canvas → patch → XML reload → assert round-trip.
   CWE-91, CWE-117.

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
     `ClearAttribute(name)` and call it from the editor's
     attribute-removal path.
   - `SVGParserContext` warning accumulator. Fix: construct fresh
     per edit or drain after each call. Unbounded growth is
     CWE-401.
   - Element-specific direct setters (`setDxList`, etc.) must mark
     dirty flags. Fix: audit and add where missing.
   - `style=` attribute is additive via `StyleComponent::setStyle`
     (M−1 keystone bug). Fix: uncomment `clearStyle()`.

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
- `TextPatch_fuzzer.cc` (M3): random `(buffer, patches)` batches,
  asserts no crash and no out-of-bounds writes.
- `StructuredEditingDriver_fuzzer.cc` (M5): random interleaving of
  tool actions and text edits, asserts `save → load → equivalent`
  on every step.
- Extended `XMLParser_fuzzer` with the new DoS caps exercised.
- Extended `XMLParser_structured_fuzzer` with
  `bool enable_token_callback`.

All must run in continuous fuzzing, not just once at merge.

## Testing and Validation

- **Unit tests** per milestone, same package as the module under test.
- **Round-trip golden tests:**
  - M0 serializers: `parse → serialize → parse → assert equivalence`.
  - M4 canvas→text: `load → drag → diff touches only transform spans`.
  - M7 save: `load → drag → save → reload → assert equivalent`.
- **Tree-identity assertions:** the M5 test fixture checks pointer
  equality of the `XMLDocument` and `SVGDocument` across edit sequences.
  Any violation fails the test.
- **`ReplaceDocumentCommand` counter:** per-test upper bound = initial
  loads + explicit opens + declared structural fallbacks.
- **Fuzzers** — listed in Security §.
- **Benchmarks:** `//donner/editor/benchmarks:structured_editing_bench`
  gates CI on 10%/5% variance + 1ms p99 absolute wall.
- **Banned-patterns lint:** forbids hardcoded SVG element/attribute
  literals under `donner/editor/text/**`.
- **Real-editing-session measurement:** before M5 ships, instrument
  the editor on a corpus of actual editing sessions and report the
  `AttributeValue : TextContent : Structural` ratio. If structural
  is >30% on realistic input, the fast-path design is suspect.

## Reversibility

Blast radius is large; reversibility is load-bearing.

- **M−1 fixes** are narrow, useful independently, and ship as standalone
  PRs. Each is revertible on its own.
- **M0 core additions** (serializers, `XMLTokenType`, `EscapeAttributeValue`,
  `PatchArena`) are subtractive-safe and have no dependent in-tree consumers
  besides the editor.
- **M1 token callback** lands as a new API without changing existing
  `XMLParser::Parse` behavior. Existing callers instantiate `NoTokenSink`
  (the default) and get dead-code-stripped to the current path.
- **M2 syntax highlighting** is purely a display change. Reverting
  restores the regex-based highlighter.
- **M3–M6 are flag-gated.** `EditorApp::setStructuredEditingEnabled(false)`
  routes `TextEdit` commands to `ReplaceDocumentCommand`, the current M3
  behavior. The flag defaults to `false` on M6 land. M8 flips the default
  after a fuzzing soak.
- **M7 save** is flag-orthogonal. Reverting removes the menu items.

If the design is abandoned, M−1–M2 and M7 stay (they're useful on their
own). Only M3–M6 get removed, and they live entirely under
`donner/editor/text_sync/` to make the surgery obvious.

## Alternatives Considered

### Tree-sitter incremental parsing
Rejected. 1.5MB grammar artifact, parallel parse tree to keep in sync
with `xml::XMLParser`, and the attribute-level fast path is sufficient
for SVG editing without adding a dependency.

### Full re-parse with DOM diffing
Rejected. The parse dominates the latency we're trying to avoid, and
the diff step is more complex than incremental classification.

### Source text as source of truth (canvas-only editing)
Rejected. Loses formatting, comments, attribute order, and authorial
intent. Also rules out typing SVG directly.

### Text and canvas as peers (the prototype's design)
Rejected in favor of the framing flip (DuckBot §1). The peer model
forces a delta accumulator, a classifier that has to be correct rather
than merely fast, and a sync pump that can drift. The tree-spine
model makes the classifier a fast-path optimization and lets the
same-frame drain guarantee consistency by construction.

### Rope-backed `XMLNode` source storage
Rejected for now. A rope would make offset bookkeeping automatic but
requires plumbing through every parser. The arena-based patch
sideband is a smaller, contained change that hits the perf target.
Listed under Future Work.

### Routing canvas → text writeback through `EditorCommand`
Rejected. See "Why the sideband is not another EditorCommand" in
Proposed Architecture.

### `std::function<void(XMLTokenType, SourceRange)>` for the token
callback
Rejected (ParserBot §1). Heap-capture risk, per-token indirect call,
`-fno-exceptions` footgun. Template-on-sink with `NoTokenSink` default
is the shape.

### Caching `SourceRange` per attribute on every `XMLNode`
Rejected (ParserBot §3). For a 10k-element 5-attr-avg SVG that's
~1.2 MB resident plus an unordered_map per element. The re-parse
strategy in `GetAttributeLocation` reads ~200 bytes hot per call and
costs a few µs. An editor writes back one attribute per drag frame
— re-parse wins by two orders of magnitude.

### Order-statistics tree for the delta accumulator
Rejected (PerfBot §2, DuckBot §7). Allocates per insert (no-malloc
rule violation) and the sequential-typing access pattern makes a
`std::vector<(offset, delta)>` with a cursor equivalent at zero
allocation. Ultimately the accumulator disappeared entirely under
the tree-spine framing.

### `CSS::ParseStylesheet` full re-parse on every `<style>` edit
Rejected for the fast path but **accepted as the structural
fallback**. Per CSSBot §6, donner's cascade today doesn't incrementally
track per-stylesheet contributions — any `<style>` edit today triggers
`needsFullStyleRecompute = true` which is O(document). The right
incremental path belongs in
[`incremental_invalidation.md`](incremental_invalidation.md), not here.
Editor's M5 accepts the full restyle cost on `<style>` edits as a
known deficiency and documents it.

## Open Questions

1. **What fraction of real keystrokes hit `Structural`?** The HARD
   RULE only works if fallback is rare. Must be measured on a real
   editing corpus *before* M5 lands. If the measured structural rate
   is >30%, the design needs another round. Tracked as an M−1
   deliverable.

2. **Subtree-identity structural fallback.** Today's `Structural`
   classification calls `ReplaceDocumentCommand` for the whole
   buffer. An optimization: if the edit's byte range is contained
   within a single top-level element, re-parse only that element and
   splice it into the existing tree. Preserves tree identity for
   siblings. Follow-up after the structural-rate measurement decides
   whether it's worth the complexity.

3. **Entity-reference boundaries.** The classifier's "entire byte
   range sits between the quotes" rule is surface-simple. What about
   an edit inside `&amp;quot;` — does typing inside the entity
   reference get classified as `AttributeValue` or `Structural`?
   Needs a classifier test case and explicit logic.

4. **Inline `style` surgical patching.** When a tool changes one CSS
   property, do we rewrite the whole `style="…"` value or surgically
   patch the one declaration span? CSSBot flagged that the prereq
   (`Declaration::sourceRange`) is a 5-line parser change. **Initial
   answer:** surgical patch via `Declaration::toCssText` +
   sub-applyPatches inside the attribute span, falling back to whole-
   value rewrite if the declaration can't be located.

5. **Attribute order on writeback.** Preserve original source order
   (author-friendly, more code) or normalize to canonical order
   (simpler, surprises authors)? **Initial answer:** preserve order;
   new attributes append at end.

6. **Latent bugs to file separately** (surfaced by CSSBot):
   `mergeStyleDeclarations` does case-insensitive name matching,
   which is **incorrect for custom properties** (`--foo`). Custom
   property names are case-sensitive per spec. Not this doc's to
   fix, but track as a bug if the editor starts writing custom
   properties.

7. **macOS sandbox story for `nfd_extended`.** Does it preserve
   security-scoped URLs or return paths the sandbox refuses? Test
   in M7; commit to sandboxed-with-scope or unsandboxed-with-doc.

8. **ImGui widget cost on large buffers.** Is the source pane
   itself (immediate-mode, per-frame render) a dominant cost on
   10k-line files? Must be measured before we optimize anything
   under it.

## Future Work

- [ ] Cross-boundary undo: one undo reverts both the canvas and the
      corresponding text splice.
- [ ] Per-element attribute filtering for autocomplete
      (requires new registry metadata — separate project).
- [ ] CSS value vocabulary reflection for value autocomplete
      (separate project).
- [ ] Named SVG color suggestions inside color attribute values.
- [ ] Snippet expansion (`rect` → full element template).
- [ ] `Format Document` command (normalize indentation, attribute
      spacing, quote style).
- [ ] Color picker integration.
- [ ] Rope-backed `XMLNode` storage for O(1) offset bookkeeping.
- [ ] Tree-sitter as an alternative tokenizer behind a compile-time
      flag, if profiling shows the XML callback path is a bottleneck
      on very large files.
- [ ] Real-time collaborative editing with operational transform.
