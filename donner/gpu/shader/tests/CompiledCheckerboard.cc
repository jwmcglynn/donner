#include "donner/gpu/shader/tests/CompiledCheckerboard.h"

#include <cstddef>
#include <cstdlib>
#include <string_view>

#include "donner/gpu/shader/programs/CheckerboardArtifactValidation.h"
#include "donner/gpu/shader/programs/CheckerboardSource.h"
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

constexpr auto kArtifact = wgsl::Compile<programs::kCheckerboardSource, wgsl::Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(programs::ValidateCheckerboardArtifact<kView>());
constexpr auto kMutatedSource = ReplaceExact(
    ReplaceExact(ReplaceExact(programs::kCheckerboardSource, "@binding(0)", "@binding(4)"),
                 "fn vs_main(", "fn vs_test("),
    "fn fs_main(", "fn fs_test(");
constexpr auto kMutatedArtifact = wgsl::Compile<kMutatedSource, wgsl::Projection::All>();
constexpr CompiledShaderView kMutatedView = kMutatedArtifact.view();
static_assert(programs::ValidateCheckerboardArtifact<kMutatedView>());
}  // namespace
const CompiledShaderView& CheckerboardAllProjections() {
  return kView;
}
const CompiledShaderView& CheckerboardMutatedAllProjections() {
  return kMutatedView;
}
}  // namespace donner::gpu::shader::tests
