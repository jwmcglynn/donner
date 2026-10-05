#include "donner/gpu/shader/tests/CompiledSnapshotUnpremultiply.h"

#include <cstddef>
#include <cstdlib>
#include <string_view>

#include "donner/gpu/shader/programs/SnapshotUnpremultiplyArtifactValidation.h"
#include "donner/gpu/shader/programs/SnapshotUnpremultiplySource.h"
namespace donner::gpu::shader::tests {
namespace {
template <size_t N>
consteval wgsl::SourceText<N> ReplaceExact(wgsl::SourceText<N> source, std::string_view oldText,
                                           std::string_view newText) {
  if (oldText.size() != newText.size()) {
    std::abort();
  }
  const size_t offset = source.view().find(oldText);
  if (offset == std::string_view::npos) {
    std::abort();
  }
  for (size_t index = 0; index < newText.size(); ++index) {
    source.bytes[offset + index] = newText[index];
  }
  return source;
}

constexpr auto kArtifact =
    wgsl::Compile<programs::kSnapshotUnpremultiplySource, wgsl::Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(programs::ValidateSnapshotUnpremultiplyArtifact<kView>());
constexpr auto kMutatedSource =
    ReplaceExact(ReplaceExact(ReplaceExact(ReplaceExact(programs::kSnapshotUnpremultiplySource,
                                                        "@binding(0)", "@binding(6)"),
                                           "@binding(1)", "@binding(5)"),
                              "@workgroup_size(8, 8, 1)", "@workgroup_size(4, 2, 1)"),
                 "fn cs_main(", "fn cs_test(");
constexpr auto kMutatedArtifact = wgsl::Compile<kMutatedSource, wgsl::Projection::All>();
constexpr CompiledShaderView kMutatedView = kMutatedArtifact.view();
static_assert(programs::ValidateSnapshotUnpremultiplyArtifact<kMutatedView>());

// With the input texel (-12, 3, 35, 5), i32 and u32 scalars and vectors shift both ways by runtime
// amounts, where 35 shifts by 3; the i32 right shift is arithmetic and the u32 right shift of the
// top bit is logical. An i32 scalar also shifts left by a literal, an i32 vector shifts through
// compound assignment, and an abstract value concretizes to i32 against a runtime amount.
constexpr wgsl::SourceText kShiftOperatorsSource{R"wgsl(
@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, write>;
@compute @workgroup_size(1, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3u) {
  let value = textureLoad(inputTexture, vec2i(gid.xy), 0);
  let signedValue = i32(value.x);
  let amount = u32(value.y);
  let wide = u32(value.z);
  let small = u32(value.w);
  var lanes = vec2<i32>(signedValue, i32(small));
  lanes <<= vec2<u32>(amount, wide);
  lanes >>= vec2<u32>(1u, amount);
  let high = vec2<u32>(0x80000000u, small) >> vec2<u32>(wide, 1u);
  let unsignedLanes = vec2<u32>(small, 3u) << vec2<u32>(1u, wide);
  let unsignedScalar = (small << wide) + (240u >> wide);
  let flag = 1 << amount;
  let sum = f32(lanes.y) + f32(high.y) + f32(flag) + f32(unsignedLanes.x + unsignedLanes.y) +
            f32(unsignedScalar);
  textureStore(outputTexture, vec2i(gid.xy),
               vec4f(f32((signedValue >> amount) << 1u), f32(high.x), f32(lanes.x), sum));
}
)wgsl"};
constexpr auto kShiftOperatorsArtifact =
    wgsl::Compile<kShiftOperatorsSource, wgsl::Projection::All>();
constexpr CompiledShaderView kShiftOperatorsView = kShiftOperatorsArtifact.view();
}  // namespace
const CompiledShaderView& SnapshotUnpremultiplyAllProjections() {
  return kView;
}
const CompiledShaderView& SnapshotUnpremultiplyMutatedAllProjections() {
  return kMutatedView;
}
const CompiledShaderView& ShiftOperatorsAllProjections() {
  return kShiftOperatorsView;
}
}  // namespace donner::gpu::shader::tests
