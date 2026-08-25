# GPU runtime provenance record

Copy this template into the pull request description of every change that adds or modifies code in
`donner/gpu/`, and fill in every section. A section that does not apply says so and why; a section
left blank means the record is incomplete.

This record exists because the GPU runtime is a clean-room implementation. An independent
provenance and license audit is a blocking release gate, and it can only be performed against
contemporaneous records. Reconstructing one after the fact is not the same evidence.

## Permitted inputs

Only these may inform an implementation:

- Donner's own renderer requirements, call sites, tests, captures, design docs, and shader
  algorithms.
- Normative public specifications: WebGPU, WGSL, Vulkan, SPIR-V.
- Official Apple Metal documentation and platform SDK interfaces.
- Black-box outputs of the current Donner renderer produced from Donner-owned test inputs: pixels,
  public error outcomes, counters, command captures, and timing measurements.

## Prohibited inputs

- Source from `wgpu`, `wgpu-native`, Naga, Dawn, Tint, or their internal tests, whether copied or
  translated line by line.
- Their internal object models, private algorithms, generated headers, state trackers, shader IR,
  or backend workarounds.
- Linking any of those implementations into the runtime, shader toolchain, tests, or CI.

Reading a prohibited project's public documentation to understand a platform concept is not the
same as reading its implementation. If you read its implementation, say so here and expect the
change to be reworked from the specification instead.

---

## Record

### What this change implements

State the Donner requirement or the normative specification section, with a link or a section
number. "Because the renderer needs it" is not a requirement; name the call site.

### Design notes for nontrivial algorithms

For each nontrivial algorithm or state machine, describe the approach in your own words and why it
was chosen. A change with no nontrivial algorithm says so.

### Behavior-establishing tests

Name each test target and case that establishes the behavior. A behavior with no test is not
established.

### Frozen-baseline comparison

If the change alters rendered output, structural counters, or public error outcomes, name the
frozen baselines it was compared against and the result. See `donner/gpu/baseline/README.md`.

### Third-party headers, tools, and SDKs used

List every third-party header, platform SDK, out-of-process validator, and toolchain used to build
or validate the change, with versions where the behavior depends on them. Out-of-process validators
are not implementation dependencies, but they belong in the record.

### Confirmation

- [ ] No prohibited implementation source was consulted, copied, or translated for this change.
- [ ] No prohibited implementation is linked into the runtime, tests, or CI by this change.
- [ ] Every third-party input used is listed above.
