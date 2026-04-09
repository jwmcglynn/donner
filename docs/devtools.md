# Devtools {#Devtools}

\tableofcontents

## Generating Documentation

Doxygen may be generated with:

```sh
tools/doxygen.sh
```

It requires the `doxygen` package to be installed. The generated documentation will be in `generated-doxygen/html/index.html`.

Tools required to generate the documentation are:
- `doxygen`
- `graphviz` (for class diagrams)

## Code Coverage

To generate code coverage locally:

```sh
tools/coverage.sh
```

Thn open `coverage-report/index.html` in a browser.

## Binary Size

To generate a binary size report, run:

```sh
tools/binary_size.sh
```

Then open `build-binary-size/binary_size_report.html` in a browser.

## Clang Tidy

clang-tidy may be invoked by building with `--config=clang-tidy`:

```sh
bazel build --config clang-tidy //...
```

## Lines of Code

```sh
tools/cloc.sh
```

# Security

- \subpage Fuzzing
