# Terminal image previews {#TerminalImageViewerGuide}

TerminalImageViewer renders RGBA buffers directly to the terminal for debugging test failures.
It is enabled by default for `ImageComparisonTestFixture` failures and can be adjusted at runtime
without recompiling tests.

## Usage in tests

- `ImageComparisonParams::showTerminalPreview` gates whether the fixture emits the 2x2 grid of
  Actual/Expected/Diff panels.
- The fixture scales each panel to fit the current terminal width while preserving aspect ratios.
- Captions appear above each panel; the bottom-right cell remains empty to keep alignment stable.

## Environment controls

- `DONNER_ENABLE_TERMINAL_IMAGES` (default: `1`): set to `0` to disable previews in all tests.
- `DONNER_TERMINAL_PIXEL_MODE`: override sampling granularity. Accepts `quarter` (default) or
  `half`.
- `COLUMNS`: overrides the detected terminal width when scaling panels.

When running inside a VS Code interactive terminal, the renderer automatically enables VS Code
line endings and truecolor output. Capability detection can also be overridden by passing
`TerminalImageViewerConfig` directly to the renderer in custom harnesses.
