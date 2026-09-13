# WGSL compute shader compilation {#WgslCompiler}

Gaussian and box blur are authored as inline WGSL in
`donner/gpu/shader/programs/GaussianBlurSource.h`. The C++20 compiler validates that source,
produces immutable shader projections, and derives the resource interface during constant
evaluation. `GaussianBlur.cc` holds the compiled artifact and checks the host uniform layout.
Application code consumes its data through `CompiledShaderView`; it does not invoke a parser or
shader emitter at runtime.

## Authoring and ownership

`wgsl::Compile` is an immediate function. Invalid source, unsupported constructs, exhausted
capacities, or projection failures fail C++ compilation. Its source template argument is an owning
structural string, so an inline literal needs no literal-operator extension:

```cpp
constexpr auto shader = donner::gpu::shader::wgsl::Compile<R"wgsl(
@group(0) @binding(0) var outputImage: texture_storage_2d<rgba32float, write>;
@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
  textureStore(outputImage, vec2<i32>(gid.xy), vec4<f32>(1f));
}
)wgsl">();
```

The result owns exact-sized WGSL, MSL and SPIR-V arrays plus reflected resource/member data. Its
`view()` borrows those arrays and cannot be called on a temporary. Keep compiler instantiations in
implementation files, and keep the owning artifact alive while a view is used. Text views carry an
explicit length and do not promise a trailing NUL.

The `Projection` template argument selects retained outputs. The browser artifact retains WGSL.
Desktop Gaussian artifacts retain WGSL for the transition adapter plus the native projection for
that platform. Cross-projection tests compile the same source for all outputs. Platform GPU
compilers still perform their normal final compilation; no ordinary-execution WGSL frontend is
substituted when constant evaluation fails.

## Reflected host interface

`MakeShaderDescriptor` selects precompiled bytes for the device and supplies compute-entry and
buffer-range metadata. `MakeComputeBindingLayout` derives layout entries from the same resource
records. Gaussian dispatch resolves its input, output and parameter bindings by their authored
names; changing a binding number changes the layout and resource entries together.

`GaussianBlurParams` is the host parameter type. Its implementation checks the resource's total
size/alignment and every member's offset, size and numeric type against the compiled WGSL
interface. An incompatible shader member edit therefore fails compilation instead of silently
changing the bytes the shader reads. Gaussian dispatch requires a two-dimensional workgroup and
derives its x/y sizes from reflected metadata.

## Supported profile and limits

The frontend covers the constructs required by the Gaussian family, rather than claiming full
WGSL conformance: flat numeric uniform structures, group-zero sampled/storage textures, typed
numeric literals, scalar/vector expressions and conversions, local bindings, conditionals,
incrementing loops, read-only numeric helpers, and one compute entry with a global-invocation ID.
`Parser.h` describes exact literal and constant-expression restrictions. Helpers cannot write
textures; texture writes occur in the compute entry. Local declarations require initializers.
Unsupported language constructs fail explicitly.

`ModuleLimits` bounds source bytes, tokens, identifier storage, structures, bindings, symbols,
expressions, statements, functions and nesting. Projection sinks have independent output bounds.
An error invalidates the complete result; a truncated output is never a usable shader.

The parser's loop restriction is a syntax/profile constraint, not a proof that arbitrary runtime
parameters terminate. Gaussian's filter-admission path supplies its documented finite sigma and
box-extent bounds. Shader compilation does not make arbitrary GPU execution safe.

MSL/SPIR-V lowering preserves short-circuit operators, protects texture accesses, and handles
defined integer division/remainder and numeric conversion edges. No general optimizer runs in the
frontend. Backend compilers retain responsibility for final target code generation.

## Validation

The focused compiler tests cover the real Gaussian module, exact artifact/interface construction,
binding/type edits, malformed source and text-emission regressions:

```sh
bazel test //donner/gpu/shader/wgsl:wgsl_tests
```

`//donner/gpu/shader/wgsl:parser_fuzzer` replays checked-in inputs through parsing and bounded
emission; `_bin` and `_soak` provide mutation testing. These targets use normal allocator paths.
The existing WGSL, offline MSL and SPIR-V validation suites consume the compiled Gaussian
artifact. Native Metal/Vulkan blur acceptance and the Geode filter suites exercise its pixels,
uniform metadata and clipping behavior.
