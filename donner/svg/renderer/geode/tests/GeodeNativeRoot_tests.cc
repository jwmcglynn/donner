#include "donner/svg/renderer/geode/GeodeNativeRoot.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string_view>

namespace donner::geode {
namespace {

using testing::Eq;
using testing::HasSubstr;

constexpr GpuBackendKind kNativePlatformDefault =
#if defined(__APPLE__)
    GpuBackendKind::NativeMetal;
#elif defined(__linux__)
    GpuBackendKind::NativeVulkan;
#else
#error GeodeNativeRoot_tests requires a native Metal or Vulkan platform
#endif

TEST(GeodeNativeRoot, UnnamedBackendUsesTheNativePlatformDefault) {
  const gpu::Result<GpuBackendKind> resolved = ResolveGpuBackendKind({}, "", std::nullopt);
  ASSERT_FALSE(resolved.hasError());
  EXPECT_THAT(resolved.result(), Eq(kNativePlatformDefault));
  EXPECT_FALSE(BuildDefaultGpuBackendKind().has_value());
}

TEST(GeodeNativeRoot, AProcessRequestIsParsedWithoutChangingThePlatformDefault) {
  const gpu::Result<GpuBackendKind> metal = ResolveGpuBackendKind({}, "MeTaL", std::nullopt);
  const gpu::Result<GpuBackendKind> vulkan = ResolveGpuBackendKind({}, "VuLkAn", std::nullopt);
  ASSERT_FALSE(metal.hasError());
  ASSERT_FALSE(vulkan.hasError());
  EXPECT_THAT(metal.result(), Eq(GpuBackendKind::NativeMetal));
  EXPECT_THAT(vulkan.result(), Eq(GpuBackendKind::NativeVulkan));
  EXPECT_THAT(ResolveGpuBackendKind({}, "", std::nullopt).result(), Eq(kNativePlatformDefault));
}

TEST(GeodeNativeRoot, CallerChoiceOutranksEnvironmentAndBuildDefault) {
  GpuRootSelection selection;
  selection.backend = GpuBackendKind::NativeVulkan;
  const gpu::Result<GpuBackendKind> resolved =
      ResolveGpuBackendKind(selection, "metal", GpuBackendKind::NativeMetal);
  ASSERT_FALSE(resolved.hasError());
  EXPECT_THAT(resolved.result(), Eq(GpuBackendKind::NativeVulkan));
}

TEST(GeodeNativeRoot, BuildDefaultAppliesOnlyWhenCallerAndEnvironmentAreSilent) {
  const gpu::Result<GpuBackendKind> fromBuild =
      ResolveGpuBackendKind({}, "", GpuBackendKind::NativeMetal);
  const gpu::Result<GpuBackendKind> fromEnvironment =
      ResolveGpuBackendKind({}, "vulkan", GpuBackendKind::NativeMetal);
  ASSERT_FALSE(fromBuild.hasError());
  ASSERT_FALSE(fromEnvironment.hasError());
  EXPECT_THAT(fromBuild.result(), Eq(GpuBackendKind::NativeMetal));
  EXPECT_THAT(fromEnvironment.result(), Eq(GpuBackendKind::NativeVulkan));
}

TEST(GeodeNativeRoot, UnknownBackendRequestFailsWithAcceptedValues) {
  const gpu::Result<GpuBackendKind> resolved = ResolveGpuBackendKind({}, "warp", std::nullopt);
  ASSERT_TRUE(resolved.hasError());
  EXPECT_THAT(resolved.error().message, HasSubstr("accepted values"));
}

TEST(GeodeNativeRoot, VulkanPresentationRequiresTheVulkanBackend) {
  GpuRootSelection selection;
  selection.requireVulkanPresentation = true;
  selection.backend = GpuBackendKind::NativeMetal;
  const gpu::Result<GpuBackendKind> resolved = ResolveGpuBackendKind(selection, "", std::nullopt);
  ASSERT_TRUE(resolved.hasError());
  EXPECT_THAT(resolved.error().message, HasSubstr("Vulkan presentation requested"));
}

TEST(GeodeNativeRoot, SurfaceExtensionsRequirePresentationSelection) {
  constexpr const char* kExtensions[] = {"VK_KHR_surface"};
  GpuRootSelection selection;
  selection.requiredVulkanInstanceExtensions = kExtensions;
  const gpu::Result<GpuBackendKind> resolved = ResolveGpuBackendKind(selection, "", std::nullopt);
  ASSERT_TRUE(resolved.hasError());
  EXPECT_THAT(resolved.error().message, HasSubstr("Vulkan instance extensions"));
}

TEST(GeodeNativeRootDeathTest, ExplicitWgpuRequestCannotFallBackToNative) {
  EXPECT_DEATH(
      {
        setenv("DONNER_GPU_BACKEND", "wgpu", 1);
        (void)SelectGpuRoot({});
      },
      "unavailable in this native build");
}

}  // namespace
}  // namespace donner::geode
