# Generating documentation

Doxygen may be generated with:

```sh
tools/doxygen.sh
```

### Experimental hdoc documentation

To use hdoc to generate documentation, run:

```sh
bazel run --config=doc //:hdoc_serve
```

This uses a libclang tool to generate documentation, but it is not as fully-featured as Doxygen.
