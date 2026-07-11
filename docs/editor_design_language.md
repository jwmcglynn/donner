# Editor Design Language {#EditorDesignLanguage}

The Donner editor uses a neutral Graphite visual system so SVG artwork remains the most colorful
object on screen. Signal Teal identifies focus, selection, and primary interactive state. Amber,
coral, and green are reserved for semantic status.

The implementation lives in `donner/editor/EditorTheme.{h,cc}`. The active theme configures ImGui
and supplies tokens to custom `ImDrawList` chrome, the canvas overlay, and source-editor colors.

## Principles

- **Artwork first.** Neutral chrome surrounds the document without tinting or competing with it.
- **One focus color.** Signal Teal marks selection, focus, active tools, links, and docking previews.
- **Structure through tone.** Surface elevation and one-pixel rules separate regions. Heavy borders
  and card-like containers are avoided.
- **Compact and stable.** Controls use fixed dimensions on a 4 px grid, so hover, labels, and state
  changes do not move surrounding UI.
- **SVG-native icons.** Tool icons are SVG assets rendered through Donner, then tinted from theme
  tokens. Cursors retain their black-and-white halo for canvas contrast.

## Core Tokens

| Role                 | Token            | Value     |
| -------------------- | ---------------- | --------- |
| Canvas surround      | `surfaceCanvas`  | `#111215` |
| Inset well           | `surfaceSunken`  | `#151619` |
| Panel background     | `surfaceBase`    | `#1B1D20` |
| Menu, tab, field     | `surfaceRaised`  | `#24272B` |
| Floating chrome      | `surfaceOverlay` | `#2B2F34` |
| Hover                | `surfaceHover`   | `#343940` |
| Active               | `surfaceActive`  | `#3C424A` |
| Subtle rule          | `borderSubtle`   | `#30343A` |
| Strong rule          | `borderStrong`   | `#464C55` |
| Primary text         | `textPrimary`    | `#F1F2F4` |
| Secondary text       | `textMuted`      | `#A8ADB5` |
| Disabled text        | `textDisabled`   | `#656B74` |
| Focus and selection  | `accentDefault`  | `#31C6B3` |
| Warning              | `warning`        | `#E3B341` |
| Error or destructive | `destructive`    | `#F0616A` |
| Success              | `success`        | `#3FB984` |

The shipped accent is `Accent::SignalTeal`. Other accent variants remain test-covered theme inputs,
but product chrome should not choose a different accent per widget.

## Geometry And Type

Spacing uses 4, 8, 12, 16, 24, and 32 px tokens. Controls use a 4 px radius; floating containers
use a 6 px radius. Tool buttons are 32 px square on desktop and 44 px square in the compact touch
profile. New fixed-format controls should use stable dimensions and reserve enough room for their
longest label.

Roboto is the UI face and Fira Code is the source face. UI text uses regular weight for values and
bold weight for identity or compact section emphasis. Large display type does not belong in panels,
toolbars, dialogs, or diagnostics surfaces.

## Application Chrome

The top application bar uses a teal identity mark, `DONNER`, a muted `SVG EDITOR` descriptor, and
compact menus. Dock tabs use neutral surfaces and a teal selected overline. They do not use ImGui's
stock blue tab fill.

The canvas toolbar is a floating overlay with one subtle shadow and one strong edge. Tool buttons
use Donner-rendered two-tone SVG artwork with a black core and white halo, a translucent teal
selected fill, and a teal selected outline. Toolbar art rasterizes at 80 px before display at 20
logical px so 1x and 2x output use exact downsampling ratios. The Fill/Stroke widget shares the same
container and token set. Only ready tools appear in the toolbar; unfinished interactions do not
occupy disabled slots. Acute pointer artwork uses concentric silhouettes so the black core cannot
leak through the white halo during downsampling.

The Inspector contains document and selection controls only. Viewport implementation telemetry does
not occupy the Inspector. Zoom state lives on the canvas in a compact control that resets to 100
percent when clicked.

### Adaptive Touch Profile

`ComputeEditorAdaptiveUiLayout` selects a canvas-first compact profile for constrained windows and
touch-preferred builds. Native windows enter it below 760 px wide or 520 px high. WebAssembly enters
it at those constraints or when the browser reports touch points or a coarse pointer, so desktop
browsers retain the full editor while iOS starts with stable touch geometry.

The compact profile keeps the same Graphite language but changes the command hierarchy:

- a fixed 52 px top bar presents `DONNER` plus icon-only Open/Samples, Undo, Redo, Layers, and
  Inspector actions;
- the canvas tool palette keeps Select, Pen, and Text with 44 px buttons, while the dense paint
  widget, source pane, text format bar, canvas scrollbars, and compositor diagnostics are omitted;
- Layers and Inspector open one at a time in an overlay sheet, on the right in landscape and at the
  bottom in portrait, with a 44 px close target;
- layer rows and their disclosure, visibility, and lock controls use at least 44 px interaction
  targets; compact Inspector fields receive equivalent vertical padding;
- selection, text-frame, and pen geometry stays visually unchanged while invisible pointer hit
  tolerance doubles for touch.

Opening a right-side sheet recenters the tool palette within the unobscured canvas region. Compact
and desktop modes use separate DockSpace roots, so resizing into the touch profile cannot rewrite a
custom desktop panel arrangement. Source visibility and desktop sidebar width remain preferences,
not transient compact state.

The UI profile consumes ordinary ImGui pointer and resize events. The WebAssembly bootstrap maps
one primary touch pointer into the existing mouse-compatible ImGui stream, with pointer capture and
cancel handling so taps and direct drags use the desktop mutation paths. Viewport sizing,
multi-touch gestures, virtual-keyboard behavior, and Safari lifecycle integration remain platform
responsibilities; they must continue to feed existing editor seams rather than add touch-specific
document mutation paths.

### Text Authoring

New text keeps the current authoring fill instead of resetting to a tool-local color. When one text
element is selected or edited, font family, size, and emphasis controls appear in a compact rounded
toolbar centered below the canvas tool palette. It floats over the canvas and does not consume a
full-width layout strip.

Point text uses font em-box extents for frame height so ascenders, descenders, and the typed glyph mix
cannot make the frame jump. A newly placed point-text frame starts hidden. Pointer movement reveals
it immediately; subsequent text input fades the frame and handles away while leaving the caret
visible. Drag-created box text keeps its authored frame visible.

Text frame handles use the same handle-box calculation as select-tool handles at every display
scale. During frame resize, pointer moves update only local frame chrome. DOM attributes, text
rewrap, source writeback, and document rendering occur once on release.

### Canvas-First Source Access

The source pane starts collapsed so the document canvas is the primary workspace. A persistent
32 px rail remains on the left edge with a source glyph and line motif. Clicking the rail opens the
source pane; the View menu and source-navigation actions also reveal it. The rail reserves layout
space rather than overlaying document content, and its hover state uses the standard focus color.

### Transform Editing

The Transform section uses stable rows for Position, Size, and Rotation. A five-column grid gives
X/W, Y/H, and Rotation fixed field starts and matching widths; variable label glyphs cannot shift
the input rectangles. Rotation includes its unit, and the advanced matrix remains behind a
disclosure with the same fixed axis/value alignment. Fields fill the available Inspector width
without changing row geometry during hover or editing.

Numeric drag fields support both interaction styles. Dragging adjusts a value at fine resolution;
a click-release without movement enters text input directly, without requiring a modifier or
double-click. Enter commits typed input and field deactivation records one undo step.

Transform activation must not hold a document read scope while reading `transform()` or
`worldBounds()`. Those SVG accessors may materialize lazy layout state and acquire write access in
concurrent-DOM mode. Each accessor owns its required lock so field activation cannot attempt a
non-recursive read-to-write lock upgrade.

### Inspector Property Lists

The Inspector presents XML attributes and computed CSS as compact property lists:

- XML attributes retain source order and use separate name and value columns.
- Computed values use CSS-shaped text. Internal wrapper names such as `PaintServer(...)` are not
  product UI.
- CSS provenance has a dedicated `SET` or `DEFAULT` column. It is not appended to the value.
- Default properties are visually quieter than values set by the active cascade.
- Solid computed fill, stroke, and color values include a swatch derived from the typed computed
  style. Paint-server references remain textual.
- Long values stay within their column and expose the full value on hover.

These lists are read-only in the visual MVP. Attribute editing must remain DOM-first and use the
editor's structured source-writeback path when it is added.

## Interaction Responsiveness

Interactive chrome must stay on the UI thread while expensive document work is coalesced, deferred,
or moved to an isolated worker:

- Queued text input performs one DOM synchronization per UI frame. Point text advances its caret
  with one adjacent-character geometry query and does not issue geometry queries for every glyph
  after each key. Box text keeps exact per-character measurement because wrapping depends on it.
- Active move and transform chrome comes from the gesture's start bounds plus its current transform.
  It does not walk outlined glyph paths on every pointer event. Cached pixels and live chrome may be
  paired only when their entity identities match.
- Revealing the source pane preserves the document point at the render-pane center. A pane-bounded
  raster is used only when it reduces pixel area, unless the full raster exceeds the backend limit.
- CSS source annotations parse an immutable source snapshot on an isolated worker. The UI thread
  accepts the result only after document-generation and source-version validation, then resolves
  deduplicated element locators in one document traversal.

Source reveal and annotation application should each remain within a 16.7 ms UI-frame budget on the
checked-in showcase document. Steady source-pane rendering should remain below that budget as well.

## Source Palette

`TextEditor::getDarkPalette()` follows the same Graphite base while using semantic hues for syntax:

- teal for structural keywords and known identifiers
- warm amber for strings and type-like values
- green for numeric values
- coral for errors and breakpoints
- neutral gray for comments, punctuation, line numbers, and ordinary identifiers

Selection remains Signal Teal at 50 percent alpha so focus-reference connectors and selected source
ranges remain legible.

## Extension Rules

1. Reuse an existing `EditorTheme` token before adding a token.
2. Do not add a raw color literal to product chrome. Test fixtures and actual document colors are
   exempt.
3. Keep spacing and fixed control dimensions on the 4 px grid.
4. Use SVG assets for editor tool icons when a suitable icon does not already exist.
5. Preserve black-core/white-halo contrast for tool and cursor artwork.
6. Keep debug telemetry behind an explicit diagnostics surface or performance overlay.
7. Add or update a focused test when a token, mapped ImGui slot, palette value, or control contract
   changes.

## Welcome And Samples

Launching without a filename, including the WebAssembly build, opens a first-run surface over the
real editor workspace. It keeps Donner as the first visual signal, offers Open SVG and a fixed
GitHub destination, and lists a bounded offline catalog of reviewed SVG samples. Loading a sample
creates an untitled document, so Save cannot overwrite a local file without an explicit path.
Each sample card uses a small bitmap rendered by Donner from the exact bundled SVG source. ImGui
only blits the cached texture; it does not approximate sample artwork with hand-drawn UI geometry.
The catalog has a dedicated texture cache so Layers-panel retention cannot evict welcome previews.
The shell renders at most one missing preview per UI frame and keeps a fixed placeholder slot while
the bounded catalog fills in, avoiding a first-frame Wasm stall or card-layout shift.

The welcome surface owns the workspace while visible rather than competing with docked Layers and
Inspector panels. Its sample controls use at least 44 logical pixels of height, lay out in one
column below the compact breakpoint, and use at most three columns on wider displays. Cards reserve
a fixed preview slot while keeping their complete surface clickable as a touch target. The surface
can be reopened through File > Open Sample and dismissed to reveal the current document.

Document replacement requested from the sample surface is deferred to the next orchestration frame.
The shell waits for the renderer to become idle and detaches any prior direct-presentation callback
before releasing old WebGPU resources. This ordering prevents the prior frame callback from
retaining presentation handles across document replacement. If the document or source buffer has
unsaved edits, replacement stops at an explicit Discard and Load confirmation; Cancel leaves both
the current document and pending source text untouched.

## Verification

Theme, menu, and source-palette contracts run in:

```sh
bazel test //donner/editor/tests:editor_theme_tests \
  //donner/editor/tests:editor_shell_tests \
  //donner/editor/tests:editor_shell_layout_tests \
  //donner/editor/tests:editor_dock_layout_tests \
  //donner/editor/tests:layers_panel_tests \
  //donner/editor/tests:menu_bar_presenter_tests \
  //donner/editor/tests:sample_picker_presenter_tests \
  //donner/editor/tests:sidebar_presenter_tests \
  //donner/editor/tests:text_editor_tests
```

The Inspector UI fuzzer exercises real ImGui frames, Transform lifecycle transitions, selection,
reload, undo, lock state, and busy snapshots under concurrent-DOM access:

```sh
bazel run --config=asan-fuzzer //donner/editor/tests:inspector_ui_fuzzer -- \
  -max_total_time=30 -max_len=4096 -timeout=5
```

For mutation-based local runs, invoke the built fuzzer with a writable corpus directory. The Bazel
wrapper's checked-in corpus arguments are also useful as deterministic seed replays.

Full-shell visual verification uses the Geode replay harness with a worker-settled frame:

```sh
bazel run --config=geode //donner/editor/tests:editor_rnr_gl_replay -- \
  --rnr zoom-out-drag-jump.rnr \
  --capture-frame 10 --max-frame 10 --crop full \
  --worker-scheduling drain-each-frame
```

The visual pass checks app-bar identity, tab colors, the collapsed source rail, source contrast,
toolbar fit and icon contrast, canvas framing, zoom-control placement, Transform alignment, panel
density, and absence of debug-only Inspector content. Raster tests also require every custom cursor
and toolbar icon to retain visible opaque black and white pixels without touching the bitmap edge.
Text-tool tests additionally require no DOM mutation during resize moves, stable point-text em-box
height, frame reveal/fade transitions, and high-density handle-size parity. The Inspector UI fuzzer
includes text creation, active fill, typing, frame reveal, resize, release, reload, and undo.

## Security And Privacy

The sample catalog is compiled into the editor and never fetches document content. Sample loading
uses the same parser and full document-replacement path as a local open, produces an untitled
document, and waits for renderer ownership to return before releasing presentation resources. The
only external destination is the fixed Donner repository URL: browser builds open it after an
explicit click with `noopener`, while native builds copy the same fixed URL only after an explicit
click. Neither path derives a destination from SVG content.

SVG icon assets continue through Donner's renderer. Public captures and documentation use public
sample documents and exclude private paths, host data, credentials, and operator content.
