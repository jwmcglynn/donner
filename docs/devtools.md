# Devtools {#Devtools}

## Generating documentation

Doxygen may be generated with:

```sh
tools/doxygen.sh
```

## Code coverage

To generate code coverage locally:

```sh
tools/coverage.sh
```

Thn open `coverage-report/index.html` in a browser.

## Binary size

To generate a binary size report, run:

```sh
tools/binary_size.sh
```

Then open `build-binary-size/binary_size_report.html` in a browser.

# Updating Sourcegraph code search

Prerequisites:

- Install the `src` CLI: https://docs.sourcegraph.com/cli/quickstart

Run the build_sourcegraph_index script to generate compile commands and invoke scip-clang:
First, generate compile commands:

```sh
tools/build_sourcegraph_index.sh
```

Upload it to Sourcegraph, using the `src` CLI:

```sh
src code-intel upload -file=index.scip
```

## Clang tidy

clang-tidy may be invoked by building with `--config=clang-tidy`:

```sh
bazel build --config clang-tidy //...
```

## Lines of code

```sh
tools/cloc.sh
```

# Security

- \subpage Fuzzing
