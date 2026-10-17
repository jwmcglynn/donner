# Renderer bakeoff

Benchmarks third-party 2D renderers against Donner's tiny-skia and Geode backends by driving every
library through Donner's `RendererInterface`. Design: [0066](../../docs/design_docs/0066-renderer_bakeoff.md).

This directory is a separate Bazel module. Donner's root `.bazelignore` excludes it, so its
third-party dependencies never enter the main module's graph. Run every command below from this
directory.

## Build and test

```sh
bazel build //:renderer_bakeoff //report:bakeoff_report
bazel test //harness:harness_tests
```

## Run

```sh
tools/run_bakeoff.sh --out results/<run-name> --suite all --sizes "512 1024"
tools/measure_code_size.sh --out results/<run-name>
bazel run //report:bakeoff_report -- "$PWD/results/<run-name>"
```

`run_bakeoff.sh` starts one process per (backend, scene, size), appends JSON Lines records to
`results.jsonl`, writes snapshots under `png/`, and records host load and concurrently running CI
jobs in `host_load.tsv`. The report compares every snapshot with tiny-skia's and writes `report.md`
and `summary.json`. `results/` is ignored by Git.

A single measurement:

```sh
bazel-bin/renderer_bakeoff --backend geode --scene-file corpus/clip_paths.svg --size 1024
bazel-bin/renderer_bakeoff --describe          # backends and their feature support
bazel-bin/renderer_bakeoff --list-synthetic    # generated stress scenes
```

## Layout

- `harness/`: `AdapterBase` (shared `RendererInterface` plumbing for third-party adapters), feature
  accounting, paint resolution, the null traversal recorder, the backend registry and the
  measurement binary.
- `backends/`: one `alwayslink` library per backend.
- `corpus/`: independently authored scenes, one feature category each. Stress scenes are generated
  by `harness/SyntheticScenes.cc`; Donner's splash files are added by the run script.
- `code_size/`: the minimal program linked once per backend for code-size measurement.
- `report/`: the report generator.
- `tools/`: run and measurement scripts.
