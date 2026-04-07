# donner-svg tool

`donner-svg` is the user-facing command line utility for rendering and previewing SVG files.

## Run

```sh
tools/donner-svg input.svg
```

Or using bazel run (tools/donner-svg is a bazel alias for //donner/svg/tool):

```sh
bazel run //donner/svg/tool -- input.svg
```

## Common usage

```sh
# Render SVG to PNG
tools/donner-svg input.svg --output output.png

# Render and show terminal preview (no PNG saved)
tools/donner-svg input.svg --preview

# Interactive preview with mouse selection
tools/donner-svg input.svg --interactive
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
