#include "donner/gpu/shader/tests/CompiledFilterBlend.h"

#include <cstddef>
#include <cstdlib>
#include <string_view>

#include "donner/gpu/shader/programs/FilterBlendArtifactValidation.h"
#include "donner/gpu/shader/programs/FilterBlendSource.h"
namespace donner::gpu::shader::tests {
namespace {
template <size_t N>
consteval wgsl::SourceText<N> ReplaceExact(wgsl::SourceText<N> source, std::string_view oldText,
                                           std::string_view newText) {
  if (oldText.size() != newText.size()) std::abort();
  const size_t offset = source.view().find(oldText);
  if (offset == std::string_view::npos) std::abort();
  for (size_t index = 0; index < newText.size(); ++index)
    source.bytes[offset + index] = newText[index];
  return source;
}

constexpr auto kArtifact = wgsl::Compile<programs::kFilterBlendSource, wgsl::Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(programs::ValidateFilterBlendArtifact<kView>());
constexpr auto kMutatedSource = ReplaceExact(
    ReplaceExact(ReplaceExact(ReplaceExact(ReplaceExact(ReplaceExact(programs::kFilterBlendSource,
                                                                     "@binding(0)", "@binding(6)"),
                                                        "@binding(1)", "@binding(5)"),
                                           "@binding(2)", "@binding(4)"),
                              "@binding(3)", "@binding(7)"),
                 "@workgroup_size(8, 8)", "@workgroup_size(4, 2)"),
    "fn cs_main(", "fn cs_test(");
constexpr auto kMutatedArtifact = wgsl::Compile<kMutatedSource, wgsl::Projection::All>();
constexpr CompiledShaderView kMutatedView = kMutatedArtifact.view();
static_assert(programs::ValidateFilterBlendArtifact<kMutatedView>());
}  // namespace
const CompiledShaderView& FilterBlendAllProjections() {
  return kView;
}
const CompiledShaderView& FilterBlendMutatedAllProjections() {
  return kMutatedView;
}
}  // namespace donner::gpu::shader::tests
