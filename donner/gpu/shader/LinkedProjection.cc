#include "donner/gpu/shader/LinkedProjection.h"

#include <mutex>
#include <vector>

#include "donner/base/Utils.h"

namespace donner::gpu::shader {
namespace {

/// One registered view of a family for devices that consume a projection this build does not
/// link.
struct AlternateProjection {
  const CompiledShaderView* linked;     //!< The family's linked artifact.
  ShaderSourceKind kind;                //!< Source kind \ref alternate carries.
  const CompiledShaderView* alternate;  //!< The family's artifact for \ref kind.
};

/// The registered alternates, empty in every production binary.
struct AlternateRegistry {
  std::mutex mutex;                          //!< Guards \ref entries.
  std::vector<AlternateProjection> entries;  //!< Registered alternates, in registration order.
};

/// Constructed on first use, so a static initializer in another translation unit can register,
/// and never destroyed, so no lookup during static destruction sees a destroyed registry.
AlternateRegistry& Registry() {
  static AlternateRegistry& registry = *new AlternateRegistry();
  return registry;
}

}  // namespace

const CompiledShaderView& SelectLinkedProjection(ShaderSourceKind deviceKind,
                                                 const CompiledShaderView& linked) {
  if (deviceKind == kLinkedShaderSourceKind) {
    return linked;
  }
  AlternateRegistry& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  for (const AlternateProjection& entry : registry.entries) {
    if (entry.linked == &linked && entry.kind == deviceKind) {
      return *entry.alternate;
    }
  }
  return linked;
}

// Declared in LinkedProjectionTesting.h, which only test-only targets can depend on, so no
// production target can register an alternate.
void RegisterAlternateProjection(const CompiledShaderView& linked, ShaderSourceKind kind,
                                 const CompiledShaderView& alternate) {
  AlternateRegistry& registry = Registry();
  std::lock_guard<std::mutex> lock(registry.mutex);
  for (const AlternateProjection& entry : registry.entries) {
    if (entry.linked == &linked && entry.kind == kind) {
      UTILS_RELEASE_ASSERT_MSG(entry.alternate == &alternate,
                               "a different alternate projection is already registered");
      return;
    }
  }
  registry.entries.push_back(AlternateProjection{&linked, kind, &alternate});
}

}  // namespace donner::gpu::shader
