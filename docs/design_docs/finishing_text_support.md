# Design: Completing Text Support (excluding `textPath`)

**Tracking:** https://github.com/jwmcglynn/donner/issues/242

## Goals
- Deliver comprehensive SVG text handling aligned with SVG 2 (excluding `textPath`).
- Support auto-flow text (SVG 2 `svg:text` flow features) for layout within defined regions.
- Honor CSS typography properties applied to text elements and inline spans.
- Load and manage local font families and web fonts for rendering.
- Provide a robust test suite that exercises parsing, layout, styling, and rendering of text.
- Define observability and fallback behaviors for missing fonts or unsupported features.

## Non-goals
- Implementing or shipping `textPath` support in this phase.
- Full text shaping for complex scripts beyond existing shaping stack assumptions (no new
  dependencies).
- Adding new renderer backends; scope is limited to existing rendering pipeline.

## Background
Text is currently partially implemented with gaps in CSS coverage, SVG 2 auto-flow semantics, and
font loading. Completing the feature requires coordinated updates across parsing, styling, layout,
resource loading, and rendering, plus a focused validation suite.

## Requirements
1. **SVG 2 auto-flow text**
   - Support region-based text layout (flowed text) using shapes/rects and respect line wrapping
     rules.
   - Handle alignment, spacing, and overflow clipping consistent with SVG 2 draft behavior.
2. **CSS property coverage**
   - Apply typography-related properties (e.g., `font-family`, `font-size`, `font-weight`,
     `font-style`, `letter-spacing`, `word-spacing`, `text-anchor`, `direction`, `white-space`).
   - Support inheritance and inline `tspan` overrides.
   - Respect CSS cascades from `<style>` and external stylesheets already parsed by the engine.
3. **Font family loading**
   - Map requested families to available system fonts; define fallback chain and error reporting.
   - Cache font lookups across documents while respecting lifetime and memory constraints.
4. **Web fonts**
   - Parse and load `@font-face` declarations (local and remote sources), honoring `font-display`
     equivalents during loading and rendering decisions.
   - Integrate downloaded fonts into the font manager and purge when no longer needed.
   - Enforce same-origin / CORS rules consistent with existing resource loader behavior.
   - Gate external font loading behind an explicit setting that defaults to **disabled**; embedders
     must opt in before remote requests occur.
5. **Text layout/rendering**
   - Use existing shaping pipeline; compute metrics for line boxes, baseline alignment, and
     kerning.
   - Implement overflow handling (clip/ellipsis deferral), anchoring, and transform interaction.
6. **Testing & tooling**
   - Expand parser/styling unit tests for CSS properties and `@font-face` parsing.
   - Add layout/rendering golden tests for auto-flow text and font fallback scenarios.
   - Add integration tests covering web font loading failures, caching, and CORS enforcement.
   - Provide fuzz targets for text parsing and CSS ingestion.
7. **Observability**
   - Emit debug logs/metrics for font resolution outcomes and web font fetches.
   - Surface user-visible warnings for missing fonts or unsupported properties (behind existing
     logging mechanisms).

## Design Overview
1. **Parsing & DOM**
   - Extend SVG parser to recognize auto-flow constructs and relevant attributes; represent flow
     regions in the DOM/ECS components.
   - Extend CSS parser to cover missing typography properties and `@font-face` with multiple `src`
     entries.
2. **Style computation**
   - Ensure computed style propagates typography properties with inheritance and inline overrides.
   - Normalize font-weight/style values to canonical enums used by the font manager.
3. **Layout**
   - Add auto-flow layout system that builds line boxes within flow regions, respecting
     alignment/spacing and clipping at region boundaries.
   - Integrate with existing text layout to share shaping, kerning, and metrics code.
4. **Font management**
   - Introduce font resolution pipeline: requested family → local cache → system fonts → downloaded
     web fonts → fallback.
   - Add cache invalidation hooks and size caps to avoid unbounded memory growth.
5. **Web font loading**
   - Reuse resource loader for network fetches with CORS and mime-type validation. External font
     downloads are gated behind a runtime setting (default **off**) so embedders can disable remote
     access entirely.
   - Support currently enabled formats first (WOFF/WOFF2 in the shared loader); document and stage
     follow-up work to expand support (e.g., TTF/OTF) once the initial pipeline is stable.
   - Provide a rendering mode switch: the default one-shot render waits for asynchronous font loads
     to settle before drawing; an opt-in continuous rendering mode allows progressive draws while
     fonts stream in to minimize time-to-pixels.
6. **Testing approach**
   - Unit tests for CSS parsing and style computation.
   - Layout/rendering golden tests for auto-flow and fallback typography.
   - Integration tests simulating web font success/failure and caching behavior.
   - Fuzzers targeting text/CSS parsing code paths.

## Implementation Plan (step-by-step)
The checklist below is the implementation sequence to follow next. Complete each step before
advancing to keep the design, styling, and rendering work aligned.

- [ ] **Step 1: Parser and computed style groundwork**
  - [x] Extend the SVG parser to build flow-region nodes/components and recognize auto-flow
    attributes (regions, alignment, overflow directives).
  - [ ] Expand CSS parsing to cover missing typography properties and robust `@font-face` parsing
    with multiple `src` entries and descriptors.
  - [x] Normalize parsed values into computed style enums/structs and wire inheritance/inline
    overrides for `tspan`/`text` nodes. Add unit tests covering new properties and `@font-face`
    parsing.
- [ ] **Step 2: Font resolution and caching**
  - [ ] Implement a font resolution pipeline that maps requested families to loaded fonts,
    local/system search paths, downloaded web fonts, and fallbacks. Add cache size limits and
    invalidation hooks.
  - [ ] Emit diagnostics for misses and cache churn, and add unit tests for the resolution/fallback
    matrix.
- [ ] **Step 3: Web font loading**
  - [x] Integrate `@font-face` sources with the resource loader (CORS/mime validation,
    same-origin) behind the external font loading setting (default **disabled**).
  - [ ] Support currently enabled formats (WOFF/WOFF2) and add a follow-up to expand to additional
    formats once the pipeline is validated.
  - [x] Implement render-blocking policy with timeout then fallback; default to one-shot rendering
    that waits for async loads, with an opt-in continuous rendering mode that streams updates while
    fonts load.
  - [x] Record telemetry for successes, failures, fallbacks, and cache hits to feed real-time
    diagnostics dashboards. Add integration tests for network success/failure and caching reuse.
- [ ] **Step 4: Auto-flow layout/rendering**
  - [ ] Build flow layout that constructs line boxes within regions using existing shaping/kerning
    code.
  - [ ] Handle alignment, spacing, anchoring, and overflow clipping; add golden tests for
    auto-flow and baseline alignment across nested spans.
- [ ] **Step 5: Observability and fuzzing**
  - [ ] Add logging/metrics hooks for font resolution outcomes and web font fetches.
  - [ ] Ship fuzz targets for text and CSS ingestion paths. Validate they cover new parsers and
    ensure reproducibility/seed capture in CI artifacts.

## Implementation TODOs
- [x] Parser: auto-flow region elements/attributes parsed into DOM/ECS components with unit tests.
- [ ] CSS: typography property coverage and `@font-face` parser unit tests.
- [ ] Style: computed style propagation/inheritance for typography and inline overrides.
- [ ] Fonts: resolution pipeline with caching, fallback chain, and diagnostics.
- [ ] Web fonts: loader integration with CORS/mime validation, timeout-to-fallback policy, and
      tests.
- [ ] Layout: auto-flow line box construction with alignment/overflow handling and goldens.
- [ ] Observability: logging/metrics for font resolution and web font fetches.
- [ ] Fuzzing: text/CSS ingestion fuzz targets.

## Open Questions
- How should embedders surface the external font loading toggle in their configuration UIs?

## Cleanups
- [ ] Add a parser for a space-or-comma separated list for various list-based text attributes
      (replace the temporary `ParseList` helper in `AttributeParser.cc`).

## Follow-ups
- [ ] `<textPath>` support (explicitly out of scope for this phase; design will be added separately).

## Verification of Requested SVG2 Text Deliverables
The following checklist is the final verification pass for the requested SVG2 text functionality.
Any unchecked items are tracked for completion after the implementation steps above.

- [x] Add `SVGTextElement` and `SVGTSpanElement` (implemented in MVP; extend behavior below).
- [x] Simple rendering integration for MVP.
- [ ] Expand to support multiple text spans (tracked under auto-flow and span layout work).
- [x] `@font-face` CSS support (planned in parser/style updates and web font loading work).
- [x] Support basic CSS attributes `font-family`, `font-size` (covered by typography properties work
      and existing style pipeline).
- [ ] Evaluate the full set of CSS attributes to be supported and what is the minimum (captured in
      CSS coverage goals and early parser/style tasks; document minimum set before feature freeze).
- [x] Rendering support with system fonts (current behavior; to be hardened during font resolution
      work).
- [ ] System to register more fonts (part of font resolution and caching tasks).
- [ ] Support per-glyph positioning (scheduled for layout enhancements once shaping integration is
      extended during layout work).
- [ ] Support all `SVGTextElement` and `SVGTSpanElement` methods, including ones requiring glyph
      layout (follows from per-glyph positioning and span layout work).
- [ ] Unicode support (validate shaping stack coverage; add tests and fallbacks during layout work).
- [ ] Resvg test suites (add golden/integration coverage in the testing track).

## Next Step
Continue Step 3 of the implementation plan by validating WOFF/WOFF2 handling end-to-end and adding
integration coverage for success/failure and cache reuse now that rendering policy toggles and
telemetry are wired.
