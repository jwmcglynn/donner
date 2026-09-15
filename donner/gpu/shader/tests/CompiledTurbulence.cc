#include "donner/gpu/shader/tests/CompiledTurbulence.h"

#include <cstddef>
#include <cstdlib>
#include <string_view>

#include "donner/gpu/shader/programs/TurbulenceSource.h"

namespace donner::gpu::shader::tests {
namespace {

template <size_t N>
consteval wgsl::SourceText<N> ReplaceExact(wgsl::SourceText<N> source, std::string_view oldText,
                                           std::string_view newText) {
  if (oldText.size() != newText.size()) std::abort();
  for (size_t offset = 0; offset + oldText.size() <= source.view().size(); ++offset) {
    bool matches = true;
    for (size_t index = 0; index < oldText.size(); ++index)
      matches = matches && source.bytes[offset + index] == oldText[index];
    if (!matches) continue;
    for (size_t index = 0; index < newText.size(); ++index)
      source.bytes[offset + index] = newText[index];
    return source;
  }
  std::abort();
}

constexpr auto kArtifact = wgsl::Compile<programs::kTurbulenceSource, wgsl::Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
constexpr auto kMutatedSource = ReplaceExact(
    ReplaceExact(ReplaceExact(programs::kTurbulenceSource, "@binding(1)", "@binding(7)"),
                 "@binding(2)", "@binding(6)"),
    "@workgroup_size(8, 8, 1)", "@workgroup_size(4, 2, 1)");
constexpr auto kMutatedArtifact = wgsl::Compile<kMutatedSource, wgsl::Projection::All>();
constexpr CompiledShaderView kMutatedView = kMutatedArtifact.view();

constexpr wgsl::SourceText kMathSource{R"wgsl(
@group(0) @binding(0) var inputTexture:texture_2d<f32>;
@group(0) @binding(1) var outputTexture:texture_storage_2d<rgba32float,write>;
fn weighted(a:f32,b:f32,c:f32,d:f32,e:f32,f:f32,g:f32,h:f32)->f32 {
  return a+b*2.0+c*3.0+d*4.0+e*5.0+f*6.0+g*7.0+h*8.0;
}
@compute @workgroup_size(1,1,1)
fn cs_main(@builtin(global_invocation_id) gid:vec3u) {
  let v=textureLoad(inputTexture,vec2i(gid.xy),0);
  textureStore(outputTexture,vec2i(gid.xy),vec4f(weighted(v.x,v.y,v.z,v.w,1.0,2.0,4.0,8.0)));
}
)wgsl"};
constexpr auto kMathArtifact = wgsl::Compile<kMathSource, wgsl::Projection::All>();
constexpr CompiledShaderView kMathView = kMathArtifact.view();
}  // namespace

const CompiledShaderView& TurbulenceAllProjections() {
  return kView;
}

const CompiledShaderView& TurbulenceMutatedAllProjections() {
  return kMutatedView;
}

const CompiledShaderView& EightArgumentCallAllProjections() {
  return kMathView;
}
}  // namespace donner::gpu::shader::tests
