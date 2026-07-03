---
name: donner-parsers-css-text
description: >-
  Conventions for Donner's parser suite (XML/SVG/CSS/scalar), the ParseWarningSink diagnostics
  contract, the CSS engine and PropertyRegistry, and the text-rendering tiers
  (TextBackendSimple/TextBackendFull). Use when editing or adding a parser under
  donner/base/parser, donner/base/xml, donner/svg/parser, or donner/css/parser; when emitting or
  testing ParseDiagnostic/ParseWarningSink output; when adding a CSS property or touching
  selectors/cascade/StyleSystem; or when changing text shaping/fonts under donner/svg/text and
  deciding which build tiers (--config=text-full, tiny) must be tested.
---

# Donner Parsers, CSS Engine, and Text Tiers

## 1. Parser namespace map

Four parser layers, each in its own namespace and directory (full reference:
`docs/parser_namespaces.dox`):

| Namespace             | Directory             | Contents                                                                                                                                                                                                         |
| --------------------- | --------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `donner::parser`      | `donner/base/parser/` | Shared scalar parsers: `NumberParser`, `LengthParser`, `IntegerParser`, `DataUrlParser`, `LineOffsets`                                                                                                           |
| `donner::xml`         | `donner/base/xml/`    | `XMLParser`, `XMLTokenizer.h`, `XMLIncrementalParser`, `XMLDocument`/`XMLNode`                                                                                                                                   |
| `donner::svg::parser` | `donner/svg/parser/`  | `SVGParser::ParseSVG` entry point, plus `PathParser`, `TransformParser`, `ViewBoxParser`, `AttributeParser`, `ListParser`, ...                                                                                   |
| `donner::css::parser` | `donner/css/parser/`  | `StylesheetParser`, `SelectorParser`, `DeclarationListParser`, `ValueParser`, `ColorParser`, `AnbMicrosyntaxParser`; tokenizer internals in `donner/css/parser/details/Tokenizer.h` and `ComponentValueStream.h` |

Put a new parser in the layer matching what it consumes: a bare scalar grammar goes in
`donner/base/parser`, an SVG attribute microsyntax in `donner/svg/parser`, a CSS value grammar in
`donner/css/parser`. Wrong layer = wrong namespace = dependency cycles later.

## 2. Diagnostics contract (fatal vs. warning)

Two channels, never mixed (full reference: `docs/parser_diagnostics.md`):

- **Fatal errors** → return `ParseResult<T>` (`donner/base/ParseResult.h`). Holds result, error
  (`ParseDiagnostic`), or both (partial result + error).
- **Non-fatal warnings** → `ParseWarningSink&` parameter (`donner/base/ParseWarningSink.h`).
  Warning-producing entry points (`SVGParser::ParseSVG`, `StylesheetParser::Parse`,
  `CSS::ParseStylesheet`, `StyleSystem::computeStyle`) take the sink **explicitly — no
  sink-omitting convenience overloads**. Rationale: warning collection must be visible at every
  call site, so silent discard is always a deliberate `ParseWarningSink::Disabled()`.

Choosing a channel for a _new_ diagnostic: a leaf value parser whose contract is "produce this
value" returns a fatal `ParseResult` error when it can't (`PathParser`, `TransformParser`,
`SelectorParser` all do). The _enclosing_ layer decides whether that is recoverable: document- and
stylesheet-level parsing downgrades per-attribute / per-declaration failures to warnings and
continues with the default value (e.g. a bad `stdDeviation` becomes a sink warning at the SVG
layer; `PropertyRegistry::parseProperty` returns an `optional<ParseDiagnostic>` that `StyleSystem`
forwards to the sink). So: warn when a fallback exists and parsing continues; error when this
call cannot fulfill its contract.

Rules that prevent recurring bugs:

- **Lazy emission.** Use the factory form so message formatting is skipped when the sink is
  disabled:
  ```cpp
  sink.add([&] { return ParseDiagnostic::Warning("...", sourceRange); });
  ```
  Eagerly formatting a string that Disabled() throws away is wasted work on the hot parse path.
- **Discarding**: `auto disabled = ParseWarningSink::Disabled();` then pass `disabled`. Never
  invent an overload without the sink parameter.
- **CRITICAL — subparser offset remapping.** When a subparser runs on a _substring_ of the parent
  input, its diagnostics carry substring-local offsets. Merge with:
  ```cpp
  ParseWarningSink subSink;
  auto subResult = SubParser::Parse(substring, subSink);
  parentSink.mergeFromSubparser(std::move(subSink), parentOffset);
  ```
  Forgetting the remap yields diagnostics whose caret points at the wrong document location —
  the message looks right but the editor squiggle lands on unrelated text. Inside SVG parsing,
  `donner/svg/parser/details/SVGParserContext.h` provides `addSubparserWarning()` /
  `fromSubparser()` which do the same remap per attribute value. Template for testing the remap:
  `TEST(SVGParser, Attributes)` in `donner/svg/parser/tests/SVGParser_tests.cc` asserts the
  document-absolute `(line, offset)` of an attribute-level warning via its `ParseWarningIs`
  matcher — copy that shape to lock down a remapped diagnostic position.
- `XMLParser::Parse` takes only `Options` (no sink) — XML-level failures are all fatal via
  `ParseResult<XMLDocument>`. Warnings begin at the SVG layer.
- Rendering for humans: `DiagnosticRenderer::format` / `formatAll`
  (`donner/base/DiagnosticRenderer.h`) prints source context with caret/tilde markers.

## 3. Testing a parser

- Use the gmock matchers in `donner/base/tests/ParseResultTestUtils.h`: `NoParseError()`,
  `ParseErrorIs(...)`, `ParseErrorPos(line, offset)`, `ParseErrorRange(start, end)`,
  `ParseErrorEndOfString()`, `ParseResultIs(...)`, `ParseResultAndError(...)`. A failing
  `EXPECT_THAT(result, ParseErrorRange(0, 3))` prints expected-vs-actual offsets;
  `EXPECT_TRUE(result.hasError())` prints "false" and forces a rerun. Matcher-first is the
  project-wide rule (see project `CLAUDE.md` §Test Diagnosability).
- Write **range-accuracy tests**: assert the exact `[start, end)` byte span of each diagnostic,
  not just the message. Wrong ranges are invisible until an editor draws the squiggle.
- `DiagnosticRenderer` behavior is locked by golden-string tests in
  `donner/base/tests/DiagnosticRenderer_tests.cc` — extend those when changing renderer output.
- **TRAP:** `DataUrlParser` (`donner/base/parser/DataUrlParser.h`) still returns the legacy
  `std::variant<Result, DataUrlParserError>` instead of `ParseResult`. This is a documented
  deferred migration — never copy it as the template for a new parser.
- Most string-consuming parsers have a libFuzzer target (`donner_cc_fuzzer` in the package
  `BUILD.bazel`, e.g. `//donner/css/parser:stylesheet_parser_fuzzer`) with a checked-in corpus
  under `tests/*_corpus/`. When adding or changing grammar, extend the fuzzer + corpus and run it
  with `--config=asan-fuzzer`. List current targets:
  `grep -rn 'donner_cc_fuzzer' --include=BUILD.bazel donner/ -A 1`. Details: **donner-fuzzing**.

## 4. CSS pipeline (parse → cascade)

Full architecture doc: `docs/css.md`; token-stream design:
`docs/design_docs/0019-css_token_stream.md`.

```
source text
  → Tokenizer (donner/css/parser/details/Tokenizer.h)
  → ComponentValue stream (details/ComponentValueStream.h)
  → StylesheetParser::Parse(str, warningSink)
  → Stylesheet of SelectorRule { Selector, declarations }
```

- High-level facade: `css::CSS::ParseStylesheet(str, warningSink)` in `donner/css/CSS.h`.
- Selector-only parsing: `SelectorParser::Parse(str)` returns `ParseResult<Selector>` (no sink —
  selector parsing has no warning channel).
- Cascade lives in `donner/svg/components/style/StyleSystem.h`:
  `StyleSystem::computeStyle(handle, sink)` / `computeAllStyles(registry, sink)` produce
  `ComputedStyleComponent` per entity. `MatchedStyleRule` (same header) records which rule
  matched, its `css::Specificity`, and the rule/selector/declaration `SourceRange`s in the SVG
  source — this is what editor tooling uses to map computed style back to text.

## 5. Adding a CSS property — checklist

All in `donner/svg/properties/`. Most registry gaps compile fine but fail _silently_: a member
missing from the `allProperties()` tuple never cascades, and a name listed in
`kValidPresentationAttributeEntries` but not `kProperties` lands in `unparsedProperties` with no
diagnostic. Only a name absent from **both** maps produces a runtime "Unknown property" warning
(`PropertyRegistry::parseProperty` → `StyleSystem` warning sink).

1. Add a `Property<T, PropertyCascade::...>` member to `PropertyRegistry`
   (`PropertyRegistry.h`) with name string + default-value lambda. The three cascade modes
   (`Property.h`): `None` (does not inherit), `Inherit` (inherits unconditionally),
   `PaintInherit` (inherits unless the child is instantiated as a paint server — the `<pattern>`
   recursion exception). Wrong choice compiles fine and silently changes inheritance.
2. Add the member to the `allProperties()` tuple in the same header (cascade iterates this tuple;
   a member absent from the tuple is dead — it never inherits). Tuple position is not
   semantically significant — append matching the member order for readability.
3. Register the parse function in the `kProperties` compile-time map in `PropertyRegistry.cc`
   (a lambda dispatching to a parser in `PropertyParsing` or `donner/css/parser/...`).
4. If it is also an SVG presentation attribute, add it to `kValidPresentationAttributeEntries`
   in `PropertyRegistry.cc`. That table is a fixed-size `std::array<..., N>` — bump the
   hand-maintained `N` in the declaration too, or you get an opaque aggregate-initializer count
   error. Only touch `ParsePresentationAttribute` (`PresentationAttributeParsing.h/.cc`) if the
   attribute needs _element-specific microsyntax_ (like `x`/`width` on `<rect>` vs `<image>`, or
   `stdDeviation`); a plain CSS property registered in `kProperties` is picked up generically —
   the switch there defaults to `return false` for all other elements.
5. Tests in `donner/svg/properties/tests/` (`PropertyRegistry_tests.cc`,
   `PropertyParsing_tests.cc`) plus a `StyleSystem` cascade test if inheritance is nontrivial.
6. If the property is renderable, check the resvg suite for newly-passing/failing cases — see
   **donner-resvg-triage**.
7. Ask: does an existing fuzzer reach the new value grammar? If not, extend one (§3).

To see the current property inventory, query the source (do not trust any hardcoded count):
`grep -n 'Property<' donner/svg/properties/PropertyRegistry.h`.

## 6. Text-rendering tiers

The `.bazelrc` is the source of truth for tier flags — if any doc disagrees with `.bazelrc`,
`.bazelrc` wins:

| Build                | Flags set                  | Backend             | Libraries                                               |
| -------------------- | -------------------------- | ------------------- | ------------------------------------------------------- |
| default (no config)  | `text=true`                | `TextBackendSimple` | stb_truetype (TTF/OTF/WOFF1)                            |
| `--config=text-full` | `text=true text_full=true` | `TextBackendFull`   | FreeType + HarfBuzz + WOFF2; defines `DONNER_TEXT_FULL` |
| `--config=no-text`   | `text=false`               | none                | —                                                       |
| `--config=tiny`      | `filters=false text=false` | none                | —                                                       |

Verify current flag wiring anytime with: `grep -n 'text' .bazelrc` (from the repo root).

Architecture (headers in `donner/svg/text/`):

- `TextEngine` (`TextEngine.h`) is the facade — owns the selected `TextBackend` and exposes
  `layout(computedText, params)`, font metrics, and `glyphOutline(font, glyphIndex, scale)`.
  Callers never see which backend is active.
- `TextBackend` (`TextBackend.h`) is the abstract interface; `TextBackendSimple` /
  `TextBackendFull` implement it (`shapeRun`, metrics, outlines, bitmap glyphs).
- ECS side: `TextSystem` (`donner/svg/components/text/TextSystem.h`)
  `instantiateAllComputedComponents(registry, sink)` builds `ComputedTextComponent` per text
  element.
- Font registration: `FontManager::addFontFace(const css::FontFace&)`
  (`donner/svg/resources/FontManager.h`). Font design history:
  `docs/design_docs/0008-css_fonts.md`, `docs/design_docs/0010-text_rendering.md`.

## 7. BUILD gating pitfall: `@harfbuzz` is dev-only

`//donner/svg/text:text_backend_full` is `target_compatible_with`-gated on
`//donner/svg/renderer:text_full_enabled`, and `text_engine` pulls it in only via a `select()`
(see the comment in `donner/svg/text/BUILD.bazel` above the `text_backend_full` target).
Reason: `@harfbuzz` lives in the dev-only non-BCR (Bazel Central Registry) module extension —
an unconditional dep on `text_backend_full` breaks every BCR consumer of Donner, where
`@harfbuzz` is not declared at all. Never add a plain `deps = [":text_backend_full"]`; copy the
existing `select()` pattern.

## 8. Testing across tiers

- **Unit tests get variants** via `donner_cc_test(..., variants = ["tiny", "text_full", "geode"])`
  — wrapper mechanics are owned by **donner-build-test**. The text-specific nuance: variant names
  use underscores (`text_full`), bazel configs use dashes (`--config=text-full`).
- **The resvg image suite** is a `donner_variant_cc_test` with `named_variants` `default_text`
  (stb_truetype), `max` (full text), and `geode` — targets
  `//donner/svg/renderer/tests:resvg_test_suite_default_text` / `_max` / `_geode`. The bare
  `:resvg_test_suite` alias inherits the command-line config, so
  `bazel test --config=text-full //donner/svg/renderer/tests:resvg_test_suite` runs the full-text
  tier explicitly.
- **Text unit targets** (in `donner/svg/renderer/tests/BUILD.bazel`): `text_engine_tests` and
  `text_backend_tests` (link both backends; need the `//third_party/resvg-test-suite:fonts` data
  dep and are gated on `resvg_test_suite_enabled`), plus `text_engine_helpers_tests`
  (backend-mocked, always available).
- **Text pixel diffs:** hundreds of differing pixels per glyph means a position/transform/shaping
  bug — "anti-aliasing" is a banned explanation (project `CLAUDE.md`). Use the shared comparator
  from **donner-pixel-diff**; suite skip/triage conventions are in **donner-resvg-triage**;
  Geode-vs-tiny-skia text parity history is `docs/design_docs/0038-geode_tinyskia_text_parity.md`.

## 9. Where to go deeper

- `docs/parser_diagnostics.md` — full diagnostics type reference, usage, and testing patterns.
- `docs/css.md` — CSS class anatomy, selector support matrix, parsing APIs.
- `docs/parser_namespaces.dox` — namespace/dependency rationale.
- Sibling skills: **donner-build-test** (configs, variants, CI gate), **donner-fuzzing**
  (running/extending fuzzers), **donner-resvg-triage** (suite conventions),
  **donner-pixel-diff** (bitmap comparison rules), **donner-cpp-conventions** (naming, style).
