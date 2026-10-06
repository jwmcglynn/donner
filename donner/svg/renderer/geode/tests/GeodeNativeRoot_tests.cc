#include "donner/svg/renderer/geode/GeodeNativeRoot.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
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
  const gpu::Result<GpuBackendKind> resolved = ResolveGpuBackendKind({}, "");
  ASSERT_FALSE(resolved.hasError());
  EXPECT_THAT(resolved.result(), Eq(kNativePlatformDefault));
}

TEST(GeodeNativeRoot, AProcessRequestIsParsedWithoutChangingThePlatformDefault) {
  const gpu::Result<GpuBackendKind> metal = ResolveGpuBackendKind({}, "MeTaL");
  const gpu::Result<GpuBackendKind> vulkan = ResolveGpuBackendKind({}, "VuLkAn");
  ASSERT_FALSE(metal.hasError());
  ASSERT_FALSE(vulkan.hasError());
  EXPECT_THAT(metal.result(), Eq(GpuBackendKind::NativeMetal));
  EXPECT_THAT(vulkan.result(), Eq(GpuBackendKind::NativeVulkan));
  EXPECT_THAT(ResolveGpuBackendKind({}, "").result(), Eq(kNativePlatformDefault));
}

TEST(GeodeNativeRoot, CallerChoiceOutranksEnvironment) {
  GpuRootSelection selection;
  selection.backend = GpuBackendKind::NativeVulkan;
  const gpu::Result<GpuBackendKind> resolved = ResolveGpuBackendKind(selection, "metal");
  ASSERT_FALSE(resolved.hasError());
  EXPECT_THAT(resolved.result(), Eq(GpuBackendKind::NativeVulkan));
}

TEST(GeodeNativeRoot, UnknownBackendRequestFailsWithAcceptedValues) {
  const gpu::Result<GpuBackendKind> resolved = ResolveGpuBackendKind({}, "warp");
  ASSERT_TRUE(resolved.hasError());
  EXPECT_THAT(resolved.error().message, HasSubstr("accepted values"));
}

TEST(GeodeNativeRoot, VulkanPresentationRequiresTheVulkanBackend) {
  GpuRootSelection selection;
  selection.requireVulkanPresentation = true;
  selection.backend = GpuBackendKind::NativeMetal;
  const gpu::Result<GpuBackendKind> resolved = ResolveGpuBackendKind(selection, "");
  ASSERT_TRUE(resolved.hasError());
  EXPECT_THAT(resolved.error().message, HasSubstr("Vulkan presentation requested"));
}

TEST(GeodeNativeRoot, SurfaceExtensionsRequirePresentationSelection) {
  constexpr const char* kExtensions[] = {"VK_KHR_surface"};
  GpuRootSelection selection;
  selection.requiredVulkanInstanceExtensions = kExtensions;
  const gpu::Result<GpuBackendKind> resolved = ResolveGpuBackendKind(selection, "");
  ASSERT_TRUE(resolved.hasError());
  EXPECT_THAT(resolved.error().message, HasSubstr("Vulkan instance extensions"));
}

TEST(GeodeNativeRoot, WgpuNamesNoNativeBackend) {
  const gpu::Result<GpuBackendKind> resolved = ResolveGpuBackendKind({}, "wgpu");
  ASSERT_TRUE(resolved.hasError()) << "\"wgpu\" resolved to " << resolved.result();
  EXPECT_THAT(resolved.error().message, HasSubstr("accepted values: metal, vulkan"));
}

TEST(GeodeNativeRoot, TheExternalBackendIsNeverSelected) {
  const gpu::Result<GpuBackendKind> fromEnvironment = ResolveGpuBackendKind({}, "external");
  ASSERT_TRUE(fromEnvironment.hasError()) << "resolved to " << fromEnvironment.result();
  EXPECT_THAT(fromEnvironment.error().message, HasSubstr("accepted values: metal, vulkan"));

  GpuRootSelection namedByCaller;
  namedByCaller.backend = GpuBackendKind::External;
  const gpu::Result<GpuBackendKind> fromCaller = ResolveGpuBackendKind(namedByCaller, "");
  ASSERT_TRUE(fromCaller.hasError()) << "resolved to " << fromCaller.result();
  EXPECT_THAT(fromCaller.error().type, Eq(gpu::GpuErrorType::Unsupported));

  EXPECT_THAT(SelectGpuRoot(namedByCaller), testing::IsNull());
}

TEST(GeodeNativeRootDeathTest, ExplicitWgpuRequestCannotFallBackToNative) {
  EXPECT_DEATH(
      {
        setenv("DONNER_GPU_BACKEND", "wgpu", 1);
        (void)SelectGpuRoot({});
      },
      "DONNER_GPU_BACKEND=wgpu names no GPU backend");
}

}  // namespace
}  // namespace donner::geode
