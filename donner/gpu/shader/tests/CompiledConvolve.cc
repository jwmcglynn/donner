#include "donner/gpu/shader/tests/CompiledConvolve.h"

#include <cstddef>
#include <cstdlib>
#include <string_view>

#include "donner/gpu/shader/programs/ConvolveMatrixSource.h"

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

constexpr auto kArtifact = wgsl::Compile<programs::kConvolveMatrixSource, wgsl::Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
constexpr auto kMutatedSource =
    ReplaceExact(ReplaceExact(programs::kConvolveMatrixSource, "@binding(2)", "@binding(7)"),
                 "@workgroup_size(8, 8, 1)", "@workgroup_size(4, 2, 1)");
constexpr auto kMutatedArtifact = wgsl::Compile<kMutatedSource, wgsl::Projection::All>();
constexpr CompiledShaderView kMutatedView = kMutatedArtifact.view();
constexpr auto kHighIndexSource =
    ReplaceExact(kMutatedSource, "clamp(kernelIndex, 0i, 24i)", "(kernelIndex + 1000i      )");
static_assert(kHighIndexSource.view().find("kernelIndex + 1000i") != std::string_view::npos);
constexpr auto kHighIndexArtifact = wgsl::Compile<kHighIndexSource, wgsl::Projection::All>();
constexpr CompiledShaderView kHighIndexView = kHighIndexArtifact.view();
constexpr auto kLowIndexSource =
    ReplaceExact(kMutatedSource, "clamp(kernelIndex, 0i, 24i)", "(kernelIndex - 1000i      )");
static_assert(kLowIndexSource.view().find("kernelIndex - 1000i") != std::string_view::npos);
constexpr auto kLowIndexArtifact = wgsl::Compile<kLowIndexSource, wgsl::Projection::All>();
constexpr CompiledShaderView kLowIndexView = kLowIndexArtifact.view();

}  // namespace

const CompiledShaderView& ConvolveMatrixAllProjections() {
  return kView;
}

const CompiledShaderView& ConvolveMatrixMutatedAllProjections() {
  return kMutatedView;
}

const CompiledShaderView& ConvolveMatrixHighIndexAllProjections() {
  return kHighIndexView;
}

const CompiledShaderView& ConvolveMatrixLowIndexAllProjections() {
  return kLowIndexView;
}

}  // namespace donner::gpu::shader::tests
