# Fuzzing

Parsers and subparsers within Donner SVG have fuzzers in order to harden the implementation and detect new edge cases.  Fuzzing is performed with [libFuzzer](https://llvm.org/docs/LibFuzzer.html).

## Running a Fuzzer

To run a fuzzer, first build it with `--config=asan-fuzzer`:

```
bazel build --config=asan-fuzzer //src/css/parser:declaration_list_parser_fuzzer
```

Then run it and pass it a directory to use for building the corpus.  This will run indefinitely, until either a crash has been encountered or it is terminated with a Ctrl-C.

```
mkdir ~/declcorpus
bazel-bin/src/css/parser/declaration_list_parser_fuzzer ~/declcorpus/
```

To maximum throughput, run with multiple simultaneous jobs:

```

bazel-bin/src/css/parser/declaration_list_parser_fuzzer ~/declcorpus/ -jobs=8
```

When a failure occurs, a repro file is saved out.  To guard against future crashes, copy the file to the corpus directory in-tree. This will then be validated during normal `bazel test //...` runs.

```
mv ./crash-0f6f12d023e0ad5ed83b29763cd67315e0ee0c6b src/css/parser/tests/declaration_list_parser_corpus/
```
