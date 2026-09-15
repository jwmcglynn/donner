# WGSL shader compilation {#WgslCompiler}

Every production shader is authored as inline WGSL in `donner/gpu/shader/programs/*Source.h`:
Gaussian/box blur, matrix convolution, Slug mask, offset, filter resolve, specular and diffuse
lighting, turbulence, image blit, Slug fill, dedicated gradients, feBlend, feFlood, feMerge,
feComposite, feColorMatrix, feMorphology, feComponentTransfer, feDisplacementMap, feDropShadow,
feImage, feTile, subregion clipping, color-space conversion, snapshot unpremultiply and the
transparency checkerboard. The C++20 compiler validates each source, produces immutable shader
projections and derives its resource interface during constant evaluation. Each artifact
implementation checks the shared host parameter layout. Application code consumes frozen data
through `CompiledShaderView`; it does not invoke a parser or shader emitter at runtime. The
build-time shader IR remains only as an emitter and native-execution fixture (solid fill and the
color-matrix test kernel); no production pipeline is generated from it.

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

## v1 language boundary {#WgslV1Boundary}

The compiler implements a fixed WGSL profile, not the complete language. The profile is the set of
constructs the production shader families use, together with diagnostics that name everything else.
v1 makes these claims and no others:

- Every production shader under `donner/gpu/shader/programs/` compiles during C++ constant
  evaluation under this profile, and its WGSL, MSL and SPIR-V projections are frozen into the
  artifact libraries described above.
- Source outside the profile fails C++ compilation with a named `ErrorCode` and a source span. No
  runtime compiler, partial artifact or alternate code path stands in for a rejected shader.
- Conformance with the WGSL specification is claimed only for the constructs listed under
  "Supported profile and limits", as exercised by the shader tests, the offline Metal and SPIR-V
  validators, the native execution suites and the parser fuzzer. Full WGSL conformance, the
  optional feature extensions and the WebGPU conformance test suite are outside v1.

Outside the profile, and rejected explicitly: `f16` and matrices with a non-f32 element type;
`atomic`, `bitcast`, `override` and pipeline-overridable constants; `alias`, `const_assert`,
`enable` and `requires` directives; the `workgroup` and `private` address spaces, workgroup shared
memory and barriers; `binding_array`, depth, cube, 1D, 3D and arrayed textures and texture builtins
other than `textureDimensions`, `textureLoad`, `textureSample`, `textureSampleLevel` and
`textureStore`; bit shifts, and bitwise operators other than the integer AND the Slug profile lists;
constant f32 arithmetic in constant expressions (integer constant expressions only); `continuing`
blocks; pointers outside a call argument or dereference, including pointers to resources,
immutables, members and array elements; fixed arrays of structures, nested arrays, array parameters
and returns; module-scope mutable variables; bindings outside group zero; and any source byte
outside ASCII. `Parser.h` and the profile section above are the exact record; the compiler names the
construct it rejects.

Extending the profile is the intended response when a production shader needs a construct the
compiler rejects: add the construct to the parser and to all three emitters together, add positive
cases for each projection and a negative case for the previous rejection, extend the validators
and fuzzer corpus, and update the profile section. Working around the compiler by generating the
shader elsewhere is not part of the design.

## Supported profile and limits

The compute frontend supports the migrated filter families with: numeric buffer structures, including bounded nesting,
fixed numeric array members of buffer structs, root runtime storage arrays, group-zero sampled/storage textures and samplers, bounded decimal/hexadecimal
numeric literals and abstract scalar constants, scalar/vector expressions and conversions, local bindings, conditionals,
incrementing loops, read-only numeric helpers with up to eight parameters, and one compute entry with a global-invocation ID.
`Parser.h` describes exact literal and constant-expression restrictions. Helpers cannot write
textures; texture writes occur in the compute entry. Mutable local declarations may omit an initializer and receive a zero value; immutable declarations require one.
`floor`, `sign`, `sin`, `cos` and `pow` support runtime f32 scalar/vector operands.
`pow` requires matching operand shapes. Specular lighting retains the explicit zero-exponent
guard and shares its 144-byte storage layout with diffuse lighting; all 36 fields are verified. Integer `sign` and constant builtin
calls are outside this profile and fail explicitly. Offset retains its half-away-from-zero
rounding helper; replacing it with WGSL `round` changes exact half-pixel shifts.

Unsupported language constructs fail explicitly. Fixed arrays have 1 through 8,192 elements, with integer constant-expression extents; fixed numeric local arrays support zero construction, up to eight explicit constructor arguments,
whole local copies and indexed writes. Fixed numeric arrays can also be copied from buffers into
local values. Direct array parameters, array returns, nested arrays and fixed arrays of structures
remain outside this profile. Constant out-of-range indices
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
The supported builtins are vertex index, instance index, vertex-output/fragment-input position and
compute global invocation ID. User locations support f32 scalars/vectors with default interpolation
and scalar/vector integer interstage values with explicit flat interpolation. Structured compute
inputs and optional interpolation sampling modes remain outside this profile. Duplicate
locations/builtins, missing decorations, invalid stage/type combinations and vertex entries without
position are rejected. Bounded nested structure values, including fixed numeric array members, may
be passed to and returned from helpers.

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

Runtime storage arrays are supported at binding roots with numeric or bounded structure elements.
Fixed numeric arrays may appear in buffers and copied structure values; uniform arrays require a stride divisible by 16. Nested uniform layouts are checked recursively, including required alignment and separation after structure members.
Reflection carries runtime element stride and a minimum range containing one element into the
existing device buffer-requirement contract. Nested/member runtime arrays, array aliases and writes to buffer arrays remain unsupported.

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
`break`, `continue`, `discard`, bitwise AND, vector math and derivatives. Its per-fragment ray-event
sorting adds function-address-space pointers, `loop`/`while` and compound assignment, described below. `fwidth` and implicit-LOD `textureSample` require uniform fragment control, including through helper
calls. A bounded dependency graph follows parameters, mutable values, branches, switches,
short-circuit operands and loop-carried state. Complete branch reconvergence restores uniform
control; divergent early returns and loop exits retain their dependencies. Partial aggregate
writes conservatively taint the whole value. This profile does not claim full WGSL conformance. Module constants cannot call
runtime helpers. Logical/bitwise grouping and relational non-associativity are validated explicitly.
MSL helpers receive only the resources in their validated transitive use masks. Immutable parse and
emission phases are shared by source/projection template instances before freezing the exact-sized
artifact. No compiler phase executes at runtime. The production Slug mask caller consumes the frozen artifact and reflected interface.

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
The Slug and image-blit targets' positive Clang evaluator budgets are emitted in CMake only for Clang/AppleClang;
other compilers retain their own evaluator defaults.

## Image sampling and local control flow

Image blit uses a shared 176-byte `ImageBlitParams` block. Each retained artifact verifies every
authored member against the host layout. Its live caller derives all six bindings and both entry
names from reflection. Filtered textures are distinct from textures used only for texel reads; the
unused clip sampler is removed from the shader and host layout.

`textureSample` and `textureSampleLevel` support 2D float textures, a regular sampler and vec2f
coordinates; explicit LOD adds an f32 level. Optional offsets and other sampling overloads are
outside this profile. Explicit LOD permits varying control. Neither implicit sampling nor
derivatives may follow a possible discard, including helper and loop-carried paths.

`mix` accepts f32 scalar/vector operands and a matching or scalar factor. Numeric scalar/vector
addition and subtraction support both operand orders. Switches have one integer selector per
clause, distinct constant cases and exactly one default. Their breaks leave the switch; continues
still target the enclosing loop. Constructor and call argument lists accept trailing commas.
Structure constructors accept zero arguments or one typed value per member, up to eight
arguments. Structures containing fixed numeric arrays support value copies, parameters, returns and member-array assignment. Fixed arrays of structures and structures used both as direct buffer roots and as nested/runtime-array elements remain explicitly unsupported in this profile.

Native image tests compare nearest/linear/pixelated sampling, alpha handling, masks and all CSS
blend modes with existing CPU references through the strict bitmap comparator. Separate controls
change entry names and binding numbers and exercise explicit-LOD sampling under varying control.
Platform-only linked probes verify that unused WGSL/MSL/SPIR-V payloads remain excluded.

## Function pointers, unbounded loops and compound assignment

A helper parameter may be declared `ptr<function, T>` where `T` is a scalar, vector or structure.
`&localVar` produces such a pointer and `(*p)` reads or writes the pointee, including
`(*p).member`, `(*p).member[index]` and compound assignment through either. The restriction is
deliberate and narrow: only a whole function-scope `var` has an address, so `&resource`,
`&immutable`, `&value.member` and `&array[index]` are rejected. Pointers cannot be declared as
locals, stored in a `let`, returned, placed in a structure member or taken by an entry point. A
pointer argument matches only a pointer parameter of the same pointee type, so a value and a
pointer are never substituted for one another. Two pointer arguments of one call may not address
the same variable. Together these confine every pointer to a call argument or a dereference, with
no aliasing of a resource and no pointer outliving its pointee.

MSL lowers a pointer parameter to `thread T&`, so both operators disappear at the call site and at
the dereference. SPIR-V declares an `OpTypePointer Function` parameter; a whole-pointee read or
write is an `OpLoad` or `OpStore` on that parameter, and only member and index accesses go through
an `OpAccessChain`. A helper that returns no value is called as a statement, which is the only
expression statement the profile accepts.

`loop { ... }` and `while (condition) { ... }` join the existing incrementing `for`. A `loop` must
contain a `break` that exits it, so the statement after the loop is always reachable and an
unconditional infinite loop is rejected before emission. `continuing` blocks are outside this
profile. `break` and `continue` keep their existing meaning; `continue` in a `while` re-evaluates
the condition. Loop nesting is bounded at eight levels by `ModuleLimits::kMaxLoopDepth`, and an
exceeded bound fails with a structured diagnostic rather than a truncated artifact. The same bound
now applies to `for`, whose nesting was previously limited only by the general sixteen-level
nesting bound. Neither form is
a termination proof: only `for` retains the finite-increment restriction, so a `loop` or `while`
whose exit depends on runtime data terminates only because the authored algorithm does.

`target += value` and `target -= value` are parsed as an assignment of the sum or difference, so
integer wrapping, division guards and both lowerings match the spelled-out form byte for byte. The
right-hand side is materialized against the target's type and the operator is grouped with it, so
the compound form is slightly stricter than writing the assignment out: an abstract scalar against
a vector target is a type mismatch, and an ungrouped mixed operator on the right-hand side is an
unsupported construct. Both fail closed. The target is read and written, so it may not contain a
call.

`var` declarations without a declared type concretize an abstract initializer by the WGSL rules:
`var x = 0;` is `i32`, `var x = 0u;` is `u32` and `var x = 0.5;` is `f32`; an already-concrete
initializer keeps its own type.

A `bool` member is accepted in a function-scope value structure, including as a constructor
argument and in a zero-initialized `var` whose structure also has a fixed array member. Because
`bool` has no defined buffer representation, a structure that reaches one is not host-shareable:
using it as a uniform or storage binding root, or as the element of a runtime storage array, is
rejected, and SPIR-V emits it without explicit layout decorations. Reflection is unchanged for
every structure that can appear in a buffer.

The uniformity analysis carries writes through a pointer across the call boundary. Each helper
records, for every pointer parameter, what its pointee depends on when the function returns,
including non-uniform sources the callee read itself such as texel loads, sampling and read-only
storage. The caller substitutes that mask into the variable whose address it passed, so a pointer
write taints the caller exactly as the same write would if it were inlined. A call with no pointer
argument allocates no dependency edges. `loop` and `while` reuse the existing loop analysis, and a
pointer parameter takes part in it: its pointee gets a loop-carried value, so a write in one
iteration reaches the next iteration's read. Loop-carried values keep their dependencies, a
data-dependent `break` still reconverges at the merge, and a data-dependent early `return` retains
its dependencies exactly as it does for `for`. A derivative or implicit-LOD sample therefore stays legal before the loops and
is rejected when it follows a divergent early exit or branches on a value written through a
pointer.

## Slug fill

`SlugFillSource.h` is authoritative for the ordinary and batched entry-point pairs. The live
pipeline and every fill bind-group path use the frozen resource interface and entry names. Shared
416-byte draw parameters and 256-byte instance records replace the duplicated host declarations.
Compile-time guards cover every host field, nested transform member, both band layouts and all
eleven resource kinds, ranges and strides. The unused clip sampler is removed.

The profile supports vertex `instance_index` and flat scalar/vector integer interstage values.
Flat interpolation uses the first vertex; optional interpolation sampling modes remain outside
this profile. Reflection retains the builtin and flat qualifier. Native acceptance uses differing
per-vertex values and nonzero vertex/instance bases, plus differently translated storage records.

The bounded module permits 64 KiB of source, 16,384 tokens/identifier bytes, 16 structures,
256 members, 1,024 symbols/statements, 4,096 expressions and 64 functions. A fixed type may occupy
at most 1 MiB; layout growth is checked before recording member offsets. Text emission is bounded
at 128 KiB. Slug fill, the Slug mask and the dedicated gradients each use a local 4,194,304-step Clang
evaluator cap; existing smaller family caps remain independently checked.

Native tests cover ordinary and batched fills, fractional/binary coverage, clipping, patterns,
linear/radial gradients, painter ordering and reads limited to a declared record range. Duplicate
contour even-odd cancellation is checked in binary mode; analytic single-contour coverage is a
separate case. The inherited analytic coverage approximation is not a proof of duplicate-contour
cancellation. These tests use strict pixel comparison and do not change the coverage algorithm.

## Dedicated gradients and filter blend

`SlugGradientSource.h` owns the dedicated linear/radial gradient program. Its 672-byte,
16-byte-aligned host block contains sixteen straight-alpha stop colors, packed offsets, transforms,
clip planes and bounding geometry. Both transient and resident draw paths use the reflected ten
resources and two entry names. Each band layout and every host member is checked, including the
stop and clip array strides. The unused clip sampler and its last device allocation are removed.
Gradient interpolation remains in straight alpha, with premultiplication at fragment output.

`FilterBlendSource.h` owns the sixteen SVG blend modes with a 16-byte parameter block aligned to
four bytes. Both sampled inputs, the float storage output and parameters use reflected bindings.
The runtime compute path derives independent X/Y dispatch counts from the entry's workgroup shape;
other two-input programs retain their existing binding defaults until they are migrated. The
obsolete raw-wgpu blend pipeline and its separate parameter pool are removed. Hue and saturation
use range normalization so the middle color channel survives saturation adjustment.

The portable v1 profile still rejects local declarations that shadow module names and MSL entry
names reserved by the target language. The gradient's local `linear_t` becomes `linear_parameter`,
and feBlend's entry becomes `cs_main`; these identifier changes preserve shader behavior. Full
language/target name handling belongs to the broader compiler scope.

Dedicated gradients have a bounded 4,194,304-step Clang evaluator cap; feBlend retains the default
1,048,576-step cap. Native acceptance covers negative repeat/reflect coordinates, transformed
ramps, unequal stop alphas, sixteen-stop arrays, empty/single-stop ramps, radial/focal cases,
clipping, winding and declared ranges. Blend references cover all sixteen modes, premultiplied and
transparent inputs, differently sized input textures and the default switch branch. Mutation
controls change bindings, entry names and feBlend's workgroup to 4x2 on a 7x5 output.

## Remaining production families

The last fifteen production programs moved from IR builders and build-time generated descriptor
headers to authored sources under the same contract as the earlier families. Each family has a
WGSL-only artifact for the WebGPU adapter, a native-only artifact, an all-projection test control,
a mutation control that renumbers every binding, renames the entry points and, for compute, changes
the workgroup to 4x2, and three linked isolation probes. Host parameter layouts live next to the
artifact accessors (`FloodParams`, `CompositeParams` with `CompositeOperator`,
`FilterColorMatrixParams`, `TileParams`, `MorphologyParams`, `DisplacementMapParams`,
`DropShadowParams`, `FilterImageParams`, `ColorSpaceConvertParams` with the shared
`ColorTransferTable`, `CheckerboardParams`) or are shared: subregion clipping reuses
`FilterResolveParams`, and diffuse lighting shares `LightingParams` and the complete 36-field
`ValidateLightingArtifact` check with specular lighting. Component transfer keeps its packed
read-only float array with `kComponentTransferHeaderWords` records before the tables.

The Geode filter engine creates every program from reflection: the single-input helper resolves
its authored input name (`imageTexture` for feImage), the two-input helper takes the authored
source, backdrop, output and optional parameter names (feMerge has none), flood and turbulence use
the plain reflected layout, and every dispatch derives its workgroup counts from the entry
metadata. The legacy host-side binding enums, workgroup constants, generated descriptor headers
and the build-time emitter tool are gone. The checkerboard render pipeline and the snapshot
readback pipeline read entry names, binding slots and workgroup shape from their artifacts, and
their pass code binds the reflected slot rather than a literal index.

Two sources changed spelling without changing behavior to stay inside the portable profile. The
snapshot half-alpha term uses unsigned division by two instead of a right shift, and the feImage
cubic weight constant is the f32 literal `0.33333334f`, the same value `1f / 3f` folds to,
because constant f32 arithmetic is outside the profile. Every other family compiled unchanged,
including the morphology loops, the runtime component-transfer array, the 8,192-entry transfer
table and the vertex/fragment checkerboard.

Native acceptance runs every family on Metal and Vulkan from the native artifact and from the
mutation control through reflected bindings. The new slices compare bit-exactly where the inputs
are dyadic (flood, merge, every composite operator including an unknown index, the dyadic
color-matrix cases, subregion clipping under identity, scaled, rotated and empty rectangles,
snapshot unpremultiply against the host rounding formula, and both color-space directions through
the shared table) and through the strict 8-bit comparator for the saturate, hue-rotate and
luminance-to-alpha matrices. The checkerboard oracle also builds a replace-mode pipeline straight
from the mutation artifact. Offline MSL and SPIR-V validators compile the production and mutated
projections of all fifteen families.
