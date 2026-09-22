#include "donner/gpu/shader/tests/CompiledGaussian.h"

#include <cstddef>
#include <cstdlib>
#include <string_view>

#include "donner/gpu/shader/programs/GaussianBlurSource.h"

namespace donner::gpu::shader::tests {
namespace {

constexpr auto kArtifact = wgsl::Compile<programs::kGaussianBlurSource, wgsl::Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();

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

constexpr auto kMutatedSource =
    ReplaceExact(ReplaceExact(programs::kGaussianBlurSource, "@binding(2)", "@binding(7)"),
                 "@workgroup_size(8, 8, 1)", "@workgroup_size(4, 2, 1)");
constexpr auto kMutatedArtifact = wgsl::Compile<kMutatedSource, wgsl::Projection::All>();
constexpr CompiledShaderView kMutatedView = kMutatedArtifact.view();

}  // namespace

const CompiledShaderView& GaussianBlurAllProjections() {
  return kView;
}

const CompiledShaderView& GaussianBlurMutatedAllProjections() {
  return kMutatedView;
}

}  // namespace donner::gpu::shader::tests
