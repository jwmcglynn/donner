# Updating Sourcegraph Code Intelligence

Prerequisites:
- Install the `src` CLI: https://docs.sourcegraph.com/cli/quickstart

First, generate compile commands:
```sh
tools/refresh_compile_commands.sh
```

Then run scip-clang to generate `index.scip`:
```sh
bazel run //third_party/scip-clang -- --compdb-path compile_commands.json
```

And then finally upload it to Sourcegraph, using the `src` CLI:
```sh
src code-intel upload -file=index.scip
```
