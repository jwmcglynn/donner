#include "donner/gpu/shader/tests/CompiledSlugMask.h"

#include "donner/gpu/shader/programs/SlugMaskArtifactValidation.h"
#include "donner/gpu/shader/programs/SlugMaskSource.h"

namespace donner::gpu::shader::tests {
namespace {
constexpr auto kArtifact = wgsl::Compile<programs::kSlugMaskSource, wgsl::Projection::All>();
constexpr CompiledShaderView kView = kArtifact.view();
static_assert(programs::ValidateSlugMaskArtifact<kView>());
}  // namespace
const CompiledShaderView& SlugMaskAllProjections() {
  return kView;
}
}  // namespace donner::gpu::shader::tests
