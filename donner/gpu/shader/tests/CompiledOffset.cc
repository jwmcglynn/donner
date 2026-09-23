#include "donner/gpu/shader/tests/CompiledOffset.h"

#include <cstddef>
#include <cstdlib>
#include <string_view>

#include "donner/gpu/shader/programs/OffsetSource.h"

namespace donner::gpu::shader::tests {
namespace {

template <size_t N>
consteval wgsl::SourceText<N> ReplaceExact(wgsl::SourceText<N> source, std::string_view oldText,
                                           std::string_view newText) {
  if (oldText.size() != newText.size()) {
    std::abort();
  }
  for (size_t offset = 0; offset + oldText.size() <= source.view().size(); ++offset) {
    bool matches = true;
    for (size_t index = 0; index < oldText.size(); ++index) {
      matches = matches && source.bytes[offset + index] == oldText[index];
    }
    if (!matches) {
      continue;
    }
    for (size_t index = 0; index < newText.size(); ++index) {
      source.bytes[offset + index] = newText[index];
    }
    return source;
  }
  std::abort();
}

constexpr auto kArtifact = wgsl::Compile<programs::kOffsetSource, wgsl::Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
constexpr auto kMutatedSource =
    ReplaceExact(ReplaceExact(programs::kOffsetSource, "@binding(2)", "@binding(7)"),
                 "@workgroup_size(8, 8, 1)", "@workgroup_size(4, 2, 1)");
constexpr auto kMutatedArtifact = wgsl::Compile<kMutatedSource, wgsl::Projection::All>();
constexpr CompiledShaderView kMutatedView = kMutatedArtifact.view();

constexpr wgsl::SourceText kFloorSource{R"wgsl(
@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var outputTexture: texture_storage_2d<rgba32float, write>;
@compute @workgroup_size(1, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3u) {
  let value = textureLoad(inputTexture, vec2i(gid.xy), 0);
  textureStore(outputTexture, vec2i(gid.xy), floor(value));
}
)wgsl"};
constexpr auto kFloorArtifact = wgsl::Compile<kFloorSource, wgsl::Projection::All>();
constexpr CompiledShaderView kFloorView = kFloorArtifact.view();
constexpr auto kSignSource = ReplaceExact(kFloorSource, "floor(value)", "sign (value)");
constexpr auto kSignArtifact = wgsl::Compile<kSignSource, wgsl::Projection::All>();
constexpr CompiledShaderView kSignView = kSignArtifact.view();

}  // namespace

const CompiledShaderView& OffsetAllProjections() {
  return kView;
}

const CompiledShaderView& OffsetMutatedAllProjections() {
  return kMutatedView;
}

const CompiledShaderView& FloorAllProjections() {
  return kFloorView;
}
const CompiledShaderView& SignAllProjections() {
  return kSignView;
}

}  // namespace donner::gpu::shader::tests
