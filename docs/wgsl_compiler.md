# WGSL shader compilation {#WgslCompiler}

Gaussian/box blur, matrix convolution, Slug mask, offset and filter resolve are authored as inline WGSL in
`donner/gpu/shader/programs/*Source.h`. The C++20 compiler validates each source, produces immutable
shader projections and derives its resource interface during constant evaluation. Each artifact
implementation checks the shared host parameter layout. Application code consumes frozen data
through `CompiledShaderView`; it does not invoke a parser or shader emitter at runtime.

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
)wgsl", donner::gpu::shader::wgsl::Projection::Wgsl>();
```

The result owns exact-sized WGSL, MSL and SPIR-V arrays plus reflected resource/member data. Its
`view()` borrows those arrays and cannot be called on a temporary. Keep compiler instantiations in
implementation files, and keep the owning artifact alive while a view is used. Text views carry an
explicit length and do not promise a trailing NUL.

The `Projection` template argument is required. Production code selects exactly the representation
its consumer uses. The Geode adapter links WGSL-only artifacts. Native Metal and Vulkan consumers
link separate artifact libraries with MSL-only and SPIR-V-only data respectively; their getters have
distinct names so an application can intentionally use more than one backend without symbol
collisions. Test controls spell `Projection::All` explicitly.

Unused projection arrays have zero elements in the owning artifact. Neither a runtime selector nor
linker dead stripping decides whether to retain them. The compiler frontend and emitters are used
during constant evaluation; application descriptors consume only the frozen bytes and metadata.
Native GPU tools still perform their normal final compilation.

## Reflected host interface

`MakeShaderDescriptor` selects precompiled bytes for the device and supplies compute-entry and
buffer-range metadata. `MakeBindingLayout` derives layout entries and stage visibility from the same resource
records. Gaussian, convolution, offset, filter-resolve and specular-lighting dispatch resolve their input, output and parameter bindings by
their authored names; changing a binding number changes the layout and resource entries together.

`GaussianBlurParams` is the host parameter type. Its implementation checks the resource's total
size/alignment and every member's offset, size and numeric type against the compiled WGSL
interface. An incompatible shader member edit therefore fails compilation instead of silently
changing the bytes the shader reads. Gaussian dispatch requires a two-dimensional workgroup and
derives its x/y sizes from reflected metadata.

`ConvolveMatrixParams` is a read-only storage block. Its fixed coefficient array has 25 f32 elements,
stride 4 and offset 32; the complete block is 132 bytes. Array length, stride, member types and host
field offsets are checked against reflection independently for each retained projection.

## Supported profile and limits

The compute frontend supports the migrated filter families with: flat numeric buffer structures,
fixed numeric array members of buffer structs, root runtime storage arrays, group-zero sampled/storage textures and samplers, bounded decimal/hexadecimal
numeric literals and abstract scalar constants, scalar/vector expressions and conversions, local bindings, conditionals,
incrementing loops, read-only numeric helpers, and one compute entry with a global-invocation ID.
`Parser.h` describes exact literal and constant-expression restrictions. Helpers cannot write
textures; texture writes occur in the compute entry. Mutable local declarations may omit an initializer and receive a zero value; immutable declarations require one.
`floor`, `sign`, `sin`, `cos` and `pow` support runtime f32 scalar/vector operands.
`pow` requires matching operand shapes. Specular lighting retains the explicit zero-exponent
guard and shares its 144-byte storage layout with diffuse lighting; all 36 fields are verified. Integer `sign` and constant builtin
calls are outside this profile and fail explicitly. Offset retains its half-away-from-zero
rounding helper; replacing it with WGSL `round` changes exact half-pixel shifts.

Unsupported language constructs fail explicitly. Fixed arrays have 1 through 8,192 elements, with integer constant-expression extents; local, parameter, return and nested arrays are outside
this profile. Constant out-of-range indices
fail compilation. Native dynamic indices are clamped before memory access; authored convolution
also clamps its coefficient index explicitly for consistent WebGPU execution. Buffer layouts that
MSL cannot represent, including unsupported vec3 packing, fail projection instead of changing
member offsets.

`ModuleLimits` bounds source bytes, tokens, identifier storage, structures, bindings, symbols,
expressions, statements, functions and nesting. Projection sinks have independent output bounds.
An error invalidates the complete result; a truncated output is never a usable shader. Array
extents accept literals, named constants and arithmetic; runtime values and out-of-range extents
are rejected before layout. Larger buffer arrays are represented by their type/stride, without
allocating one compiler expression per element.

The parser's loop restriction is a syntax/profile constraint, not a proof that arbitrary runtime
parameters terminate. Gaussian's filter-admission path supplies its documented finite sigma and
box-extent bounds. Shader compilation does not make arbitrary GPU execution safe.

MSL/SPIR-V lowering preserves short-circuit operators, protects texture accesses, and handles
defined integer division/remainder and numeric conversion edges. No general optimizer runs in the
frontend. Backend compilers retain responsibility for final target code generation.

Filter resolve writes `rgba8unorm` storage images; the earlier filters use `rgba32float`.
The format is reflected into the bind layout and encoded in SPIR-V image types. Metal uses typed
float writes to the selected texture format. The resolve shader shares a 48-byte parameter block
and an 8,192-entry read-only transfer table with its host. Its table binding is reflected along with
its input, output, parameters and workgroup shape.

## Graphics entry interfaces

Artifacts contain an entry-point array with stage, workgroup dimensions, accessed-resource bits,
and ranges into a flattened input/output array. Function resource use includes called helpers.
Runtime descriptors add compute metadata only for Compute entries, and buffer requirements carry
the actual entry name and stage. The blur and convolution consumers explicitly require one Compute
entry before reading dispatch metadata.

Vertex and fragment entries accept direct scalar/vector interfaces or flat numeric IO structures.
The supported builtins are vertex index, vertex-output/fragment-input position and compute global
invocation ID. User locations currently support f32 scalars/vectors with default interpolation;
integer interpolation and structured compute inputs remain explicitly unsupported. Duplicate
locations/builtins, missing decorations, invalid stage/type combinations and vertex entries without
position are rejected. Flat structure values may be passed to and returned from helpers.

MSL projects entry interfaces through stage-specific wrappers over ordinary value structures.
SPIR-V declares stage IO variables and reconstructs parameters and return values from the same
validated interface records. A shared full-screen triangle fixture verifies the two-entry artifact,
reflection, ordinary/constant-evaluation agreement and offline native compilation. The authored
Slug mask uses these interfaces; native execution also covers analytic/binary coverage, winding
rules, clipping and declared buffer ranges.

The matrix profile accepts f32 matrix types and their `matCxRf` aliases, column-vector, copy and
zero constructors, value parameters/returns, constant column reads and dimension-checked
multiplication. Vector aliases such as `vec2f` and `vec2i` share the normal type/constructor path.
Reflection records matrix row/column counts and column stride. Uniform matrices with 8-byte column
stride are explicitly outside the portable Vulkan 1.1 layout profile; storage layouts and value
matrices retain their natural stride. Dynamic matrix column indexing, scalar-list matrix
constructors and constant matrix arithmetic remain unsupported.

Runtime storage arrays are supported at binding roots with numeric or flat structure elements.
Fixed numeric arrays may appear in buffers; uniform arrays require a stride divisible by 16.
Reflection carries runtime element stride and a minimum range containing one element into the
existing device buffer-requirement contract. Nested/member runtime arrays, array aliases and array
writes remain unsupported.

Metal reads the existing reserved table of exact declared buffer lengths, converts bytes to element
counts using the reflected stride, and guards empty/clamped reads. SPIR-V emits buffer-block
wrappers, `OpArrayLength` and guarded value loads. A structure member is extracted from the guarded
loaded value, so member access cannot bypass the empty-array guard. The shared storage fixture
covers nested reads, direct structure-member reads and fixed uniform vector arrays.

## Validation

The focused compiler tests cover the real Gaussian and convolution modules, exact artifact/interface construction,
binding/type edits, malformed source and text-emission regressions:

```sh
bazel test //donner/gpu/shader/wgsl:wgsl_tests
```

`//donner/gpu/shader/wgsl:parser_fuzzer` replays checked-in inputs through parsing and bounded
emission; `_bin` and `_soak` provide mutation testing. These targets use normal allocator paths.
The existing WGSL, offline MSL and SPIR-V validation suites consume the compiled Gaussian
artifact. Native Metal/Vulkan blur acceptance and the Geode filter suites exercise its pixels,
uniform metadata and clipping behavior.

## Linked binary isolation

`//donner/gpu/shader/artifact_tests:all` builds separate consumers for each family's WGSL and native
artifact, plus explicit all-projection controls. The consumers read every retained payload through
volatile pointers. Inspection verifies the executable container format and checks that selected
payload markers are present and excluded payload markers are absent. Native Apple binaries exclude
WGSL and SPIR-V; native Linux binaries exclude WGSL and MSL. The WebGPU consumers exclude both
native formats. All-projection controls retain all three deliberately.

Report section/payload sizes as well as file sizes: executable page alignment, symbol tables and
metadata retention can hide or exaggerate a change in payload bytes. Cross-linked ELF inspection
proves byte retention; native driver execution remains a separate validation step.

The numeric/control profile also covers the Slug mask's scalar abstract arithmetic, module constants,
`break`, `continue`, `discard`, bitwise AND, vector math and derivatives. `fwidth` is restricted to
straight-line fragment-entry code before any conditional or loop and outside short-circuit operands;
this conservative profile does not claim general uniformity analysis. Module constants cannot call
runtime helpers. Logical/bitwise grouping and relational non-associativity are validated explicitly.
MSL helpers receive only the resources in their validated transitive use masks. Immutable parse and
emission phases are shared by source/projection template instances before freezing the exact-sized
artifact. No compiler phase executes at runtime. The complete production Slug caller migration and
its performance qualification are still pending.

WGSL `discard` retains the source-level Next behavior, so it does not satisfy a value-returning
function's authored return requirement. The accepted profile disallows derivatives after discard
and has no observable fragment-side resource writes; native lowering can end the discarded
invocation early. See the [WGSL discard and behavior rules](https://www.w3.org/TR/WGSL/#discard-statement).

The full mask is registered in the offline MSL/SPIR-V validator suites. Native Metal and Vulkan
acceptance cases use the platform-only artifact to render a rectangle with fractional horizontal
edges, binary coverage, nested clip values, both winding rules, and deliberately shorter declared
buffer ranges than the underlying allocations. They use the existing strict bitmap comparator.
These cases are the execution gate for the mask, not a claim that every supported platform has
already passed. The adapter pipeline reads entry names and binding slots from the frozen interface.
The Slug target's positive Clang evaluator budget is emitted in CMake only for Clang/AppleClang;
other compilers retain their own evaluator defaults.
