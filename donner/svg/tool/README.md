# donner-svg tool

`donner-svg` is the user-facing command line utility for rendering and previewing SVG files.

## Run

```sh
bazel run //donner/svg/tool -- input.svg
```

Or use the convenience wrapper:

```sh
tools/donner-svg input.svg
```

## Common usage

```sh
# Render SVG to PNG
bazel run //donner/svg/tool -- input.svg --output output.png

# Render and show terminal preview (no PNG saved)
bazel run //donner/svg/tool -- input.svg --preview

# Interactive preview with mouse selection
bazel run //donner/svg/tool -- input.svg --interactive
```

## Flags

- `--output <png>`: Output filename (default: `output.png`).
- `--width <px>`: Override output width.
- `--height <px>`: Override output height.
- `--preview`: Show terminal preview using `TerminalImageViewer`. Skips PNG output unless `--output` is also specified.
- `--interactive`: Enable interactive terminal selection mode (implies `--preview`).
- `--quiet`: Suppress parse warnings.
- `--verbose`: Enable renderer verbose logging.
- `--experimental`: Enable parser experimental features.
- `--help`: Print usage.
