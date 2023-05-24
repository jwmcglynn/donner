# Sourcegraph scip-clang indexer

This indexer generates code intelligence data for Sourcegraph. The latest release can be found at https://github.com/sourcegraph/scip-clang/releases/

## Updating

To update to a newer version, run:

```sh
bazel run //third_party/scip-clang:fetch_new_version -- <tag>

# Example:
bazel run //third_party/scip-clang:fetch_new_version -- v0.1.2
```

## Generating Sourcegraph Code Intelligence Indexes

See [Updating Sourcegraph Code Intelligence](/docs/Updating_Sourcegraph.md).
