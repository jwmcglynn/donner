# Donner SVG Preview — VS Code Extension

A VS Code custom editor extension that previews SVG files using the Donner SVG
rendering engine (compiled to WASM). Until the WASM build is ready, a mock
fallback uses the browser's native SVG renderer.

## Quick Start

```bash
cd tools/vscode_extension
npm install
npm run compile
```

### Install locally

```bash
npm run package          # Produces donner-svg-preview-0.1.0.vsix
code --install-extension donner-svg-preview-0.1.0.vsix
```

Then open any `.svg` file and use **"Open With…" → "Donner SVG Preview"**.

## Development

```bash
npm run watch   # Rebuild on file change (esbuild watch mode)
```

Press **F5** in VS Code to launch the Extension Development Host.

## Building with the Real WASM Renderer

Once the Donner WASM renderer target is available:

```bash
# From the repository root
bazel build --config=editor-wasm //donner/svg/renderer/wasm:donner_renderer_wasm

# Stage assets into the extension
tools/vscode_extension/scripts/stage_wasm_assets.sh
```

Then rebuild and package the extension.

## Architecture

```
┌─────────────────────────────────────────────────┐
│  VS Code                                        │
│  ┌────────────────┐    ┌──────────────────────┐ │
│  │  TextDocument   │───▶│  SvgPreviewEditor    │ │
│  │  (source of     │    │  Provider            │ │
│  │   truth)        │    │  - sends SVG text    │ │
│  └────────────────┘    │  - tracks saves/edits│ │
│                         └────────┬─────────────┘ │
│                                  │ postMessage   │
│                         ┌────────▼─────────────┐ │
│                         │  Webview (boot.ts)   │ │
│                         │  - loads WASM/mock   │ │
│                         │  - renders to canvas │ │
│                         │  - zoom / pan        │ │
│                         └──────────────────────┘ │
└─────────────────────────────────────────────────┘
```

### WASM Interface Contract

The real WASM module (implemented separately) exports:

```typescript
interface DonnerWasm {
  donner_init(): void;
  donner_render_svg(svg_text: string, width: number, height: number): number;
  donner_free_pixels(ptr: number): void;
  donner_get_last_error(): string;
}
```

The mock module (`webview/mock_donner.ts`) provides the same API surface using
the browser's native `<img>` SVG rendering as a fallback.

## Controls

| Action | Input |
|--------|-------|
| Zoom | Mouse wheel |
| Pan | Click + drag |
| Fit to view | **Fit** button |
| Reset zoom | **1:1** button |

## Features

- Live preview updates on file save
- "Stale" badge when the document has unsaved changes
- Dark / Light / High Contrast theme support
- Checkerboard transparency background
- Error overlay for parse failures
- 32 MiB file size guard
