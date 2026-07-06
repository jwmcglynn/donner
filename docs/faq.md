# Frequently Asked Questions {#faq}

\tableofcontents

This page collects the questions that actually bite people when they first put Donner to work:
things that parse without error, compile without warning, and then quietly render nothing. Each
answer starts with the short version, then explains why Donner behaves that way and what to do
about it. If you are just getting started, read \ref GettingStarted and \ref DonnerAPI first; this
page assumes you already have an \ref donner::svg::SVGDocument in hand.

Donner targets the [SVG 2 specification](https://www.w3.org/TR/SVG2/), so "correct" here means
"what SVG 2 says", which is occasionally not "what SVG 1.1 said" or "what one particular browser
does". Where that distinction matters, the answer calls it out.

## Why does my gradient not show up? {#faq-gradient}
Almost always because the gradient has no `<stop>` children, or its `objectBoundingBox` coordinate
space collapsed to zero. A gradient with zero stops is not an error and paints nothing; add at least
one \ref xml_stop and it appears.

Donner resolves paint in two stages, and knowing the split explains every gradient mystery. First,
at style-resolution time, `fill="url(#g)"` is resolved to a reference: this only checks that `#g`
exists and is a gradient or pattern, not that it is *usable*. Second, at draw time, that reference
is turned into an actual shader, and this is where stops and bounding boxes are inspected. So a
stopless gradient sails through stage one and only produces nothing in stage two.

The specific rules Donner implements (all matching SVG 2):

- **No stops:** the element is not painted. If you wrote the fallback form `fill="url(#g) red"`,
  the fallback color is used instead; with no fallback, the shape is simply not filled.
- **Exactly one stop:** paints as a solid color of that stop. This is spec behavior, not a bug.
- **`gradientUnits="objectBoundingBox"` (the default) on a shape with a zero-area bounding box:**
  no gradient. The classic trap is a gradient stroke on a perfectly horizontal or vertical line,
  where one bounding-box dimension is zero. The gradient cannot be mapped, so Donner falls back to
  the fallback color if you gave one and otherwise draws nothing. `stroke="url(#g) green"` on a flat
  line paints green.
- **`href` template gradients:** a gradient with no stops of its own but an `href` to another
  gradient inherits that gradient's stops and attributes (`x1`, `y1`, `spreadMethod`, and so on).
  A gradient with its own stops ignores the template's stops but still inherits unspecified
  attributes. Pointing `href` at a non-gradient element is ignored with a parse warning.

A gradient that renders nothing does **not** emit a warning on the normal render path, so do not
wait for a log line. Diagnose it by reasoning about stops and bounding boxes. See
\ref xml_linearGradient, \ref xml_radialGradient, and \ref donner::svg::SVGGradientElement, and the
closely related \ref faq-not-rendered.

## Why is my text invisible, or not shaping correctly? {#faq-text}
Two different failures wear the same "I see no text" costume: the glyphs are painted but you cannot
see them (a fill problem), or the glyphs were never produced (a font or shaping problem). Check the
fill first, because it is the more common one.

**The fill.** Text obeys the ordinary `fill` property, which defaults to solid black. Black text on a
transparent canvas is fine, but black text on a black or dark background is invisible, and
`fill="none"` or `fill-opacity="0"` paints nothing at all. This is not text-specific; it is the same
default that fills your shapes.

**The font.** Donner does not read system fonts. There is no fontconfig, CoreText, or DirectWrite
lookup anywhere in the text path. Fonts come from `@font-face` rules or from calling
`donner::svg::FontManager::loadFontData()` yourself, plus one embedded fallback (Public Sans). So
text with no matching font does *not* vanish: it falls back to Public Sans and renders with Latin
coverage. If your Latin text renders but your styled font does not, your `@font-face` source did not
load (see below); if your text is CJK, Arabic, or emoji, read on.

**The shaping tier.** The default build shapes text with stb_truetype: one code point maps to one
glyph through the font's cmap, with `kern`-table kerning and nothing else. No GSUB/GPOS means no
ligatures, no mark positioning, no cursive joining, and no contextual substitution. Complex-script
shaping (Arabic, Indic), OpenType features, WOFF2 web fonts, and color-emoji bitmap glyphs are
available only in the HarfBuzz tier, built with `--config=text-full`. Under the default build, a
code point the resolved font cannot map becomes `.notdef` (glyph 0), and Donner silently skips it
rather than drawing a tofu box, so unsupported scripts render as empty space.

**Other ways to get invisible text:** a zero `font-size` produces a zero scale and no glyphs; an
`@font-face` whose only source is `url()` needs a `ResourceLoaderInterface` to fetch it (with none
provided, Donner warns and falls back); a `local()`-only `@font-face` never resolves because system
font lookup is not implemented; and building with `--config=tiny` or `--config=no-text` compiles the
text engine out entirely, so `<text>` renders nothing.

See \ref xml_text, \ref xml_tspan, \ref xml_textPath, and \ref elements_text.

## When should I use SVGPathElement's API instead of setting the d string? {#faq-path-api}
Use `setD()` and `d()` when you are round-tripping SVG source or letting the CSS cascade drive the
geometry; use `setSpline()` when you already have geometry in hand and want to skip parsing
entirely. They are two doors into the same \ref donner::svg::SVGPathElement, and picking the right
one is mostly about whether you are holding a string or a shape.

`setD(RcString)` stores the path-data string and marks the element dirty; the string is parsed
lazily, the next time the computed geometry is built. `d()` returns exactly that stored string, not
a normalized re-serialization. This is the door to use when the `d` value comes from a file, an
attribute, or a `d:` CSS property, because it participates in the normal attribute and cascade
machinery.

`setSpline(const Path&)` takes a pre-built \ref donner::Path and installs it as an override. It
clears the `d` attribute, so afterward `d()` returns the empty string: Donner does not reverse a
spline back into path-data text. Use it when you compute geometry programmatically, because it
bypasses the parser completely (no tokenizing, no validation, no per-rebuild reparse cost).

To read back the resolved geometry regardless of which door you used, call `computedSpline()` on the
\ref donner::svg::SVGGeometryElement base class; it returns `std::optional<Path>` and is empty when
the path is empty. A malformed `d` string never throws and never aborts the parse: Donner keeps
whatever it parsed before the error (path parsing is deliberately partial) and records a warning, so
a slightly broken `d` gives you a truncated path, not an empty document. See \ref xml_path and
\ref faq-stroke-scale for how that geometry then gets stroked.

## Why is my &lt;symbol&gt; the wrong size when I instantiate it with &lt;use&gt;? {#faq-symbol-size}
Ninety percent of the time the symbol has no `viewBox`, and without a `viewBox` a symbol does not
scale. Setting `width`/`height` on the `<symbol>` or the `<use>` without a `viewBox` only changes
the clipping rectangle, not the content, so your artwork gets cropped instead of resized. Add a
`viewBox` and the content scales to fit.

A \ref xml_symbol establishes its own SVG viewport, much like a nested `<svg>`. The `viewBox` is what
maps the symbol's internal coordinates onto that viewport; it is the ingredient that makes the
content stretch or shrink to whatever box the \ref xml_use gives it. With a `viewBox`, the same
symbol renders crisply at 40x40 or 120x120. Without one, the coordinates are taken literally in the
parent's user space and the `<use>` dimensions merely clip.

The sizing rules that trip people up:

- Missing `width`/`height` on both `<symbol>` and `<use>` default to `100%`, meaning the symbol
  fills its container rather than snapping to some intrinsic size.
- `width`/`height` on `<use>` override the symbol's own `width`/`height`. But `x`/`y` do not
  override; the symbol's and the use's `x`/`y` add together.
- `preserveAspectRatio` defaults to `xMidYMid meet`, so a symbol dropped into a box with a different
  aspect ratio is letterboxed, not stretched. Use `preserveAspectRatio="none"` to stretch.
- `refX`/`refY` default to the top-left corner, which is *not* the same as `0`. If you expected the
  symbol centered on the `<use>` point, set `refX`/`refY` explicitly.

The full coordinate chain, with worked diagrams, lives in \ref SymbolElementUsage. See also
\ref donner::svg::SVGSymbolElement and \ref donner::svg::SVGUseElement.

## Why doesn't my filter output match Chrome's exactly? {#faq-filter-chrome}
Because Donner is not trying to match Chrome; it is trying to match the
[Filter Effects](https://drafts.fxtf.org/filter-effects/) spec, and it validates that against the
resvg test suite, not against a browser. The default backend is Donner's own CPU pixel executor
(tiny-skia), so pixel-exact parity with Chrome's GPU rasterizer is not a goal and small differences
in anti-aliasing and blur edges are expected.

Donner implements all 17 filter primitives plus the CSS shorthand functions (`blur()`,
`drop-shadow()`, and friends), with correct `in`/`result` buffer routing, `filterUnits`,
`primitiveUnits`, and per-primitive `color-interpolation-filters`. Filter primitives default to
`linearRGB` interpolation per spec (CSS shorthand functions use `sRGB`), which is correct but
occasionally surprising if you were comparing against a tool that got it wrong.

The differences that are actually behavioral, not just sub-pixel:

- **`enable-background` / `BackgroundImage` / `BackgroundAlpha` do nothing.** These SVG 1.1 filter
  inputs were removed in SVG 2. Donner treats them as unresolved references, which produce
  transparent black. Use CSS `mix-blend-mode` and `isolation` for backdrop effects. See
  \ref faq-unsupported.
- **The filter region is bounded.** Region expansion is capped at 4096px and extreme blur radii and
  convolution kernels are clamped, as a resource-exhaustion guard. A filter that balloons the region
  in a browser may be clipped in Donner.

For the architecture, backend details, and `feImage` handling, see \ref FilterEffectsGuide and
\ref xml_filter.

## How do I load an SVG and render it to pixels in the fewest lines? {#faq-render-pixels}
Parse the source, check for an error, and hand the document to a \ref donner::svg::Renderer. That is
five meaningful lines, and you do not need to set a canvas size if the SVG declares its own
`width`/`height`/`viewBox`.

```cpp
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

using namespace donner;
using namespace donner::svg;

std::string_view source = /* your SVG text */;

ParseWarningSink warnings;
ParseResult<SVGDocument> maybeDocument = parser::SVGParser::ParseSVG(source, warnings);
if (maybeDocument.hasError()) {
  std::cerr << "Parse error: " << maybeDocument.error() << "\n";
  return 1;
}
SVGDocument document = std::move(maybeDocument.result());

Renderer renderer;
renderer.draw(document);
```

From there, pick your output. `renderer.save("out.png")` writes a PNG; `renderer.takeSnapshot()`
returns a \ref donner::svg::RendererBitmap with the raw RGBA pixels; `renderer.width()` and
`renderer.height()` give the dimensions. To force a specific output size (equivalent to resizing a
browser window), call `document.setCanvasSize(w, h)` before drawing, or
`document.useAutomaticCanvasSize()` to go back to auto.

Two gotchas worth internalizing early. First, the source buffer you pass to `ParseSVG` is referenced
internally by the returned `SVGDocument`, so it must outlive the document; do not free it while the
document is still alive. Second, `ParseSVG` takes a `ParseWarningSink&` by reference and it is not
optional; pass `ParseWarningSink::Disabled()` if you genuinely do not care about warnings (it skips
the formatting work entirely), but see \ref faq-not-rendered before you decide you do not care. A
complete runnable version is `examples/cmake_consumer/main.cc`.

## Do I have to keep the SVG source string alive after parsing? {#faq-lifetime}
Yes. The buffer you pass to `SVGParser::ParseSVG` is referenced internally by the returned
\ref donner::svg::SVGDocument, not copied wholesale, so it must outlive the document. Free it too
early and you get a dangling reference, not a clean error.

This is the most common lifetime footgun in Donner. `ParseSVG` takes a `std::string_view`, and the
document holds onto pieces of that memory (for example, string data it did not need to allocate a
fresh copy of). If the `std::string`, `std::vector<char>`, or other owner of that text goes out of
scope while you are still holding the `SVGDocument`, later DOM reads or renders will touch freed
memory.

The fix is simple: keep the source owner alive at least as long as the document. Store them
together, or give the source the same or wider scope as the `SVGDocument`. If you cannot guarantee
that, copy the SVG text into a buffer you own and keep it. See \ref faq-render-pixels for the full
parse-to-pixels flow.

## How do I mutate the DOM and re-render efficiently? {#faq-mutate}
Keep one \ref donner::svg::Renderer alive and call `draw()` again after your edits; do not reparse
the SVG and do not construct a fresh renderer per frame. Donner's renderer is two-phase: it builds
and caches a render tree, and DOM mutations mark only the affected elements dirty, so a redraw
recomputes just what changed.

The efficient loop is: find the element (`document.querySelector("#id")`), change it
(`setAttribute`, `setStyle`, or a typed setter like `SVGCircleElement::setCx`), then call
`renderer.draw(document)` again. The unchanged subtree is reused from the cached render tree. This
is dramatically cheaper than re-parsing, which throws away all of that cached work.

When you are applying a batch of edits, wrap them so they are treated as one logical update rather
than many. `SVGDocument::withWriteAccess()` groups mutations into a single mutation revision, which
is both cheaper and cleaner than entering document access once per setter:

```cpp
document.withWriteAccess([&](SVGDocumentMutation& mutation) {
  std::optional<SVGElement> rect = document.querySelector("#status");
  if (!rect) return;
  mutation.setAttribute(*rect, "fill", "green");
  mutation.setAttribute(*rect, "stroke", "black");
});
```

For an interactive viewer that draws every frame, use the frame lifecycle
(`beginFrame()` / `endFrame()`) from the \ref donner::svg::RendererInterface API instead of `draw()`,
which is a convenience wrapper around it. If more than one thread touches the document, read
\ref faq-threads first. See also \ref DonnerAPI for the DOM manipulation surface.

## What SVG features does Donner not support, and what happens if I use them? {#faq-unsupported}
Donner implements SVG 2, so the features it omits are mostly things SVG 2 itself removed or
deprecated, plus a handful of not-yet-implemented corners. The general rule for an unsupported
feature is that it is ignored quietly: the document still parses, and the affected element either
renders without that feature or does not render at all. You rarely get a hard error.

Removed or deprecated in SVG 2, and intentionally not implemented:

- **SVG fonts** (`<font>`, `<glyph>`, `<missing-glyph>`) and the alternate-glyph family
  (`<altGlyph>`, `<altGlyphDef>`, `<glyphRef>`): not recognized. Use TrueType/OpenType or WOFF2 via
  `@font-face`.
- **`<tref>`** and **`<cursor>`**: not implemented; use plain text and the CSS `cursor` property.
- **The CSS 2 `clip: rect(...)` property:** not implemented. Use `clip-path` with an `inset()`
  shape.
- **`enable-background`, `BackgroundImage`, `BackgroundAlpha`:** removed filter inputs; they resolve
  to transparent black. See \ref faq-filter-chrome.
- **`xml:base`, `xml:lang`:** not implemented (`xml:space` is honored for `<text>` whitespace).

Two behaviors worth singling out because they look like bugs:

- **`vector-effect="non-scaling-stroke"` is accepted but does nothing.** The attribute name is
  recognized so it does not raise a parse error, but no code consumes it, so it is a silent no-op.
  See \ref faq-stroke-scale.
- **An unknown element becomes an `SVGUnknownElement`.** It parses fine and lives in the DOM, but it
  has no geometry and renders nothing. See \ref faq-not-rendered.

The authoritative list, with per-feature rationale and the modern replacement, is
[Unsupported SVG 1.1 Features](unsupported_svg1_features.html).

## Why did parsing succeed but my element still not render? {#faq-not-rendered}
A successful parse only means the XML and CSS were well-formed; it says nothing about whether an
element is *renderable*. The usual suspects are being inside `<defs>`, having `display:none`,
collapsing to zero size, or living in the wrong XML namespace. Start by dumping the parse warnings,
because the namespace case in particular reports one.

The exclusion rules Donner applies while building the render tree:

- **`display: none`** removes the element and its entire subtree from rendering. (`visibility:
  hidden` is different: the element still participates in layout but is not painted.)
- **Children of `<defs>`** are non-renderable by design; they only appear when referenced, for
  example through \ref xml_use or a paint `url()`. The same is true of the contents of
  \ref xml_mask, \ref xml_pattern, and \ref xml_symbol, which render only when instantiated.
- **Zero-size geometry** drops out. A `rect`, `image`, or nested `svg` with `width="0"` or
  `height="0"`, or a degenerate viewport, has empty bounds and is not drawn.
- **Unknown tags** become `SVGUnknownElement` and render nothing (see \ref faq-unsupported).
- **Wrong or missing namespace** is the sneaky one. An element whose namespace is not
  `http://www.w3.org/2000/svg` is dropped from the tree with a warning. This is the "I pasted an SVG
  fragment with no `xmlns` and nothing shows up" case. Either add the `xmlns` declaration or set
  `SVGParser::Options::parseAsInlineSVG = true`.

To see the warnings, pass a live \ref donner::ParseWarningSink and iterate it:

```cpp
ParseWarningSink warnings;
auto maybeDocument = parser::SVGParser::ParseSVG(source, warnings);
// ... handle hasError() ...
if (warnings.hasWarnings()) {
  for (const ParseDiagnostic& w : warnings.warnings()) std::cerr << "  " << w << "\n";
}
```

One more gotcha if your CSS is not applying: `SVGParser::Options::disableUserAttributes` defaults to
`true`, which drops non-presentation (user-defined) attributes for performance. That means attribute
selectors like `rect[data-role="status"]` will not match. Set it to `false` if you rely on them. If
the missing element is a gradient fill, see \ref faq-gradient.

## Something looks off but parsing succeeded. How do I get diagnostics? {#faq-diagnostics}
Donner splits problems into two buckets and reports them through two different channels. Fatal
problems that stop parsing come back as an error on the \ref donner::ParseResult; non-fatal problems
that Donner recovered from (ignored attributes, unknown namespaces, malformed but partially parsed
values) are collected in a \ref donner::ParseWarningSink that you pass in. If your document rendered
wrong but did not fail, the warning sink is the first place to look.

Pass a live sink, then check it after handling the error:

```cpp
ParseWarningSink warnings;
ParseResult<SVGDocument> maybeDocument = parser::SVGParser::ParseSVG(source, warnings);
if (maybeDocument.hasError()) {
  std::cerr << "Fatal: " << maybeDocument.error() << "\n";   // includes line:column
  return 1;
}
for (const ParseDiagnostic& w : warnings.warnings()) {
  std::cerr << "Warning: " << w << "\n";                     // also line:column + reason
}
```

Both `ParseResult::error()` and each `ParseDiagnostic` stream a human-readable message with the
exact line and column, so you are not left guessing where in the source the trouble was. If you
genuinely do not want diagnostics, pass `ParseWarningSink::Disabled()`; it discards warnings without
even paying the cost of formatting them. But before you disable warnings on a document that "does not
render right", read \ref faq-not-rendered, because the namespace and ignored-attribute cases that
cause silent non-rendering surface here and nowhere else.

## How do I embed Donner without dragging in the editor? {#faq-embed}
You already are: depending on `@donner` gives you the parser, DOM, and renderer, and nothing else.
The editor is a separate Bazel target (`//donner/editor:editor`) that depends on the core libraries,
never the other way around, so linking Donner does not pull in ImGui, GLFW, or any editor code.

The `@donner` alias resolves to `//:donner`, whose only dependency is `//donner/svg/renderer`. That
in turn brings in the SVG DOM (`//donner/svg`), its components, and a rendering backend (tiny-skia by
default). No part of that graph reaches into `//donner/editor`. If you want even less, you can depend
on the narrower targets directly: `//donner/svg` and `//donner/svg/parser` give you parsing and the
DOM with no renderer, and `//donner/css` gives you the CSS engine standalone (see
\ref UsingTheCssApi).

If you have looked at the `svg_viewer` example and worried about its editor dependency, that example
deliberately links one optional editor helper for its text-editing demo; the parser, DOM, and
renderer it uses all come from core. Your own binary that depends on `@donner` gets none of that. For
build setup, see \ref GettingStarted (Bazel) and its CMake section.

## What threads can I touch the DOM from? {#faq-threads}
By default, exactly one: the thread that created the document. \ref donner::svg::SVGDocument starts
in single-threaded mode, which is the lowest-overhead path and asserts that DOM calls come from the
owning thread. If you need worker threads to read or mutate the same document, opt in first.

Enable concurrent access with `document.setThreadingMode(ThreadingMode::ConcurrentDom)`. After that,
the public DOM APIs on `SVGDocument` and `SVGElement` are safe from multiple threads: reads can run
concurrently, and writes are coordinated so the document stays consistent. Rendering is frame-based;
a render observes a stable snapshot of the document for that frame, so a mutation made mid-render is
not blended into the frame already being drawn but shows up on a later one.

For anything more than an occasional single call, use the batching APIs
`SVGDocument::withReadAccess()` and `SVGDocument::withWriteAccess()`: they hold one access scope for
the whole operation instead of re-entering document access on every call, which matters for
selector-heavy traversal and grouped mutations. And if you reach past the public API to raw ECS state
(`SVGDocument::registry()` or `SVGElement::entityHandle()`), you must hold an explicit access guard
in concurrent mode; the guards assert loudly if you forget.

The full contract, including how removed elements stay alive while you hold a handle to them, is in
\ref SvgDomThreadingAndLifetime, with the implementation-level details in \ref Multithreading.

## Why are my stroke widths wrong when I scale? {#faq-stroke-scale}
Because that is what SVG says should happen: `stroke-width` is measured in user units, and when you
scale the coordinate system (a large `viewBox` mapped into a small viewport, or a `transform`), the
stroke scales right along with the geometry. A shape scaled 2x gets a stroke twice as thick. This is
correct, not a Donner quirk.

The wrinkle is that the usual escape hatch does not work here.
`vector-effect="non-scaling-stroke"`, which browsers use to hold a stroke at constant device width
under scaling, is accepted by Donner's parser but not implemented: it is a silent no-op (see
\ref faq-unsupported). So there is currently no attribute you can set to opt a stroke out of scaling.

If you need a stroke that stays visually constant, you have to compensate yourself: either
pre-divide the `stroke-width` by the scale factor you are applying, or apply the scale to the
geometry's coordinates rather than through a `transform` on a stroked ancestor, so the stroke lives
in an unscaled coordinate space. It is less convenient than `non-scaling-stroke`, but it is
predictable. See \ref xml_path and \ref faq-path-api for setting geometry.

<div class="section_buttons">

| Previous           |                                        Next |
| :----------------- | ------------------------------------------: |
| [Home](index.html) | [Getting started](GettingStarted.html)      |

</div>
