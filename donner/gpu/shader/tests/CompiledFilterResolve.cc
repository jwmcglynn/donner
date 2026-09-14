#include "donner/gpu/shader/tests/CompiledFilterResolve.h"

#include <cstddef>
#include <cstdlib>
#include <string_view>

#include "donner/gpu/shader/programs/FilterResolveSource.h"

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

constexpr auto kArtifact = wgsl::Compile<programs::kFilterResolveSource, wgsl::Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
constexpr auto kMutatedSource = ReplaceExact(
    ReplaceExact(ReplaceExact(programs::kFilterResolveSource, "@binding(2)", "@binding(7)"),
                 "@binding(3)", "@binding(6)"),
    "@workgroup_size(8, 8, 1)", "@workgroup_size(4, 2, 1)");
constexpr auto kMutatedArtifact = wgsl::Compile<kMutatedSource, wgsl::Projection::All>();
constexpr CompiledShaderView kMutatedView = kMutatedArtifact.view();

}  // namespace

const CompiledShaderView& FilterResolveAllProjections() {
  return kView;
}

const CompiledShaderView& FilterResolveMutatedAllProjections() {
  return kMutatedView;
}

}  // namespace donner::gpu::shader::tests
