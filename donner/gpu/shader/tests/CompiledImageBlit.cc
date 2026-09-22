#include "donner/gpu/shader/tests/CompiledImageBlit.h"

#include <cstdlib>
#include <string_view>

#include "donner/gpu/shader/programs/ImageBlitSource.h"
namespace donner::gpu::shader::tests {
namespace {
template <size_t N, size_t Old, size_t New>
consteval auto ReplaceExact(wgsl::SourceText<N> source, const char (&before)[Old],
                            const char (&after)[New]) {
  char bytes[N - Old + New]{};
  const std::string_view pattern(before, Old - 1);
  const size_t at = source.view().find(pattern);
  if (at == std::string_view::npos ||
      source.view().find(pattern, at + 1) != std::string_view::npos) {
    std::abort();
  }
  size_t cursor = 0;
  for (size_t i = 0; i < at; ++i) {
    bytes[cursor++] = source.bytes[i];
  }
  for (size_t i = 0; i < New - 1; ++i) {
    bytes[cursor++] = after[i];
  }
  for (size_t i = at + Old - 1; i < N; ++i) {
    bytes[cursor++] = source.bytes[i];
  }
  return wgsl::SourceText<N - Old + New>(bytes);
}
constexpr auto kArtifact = wgsl::Compile<programs::kImageBlitSource, wgsl::Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
constexpr auto kMutatedSource =
    ReplaceExact(ReplaceExact(ReplaceExact(ReplaceExact(ReplaceExact(programs::kImageBlitSource,
                                                                     "@binding(0)", "@binding(7)"),
                                                        "@binding(1)", "@binding(9)"),
                                           "@binding(2)", "@binding(8)"),
                              "fn vs_main(", "fn vs_test("),
                 "fn fs_main(", "fn fs_test(");
constexpr auto kMutated = wgsl::Compile<kMutatedSource, wgsl::Projection::All>();
constexpr CompiledShaderView kMutatedView = kMutated.view();
constexpr auto kExplicitSource = ReplaceExact(
    programs::kImageBlitSource, "  var sampled = textureSample(imageTexture, imageSampler, in.uv);",
    "  var sampled = vec4f(0.0); if (in.uv.x > 0.0) { sampled = textureSampleLevel(imageTexture, "
    "imageSampler, in.uv, 0.0); }");
constexpr auto kExplicit = wgsl::Compile<kExplicitSource, wgsl::Projection::All>();
constexpr CompiledShaderView kExplicitView = kExplicit.view();
}  // namespace
const CompiledShaderView& ImageBlitAllProjections() {
  return kView;
}
const CompiledShaderView& ImageBlitMutatedAllProjections() {
  return kMutatedView;
}
const CompiledShaderView& ImageBlitExplicitLevelAllProjections() {
  return kExplicitView;
}
namespace {
constexpr wgsl::SourceText kArraySwitchSource{
    R"wgsl(@group(0) @binding(0) var inputTexture:texture_2d<f32>;
@group(0) @binding(1) var outputTexture:texture_storage_2d<rgba32float,write>;

fn scalar(mode:i32)->f32 {
  let frozen=array<f32,3>(1,2,3,);
  var working=frozen;
  working[1]=7;
  switch(mode) {
    case 0: { working[0]=9; break; }
    case 1: { return frozen[1]+working[1]; }
    default: { working[2]=5; }
  }
  return frozen[0]+working[0]+working[2]+array<f32,2>(4,8)[u32(mode & 1)];
}
@compute @workgroup_size(1,1,1)
fn cs_main(@builtin(global_invocation_id) gid:vec3u) {
  let v=textureLoad(inputTexture,vec2i(gid.xy),0);
  textureStore(outputTexture,vec2i(gid.xy),vec4f(scalar(i32(v.x)),scalar(i32(v.y)),scalar(i32(v.z)),scalar(i32(v.w))));
}
)wgsl"};
constexpr auto kArraySwitchArtifact = wgsl::Compile<kArraySwitchSource, wgsl::Projection::All>();
constexpr CompiledShaderView kArraySwitchView = kArraySwitchArtifact.view();
}  // namespace
const CompiledShaderView& ArraySwitchAllProjections() {
  return kArraySwitchView;
}
namespace {
constexpr wgsl::SourceText kLoopSwitchSource{
    R"wgsl(@group(0) @binding(0) var inputTexture:texture_2d<f32>;
@group(0) @binding(1) var outputTexture:texture_storage_2d<rgba32float,write>;

fn scalar(mode:i32)->f32 {
  var total:f32=0;
  for(var k:i32=0;k<3;k=k+1) {
    switch(k) {
      case 0: {total=total+1;continue;}
      case 1: {total=total+2;break;}
      default: {total=total+4;}
    }
    total=total+8;
  }
  return total+f32(mode);
}
@compute @workgroup_size(1,1,1)
fn cs_main(@builtin(global_invocation_id) gid:vec3u) {
  let v=textureLoad(inputTexture,vec2i(gid.xy),0);
  textureStore(outputTexture,vec2i(gid.xy),vec4f(scalar(0))+v);
}
)wgsl"};
constexpr auto kLoopSwitchArtifact = wgsl::Compile<kLoopSwitchSource, wgsl::Projection::All>();
constexpr CompiledShaderView kLoopSwitchView = kLoopSwitchArtifact.view();
}  // namespace
const CompiledShaderView& LoopSwitchAllProjections() {
  return kLoopSwitchView;
}
namespace {
constexpr wgsl::SourceText kVectorMixSource{
    R"wgsl(@group(0) @binding(0) var inputTexture:texture_2d<f32>;
@group(0) @binding(1) var outputTexture:texture_storage_2d<rgba32float,write>;
@compute @workgroup_size(1,1,1)
fn cs_main(@builtin(global_invocation_id) gid:vec3u) {
  let v=textureLoad(inputTexture,vec2i(gid.xy),0);
  textureStore(outputTexture,vec2i(gid.xy),mix(v,1.0-v,vec4f(0.25,0.5,0.75,1.0))+2.0);
}
)wgsl"};
constexpr auto kVectorMixArtifact = wgsl::Compile<kVectorMixSource, wgsl::Projection::All>();
constexpr CompiledShaderView kVectorMixView = kVectorMixArtifact.view();
}  // namespace
const CompiledShaderView& VectorMixAllProjections() {
  return kVectorMixView;
}

namespace {
constexpr wgsl::SourceText kStructConstructionSource{R"wgsl(
struct Color { rg:vec2f, b:f32, a:f32, }
struct Scalar { value:f32, }
@group(0) @binding(0) var inputTexture:texture_2d<f32>;
@group(0) @binding(1) var outputTexture:texture_storage_2d<rgba32float,write>;
@compute @workgroup_size(1,1,1)
fn cs_main(@builtin(global_invocation_id) gid:vec3u) {
  let v=textureLoad(inputTexture,vec2i(gid.xy),0);
  let c=Color(v.xy,v.z,v.w,);
  let zero=Color();
  let scalar=Scalar(c.a);
  textureStore(outputTexture,vec2i(gid.xy),vec4f(c.rg,c.b,scalar.value)+vec4f(zero.b));
}
)wgsl"};
constexpr auto kStructConstructionArtifact =
    wgsl::Compile<kStructConstructionSource, wgsl::Projection::All>();
constexpr CompiledShaderView kStructConstructionView = kStructConstructionArtifact.view();
}  // namespace
const CompiledShaderView& StructConstructionAllProjections() {
  return kStructConstructionView;
}

namespace {
constexpr wgsl::SourceText kPointerStructSource{R"wgsl(
@group(0) @binding(0) var inputTexture:texture_2d<f32>;
@group(0) @binding(1) var outputTexture:texture_storage_2d<rgba32float,write>;

const kSlots: u32 = 4u;

struct Accumulator {
  values: array<vec2f, kSlots>,
  count: u32,
  total: i32,
  filled: bool,
};

struct Summary { sum: f32, steps: i32, complete: bool, };

fn push(state: ptr<function, Accumulator>, value: f32) {
  if ((*state).count == kSlots) {
    (*state).filled = false;
    return;
  }
  (*state).values[(*state).count] = vec2f(value, 1.0);
  (*state).count += 1u;
  (*state).total += 1;
}

fn summarize(state: ptr<function, Accumulator>) -> Summary {
  var sum = 0.0;
  var steps = 0;
  var index = 0u;
  while (index < (*state).count) {
    sum += (*state).values[index].x;
    steps += 1;
    index += 1u;
  }
  return Summary(sum, steps, (*state).filled);
}

fn evaluate(seed: f32) -> f32 {
  var state: Accumulator;
  state.filled = true;
  var step = 0u;
  loop {
    if (step == kSlots) {
      break;
    }
    push(&state, seed * f32(step + 1u));
    step += 1u;
  }
  push(&state, seed);
  let summary = summarize(&state);
  var result = summary.sum;
  result -= seed;
  result += select(0.0, 0.5, !summary.complete);
  return result + f32(summary.steps + state.total) * 0.0625;
}

@compute @workgroup_size(1,1,1)
fn cs_main(@builtin(global_invocation_id) gid:vec3u) {
  let v = textureLoad(inputTexture, vec2i(gid.xy), 0);
  textureStore(outputTexture, vec2i(gid.xy),
               vec4f(evaluate(v.x), evaluate(v.y), evaluate(v.z), evaluate(v.w)));
}
)wgsl"};
constexpr auto kPointerStructArtifact =
    wgsl::Compile<kPointerStructSource, wgsl::Projection::All>();
constexpr CompiledShaderView kPointerStructView = kPointerStructArtifact.view();
}  // namespace
const CompiledShaderView& PointerStructAllProjections() {
  return kPointerStructView;
}

namespace {
constexpr wgsl::SourceText kLoopWhileSource{R"wgsl(
@group(0) @binding(0) var inputTexture:texture_2d<f32>;
@group(0) @binding(1) var outputTexture:texture_storage_2d<rgba32float,write>;

fn evaluate(seed: f32) -> f32 {
  var total = 0.0;
  var steps = 8;
  var index = 0u;
  loop {
    if (index == 4u) {
      break;
    }
    var inner = 0u;
    while (inner < index) {
      total += seed;
      inner += 1u;
    }
    steps -= 1;
    index += 1u;
  }
  var scaled = total;
  scaled -= seed;
  return scaled + f32(steps) * 0.0625;
}

@compute @workgroup_size(1,1,1)
fn cs_main(@builtin(global_invocation_id) gid:vec3u) {
  let v = textureLoad(inputTexture, vec2i(gid.xy), 0);
  textureStore(outputTexture, vec2i(gid.xy),
               vec4f(evaluate(v.x), evaluate(v.y), evaluate(v.z), evaluate(v.w)));
}
)wgsl"};
constexpr auto kLoopWhileArtifact = wgsl::Compile<kLoopWhileSource, wgsl::Projection::All>();
constexpr CompiledShaderView kLoopWhileView = kLoopWhileArtifact.view();
}  // namespace
const CompiledShaderView& LoopWhileAllProjections() {
  return kLoopWhileView;
}
}  // namespace donner::gpu::shader::tests
