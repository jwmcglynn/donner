/// @file
/// UI texture registration identity, alpha interpretation, validation and frame lifetime.

#include "donner/editor/gui/UiTextureRegistry.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::ElementsAre;
using testing::Eq;
using testing::HasSubstr;
using testing::IsEmpty;

namespace donner::editor {
namespace {

using gpu::GpuErrorType;

/// Matches a \ref UiTextureBinding field by field, printing the whole expected and actual binding
/// on mismatch so a failure names which of view, extent or alpha mode drifted.
///
/// @param view Expected view.
/// @param width Expected width in texels.
/// @param height Expected height in texels.
/// @param alphaMode Expected alpha interpretation.
MATCHER_P4(IsBinding, view, width, height, alphaMode,
           "is " + testing::PrintToString(UiTextureBinding{view, gpu::Extent2d{width, height},
                                                           alphaMode})) {
  *result_listener << "which is " << testing::PrintToString(arg);
  return arg == UiTextureBinding{view, gpu::Extent2d{width, height}, alphaMode};
}

/// A registry over a recording device, plus ownership of the texture each view is taken from.
class UiTextureRegistryTest : public testing::Test {
protected:
  /// Creates a sampled texture view on \p device, retaining the texture for the test's lifetime.
  /// @param device Device to create on. @param label Diagnostic label.
  gpu::TextureView makeView(gpu::RecordingDevice& device, const char* label) {
    gpu::Result<gpu::Texture> texture = device.createTexture(gpu::TextureDescriptor{
        label, gpu::Extent2d{8, 4}, gpu::TextureFormat::RGBA8Unorm, gpu::TextureUsage::Sampled});
    EXPECT_THAT(texture, gpu::HasResult());
    ownedTextures_.push_back(std::move(texture).result());
    gpu::Result<gpu::TextureView> view =
        device.createTextureView(ownedTextures_.back(), gpu::TextureViewDescriptor{label});
    EXPECT_THAT(view, gpu::HasResult());
    return std::move(view).result();
  }

  /// Registers \p view at the fixture's standard extent.
  /// @param view View to register. @param alphaMode Alpha interpretation to record.
  gpu::Result<UiTextureId> registerView(const gpu::TextureView& view,
                                        UiTextureAlphaMode alphaMode) {
    return registry_.registerTexture(UiTextureDescriptor{view, gpu::Extent2d{8, 4}, alphaMode});
  }

  gpu::RecordingDevice device_;
  UiTextureRegistry registry_{device_};
  // Declared last so the views' textures are released before the device they belong to.
  std::vector<gpu::Texture> ownedTextures_;
};

TEST_F(UiTextureRegistryTest, RegistrationRoundTripsViewExtentAndAlphaMode) {
  const gpu::TextureView view = makeView(device_, "thumbnail");

  const gpu::Result<UiTextureId> id = registerView(view, UiTextureAlphaMode::Straight);
  ASSERT_THAT(id, gpu::HasResult());

  const gpu::Result<UiTextureBinding> binding = registry_.lookup(id.result());
  ASSERT_THAT(binding, gpu::HasResult());
  EXPECT_THAT(binding.result(),
              IsBinding(gpu::TextureViewRef(view), 8u, 4u, UiTextureAlphaMode::Straight));
  EXPECT_THAT(registry_.liveCount(), Eq(1u));
  EXPECT_THAT(registry_.retiredCount(), Eq(0u));
}

TEST_F(UiTextureRegistryTest, AlphaModeIsPerRegistration) {
  const gpu::TextureView premultipliedView = makeView(device_, "premultiplied");
  const gpu::TextureView straightView = makeView(device_, "straight");

  const gpu::Result<UiTextureId> premultiplied =
      registerView(premultipliedView, UiTextureAlphaMode::Premultiplied);
  const gpu::Result<UiTextureId> straight =
      registerView(straightView, UiTextureAlphaMode::Straight);
  ASSERT_THAT(premultiplied, gpu::HasResult());
  ASSERT_THAT(straight, gpu::HasResult());

  const gpu::Result<UiTextureBinding> premultipliedBinding =
      registry_.lookup(premultiplied.result());
  const gpu::Result<UiTextureBinding> straightBinding = registry_.lookup(straight.result());
  ASSERT_THAT(premultipliedBinding, gpu::HasResult());
  ASSERT_THAT(straightBinding, gpu::HasResult());
  EXPECT_THAT(premultipliedBinding.result(), IsBinding(gpu::TextureViewRef(premultipliedView), 8u,
                                                       4u, UiTextureAlphaMode::Premultiplied));
  EXPECT_THAT(straightBinding.result(),
              IsBinding(gpu::TextureViewRef(straightView), 8u, 4u, UiTextureAlphaMode::Straight));
}

TEST_F(UiTextureRegistryTest, IdentifierSurvivesTheRoundTripThroughDrawData) {
  const gpu::TextureView view = makeView(device_, "thumbnail");

  const gpu::Result<UiTextureId> id = registerView(view, UiTextureAlphaMode::Premultiplied);
  ASSERT_THAT(id, gpu::HasResult());

  const UiTextureId recovered = UiTextureId::FromImTextureId(id.result().imTextureId());
  EXPECT_THAT(recovered, Eq(id.result()));
  EXPECT_THAT(registry_.lookup(recovered), gpu::HasResult());
}

TEST_F(UiTextureRegistryTest, IdentifierIsASlotAndGenerationRatherThanAnAddress) {
  const gpu::TextureView view = makeView(device_, "thumbnail");

  const gpu::Result<UiTextureId> id = registerView(view, UiTextureAlphaMode::Premultiplied);
  ASSERT_THAT(id, gpu::HasResult());

  EXPECT_THAT(id.result().slotIndex(), Eq(0u));
  EXPECT_THAT(id.result().generation(), Eq(1u));
  EXPECT_THAT(id.result().imTextureId(), Eq(static_cast<ImTextureID>(ImTextureID{1} << 32)));
}

TEST_F(UiTextureRegistryTest, NullIdentifierKeepsTheNoTextureValue) {
  EXPECT_THAT(UiTextureId().isValid(), Eq(false));
  EXPECT_THAT(UiTextureId().imTextureId(), Eq(ImTextureID{0}));
}

TEST_F(UiTextureRegistryTest, RegistrationRefusesANullView) {
  EXPECT_THAT(registry_.registerTexture(UiTextureDescriptor{
                  gpu::TextureViewRef(), gpu::Extent2d{8, 4}, UiTextureAlphaMode::Premultiplied}),
              gpu::IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("is null")));
}

TEST_F(UiTextureRegistryTest, RegistrationRefusesAViewFromAnotherDevice) {
  gpu::RecordingDevice otherDevice;
  const gpu::TextureView foreignView = makeView(otherDevice, "foreign");

  EXPECT_THAT(
      registerView(foreignView, UiTextureAlphaMode::Premultiplied),
      gpu::IsGpuErrorWithMessage(GpuErrorType::DeviceMismatch, HasSubstr("belongs to device")));
}

TEST_F(UiTextureRegistryTest, RegistrationRefusesAZeroExtent) {
  const gpu::TextureView view = makeView(device_, "thumbnail");

  EXPECT_THAT(
      registry_.registerTexture(
          UiTextureDescriptor{view, gpu::Extent2d{8, 0}, UiTextureAlphaMode::Premultiplied}),
      gpu::IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("must be nonzero")));
}

TEST_F(UiTextureRegistryTest, LookupRefusesANullIdentifier) {
  EXPECT_THAT(registry_.lookup(UiTextureId()),
              gpu::IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("is null")));
}

TEST_F(UiTextureRegistryTest, LookupRefusesASlotThisRegistryNeverHandedOut) {
  EXPECT_THAT(registry_.lookup(UiTextureId::CreateForRegistry(7, 1)),
              gpu::IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("never handed")));
}

TEST_F(UiTextureRegistryTest, LookupRefusesAnIdentifierWhoseSlotWasReused) {
  const gpu::TextureView first = makeView(device_, "first");
  const gpu::TextureView second = makeView(device_, "second");

  const gpu::Result<UiTextureId> stale = registerView(first, UiTextureAlphaMode::Premultiplied);
  ASSERT_THAT(stale, gpu::HasResult());
  ASSERT_THAT(registry_.retire(stale.result()), gpu::IsOk());
  for (uint32_t frame = 0; frame < registry_.retirementFrames(); ++frame) {
    registry_.advanceFrame();
  }

  const gpu::Result<UiTextureId> reused = registerView(second, UiTextureAlphaMode::Premultiplied);
  ASSERT_THAT(reused, gpu::HasResult());
  EXPECT_THAT(reused.result().slotIndex(), Eq(stale.result().slotIndex()));
  EXPECT_THAT(reused.result().generation(), Eq(stale.result().generation() + 1));

  EXPECT_THAT(registry_.lookup(stale.result()),
              gpu::IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("is stale")));
  const gpu::Result<UiTextureBinding> binding = registry_.lookup(reused.result());
  ASSERT_THAT(binding, gpu::HasResult());
  EXPECT_THAT(binding.result(),
              IsBinding(gpu::TextureViewRef(second), 8u, 4u, UiTextureAlphaMode::Premultiplied));
}

TEST_F(UiTextureRegistryTest, LookupRefusesARetiredRegistrationImmediately) {
  const gpu::TextureView view = makeView(device_, "thumbnail");

  const gpu::Result<UiTextureId> id = registerView(view, UiTextureAlphaMode::Premultiplied);
  ASSERT_THAT(id, gpu::HasResult());
  ASSERT_THAT(registry_.retire(id.result()), gpu::IsOk());

  EXPECT_THAT(registry_.lookup(id.result()),
              gpu::IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("was retired")));
  EXPECT_THAT(registry_.liveCount(), Eq(0u));
  EXPECT_THAT(registry_.retiredCount(), Eq(1u));
}

TEST_F(UiTextureRegistryTest, RetireRefusesAnAlreadyRetiredRegistration) {
  const gpu::TextureView view = makeView(device_, "thumbnail");

  const gpu::Result<UiTextureId> id = registerView(view, UiTextureAlphaMode::Premultiplied);
  ASSERT_THAT(id, gpu::HasResult());
  ASSERT_THAT(registry_.retire(id.result()), gpu::IsOk());

  EXPECT_THAT(registry_.retire(id.result()), gpu::IsGpuError(GpuErrorType::InvalidState));
}

TEST_F(UiTextureRegistryTest, RetiredRegistrationHoldsItsSlotUntilItsFramesPass) {
  const gpu::TextureView view = makeView(device_, "thumbnail");

  const gpu::Result<UiTextureId> id = registerView(view, UiTextureAlphaMode::Premultiplied);
  ASSERT_THAT(id, gpu::HasResult());
  ASSERT_THAT(registry_.retire(id.result()), gpu::IsOk());
  ASSERT_THAT(registry_.retirementFrames(), Eq(UiTextureRegistry::kDefaultRetirementFrames));

  for (uint32_t frame = 1; frame < registry_.retirementFrames(); ++frame) {
    EXPECT_THAT(registry_.advanceFrame(), IsEmpty())
        << "registration released after only " << frame << " frames";
    EXPECT_THAT(registry_.retiredCount(), Eq(1u));
  }

  EXPECT_THAT(registry_.advanceFrame(), ElementsAre(id.result()));
  EXPECT_THAT(registry_.retiredCount(), Eq(0u));
  EXPECT_THAT(registry_.liveCount(), Eq(0u));
}

TEST_F(UiTextureRegistryTest, AdvanceFrameLeavesLiveRegistrationsAlone) {
  const gpu::TextureView view = makeView(device_, "thumbnail");

  const gpu::Result<UiTextureId> id = registerView(view, UiTextureAlphaMode::Premultiplied);
  ASSERT_THAT(id, gpu::HasResult());

  for (uint32_t frame = 0; frame < registry_.retirementFrames() * 2; ++frame) {
    EXPECT_THAT(registry_.advanceFrame(), IsEmpty());
  }
  EXPECT_THAT(registry_.lookup(id.result()), gpu::HasResult());
  EXPECT_THAT(registry_.liveCount(), Eq(1u));
}

}  // namespace
}  // namespace donner::editor
