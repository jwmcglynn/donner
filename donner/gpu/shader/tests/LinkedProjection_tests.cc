/// @file
/// Selection between a family's linked artifact and an alternate a test registered for it.

#include "donner/gpu/shader/LinkedProjection.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/LinkedProjectionTesting.h"
#include "donner/gpu/shader/tests/CompiledShaderViewPrinter.h"

namespace donner::gpu::shader {
namespace {

using testing::Ref;

/// A source kind this build's artifacts do not carry.
constexpr ShaderSourceKind kOtherKind = kLinkedShaderSourceKind == ShaderSourceKind::Wgsl
                                            ? ShaderSourceKind::Msl
                                            : ShaderSourceKind::Wgsl;

/// A third kind, distinct from both \ref kLinkedShaderSourceKind and \ref kOtherKind.
constexpr ShaderSourceKind kThirdKind =
    kLinkedShaderSourceKind != ShaderSourceKind::Spirv && kOtherKind != ShaderSourceKind::Spirv
        ? ShaderSourceKind::Spirv
        : (kLinkedShaderSourceKind != ShaderSourceKind::Msl && kOtherKind != ShaderSourceKind::Msl
               ? ShaderSourceKind::Msl
               : ShaderSourceKind::Wgsl);

static_assert(kOtherKind != kLinkedShaderSourceKind && kThirdKind != kLinkedShaderSourceKind &&
              kThirdKind != kOtherKind);

// Views are identified by address. Every test that registers registers the same alternate for
// kRegisteredLinked, so the tests run in any order.
const CompiledShaderView kUnregisteredLinked{.wgsl = "unregistered"};
const CompiledShaderView kRegisteredLinked{.wgsl = "registered"};
const CompiledShaderView kRegisteredAlternate{.wgsl = "alternate"};
const CompiledShaderView kOtherLinked{.wgsl = "other linked"};

TEST(LinkedProjectionTest, DeviceConsumingTheLinkedKindReceivesTheLinkedArtifact) {
  RegisterAlternateProjection(kRegisteredLinked, kOtherKind, kRegisteredAlternate);

  EXPECT_THAT(SelectLinkedProjection(kLinkedShaderSourceKind, kRegisteredLinked),
              Ref(kRegisteredLinked));
  EXPECT_THAT(SelectLinkedProjection(kLinkedShaderSourceKind, kUnregisteredLinked),
              Ref(kUnregisteredLinked));
}

TEST(LinkedProjectionTest, DeviceConsumingAnotherKindReceivesTheRegisteredAlternate) {
  RegisterAlternateProjection(kRegisteredLinked, kOtherKind, kRegisteredAlternate);

  EXPECT_THAT(SelectLinkedProjection(kOtherKind, kRegisteredLinked), Ref(kRegisteredAlternate));
}

TEST(LinkedProjectionTest, WithoutAnAlternateTheLinkedArtifactIsReturnedForRefusal) {
  RegisterAlternateProjection(kRegisteredLinked, kOtherKind, kRegisteredAlternate);

  // Nothing is registered for this artifact, nor for this kind of the registered one: the caller
  // gets the linked artifact, which carries nothing in that kind, and module creation refuses it.
  EXPECT_THAT(SelectLinkedProjection(kOtherKind, kUnregisteredLinked), Ref(kUnregisteredLinked));
  EXPECT_THAT(SelectLinkedProjection(kThirdKind, kRegisteredLinked), Ref(kRegisteredLinked));
}

TEST(LinkedProjectionTest, RegisteringTheSameAlternateAgainIsANoOp) {
  RegisterAlternateProjection(kRegisteredLinked, kOtherKind, kRegisteredAlternate);
  RegisterAlternateProjection(kRegisteredLinked, kOtherKind, kRegisteredAlternate);

  EXPECT_THAT(SelectLinkedProjection(kOtherKind, kRegisteredLinked), Ref(kRegisteredAlternate));
}

TEST(LinkedProjectionDeathTest, ADifferentAlternateForTheSameFamilyAndKindIsFatal) {
  RegisterAlternateProjection(kRegisteredLinked, kOtherKind, kRegisteredAlternate);

  EXPECT_DEATH(RegisterAlternateProjection(kRegisteredLinked, kOtherKind, kOtherLinked),
               "already registered");
}

TEST(LinkedProjectionTest, AlternatesAreKeyedByTheLinkedArtifactsIdentity) {
  RegisterAlternateProjection(kRegisteredLinked, kOtherKind, kRegisteredAlternate);

  // Equal contents are not the same artifact.
  const CompiledShaderView copy = kRegisteredLinked;
  EXPECT_THAT(SelectLinkedProjection(kOtherKind, copy), Ref(copy));
  EXPECT_THAT(SelectLinkedProjection(kOtherKind, kOtherLinked), Ref(kOtherLinked));
}

}  // namespace
}  // namespace donner::gpu::shader
