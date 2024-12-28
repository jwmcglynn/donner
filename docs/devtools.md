# Devtools {#Devtools}

\tableofcontents

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
