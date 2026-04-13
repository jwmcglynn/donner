# TerminalImageViewer Developer Guide {#TerminalImageViewerDesign}

## Overview
- TerminalImageViewer renders RGBA buffers directly to terminals for debugging and quick inspection
  of rendering output.
- Quarter-pixel rendering is the default: each terminal cell encodes a 2×2 pixel block using
  block-element glyphs chosen to minimize color error in linear color space.
- Half-pixel rendering is available as an opt-in or fallback, mapping one cell to a 1×2 pixel pair
  for compatibility with terminals that cannot reliably display quarter blocks.
- Visual Studio Code interactive terminals are detected automatically; VS Code-friendly defaults
  (truecolor output and CRLF handling) are enabled when present.
- The renderer plugs into `ImageComparisonTestFixture` to print a 2×2 grid of Actual, Expected, and
  Diff images on failure, leaving the fourth cell empty for symmetric padding.

## Architecture Snapshot
- **Sampling pipeline:** Images are resized so that sampling aligns with glyph geometry. Quarter mode
  resizes to `2×columns × 2×rows`, while half mode resizes to `columns × 2×rows` with even heights
  to avoid incomplete pairs.
- **Glyph selection:** Quarter mode evaluates the timg-style set of eight block-element glyphs
  (space, single quadrants, left bar, diagonal, and upper/lower half blocks). Each candidate
  partitions a 2×2 pixel block into foreground/background sets, averages channels in squared space,
  and picks the glyph with the lowest summed squared error. Half mode maps two stacked pixels to ▄
  or ▀, emitting the terminal default background when a partner pixel is missing.
- **Color emission:** Truecolor ANSI sequences are preferred; 256-color quantization uses the
  6×6×6 cube versus grayscale split popularized by timg/tiv. Foreground/background escapes are
  cached per cell to minimize redundant SGR output, and each line ends with a full reset.
- **Capability detection:** Terminal feature detection caches truecolor and VS Code markers derived
  from environment variables. Configuration can override detection for deterministic tests.
- **Layout helpers:** A terminal preview helper scales panels to the detected terminal width, adds
  captions, and builds the 2×2 grid used by `ImageComparisonTestFixture`. The helper respects
  `showTerminalPreview` flags and environment overrides for pixel mode and width.
- **OSC 1337 path:** When configured for iTerm-compatible terminals, the renderer encodes the
  framebuffer as PNG, base64s it, and emits OSC 1337 sequences with inline payloads using cell- or
  pixel-based sizing.

## API Surface
- `TerminalImageViewerConfig` controls pixel mode, truecolor usage, VS Code integration defaults,
  and capability auto-detection.
- `TerminalImageView` describes RGBA input buffers; `TerminalImage` stores sampled cell data for
  repeated rendering.
- `TerminalImageViewer` exposes `sampleImage()`, `render()`, and `DetectConfigFromEnvironment()`
- `ImageComparisonTestFixture` includes `showTerminalPreview` and uses a shared preview helper to
  render Actual/Expected/Diff panels into the terminal when comparisons fail.

## Security and Safety
- Input images come from test fixtures; no external input is consumed. Rendering is deterministic
  and bounded by terminal dimensions.
- When capability detection is inconclusive, the renderer falls back to 256-color output rather than
  throwing. Missing pixel partners in half mode reset to the terminal background.
- Output size is constrained by terminal width to avoid flooding logs; previews are skipped when
  terminals are too narrow for the grid.

## Performance Notes
- Rendering streams rows directly to the output stream without buffering entire frames.
- Foreground/background caching reduces ANSI escape volume, and sampling avoids heap allocations by
  reusing small working buffers.
- Resizing to exact pixel-per-cell dimensions ensures constant-time block access during glyph
  selection.

## Testing and Observability
- Golden-based coverage for quarter- and half-pixel renderers, truecolor versus 256-color emission,
  and VS Code detection lives in `//donner/svg/renderer/tests:terminal_image_viewer_tests`.
- Integration tests for the fixture grid and environment overrides are in
  `//donner/svg/renderer/tests:image_comparison_terminal_preview_tests`.
- Fuzz-style randomized sampling tests assert invariant reset/newline behavior across modes.
- Observability is limited to test log output; no runtime metrics are emitted.

## Integration Guidance
- Use `ImageComparisonTestFixture` defaults to get terminal previews on failures; disable globally
  via `DONNER_ENABLE_TERMINAL_IMAGES=0` or per-test by clearing `showTerminalPreview`.
- Override `DONNER_TERMINAL_PIXEL_MODE` to switch between quarter and half sampling, or supply a
  custom `TerminalImageViewerConfig` when calling the renderer directly.
- Terminal width can be forced with `COLUMNS` when previews need deterministic sizing in CI.

## Limitations and Future Extensions
- Animated image rendering and multi-row grid layouts beyond 2×2 are not supported today.
- Capability detection is environment-driven; terminals that misreport features may require manual
  configuration.
