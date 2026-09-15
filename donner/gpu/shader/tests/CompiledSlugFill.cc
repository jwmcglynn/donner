#include "donner/gpu/shader/tests/CompiledSlugFill.h"

#include <cstdlib>

#include "donner/gpu/shader/programs/SlugFillSource.h"
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

constexpr auto kArtifact = wgsl::Compile<programs::kSlugFillSource, wgsl::Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
constexpr auto kChanged = ReplaceExact(
    ReplaceExact(ReplaceExact(ReplaceExact(programs::kSlugFillSource, "@binding(0)", "@binding(6)"),
                              "@binding(7)", "@binding(0)"),
                 "fn vs_main(", "fn vs_test("),
    "fn fs_main(", "fn fs_test(");
constexpr auto kMutated = wgsl::Compile<kChanged, wgsl::Projection::All>();
constexpr CompiledShaderView kMutatedView = kMutated.view();
}  // namespace
const CompiledShaderView& SlugFillAllProjections() {
  return kView;
}
const CompiledShaderView& SlugFillMutatedAllProjections() {
  return kMutatedView;
}
namespace {
constexpr wgsl::SourceText kFlatSource{R"wgsl(
struct V { @builtin(position) position:vec4f, @location(0) @interpolate(flat) indices:vec2u, @location(1) @interpolate(flat) tag:i32, }
@vertex fn vs_main(@builtin(vertex_index) v:u32,@builtin(instance_index) i:u32)->V {
  let corners=array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));
  var o:V;o.position=vec4f(corners[v%3u],0,1);o.indices=vec2u(v,i);o.tag=i32(v)-16;return o;
}
@fragment fn fs_main(o:V)->@location(0)vec4f{return vec4f(f32(o.indices.x),f32(o.indices.y),f32(o.tag+32),255)/255.0;}
)wgsl"};
constexpr auto kFlatArtifact = wgsl::Compile<kFlatSource, wgsl::Projection::All>();
constexpr CompiledShaderView kFlatView = kFlatArtifact.view();
}  // namespace
const CompiledShaderView& SlugFlatInterfaceAllProjections() {
  return kFlatView;
}

}  // namespace donner::gpu::shader::tests
