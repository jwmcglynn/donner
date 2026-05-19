# DOM Element Lifetime {#DomElementLifetime}

Public `SVGElement` values are lifetime-aware handles, not owning copies of
element data. Each handle retains the document state and records the entity
generation observed when the handle was created.

Attached nodes are owned by the document tree. When a node is removed, Donner
keeps the detached subtree alive while any public handle can still reach an
element in that subtree. This matches DOM expectations: user code can remove an
element, keep the `SVGElement`, mutate it while detached, and insert it again.

Detached subtrees become collectible only after all of these are true:

- The subtree is no longer attached to the document tree.
- No public `SVGElement` handle retains a node in the subtree.
- No render snapshot or observer epoch is deferring detached-node collection.

`NodeLifetimeComponent` stores the generation, external-reference state, and
detached-root metadata for each node. `NodeLifetimeCollector` destroys detached
roots once the retained handles and snapshot deferrals are gone.

## Reference Resolution

Detached nodes are absent from graph traversal and render lookups. ID and
resource resolution should prefer attached document-tree state; stale detached
entries must not keep rendering as if the node were still in the graph.

## Diagnostics And Budgets

Detached-node diagnostics report queued roots, roots retained by public handles,
roots retained by snapshot or observer epochs, and the last collection pass.
Use these counters to distinguish correct retention from a real leak.

Performance budget capture:

```sh
bazel test -c opt //donner/benchmarks:dom_lifetime_perf_capture --test_output=all
DOM_LIFETIME_BENCH_ENFORCE_BUDGETS=1 \
  bazel test -c opt //donner/benchmarks:dom_lifetime_perf_capture --test_output=all
```

The strict budget is a release gate for making concurrent DOM behavior the
default.
