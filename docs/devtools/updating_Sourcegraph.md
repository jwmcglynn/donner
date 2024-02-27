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
