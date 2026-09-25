/// @file
/// The Vulkan presentation slice: a surface and its swapchain driven through the runtime's
/// surface hooks.
///
/// The surface is headless, which is the point. Presentation is the one part of this runtime a
/// machine with no display cannot otherwise exercise, and it is also the part whose
/// synchronization is easiest to get wrong, so the whole contract runs over
/// VK_EXT_headless_surface: the same swapchain, the same acquire and present semaphores, the same
/// image layouts, with no compositor at the end of it. A loader without that extension skips,
/// naming it.

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/DeviceObserver.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/RecordingDeviceObserver.h"
#include "donner/gpu/vulkan/VulkanDevice.h"
#include "donner/gpu/vulkan/VulkanLoader.h"
#include "donner/gpu/vulkan/VulkanSwapchain.h"

namespace donner::gpu::vulkan {

namespace {

struct TeardownRecorder {
  std::vector<VkResult> fenceResults;
  size_t fenceWait = 0;
  std::vector<std::string_view> calls;
  VkResult submissionResult = VK_SUCCESS;
  VkResult idleResult = VK_SUCCESS;
  void (*onWait)() = nullptr;
  VkResult presentResult = VK_SUCCESS;
  VkResult acquireResult = VK_SUCCESS;
  std::vector<VkSemaphore> submittedWaits;
  std::vector<VkPipelineStageFlags> submittedStages;
  VkFence submittedFence = VK_NULL_HANDLE;
  VkFence presentedFence = VK_NULL_HANDLE;
};

TeardownRecorder* gTeardownRecorder = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL RecordDeviceWaitIdle(VkDevice) {
  gTeardownRecorder->calls.push_back("wait-idle");
  return gTeardownRecorder->idleResult;
}

VKAPI_ATTR VkResult VKAPI_CALL RecordWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32,
                                                   uint64_t) {
  gTeardownRecorder->calls.push_back("wait-fences");
  if (gTeardownRecorder->onWait) {
    gTeardownRecorder->onWait();
  }
  const size_t index = gTeardownRecorder->fenceWait++;
  return index < gTeardownRecorder->fenceResults.size() ? gTeardownRecorder->fenceResults[index]
                                                        : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL RecordResetFences(VkDevice, uint32_t, const VkFence*) {
  gTeardownRecorder->calls.push_back("reset-fences");
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL RecordFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t,
                                                    const VkCommandBuffer*) {
  gTeardownRecorder->calls.push_back("free-command-buffer");
}

VKAPI_ATTR void VKAPI_CALL RecordDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {
  gTeardownRecorder->calls.push_back("destroy-fence");
}

VKAPI_ATTR void VKAPI_CALL RecordDestroySemaphore(VkDevice, VkSemaphore,
                                                  const VkAllocationCallbacks*) {
  gTeardownRecorder->calls.push_back("destroy-semaphore");
}

VKAPI_ATTR void VKAPI_CALL RecordDestroySwapchain(VkDevice, VkSwapchainKHR,
                                                  const VkAllocationCallbacks*) {
  gTeardownRecorder->calls.push_back("destroy-swapchain");
}

VKAPI_ATTR void VKAPI_CALL RecordDestroySurface(VkInstance, VkSurfaceKHR,
                                                const VkAllocationCallbacks*) {
  gTeardownRecorder->calls.push_back("destroy-surface");
}

VKAPI_ATTR void VKAPI_CALL RecordDestroyCommandPool(VkDevice, VkCommandPool,
                                                    const VkAllocationCallbacks*) {
  gTeardownRecorder->calls.push_back("destroy-command-pool");
}

VKAPI_ATTR void VKAPI_CALL RecordDestroyDevice(VkDevice, const VkAllocationCallbacks*) {
  gTeardownRecorder->calls.push_back("destroy-device");
}

VKAPI_ATTR void VKAPI_CALL RecordDestroyInstance(VkInstance, const VkAllocationCallbacks*) {
  gTeardownRecorder->calls.push_back("destroy-instance");
}

template <typename T>
T FakeHandle(uint64_t value) {
  T result{};
  static_assert(sizeof(result) == sizeof(value));
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL RecordAllocateCommandBuffers(VkDevice,
                                                            const VkCommandBufferAllocateInfo*,
                                                            VkCommandBuffer* commandBuffer) {
  gTeardownRecorder->calls.push_back("allocate-command-buffer");
  *commandBuffer = FakeHandle<VkCommandBuffer>(40);
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL RecordBeginCommandBuffer(VkCommandBuffer,
                                                        const VkCommandBufferBeginInfo*) {
  gTeardownRecorder->calls.push_back("begin-command-buffer");
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL RecordEndCommandBuffer(VkCommandBuffer) {
  gTeardownRecorder->calls.push_back("end-command-buffer");
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL RecordCreateFence(VkDevice, const VkFenceCreateInfo*,
                                                 const VkAllocationCallbacks*, VkFence* fence) {
  gTeardownRecorder->calls.push_back("create-fence");
  *fence = FakeHandle<VkFence>(41);
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL RecordQueueSubmit(VkQueue, uint32_t count, const VkSubmitInfo* infos,
                                                 VkFence fence) {
  gTeardownRecorder->calls.push_back("queue-submit");
  gTeardownRecorder->submittedFence = fence;
  gTeardownRecorder->submittedWaits.clear();
  gTeardownRecorder->submittedStages.clear();
  if (count == 1 && infos[0].waitSemaphoreCount != 0) {
    gTeardownRecorder->submittedWaits.assign(
        infos[0].pWaitSemaphores, infos[0].pWaitSemaphores + infos[0].waitSemaphoreCount);
    gTeardownRecorder->submittedStages.assign(
        infos[0].pWaitDstStageMask, infos[0].pWaitDstStageMask + infos[0].waitSemaphoreCount);
  }
  return gTeardownRecorder->submissionResult;
}

VKAPI_ATTR VkResult VKAPI_CALL RecordQueuePresent(VkQueue, const VkPresentInfoKHR* info) {
  gTeardownRecorder->calls.push_back("queue-present");
  const auto* fenceInfo = static_cast<const VkSwapchainPresentFenceInfoEXT*>(info->pNext);
  if (fenceInfo && fenceInfo->swapchainCount == 1) {
    gTeardownRecorder->presentedFence = fenceInfo->pFences[0];
  }
  return gTeardownRecorder->presentResult;
}

VKAPI_ATTR VkResult VKAPI_CALL RecordAcquireNextImage(VkDevice, VkSwapchainKHR, uint64_t,
                                                      VkSemaphore, VkFence, uint32_t* imageIndex) {
  gTeardownRecorder->calls.push_back("acquire-image");
  *imageIndex = 0;
  return gTeardownRecorder->acquireResult;
}

VKAPI_ATTR void VKAPI_CALL RecordPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags,
                                                 VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                                                 const VkMemoryBarrier*, uint32_t,
                                                 const VkBufferMemoryBarrier*, uint32_t,
                                                 const VkImageMemoryBarrier*) {}

struct CreationRecorder {
  std::vector<const char*> instanceOffers;
  std::vector<const char*> deviceOffers;
  bool maintenanceSupported = true;
  bool instanceCreated = false;
  bool deviceCreated = false;
  bool maintenanceEnabled = false;
  std::vector<std::string> enabledInstanceExtensions;
  std::vector<std::string> enabledDeviceExtensions;
  std::vector<VkStructureType> queriedFeatureTypes;
  std::vector<VkStructureType> enabledFeatureTypes;
  std::vector<std::string_view> cleanup;
};
CreationRecorder* gCreationRecorder = nullptr;

VkResult CopyExtensionOffers(const std::vector<const char*>& offers, uint32_t* count,
                             VkExtensionProperties* properties) {
  if (!properties) {
    *count = static_cast<uint32_t>(offers.size());
    return VK_SUCCESS;
  }
  const size_t copied = std::min<size_t>(*count, offers.size());
  for (size_t index = 0; index < copied; ++index) {
    std::snprintf(properties[index].extensionName, VK_MAX_EXTENSION_NAME_SIZE, "%s", offers[index]);
  }
  *count = static_cast<uint32_t>(copied);
  return copied == offers.size() ? VK_SUCCESS : VK_INCOMPLETE;
}

VKAPI_ATTR VkResult VKAPI_CALL CaptureInstanceVersion(uint32_t* version) {
  *version = VK_API_VERSION_1_1;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CaptureLayers(uint32_t* count, VkLayerProperties*) {
  *count = 0;
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CaptureInstanceExtensions(const char*, uint32_t* count,
                                                         VkExtensionProperties* properties) {
  return CopyExtensionOffers(gCreationRecorder->instanceOffers, count, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL CaptureDeviceExtensions(VkPhysicalDevice, const char*,
                                                       uint32_t* count,
                                                       VkExtensionProperties* properties) {
  return CopyExtensionOffers(gCreationRecorder->deviceOffers, count, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL CaptureCreateInstance(const VkInstanceCreateInfo* info,
                                                     const VkAllocationCallbacks*,
                                                     VkInstance* instance) {
  gCreationRecorder->instanceCreated = true;
  for (uint32_t index = 0; index < info->enabledExtensionCount; ++index) {
    gCreationRecorder->enabledInstanceExtensions.emplace_back(info->ppEnabledExtensionNames[index]);
  }
  *instance = FakeHandle<VkInstance>(80);
  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL CapturePhysicalDevices(VkInstance, uint32_t* count,
                                                      VkPhysicalDevice* devices) {
  *count = 1;
  if (devices) {
    devices[0] = FakeHandle<VkPhysicalDevice>(81);
  }
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL CapturePhysicalProperties(VkPhysicalDevice,
                                                     VkPhysicalDeviceProperties* properties) {
  properties->apiVersion = VK_API_VERSION_1_1;
}

VKAPI_ATTR void VKAPI_CALL CaptureQueueFamilies(VkPhysicalDevice, uint32_t* count,
                                                VkQueueFamilyProperties* properties) {
  *count = 1;
  if (properties) {
    properties[0].queueFlags = VK_QUEUE_GRAPHICS_BIT;
  }
}

VKAPI_ATTR void VKAPI_CALL CaptureFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures* features) {
  features->robustBufferAccess = VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL CaptureFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2* features) {
  for (auto* item = static_cast<VkBaseOutStructure*>(features->pNext); item; item = item->pNext) {
    gCreationRecorder->queriedFeatureTypes.push_back(item->sType);
    if (item->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT) {
      reinterpret_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(item)
          ->swapchainMaintenance1 = gCreationRecorder->maintenanceSupported ? VK_TRUE : VK_FALSE;
    }
  }
}

VKAPI_ATTR VkResult VKAPI_CALL CaptureCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo* info,
                                                   const VkAllocationCallbacks*, VkDevice* device) {
  gCreationRecorder->deviceCreated = true;
  for (uint32_t index = 0; index < info->enabledExtensionCount; ++index) {
    gCreationRecorder->enabledDeviceExtensions.emplace_back(info->ppEnabledExtensionNames[index]);
  }
  for (const auto* item = static_cast<const VkBaseInStructure*>(info->pNext); item;
       item = item->pNext) {
    gCreationRecorder->enabledFeatureTypes.push_back(item->sType);
    if (item->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT) {
      gCreationRecorder->maintenanceEnabled =
          reinterpret_cast<const VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(item)
              ->swapchainMaintenance1 == VK_TRUE;
    }
  }
  *device = FakeHandle<VkDevice>(82);
  return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL CaptureDestroyDevice(VkDevice, const VkAllocationCallbacks*) {
  gCreationRecorder->cleanup.push_back("device");
}

VKAPI_ATTR void VKAPI_CALL CaptureDestroyInstance(VkInstance, const VkAllocationCallbacks*) {
  gCreationRecorder->cleanup.push_back("instance");
}

VulkanApi MakeCreationApi() {
  VulkanApi api;
  api.vkEnumerateInstanceVersion = CaptureInstanceVersion;
  api.vkEnumerateInstanceLayerProperties = CaptureLayers;
  api.vkEnumerateInstanceExtensionProperties = CaptureInstanceExtensions;
  api.vkEnumerateDeviceExtensionProperties = CaptureDeviceExtensions;
  api.vkCreateInstance = CaptureCreateInstance;
  api.vkEnumeratePhysicalDevices = CapturePhysicalDevices;
  api.vkGetPhysicalDeviceProperties = CapturePhysicalProperties;
  api.vkGetPhysicalDeviceQueueFamilyProperties = CaptureQueueFamilies;
  api.vkGetPhysicalDeviceFeatures = CaptureFeatures;
  api.vkGetPhysicalDeviceFeatures2 = CaptureFeatures2;
  api.vkCreateDevice = CaptureCreateDevice;
  api.vkDestroyDevice = CaptureDestroyDevice;
  api.vkDestroyInstance = CaptureDestroyInstance;
  return api;
}

CreationRecorder MaintenanceOffers(bool khr) {
  CreationRecorder recorder;
  recorder.instanceOffers = {VK_KHR_SURFACE_EXTENSION_NAME,
                             VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                             khr ? VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME
                                 : VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
  recorder.deviceOffers = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                           khr ? VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
                               : VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
  return recorder;
}

struct AdmissionRace {
  std::mutex mutex;
  std::condition_variable changed;
  bool teardownReachedWait = false;
  bool releaseTeardown = false;
  size_t queuedCreators = 0;
  std::atomic<size_t> admissions = 0;
};
AdmissionRace* gAdmissionRace = nullptr;

void RecordRaceWait() {
  std::unique_lock lock(gAdmissionRace->mutex);
  gAdmissionRace->teardownReachedWait = true;
  gAdmissionRace->changed.notify_all();
  EXPECT_THAT(gAdmissionRace->changed.wait_for(lock, std::chrono::seconds(2),
                                               [] { return gAdmissionRace->releaseTeardown; }),
              testing::IsTrue());
}

void RecordAdmission(void* context) {
  static_cast<AdmissionRace*>(context)->admissions.fetch_add(1);
}

}  // namespace

class VulkanSwapchainTestAccess {
public:
  static VulkanApi MakeApi() {
    VulkanApi api;
    api.vkDeviceWaitIdle = RecordDeviceWaitIdle;
    api.vkWaitForFences = RecordWaitForFences;
    api.vkResetFences = RecordResetFences;
    api.vkFreeCommandBuffers = RecordFreeCommandBuffers;
    api.vkDestroyFence = RecordDestroyFence;
    api.vkDestroySemaphore = RecordDestroySemaphore;
    api.vkDestroySwapchainKHR = RecordDestroySwapchain;
    api.vkDestroySurfaceKHR = RecordDestroySurface;
    api.vkDestroyCommandPool = RecordDestroyCommandPool;
    api.vkDestroyDevice = RecordDestroyDevice;
    api.vkDestroyInstance = RecordDestroyInstance;
    api.vkAllocateCommandBuffers = RecordAllocateCommandBuffers;
    api.vkBeginCommandBuffer = RecordBeginCommandBuffer;
    api.vkEndCommandBuffer = RecordEndCommandBuffer;
    api.vkCreateFence = RecordCreateFence;
    api.vkQueueSubmit = RecordQueueSubmit;
    api.vkQueuePresentKHR = RecordQueuePresent;
    api.vkAcquireNextImageKHR = RecordAcquireNextImage;
    api.vkCmdPipelineBarrier = RecordPipelineBarrier;
    return api;
  }

  static std::unique_ptr<VulkanSwapchain> MakeSurface(const VulkanApi* api,
                                                      VulkanDevice* owner = nullptr) {
    VulkanSurfaceContext context;
    if (owner) {
      context = owner->surfaceContextForTeardownTest();
    } else {
      context.api = api;
      context.instance = FakeHandle<VkInstance>(1);
      context.device = FakeHandle<VkDevice>(2);
      context.commandPool = FakeHandle<VkCommandPool>(3);
      context.lifetime = std::make_shared<VulkanSurfaceLifetime>();
    }
    auto swapchain = std::unique_ptr<VulkanSwapchain>(
        new VulkanSwapchain(context, FakeHandle<VkSurfaceKHR>(4), true));
    swapchain->swapchain_ = FakeHandle<VkSwapchainKHR>(5);
    swapchain->handoverSemaphores_.push_back(FakeHandle<VkSemaphore>(6));
    swapchain->presentFences_.push_back(FakeHandle<VkFence>(10));
    swapchain->presentFencePending_.push_back(true);
    swapchain->acquireSemaphores_.push_back(FakeHandle<VkSemaphore>(7));
    swapchain->pending_.push_back(
        VulkanSwapchain::PendingSubmission{FakeHandle<VkFence>(8), FakeHandle<VkCommandBuffer>(9)});
    return swapchain;
  }

  static void RunTeardown(TeardownRecorder& recorder) {
    VulkanApi api = MakeApi();
    gTeardownRecorder = &recorder;
    std::unique_ptr<VulkanSwapchain> swapchain = MakeSurface(&api);
    swapchain.reset();
    gTeardownRecorder = nullptr;
  }

  static std::unique_ptr<VulkanSwapchain> MakeAcquireOnlySurface(const VulkanApi* api,
                                                                 VulkanDevice* owner = nullptr) {
    VulkanSurfaceContext context{};
    if (owner) {
      context = owner->surfaceContextForTeardownTest();
    } else {
      context.api = api;
      context.instance = FakeHandle<VkInstance>(1);
      context.device = FakeHandle<VkDevice>(2);
      context.queue = FakeHandle<VkQueue>(3);
      context.commandPool = FakeHandle<VkCommandPool>(4);
      context.lifetime = std::make_shared<VulkanSurfaceLifetime>();
    }
    auto surface = std::unique_ptr<VulkanSwapchain>(
        new VulkanSwapchain(context, FakeHandle<VkSurfaceKHR>(5), true));
    surface->swapchain_ = FakeHandle<VkSwapchainKHR>(6);
    surface->acquireSemaphores_ = {FakeHandle<VkSemaphore>(7)};
    surface->acquireRingFences_ = {VK_NULL_HANDLE};
    surface->pendingAcquireWait_ = surface->acquireSemaphores_[0];
    surface->frameRingSlot_ = 0;
    return surface;
  }

  static std::unique_ptr<VulkanSwapchain> MakePresentableSurface(const VulkanApi* api,
                                                                 VulkanDevice* owner = nullptr) {
    auto surface = MakeAcquireOnlySurface(api, owner);
    surface->images_ = {FakeHandle<VkImage>(8)};
    surface->handoverSemaphores_ = {FakeHandle<VkSemaphore>(9)};
    surface->presentFences_ = {FakeHandle<VkFence>(10)};
    surface->presentFencePending_ = {false};
    surface->imageIndex_ = 0;
    surface->hasFrame_ = true;
    return surface;
  }

  static std::unique_ptr<VulkanSwapchain> MakeAcquirableSurface(const VulkanApi* api,
                                                                VulkanDevice* owner = nullptr) {
    auto surface = MakePresentableSurface(api, owner);
    surface->configuration_ = SurfaceConfiguration{};
    surface->pendingAcquireWait_ = VK_NULL_HANDLE;
    surface->hasFrame_ = false;
    return surface;
  }

  static std::unique_ptr<VulkanSwapchain> MakePartiallyConstructedSurface(const VulkanApi* api) {
    auto surface = MakeAcquireOnlySurface(api);
    surface->pendingAcquireWait_ = VK_NULL_HANDLE;
    surface->swapchain_ = VK_NULL_HANDLE;
    surface->acquireSemaphores_ = {VK_NULL_HANDLE, FakeHandle<VkSemaphore>(7)};
    surface->handoverSemaphores_ = {FakeHandle<VkSemaphore>(9), VK_NULL_HANDLE};
    surface->presentFences_ = {VK_NULL_HANDLE, FakeHandle<VkFence>(10)};
    surface->presentFencePending_ = {false, false};
    return surface;
  }

  static Status Prepare(VulkanSwapchain& surface) { return surface.prepareForDestruction(); }
  static Result<SurfaceStatus> Present(VulkanSwapchain& surface) {
    return surface.present(
        TextureSyncState{VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0});
  }
  static bool PresentFencePending(const VulkanSwapchain& surface) {
    return surface.presentFencePending_[0];
  }
  static bool PreparationBlocked(const VulkanSwapchain& surface) {
    return surface.preparationBlocked_;
  }
  static size_t PendingCount(const VulkanSwapchain& surface) { return surface.pending_.size(); }

  static Status Submit(VulkanDevice& device) {
    const SubmittedCommandBuffer commandBuffer{0, {}};
    return device.onSubmit(1, {&commandBuffer, 1});
  }

  static void RetireSurface(VulkanDevice& device, uint32_t index) {
    device.onDestroySurface(index);
  }

  static void RunOwnerTeardown(TeardownRecorder& recorder, size_t surfaceCount = 1) {
    VulkanApi api = MakeApi();
    gTeardownRecorder = &recorder;
    std::unique_ptr<VulkanDevice> device = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
    ASSERT_THAT(device, testing::NotNull());
    for (size_t i = 0; i < surfaceCount; ++i) {
      device->attachSurfaceForTeardownTest(MakeSurface(&api, device.get()));
    }
    device.reset();
    gTeardownRecorder = nullptr;
  }
};

}  // namespace donner::gpu::vulkan

namespace donner::gpu::vulkan::tests {
namespace {

using testing::Contains;
using testing::ElementsAreArray;
using testing::HasSubstr;
using testing::Not;

/// Rows of a texel copy are 256-byte aligned, so the extents below are multiples of 64 texels
/// wide and read back without a padded staging step.
constexpr uint32_t kSurfaceWidth = 64;
constexpr uint32_t kSurfaceHeight = 48;

/// Opaque red, premultiplied RGBA. Every channel is 0 or 1 so the expected bytes are exact.
constexpr std::array<double, 4> kRedClear = {1.0, 0.0, 0.0, 1.0};

class VulkanSurfaceTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = VulkanDevice::CreateWithPresentationSupport();
    if (!device_) {
      // A conforming Vulkan 1.1 driver need not offer headless surfaces, so a runner that runs
      // every other Vulkan target may legitimately not present. Distinguish the two: no baseline
      // device on a required runner is a failure, a driver without the extension is a skip that
      // names what was missing.
      const char* required = std::getenv("DONNER_REQUIRE_VULKAN");
      const bool requireVulkan = required != nullptr && std::string_view(required) == "1";
      if (requireVulkan && VulkanDevice::Create() == nullptr) {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 but no Vulkan 1.1 device could be created";
      }
      GTEST_SKIP() << "This Vulkan loader or driver does not offer VK_KHR_surface with "
                      "VK_EXT_headless_surface and VK_KHR_swapchain, which presentation without a "
                      "window needs";
    }
  }

  /// Unwraps an RHI result, failing the test on error.
  template <typename T>
  T unwrap(Result<T>&& result, const char* what) {
    if (result.hasError()) {
      ADD_FAILURE() << what << " failed: " << result.error();
    }
    return std::move(result).result();
  }

  static SurfaceDescriptor headlessDescriptor() {
    SurfaceDescriptor descriptor;
    descriptor.label = "presentation";
    descriptor.native.kind = NativeSurfaceKind::Headless;
    return descriptor;
  }

  /// A configuration this surface accepts, at \p width by \p height.
  SurfaceConfiguration configuration(const Surface& surface, uint32_t width = kSurfaceWidth,
                                     uint32_t height = kSurfaceHeight) {
    const SurfaceCapabilities capabilities =
        unwrap(device_->surfaceCapabilities(surface), "surfaceCapabilities");
    EXPECT_THAT(capabilities.formats, Not(testing::IsEmpty()));
    EXPECT_THAT(capabilities.alphaModes, Not(testing::IsEmpty()));
    return SurfaceConfiguration{
        capabilities.formats.front(), TextureUsage::RenderAttachment | TextureUsage::CopySrc,
        Extent2d{width, height}, PresentMode::Fifo, capabilities.alphaModes.front()};
  }

  Surface configuredSurface(uint32_t width = kSurfaceWidth, uint32_t height = kSurfaceHeight) {
    Surface surface = unwrap(device_->createSurface(headlessDescriptor()), "createSurface");
    EXPECT_THAT(device_->configureSurface(surface, configuration(surface, width, height)), IsOk());
    return surface;
  }

  /// Clears \p frame to \p clearColor, optionally copying it into \p readback, and returns the
  /// submission serial.
  uint64_t renderClear(const Texture& frame, std::array<double, 4> clearColor,
                       const Buffer* readback, uint32_t width, uint32_t height) {
    TextureView view = unwrap(device_->createTextureView(frame, TextureViewDescriptor{"frame"}),
                              "createTextureView");
    std::unique_ptr<CommandEncoder> encoder =
        unwrap(device_->createCommandEncoder(), "createCommandEncoder");

    RenderPassDescriptor pass;
    pass.label = "present";
    pass.colorAttachments.push_back(
        RenderPassColorAttachment{view, LoadOp::Clear, StoreOp::Store, clearColor});
    RenderPassEncoder* renderPass = unwrap(encoder->beginRenderPass(pass), "beginRenderPass");
    EXPECT_THAT(renderPass->end(), IsOk());

    if (readback != nullptr) {
      EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{frame}, *readback,
                                               TexelCopyBufferLayout{0, width * 4u, height},
                                               Extent2d{width, height}),
                  IsOk());
    }

    CommandBuffer commands = unwrap(encoder->finish(), "finish");
    return unwrap(device_->submit(std::move(commands)), "submit");
  }

  /// The texels of a frame cleared to \p clearColor, read back once its submission completed.
  std::vector<uint8_t> renderClearAndReadBack(const Texture& frame,
                                              std::array<double, 4> clearColor, uint32_t width,
                                              uint32_t height) {
    Buffer readback =
        unwrap(device_->createBuffer(BufferDescriptor{"readback", uint64_t{width} * height * 4u,
                                                      BufferUsage::CopyDst | BufferUsage::MapRead}),
               "createBuffer");
    const uint64_t serial = renderClear(frame, clearColor, &readback, width, height);
    EXPECT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
        << "Submission did not complete cleanly: " << device_->lastErrorForTest();
    return unwrap(device_->readBackBuffer(readback), "readBackBuffer");
  }

  /// The texel at (\p x, \p y) of a readback of a \p width-wide frame, in \p format's channel
  /// order normalised to RGBA so an expectation does not have to know which the surface chose.
  static std::array<uint8_t, 4> rgbaTexelAt(const std::vector<uint8_t>& pixels,
                                            TextureFormat format, uint32_t width, uint32_t x,
                                            uint32_t y) {
    const size_t offset = (size_t{y} * width + x) * 4u;
    if (offset + 4u > pixels.size()) {
      return {};
    }
    if (format == TextureFormat::BGRA8Unorm) {
      return {pixels[offset + 2], pixels[offset + 1], pixels[offset], pixels[offset + 3]};
    }
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
  }

  std::unique_ptr<VulkanDevice> device_;
};

TEST_F(VulkanSurfaceTest, ADeviceCreatedWithoutPresentationSupportRefusesEverySurface) {
  // Swapchain support has to be requested before any surface exists, so a device that did not ask
  // for it can never present. Refusing here, where the surface is still the subject, is the whole
  // reason presentation is a separate creation entry point.
  std::unique_ptr<VulkanDevice> headless = VulkanDevice::Create();
  ASSERT_NE(headless, nullptr);
  EXPECT_FALSE(headless->supportsPresentation());

  EXPECT_THAT(headless->createSurface(headlessDescriptor()),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("presentation support")));
  EXPECT_TRUE(device_->supportsPresentation());
}

TEST(VulkanPresentationCreationTest, RefusesAnUnavailableRequiredInstanceExtension) {
  static constexpr const char* kUnavailable[] = {"VK_DONNER_extension_that_does_not_exist"};
  EXPECT_THAT(VulkanDevice::CreateWithPresentationSupport(kUnavailable), testing::IsNull());
}

TEST_F(VulkanSurfaceTest, EnablesAnEmbedderRequiredInstanceExtension) {
  static constexpr const char* kRequired[] = {VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
  std::unique_ptr<VulkanDevice> required = VulkanDevice::CreateWithPresentationSupport(kRequired);
  ASSERT_THAT(required, testing::NotNull());
  EXPECT_THAT(required->createSurface(headlessDescriptor()), IsOk());
}

TEST(VulkanPresentationCreationTest, ForwardsARequiredPlatformCompanionExtension) {
  static constexpr const char* kXcbSurfaceExtension = "VK_KHR_xcb_surface";
  static constexpr const char* kOffered[] = {
      VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
      VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME,
      kXcbSurfaceExtension};
  static constexpr const char* kRequired[] = {kXcbSurfaceExtension};
  EXPECT_THAT(SelectPresentationExtensionsForTest(kOffered, kRequired),
              testing::ElementsAre(testing::StrEq(VK_KHR_SURFACE_EXTENSION_NAME),
                                   testing::StrEq(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME),
                                   testing::StrEq(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME),
                                   testing::StrEq(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME),
                                   testing::StrEq(kXcbSurfaceExtension)));
}

class MaintenanceDependencyTest : public testing::TestWithParam<bool> {};

TEST_P(MaintenanceDependencyTest, CreatesWithMatchingInstanceDeviceExtensionsAndFeature) {
  CreationRecorder recorder = MaintenanceOffers(GetParam());
  const char* instanceAlias = recorder.instanceOffers.back();
  const char* deviceAlias = recorder.deviceOffers.back();
  recorder.instanceOffers.push_back("VK_KHR_xcb_surface");
  gCreationRecorder = &recorder;
  const char* required[] = {"VK_KHR_xcb_surface", VK_KHR_SURFACE_EXTENSION_NAME, instanceAlias,
                            "VK_KHR_xcb_surface"};
  EXPECT_THAT(
      VulkanDevice::CreateNativeObjectsForPresentationTest(MakeCreationApi(), true, required),
      testing::IsTrue());
  EXPECT_THAT(recorder.enabledInstanceExtensions,
              testing::ElementsAre(VK_KHR_SURFACE_EXTENSION_NAME,
                                   VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, instanceAlias,
                                   "VK_KHR_xcb_surface"));
  EXPECT_THAT(recorder.enabledDeviceExtensions,
              testing::ElementsAre(VK_KHR_SWAPCHAIN_EXTENSION_NAME, deviceAlias));
  EXPECT_THAT(
      recorder.queriedFeatureTypes,
      testing::ElementsAre(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT));
  EXPECT_THAT(
      recorder.enabledFeatureTypes,
      testing::ElementsAre(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT));
  EXPECT_THAT(recorder.maintenanceEnabled, testing::IsTrue());
  EXPECT_THAT(recorder.cleanup, testing::ElementsAre("device", "instance"));
  gCreationRecorder = nullptr;
}

TEST_P(MaintenanceDependencyTest, MissingInstanceDependencyRefusesBeforeNativeCreation) {
  for (size_t missing = 0; missing < 3; ++missing) {
    CreationRecorder recorder = MaintenanceOffers(GetParam());
    SCOPED_TRACE(recorder.instanceOffers[missing]);
    recorder.instanceOffers.erase(recorder.instanceOffers.begin() + missing);
    gCreationRecorder = &recorder;
    EXPECT_THAT(VulkanDevice::CreateNativeObjectsForPresentationTest(MakeCreationApi(), true),
                testing::IsFalse());
    EXPECT_THAT(recorder.instanceCreated, testing::IsFalse());
    EXPECT_THAT(recorder.deviceCreated, testing::IsFalse());
    EXPECT_THAT(recorder.cleanup, testing::IsEmpty());
  }
  gCreationRecorder = nullptr;
}

TEST_P(MaintenanceDependencyTest, MissingDeviceDependencyRefusesBeforeLogicalDeviceCreation) {
  for (size_t missing = 0; missing < 2; ++missing) {
    CreationRecorder recorder = MaintenanceOffers(GetParam());
    SCOPED_TRACE(recorder.deviceOffers[missing]);
    recorder.deviceOffers.erase(recorder.deviceOffers.begin() + missing);
    gCreationRecorder = &recorder;
    EXPECT_THAT(VulkanDevice::CreateNativeObjectsForPresentationTest(MakeCreationApi(), true),
                testing::IsFalse());
    EXPECT_THAT(recorder.instanceCreated, testing::IsTrue());
    EXPECT_THAT(recorder.deviceCreated, testing::IsFalse());
    EXPECT_THAT(recorder.cleanup, testing::ElementsAre("instance"));
  }
  gCreationRecorder = nullptr;
}

TEST_P(MaintenanceDependencyTest, MissingFeatureRefusesBeforeLogicalDeviceCreation) {
  CreationRecorder recorder = MaintenanceOffers(GetParam());
  recorder.maintenanceSupported = false;
  gCreationRecorder = &recorder;
  EXPECT_THAT(VulkanDevice::CreateNativeObjectsForPresentationTest(MakeCreationApi(), true),
              testing::IsFalse());
  EXPECT_THAT(
      recorder.queriedFeatureTypes,
      testing::ElementsAre(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT));
  EXPECT_THAT(recorder.deviceCreated, testing::IsFalse());
  EXPECT_THAT(recorder.cleanup, testing::ElementsAre("instance"));
  gCreationRecorder = nullptr;
}

TEST_P(MaintenanceDependencyTest, MismatchedAliasesDoNotSatisfyDependencies) {
  CreationRecorder recorder = MaintenanceOffers(GetParam());
  recorder.deviceOffers.back() = GetParam() ? VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
                                            : VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
  gCreationRecorder = &recorder;
  EXPECT_THAT(VulkanDevice::CreateNativeObjectsForPresentationTest(MakeCreationApi(), true),
              testing::IsFalse());
  EXPECT_THAT(recorder.deviceCreated, testing::IsFalse());
  EXPECT_THAT(recorder.cleanup, testing::ElementsAre("instance"));
  gCreationRecorder = nullptr;
}

TEST_P(MaintenanceDependencyTest, InvalidRequiredPlatformExtensionRefusesBeforeNativeCreation) {
  for (const char* required :
       {static_cast<const char*>(nullptr), "VK_DONNER_unavailable_surface"}) {
    CreationRecorder recorder = MaintenanceOffers(GetParam());
    gCreationRecorder = &recorder;
    const std::array<const char*, 1> requiredExtensions = {required};
    EXPECT_THAT(VulkanDevice::CreateNativeObjectsForPresentationTest(MakeCreationApi(), true,
                                                                     requiredExtensions),
                testing::IsFalse());
    EXPECT_THAT(recorder.instanceCreated, testing::IsFalse());
    EXPECT_THAT(recorder.deviceCreated, testing::IsFalse());
    EXPECT_THAT(recorder.cleanup, testing::IsEmpty());
  }
  gCreationRecorder = nullptr;
}

INSTANTIATE_TEST_SUITE_P(KhrAndExt, MaintenanceDependencyTest, testing::Bool());

TEST(VulkanPresentationCreationTest, EnablesBothOfferedInstanceAliasesBeforeChoosingDeviceAlias) {
  for (const bool deviceOffersKhr : {false, true}) {
    CreationRecorder recorder = MaintenanceOffers(deviceOffersKhr);
    recorder.instanceOffers = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
    if (deviceOffersKhr) {
      recorder.deviceOffers.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    }
    gCreationRecorder = &recorder;
    EXPECT_THAT(VulkanDevice::CreateNativeObjectsForPresentationTest(MakeCreationApi(), true),
                testing::IsTrue());
    EXPECT_THAT(recorder.enabledInstanceExtensions,
                testing::ElementsAre(VK_KHR_SURFACE_EXTENSION_NAME,
                                     VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
                                     VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
                                     VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME));
    EXPECT_THAT(
        recorder.enabledDeviceExtensions,
        testing::ElementsAre(VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                             deviceOffersKhr ? VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
                                             : VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME));
  }
  gCreationRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest,
     NonPresentationCreationNeedsNoMaintenanceExtensionsOrFeatures) {
  CreationRecorder recorder;
  gCreationRecorder = &recorder;
  EXPECT_THAT(VulkanDevice::CreateNativeObjectsForPresentationTest(MakeCreationApi(), false),
              testing::IsTrue());
  EXPECT_THAT(recorder.enabledInstanceExtensions, testing::IsEmpty());
  EXPECT_THAT(recorder.enabledDeviceExtensions, testing::IsEmpty());
  EXPECT_THAT(recorder.queriedFeatureTypes, testing::IsEmpty());
  EXPECT_THAT(recorder.enabledFeatureTypes, testing::IsEmpty());
  EXPECT_THAT(recorder.cleanup, testing::ElementsAre("device", "instance"));
  gCreationRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, ExpandsTheUndefinedSurfaceFormatWildcard) {
  const std::vector<VkSurfaceFormatKHR> wildcard = {
      {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
  EXPECT_THAT(RuntimeSurfaceFormatsForTest(wildcard),
              testing::UnorderedElementsAre(TextureFormat::BGRA8Unorm, TextureFormat::RGBA8Unorm));
}

TEST(VulkanPresentationCreationTest, TeardownWaitsForPresentAndSubmissionBeforeDestroying) {
  TeardownRecorder recorder;
  VulkanSwapchainTestAccess::RunTeardown(recorder);
  EXPECT_THAT(recorder.calls,
              testing::ElementsAre("wait-fences", "wait-fences", "free-command-buffer",
                                   "destroy-fence", "destroy-semaphore", "destroy-fence",
                                   "destroy-semaphore", "destroy-swapchain", "destroy-surface"));
}

TEST(VulkanPresentationCreationTest, TeardownRetainsEverythingWhilePresentMayStillBePending) {
  TeardownRecorder recorder;
  recorder.fenceResults = {VK_TIMEOUT};
  VulkanSwapchainTestAccess::RunTeardown(recorder);
  EXPECT_THAT(recorder.calls, testing::ElementsAre("wait-fences"));
}

TEST(VulkanPresentationCreationTest, TeardownRetainsEverythingWhenFenceCompletionFails) {
  TeardownRecorder recorder;
  recorder.fenceResults = {VK_SUCCESS, VK_TIMEOUT};
  VulkanSwapchainTestAccess::RunTeardown(recorder);
  EXPECT_THAT(recorder.calls, testing::ElementsAre("wait-fences", "wait-fences"));
}

TEST(VulkanPresentationCreationTest, OwnerRetainsEveryNativePrerequisiteAfterPresentTimeout) {
  ASSERT_EXIT(([&] {
                TeardownRecorder recorder;
                recorder.fenceResults = {VK_TIMEOUT};
                VulkanSwapchainTestAccess::RunOwnerTeardown(recorder);
                EXPECT_THAT(recorder.calls, testing::ElementsAre("wait-fences"));
                std::exit(testing::Test::HasFailure() ? 1 : 0);
              }()),
              testing::ExitedWithCode(0), "");
}

TEST(VulkanPresentationCreationTest, LaterSurfaceFailureRetainsAnAlreadyPreparedSibling) {
  ASSERT_EXIT(([&] {
                TeardownRecorder recorder;
                recorder.fenceResults = {VK_SUCCESS, VK_SUCCESS, VK_TIMEOUT};
                VulkanSwapchainTestAccess::RunOwnerTeardown(recorder, 2);
                EXPECT_THAT(recorder.calls,
                            testing::ElementsAre("wait-fences", "wait-fences", "wait-fences"));
                std::exit(testing::Test::HasFailure() ? 1 : 0);
              }()),
              testing::ExitedWithCode(0), "");
}

TEST(VulkanPresentationCreationTest, OwnerProvesAllWorkBeforeNativeDestruction) {
  TeardownRecorder recorder;
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto device = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
  ASSERT_THAT(device, testing::NotNull());
  device->attachWorkForTeardownTest(false, 11, 12);
  device->attachWorkForTeardownTest(true, 13, 14);
  device->attachSurfaceForTeardownTest(VulkanSwapchainTestAccess::MakeSurface(&api, device.get()));
  device.reset();
  EXPECT_THAT(recorder.calls,
              testing::ElementsAre("wait-fences", "wait-fences", "wait-fences", "wait-fences",
                                   "free-command-buffer", "destroy-fence", "destroy-fence",
                                   "free-command-buffer", "free-command-buffer", "destroy-fence",
                                   "destroy-semaphore", "destroy-fence", "destroy-semaphore",
                                   "destroy-swapchain", "destroy-surface", "destroy-command-pool",
                                   "destroy-device", "destroy-instance"));
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, PendingOrdinarySubmissionRetainsTheWholeOwnerWithoutIdle) {
  ASSERT_EXIT(([&] {
                TeardownRecorder recorder;
                recorder.fenceResults = {VK_TIMEOUT};
                VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
                gTeardownRecorder = &recorder;
                auto device = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
                device->attachWorkForTeardownTest(false, 11, 12);
                device->attachSurfaceForTeardownTest(
                    VulkanSwapchainTestAccess::MakeSurface(&api, device.get()));
                device.reset();
                EXPECT_THAT(recorder.calls, testing::ElementsAre("wait-fences"));
                std::exit(testing::Test::HasFailure() ? 1 : 0);
              }()),
              testing::ExitedWithCode(0), "");
}

TEST(VulkanPresentationCreationTest, PendingUploadRetainsTheWholeOwnerWithoutIdle) {
  ASSERT_EXIT(([&] {
                TeardownRecorder recorder;
                recorder.fenceResults = {VK_TIMEOUT};
                VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
                gTeardownRecorder = &recorder;
                auto device = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
                device->attachWorkForTeardownTest(true, 13, 14);
                device.reset();
                EXPECT_THAT(recorder.calls, testing::ElementsAre("wait-fences"));
                std::exit(testing::Test::HasFailure() ? 1 : 0);
              }()),
              testing::ExitedWithCode(0), "");
}

TEST(VulkanPresentationCreationTest, QuarantineIrreversiblyRefusesEveryDeviceFactory) {
  ASSERT_EXIT(([&] {
                TeardownRecorder recorder;
                recorder.fenceResults = {VK_TIMEOUT};
                VulkanSwapchainTestAccess::RunOwnerTeardown(recorder);
                VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
                EXPECT_THAT(VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3), testing::IsNull());
                EXPECT_THAT(VulkanDevice::Create(), testing::IsNull());
                EXPECT_THAT(VulkanDevice::CreateWithPresentationSupport(), testing::IsNull());
                EXPECT_THAT(VulkanDevice::CreateWithTimelineSemaphoreForTest(), testing::IsNull());
                EXPECT_THAT(VulkanDevice::CreateNativeObjectsForPresentationTest(VulkanApi{}, true),
                            testing::IsFalse());
                EXPECT_THAT(recorder.calls, testing::ElementsAre("wait-fences"));
                std::exit(testing::Test::HasFailure() ? 1 : 0);
              }()),
              testing::ExitedWithCode(0), "");
}

TEST(VulkanPresentationCreationTest, QueuedFactoriesAreRefusedAfterShutdownFailure) {
  ASSERT_EXIT(([&] {
                TeardownRecorder recorder;
                recorder.fenceResults = {VK_TIMEOUT};
                recorder.onWait = RecordRaceWait;
                AdmissionRace race;
                gAdmissionRace = &race;
                gTeardownRecorder = &recorder;
                VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
                constexpr size_t kCreatorCount = 4;
                std::array<std::unique_ptr<VulkanDevice>, kCreatorCount> admitted;
                std::thread owner([&] {
                  auto device = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
                  device->attachWorkForTeardownTest(false, 11, 12);
                  device.reset();
                });
                {
                  std::unique_lock lock(race.mutex);
                  EXPECT_THAT(race.changed.wait_for(lock, std::chrono::seconds(2),
                                                    [&] { return race.teardownReachedWait; }),
                              testing::IsTrue());
                }
                std::array<std::thread, kCreatorCount> creators;
                for (size_t index = 0; index < kCreatorCount; ++index) {
                  creators[index] = std::thread([&, index] {
                    {
                      const std::lock_guard lock(race.mutex);
                      ++race.queuedCreators;
                      race.changed.notify_all();
                    }
                    admitted[index] =
                        VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3, RecordAdmission, &race);
                  });
                }
                {
                  std::unique_lock lock(race.mutex);
                  EXPECT_THAT(
                      race.changed.wait_for(lock, std::chrono::seconds(2),
                                            [&] { return race.queuedCreators == kCreatorCount; }),
                      testing::IsTrue());
                  race.releaseTeardown = true;
                  race.changed.notify_all();
                }
                for (std::thread& creator : creators) {
                  creator.join();
                }
                owner.join();
                EXPECT_THAT(admitted, testing::Each(testing::IsNull()));
                EXPECT_EQ(race.admissions.load(), 0u);
                EXPECT_THAT(recorder.calls, testing::ElementsAre("wait-fences"));
                EXPECT_THAT(VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3), testing::IsNull());
                std::exit(testing::Test::HasFailure() ? 1 : 0);
              }()),
              testing::ExitedWithCode(0), "");
}

TEST(VulkanPresentationCreationTest, RetainedSurfaceChainIsPreparedAndDestroyedIteratively) {
  TeardownRecorder recorder;
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto device = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
  constexpr size_t kCount = 4096;
  recorder.fenceResults.assign(kCount, VK_TIMEOUT);
  for (size_t index = 0; index < kCount; ++index) {
    device->attachSurfaceForTeardownTest(
        VulkanSwapchainTestAccess::MakeSurface(&api, device.get()));
    VulkanSwapchainTestAccess::RetireSurface(*device, static_cast<uint32_t>(index));
  }
  EXPECT_THAT(recorder.calls, testing::Each("wait-fences"));
  recorder.calls.clear();
  recorder.fenceResults.clear();
  device.reset();
  ASSERT_GE(recorder.calls.size(), 2 * kCount);
  EXPECT_THAT(std::span(recorder.calls).first(2 * kCount), testing::Each("wait-fences"));
  EXPECT_EQ(std::ranges::count(recorder.calls, "destroy-surface"), kCount);
  EXPECT_THAT(std::span(recorder.calls).last(3),
              testing::ElementsAre("destroy-command-pool", "destroy-device", "destroy-instance"));
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, SubmissionOomFreesOnlyUnsubmittedObjects) {
  for (const VkResult result : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
    TeardownRecorder recorder;
    recorder.submissionResult = result;
    VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
    gTeardownRecorder = &recorder;
    auto device = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
    EXPECT_THAT(VulkanSwapchainTestAccess::Submit(*device), IsGpuError(GpuErrorType::InvalidState));
    EXPECT_THAT(recorder.calls,
                testing::ElementsAre("allocate-command-buffer", "begin-command-buffer",
                                     "end-command-buffer", "create-fence", "queue-submit",
                                     "destroy-fence", "free-command-buffer"));
    recorder.submissionResult = VK_SUCCESS;
    EXPECT_THAT(VulkanSwapchainTestAccess::Submit(*device), IsOk());
    device.reset();
  }
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, AmbiguousSubmissionRetainsObjectsAndRefusesReuse) {
  for (const VkResult result : {VK_ERROR_UNKNOWN, VK_ERROR_VALIDATION_FAILED_EXT}) {
    ASSERT_EXIT(([&] {
                  TeardownRecorder recorder;
                  recorder.submissionResult = result;
                  recorder.fenceResults = {VK_TIMEOUT};
                  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
                  gTeardownRecorder = &recorder;
                  auto device = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
                  EXPECT_THAT(VulkanSwapchainTestAccess::Submit(*device),
                              IsGpuError(GpuErrorType::InvalidState));
                  EXPECT_THAT(VulkanSwapchainTestAccess::Submit(*device),
                              IsGpuError(GpuErrorType::InvalidState));
                  device.reset();
                  EXPECT_THAT(recorder.calls,
                              testing::ElementsAre("allocate-command-buffer",
                                                   "begin-command-buffer", "end-command-buffer",
                                                   "create-fence", "queue-submit", "wait-fences"));
                  std::exit(testing::Test::HasFailure() ? 1 : 0);
                }()),
                testing::ExitedWithCode(0), "");
  }
}

TEST(VulkanPresentationCreationTest, SubmitDeviceLossDrainsBeforeFreeingObjects) {
  TeardownRecorder recorder;
  recorder.submissionResult = VK_ERROR_DEVICE_LOST;
  recorder.idleResult = VK_ERROR_DEVICE_LOST;
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto device = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
  EXPECT_THAT(VulkanSwapchainTestAccess::Submit(*device), IsGpuError(GpuErrorType::InvalidState));
  EXPECT_THAT(recorder.calls,
              testing::ElementsAre("allocate-command-buffer", "begin-command-buffer",
                                   "end-command-buffer", "create-fence", "queue-submit",
                                   "wait-idle", "destroy-fence", "free-command-buffer"));
  device.reset();
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, AcquireOnlyPrepareConsumesWaitBeforeDestroying) {
  TeardownRecorder recorder;
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto surface = VulkanSwapchainTestAccess::MakeAcquireOnlySurface(&api);

  EXPECT_THAT(VulkanSwapchainTestAccess::Prepare(*surface), IsOk());
  EXPECT_THAT(recorder.submittedWaits, testing::ElementsAre(FakeHandle<VkSemaphore>(7)));
  EXPECT_THAT(recorder.submittedStages, testing::ElementsAre(kAcquireWaitStage));
  EXPECT_THAT(recorder.calls, Not(Contains("destroy-semaphore")));
  EXPECT_THAT(recorder.calls, Not(Contains("destroy-swapchain")));

  surface.reset();
  EXPECT_THAT(recorder.calls, Contains("destroy-semaphore"));
  EXPECT_THAT(recorder.calls, Contains("destroy-swapchain"));
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, SubmitOomRestoresAcquireWaitForOneRetry) {
  TeardownRecorder recorder;
  recorder.submissionResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto surface = VulkanSwapchainTestAccess::MakeAcquireOnlySurface(&api);

  EXPECT_THAT(VulkanSwapchainTestAccess::Prepare(*surface), IsGpuError(GpuErrorType::InvalidState));
  EXPECT_TRUE(surface->owesAcquireWaitForTest());
  EXPECT_THAT(recorder.calls, Contains("free-command-buffer"));
  EXPECT_THAT(recorder.calls, Contains("destroy-fence"));

  recorder.submissionResult = VK_SUCCESS;
  EXPECT_THAT(VulkanSwapchainTestAccess::Prepare(*surface), IsOk());
  EXPECT_FALSE(surface->owesAcquireWaitForTest());
  EXPECT_EQ(std::count(recorder.calls.begin(), recorder.calls.end(), "queue-submit"), 2);
  surface.reset();
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, AmbiguousSubmitRetainsEveryNativeObject) {
  TeardownRecorder recorder;
  recorder.submissionResult = VK_ERROR_VALIDATION_FAILED_EXT;
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto surface = VulkanSwapchainTestAccess::MakeAcquireOnlySurface(&api);

  EXPECT_THAT(VulkanSwapchainTestAccess::Prepare(*surface), IsGpuError(GpuErrorType::InvalidState));
  EXPECT_TRUE(VulkanSwapchainTestAccess::PreparationBlocked(*surface));
  EXPECT_EQ(VulkanSwapchainTestAccess::PendingCount(*surface), 1u);
  recorder.calls.clear();
  surface.reset();
  EXPECT_THAT(recorder.calls, Not(Contains("free-command-buffer")));
  EXPECT_THAT(recorder.calls, Not(Contains("destroy-fence")));
  EXPECT_THAT(recorder.calls, Not(Contains("destroy-semaphore")));
  EXPECT_THAT(recorder.calls, Not(Contains("destroy-swapchain")));
  EXPECT_THAT(recorder.calls, Not(Contains("destroy-surface")));
  gTeardownRecorder = nullptr;
}

class PresentResultOwnershipTest : public testing::TestWithParam<VkResult> {};
TEST_P(PresentResultOwnershipTest, AssociatesMaintenanceFenceForEveryPossiblyEnqueuedResult) {
  TeardownRecorder recorder;
  recorder.presentResult = GetParam();
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto surface = VulkanSwapchainTestAccess::MakePresentableSurface(&api);

  (void)VulkanSwapchainTestAccess::Present(*surface);
  EXPECT_TRUE(VulkanSwapchainTestAccess::PresentFencePending(*surface));
  EXPECT_EQ(recorder.presentedFence, FakeHandle<VkFence>(10));
  surface.reset();
  EXPECT_THAT(recorder.calls, Contains("destroy-swapchain"));
  gTeardownRecorder = nullptr;
}
INSTANTIATE_TEST_SUITE_P(EnqueuedResults, PresentResultOwnershipTest,
                         testing::Values(VK_SUCCESS, VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR,
                                         VK_ERROR_SURFACE_LOST_KHR,
                                         VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT,
                                         VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT,
                                         VK_ERROR_DEVICE_LOST));

class PresentOomOwnershipTest : public testing::TestWithParam<VkResult> {};
TEST_P(PresentOomOwnershipTest, LeavesMaintenanceFenceUnassociated) {
  TeardownRecorder recorder;
  recorder.presentResult = GetParam();
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto surface = VulkanSwapchainTestAccess::MakePresentableSurface(&api);

  EXPECT_THAT(VulkanSwapchainTestAccess::Present(*surface), IsGpuError(GpuErrorType::InvalidState));
  EXPECT_FALSE(VulkanSwapchainTestAccess::PresentFencePending(*surface));
  // The successful handover remains tracked until its fence proves completion.
  EXPECT_EQ(VulkanSwapchainTestAccess::PendingCount(*surface), 1u);
  surface.reset();
  gTeardownRecorder = nullptr;
}
INSTANTIATE_TEST_SUITE_P(PreEnqueueResults, PresentOomOwnershipTest,
                         testing::Values(VK_ERROR_OUT_OF_HOST_MEMORY,
                                         VK_ERROR_OUT_OF_DEVICE_MEMORY));

TEST(VulkanPresentationCreationTest, ReportsEachHandoverSubmissionTheQueueAccepted) {
  TeardownRecorder recorder;
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  int reported = 0;

  // Presenting submits the handover barrier on the queue before handing the frame over.
  auto presented = VulkanSwapchainTestAccess::MakePresentableSurface(&api);
  presented->setQueueSubmissionCallback([&reported] { ++reported; });
  EXPECT_THAT(VulkanSwapchainTestAccess::Present(*presented), HasResult());
  EXPECT_EQ(reported, 1);

  // Abandoning submits one too, to consume the frame's acquisition wait.
  auto abandoned = VulkanSwapchainTestAccess::MakePresentableSurface(&api);
  abandoned->setQueueSubmissionCallback([&reported] { ++reported; });
  EXPECT_THAT(abandoned->abandon(), IsOk());
  EXPECT_EQ(reported, 2);

  presented.reset();
  abandoned.reset();
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, DoesNotReportAHandoverSubmissionTheQueueRefused) {
  TeardownRecorder recorder;
  recorder.submissionResult = VK_ERROR_OUT_OF_HOST_MEMORY;
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  int reported = 0;
  auto surface = VulkanSwapchainTestAccess::MakePresentableSurface(&api);
  surface->setQueueSubmissionCallback([&reported] { ++reported; });

  EXPECT_THAT(VulkanSwapchainTestAccess::Present(*surface), Not(HasResult()));
  EXPECT_EQ(reported, 0);

  surface.reset();
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, UnknownPresentAssociationBlocksPreparation) {
  TeardownRecorder recorder;
  recorder.presentResult = VK_ERROR_VALIDATION_FAILED_EXT;
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto surface = VulkanSwapchainTestAccess::MakePresentableSurface(&api);

  EXPECT_THAT(VulkanSwapchainTestAccess::Present(*surface), IsGpuError(GpuErrorType::InvalidState));
  EXPECT_TRUE(VulkanSwapchainTestAccess::PresentFencePending(*surface));
  EXPECT_TRUE(VulkanSwapchainTestAccess::PreparationBlocked(*surface));
  EXPECT_THAT(VulkanSwapchainTestAccess::Prepare(*surface), IsGpuError(GpuErrorType::InvalidState));
  recorder.calls.clear();
  surface.reset();
  EXPECT_THAT(recorder.calls, Not(Contains("destroy-fence")));
  EXPECT_THAT(recorder.calls, Not(Contains("destroy-semaphore")));
  EXPECT_THAT(recorder.calls, Not(Contains("destroy-swapchain")));
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, DeviceLostSubmitNeedsAndAcceptsLaterLossProof) {
  TeardownRecorder recorder;
  recorder.submissionResult = VK_ERROR_DEVICE_LOST;
  recorder.fenceResults = {VK_ERROR_DEVICE_LOST};
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto surface = VulkanSwapchainTestAccess::MakeAcquireOnlySurface(&api);

  EXPECT_THAT(VulkanSwapchainTestAccess::Prepare(*surface), IsOk());
  EXPECT_THAT(recorder.calls, Contains("wait-fences"));
  EXPECT_EQ(VulkanSwapchainTestAccess::PendingCount(*surface), 1u);
  surface.reset();
  EXPECT_THAT(recorder.calls, Contains("free-command-buffer"));
  EXPECT_THAT(recorder.calls, Contains("destroy-fence"));
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, PartialConstructionDestroysOnlyCreatedNativeHandles) {
  TeardownRecorder recorder;
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto surface = VulkanSwapchainTestAccess::MakePartiallyConstructedSurface(&api);
  EXPECT_THAT(surface->prepareForDestruction(), IsOk());
  EXPECT_THAT(recorder.calls, testing::IsEmpty());
  surface.reset();
  EXPECT_THAT(recorder.calls, testing::ElementsAre("destroy-semaphore", "destroy-fence",
                                                   "destroy-semaphore", "destroy-surface"));
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, AcquisitionLossRetainsWaitUntilLaterCompletionProof) {
  TeardownRecorder recorder;
  recorder.acquireResult = VK_ERROR_DEVICE_LOST;
  recorder.submissionResult = VK_ERROR_DEVICE_LOST;
  recorder.fenceResults = {VK_ERROR_DEVICE_LOST};
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto surface = VulkanSwapchainTestAccess::MakeAcquirableSurface(&api);
  const Result<SurfaceStatus> acquired = surface->acquire();
  ASSERT_THAT(acquired, IsOk());
  EXPECT_EQ(acquired.result(), SurfaceStatus::DeviceLost);
  EXPECT_THAT(surface->owesAcquireWaitForTest(), testing::IsTrue());
  EXPECT_THAT(recorder.calls, testing::ElementsAre("acquire-image"));
  EXPECT_THAT(surface->prepareForDestruction(), IsOk());
  EXPECT_THAT(recorder.calls, testing::ElementsAre("acquire-image", "allocate-command-buffer",
                                                   "begin-command-buffer", "end-command-buffer",
                                                   "create-fence", "queue-submit", "wait-fences"));
  surface.reset();
  EXPECT_THAT(recorder.calls, Contains("destroy-surface"));
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, AcquireDeviceLossDeclaresSharedRootBeforeStatusReturns) {
  TeardownRecorder recorder;
  recorder.acquireResult = VK_ERROR_DEVICE_LOST;
  recorder.submissionResult = VK_ERROR_DEVICE_LOST;
  recorder.fenceResults = {VK_ERROR_DEVICE_LOST};
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  const auto rootLoss = std::make_shared<DeviceLostState>();
  auto owner = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3, nullptr, nullptr, rootLoss);
  auto sibling = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3, nullptr, nullptr, rootLoss);
  auto surface = VulkanSwapchainTestAccess::MakeAcquirableSurface(&api, owner.get());

  const Result<SurfaceStatus> acquired = surface->acquire();
  ASSERT_THAT(acquired, IsOk());
  EXPECT_EQ(acquired.result(), SurfaceStatus::DeviceLost);
  EXPECT_TRUE(owner->isLost());
  EXPECT_TRUE(sibling->isLost());
  EXPECT_EQ(rootLoss->timedOutSite.load(), DeviceLostWaitSite::None);

  surface.reset();
  owner.reset();
  sibling.reset();
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, PresentDeviceLossDeclaresSharedRootBeforeStatusReturns) {
  TeardownRecorder recorder;
  recorder.presentResult = VK_ERROR_DEVICE_LOST;
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  const auto rootLoss = std::make_shared<DeviceLostState>();
  auto owner = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3, nullptr, nullptr, rootLoss);
  auto sibling = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3, nullptr, nullptr, rootLoss);
  auto surface = VulkanSwapchainTestAccess::MakePresentableSurface(&api, owner.get());

  const Result<SurfaceStatus> presented = VulkanSwapchainTestAccess::Present(*surface);
  ASSERT_THAT(presented, IsOk());
  EXPECT_EQ(presented.result(), SurfaceStatus::DeviceLost);
  EXPECT_TRUE(owner->isLost());
  EXPECT_TRUE(sibling->isLost());
  EXPECT_EQ(rootLoss->timedOutSite.load(), DeviceLostWaitSite::None);

  surface.reset();
  owner.reset();
  sibling.reset();
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, OwnerAcceptsDeviceLossFromEveryCompletionWait) {
  TeardownRecorder recorder;
  recorder.fenceResults = {VK_ERROR_DEVICE_LOST, VK_ERROR_DEVICE_LOST, VK_ERROR_DEVICE_LOST,
                           VK_ERROR_DEVICE_LOST};
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  auto device = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
  device->attachWorkForTeardownTest(false, 11, 12);
  device->attachWorkForTeardownTest(true, 13, 14);
  device->attachSurfaceForTeardownTest(VulkanSwapchainTestAccess::MakeSurface(&api, device.get()));
  device.reset();
  ASSERT_GE(recorder.calls.size(), 4u);
  EXPECT_THAT(std::span(recorder.calls).first(4), testing::Each("wait-fences"));
  EXPECT_THAT(recorder.calls, Not(Contains("wait-idle")));
  EXPECT_THAT(std::span(recorder.calls).last(3),
              testing::ElementsAre("destroy-command-pool", "destroy-device", "destroy-instance"));
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, ADeviceLossFirstSeenAtTeardownReachesTheRoot) {
  TeardownRecorder recorder;
  recorder.fenceResults = {VK_ERROR_DEVICE_LOST};
  VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
  gTeardownRecorder = &recorder;
  const auto rootLoss = std::make_shared<DeviceLostState>();
  auto device = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3, nullptr, nullptr, rootLoss);
  device->attachWorkForTeardownTest(false, 11, 12);
  device.reset();
  EXPECT_THAT(recorder.calls, Contains("wait-fences"));
  EXPECT_THAT(rootLoss->lost.load(), testing::IsTrue())
      << "a loss the driver reports while teardown proves the device's work complete belongs to "
         "every other device over the root";
  EXPECT_THAT(rootLoss->timedOutSite.load(), testing::Eq(DeviceLostWaitSite::None))
      << "the driver reported this loss; no wait gave up";
  gTeardownRecorder = nullptr;
}

TEST(VulkanPresentationCreationTest, FailedChildDestructionPoisonsAndRetainsTheActualOwner) {
  ASSERT_EXIT(([&] {
                TeardownRecorder recorder;
                recorder.fenceResults = {VK_TIMEOUT};
                VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
                gTeardownRecorder = &recorder;
                auto owner = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
                auto child = VulkanSwapchainTestAccess::MakeSurface(&api, owner.get());
                child.reset();
                EXPECT_THAT(recorder.calls, testing::ElementsAre("wait-fences"));
                EXPECT_THAT(VulkanSwapchainTestAccess::Submit(*owner),
                            IsGpuError(GpuErrorType::InvalidState));
                EXPECT_THAT(owner->createBuffer({"refused", 16, BufferUsage::CopyDst}),
                            IsGpuError(GpuErrorType::InvalidState));
                owner.reset();
                EXPECT_THAT(recorder.calls, testing::ElementsAre("wait-fences"));
                EXPECT_THAT(VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3), testing::IsNull());
                std::exit(testing::Test::HasFailure() ? 1 : 0);
              }()),
              testing::ExitedWithCode(0), "");
}

TEST(VulkanPresentationCreationTest, UnregisteredLiveChildPreventsOwnerPrerequisiteDestruction) {
  ASSERT_EXIT(([&] {
                TeardownRecorder recorder;
                VulkanApi api = VulkanSwapchainTestAccess::MakeApi();
                gTeardownRecorder = &recorder;
                auto owner = VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3);
                auto child = VulkanSwapchainTestAccess::MakeSurface(&api, owner.get());
                owner.reset();
                EXPECT_THAT(recorder.calls, testing::IsEmpty());
                EXPECT_THAT(VulkanDevice::CreateForTeardownTest(&api, 1, 2, 3), testing::IsNull());
                child.reset();
                EXPECT_THAT(recorder.calls, Not(Contains("destroy-command-pool")));
                EXPECT_THAT(recorder.calls, Not(Contains("destroy-device")));
                EXPECT_THAT(recorder.calls, Not(Contains("destroy-instance")));
                std::exit(testing::Test::HasFailure() ? 1 : 0);
              }()),
              testing::ExitedWithCode(0), "");
}

TEST_F(VulkanSurfaceTest, PointsAWindowSystemKindAtTheEmbedderPath) {
  // Making a surface from a raw window would need that window system's client headers. An
  // embedder already links one, so the refusal names the path that works rather than leaving
  // presentation looking unavailable.
  SurfaceDescriptor xlib;
  xlib.native.kind = NativeSurfaceKind::XlibWindow;
  xlib.native.display = this;
  xlib.native.window = 1;
  EXPECT_THAT(device_->createSurface(xlib),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("EmbedderSurface")));

  SurfaceDescriptor metal;
  metal.native.kind = NativeSurfaceKind::MetalLayer;
  metal.native.display = this;
  EXPECT_THAT(device_->createSurface(metal),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("MetalLayer")));
}

TEST_F(VulkanSurfaceTest, PresentsThroughASurfaceTheEmbedderCreatedAndStillOwns) {
  // Stands in for an embedder whose windowing library makes the surface: it creates one against
  // the instance the device exposes, hands over the handle, and keeps ownership. The runtime must
  // build and tear down a swapchain on it without ever destroying the surface itself.
  const VulkanDevice::NativeContextForTest native = device_->nativeContextForTest();
  ASSERT_NE(device_->nativeInstance(), nullptr);
  ASSERT_EQ(device_->nativeInstance(), native.instance);
  ASSERT_NE(native.api->vkCreateHeadlessSurfaceEXT, nullptr);

  VkInstance instance = static_cast<VkInstance>(native.instance);
  VkHeadlessSurfaceCreateInfoEXT surfaceInfo = {};
  surfaceInfo.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
  VkSurfaceKHR embedderSurface = VK_NULL_HANDLE;
  ASSERT_EQ(
      native.api->vkCreateHeadlessSurfaceEXT(instance, &surfaceInfo, nullptr, &embedderSurface),
      VK_SUCCESS);

  uint64_t handle = 0;
  std::memcpy(&handle, &embedderSurface, sizeof(embedderSurface));

  {
    SurfaceDescriptor descriptor;
    descriptor.label = "embedder";
    descriptor.native.kind = NativeSurfaceKind::EmbedderSurface;
    descriptor.native.window = handle;

    Surface surface = unwrap(device_->createSurface(descriptor), "createSurface");
    ASSERT_THAT(device_->configureSurface(surface, configuration(surface)), IsOk());

    SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
    ASSERT_TRUE(frame.texture.isValid());
    const uint64_t serial =
        renderClear(frame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
    EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);
    EXPECT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
        << device_->lastErrorForTest();

    ASSERT_THAT(device_->destroySurface(std::move(surface)), IsOk());
  }

  // Still the embedder's to destroy, and still valid: a second runtime surface over the same
  // handle would be impossible if the first had destroyed it.
  SurfaceDescriptor again;
  again.native.kind = NativeSurfaceKind::EmbedderSurface;
  again.native.window = handle;
  Surface reused = unwrap(device_->createSurface(again), "createSurface");
  EXPECT_THAT(device_->configureSurface(reused, configuration(reused)), IsOk())
      << "The runtime destroyed a surface it does not own";
  EXPECT_THAT(device_->destroySurface(std::move(reused)), IsOk());

  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
  native.api->vkDestroySurfaceKHR(instance, embedderSurface, nullptr);
}

TEST_F(VulkanSurfaceTest, ReportsWhatTheSurfaceCanPresent) {
  const Surface surface = unwrap(device_->createSurface(headlessDescriptor()), "createSurface");

  const SurfaceCapabilities capabilities =
      unwrap(device_->surfaceCapabilities(surface), "surfaceCapabilities");
  EXPECT_THAT(capabilities.formats, Not(testing::IsEmpty()))
      << "A surface that presents no format this runtime names could never be configured";
  EXPECT_THAT(capabilities.presentModes, Contains(PresentMode::Fifo))
      << "Queued presentation is always available in Vulkan";
  EXPECT_THAT(capabilities.alphaModes, Not(testing::IsEmpty()));
  EXPECT_TRUE(HasAllFlags(capabilities.usages, TextureUsage::RenderAttachment))
      << "A frame this runtime can draw into is the whole point of the surface";
}

TEST_F(VulkanSurfaceTest, RefusesAConfigurationOutsideItsCapabilities) {
  const Surface surface = unwrap(device_->createSurface(headlessDescriptor()), "createSurface");

  SurfaceConfiguration floatFormat = configuration(surface);
  floatFormat.format = TextureFormat::R8Unorm;
  EXPECT_THAT(device_->configureSurface(surface, floatFormat),
              IsGpuError(GpuErrorType::Unsupported));

  SurfaceConfiguration storage = configuration(surface);
  storage.usage = TextureUsage::RenderAttachment | TextureUsage::StorageBinding;
  const SurfaceCapabilities capabilities =
      unwrap(device_->surfaceCapabilities(surface), "surfaceCapabilities");
  if (!HasAllFlags(capabilities.usages, TextureUsage::StorageBinding)) {
    EXPECT_THAT(device_->configureSurface(surface, storage), IsGpuError(GpuErrorType::Unsupported));
  }
}

TEST_F(VulkanSurfaceTest, RejectsAConfigurationWiderThanATextureMayBe) {
  const Surface surface = unwrap(device_->createSurface(headlessDescriptor()), "createSurface");
  EXPECT_THAT(
      device_->configureSurface(surface, configuration(surface, kMaxTextureDimension + 1, 64)),
      IsGpuError(GpuErrorType::LimitExceeded));
}

TEST_F(VulkanSurfaceTest, AcquiringBeforeConfiguringIsReported) {
  const Surface surface = unwrap(device_->createSurface(headlessDescriptor()), "createSurface");
  EXPECT_THAT(device_->acquireCurrentTexture(surface), IsGpuError(GpuErrorType::InvalidState));
}

TEST_F(VulkanSurfaceTest, AFrameIsARenderTargetTheRuntimeCanDrawIntoAndReadBack) {
  const Surface surface = configuredSurface();
  const TextureFormat format = configuration(surface).format;

  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_EQ(frame.status, SurfaceStatus::Success);
  ASSERT_TRUE(frame.texture.isValid());

  const std::vector<uint8_t> pixels =
      renderClearAndReadBack(frame.texture, kRedClear, kSurfaceWidth, kSurfaceHeight);
  ASSERT_EQ(pixels.size(), size_t{kSurfaceWidth} * kSurfaceHeight * 4u)
      << "A frame is the extent the surface was configured with";
  const std::array<uint8_t, 4> expected = {255, 0, 0, 255};
  EXPECT_THAT(rgbaTexelAt(pixels, format, kSurfaceWidth, 0, 0), ElementsAreArray(expected));
  EXPECT_THAT(rgbaTexelAt(pixels, format, kSurfaceWidth, kSurfaceWidth - 1, kSurfaceHeight - 1),
              ElementsAreArray(expected));
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());

  EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);
  EXPECT_THAT(device_->createTextureView(frame.texture, TextureViewDescriptor{"after"}),
              IsGpuError(GpuErrorType::InvalidHandle))
      << "The swapchain owns the frame once it has been handed over";
}

TEST_F(VulkanSurfaceTest, RefusesPresentationAfterTheAcquiredTextureIsReleasedAndRecycled) {
  const Surface surface = configuredSurface();
  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  const uint32_t frameSlot = frame.texture.slotIndex();
  frame.texture = Texture{};

  Texture replacement =
      unwrap(device_->createTexture(TextureDescriptor{
                 "replacement", {1, 1}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}),
             "createTexture");
  ASSERT_EQ(replacement.slotIndex(), frameSlot);
  EXPECT_THAT(device_->presentSurface(surface),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("released or replaced")));

  SurfaceTexture recovered =
      unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture after refusal");
  EXPECT_TRUE(recovered.texture.isValid());
  EXPECT_THAT(device_->abandonCurrentTexture(surface), IsOk());
}

TEST_F(VulkanSurfaceTest, PresentsMoreFramesThanTheSwapchainHoldsImages) {
  const Surface surface = configuredSurface();

  // A swapchain hands out a small fixed number of images and takes one back only when it is
  // presented, so an acquisition semaphore that was never waited on, or a present that never
  // completed, starves this loop well before it ends. The validation layer's synchronization
  // checks watch the same frames go past.
  for (int frameIndex = 0; frameIndex < 8; ++frameIndex) {
    SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
    ASSERT_EQ(frame.status, SurfaceStatus::Success) << "frame " << frameIndex;
    ASSERT_TRUE(frame.texture.isValid()) << "frame " << frameIndex;

    const uint64_t serial =
        renderClear(frame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
    EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success)
        << "frame " << frameIndex;
    EXPECT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
        << "frame " << frameIndex << ": " << device_->lastErrorForTest();
  }

  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, PresentsAFrameNothingDrewInto) {
  const Surface surface = configuredSurface();

  // Nothing submits between the acquisition and the present, so the acquisition semaphore is
  // still outstanding and the present is what has to consume it. A semaphore left signalled is a
  // synchronization-validation error the next time it is waited on, which the next frame does.
  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(frame.texture.isValid());
  EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);

  SurfaceTexture next = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  EXPECT_TRUE(next.texture.isValid());
  EXPECT_THAT(device_->abandonCurrentTexture(surface), IsOk());
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, RefusesToExportAnAcquiredFrameTheSwapchainMayRecycle) {
  const Surface surface = configuredSurface();
  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_THAT(frame.texture.isValid(), testing::IsTrue());

  EXPECT_THAT(device_->exportTexture(frame.texture),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("surface frame")))
      << "a sibling cannot retain the swapchain's borrowed image after presentation";

  EXPECT_THAT(device_->abandonCurrentTexture(surface), IsOk());
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, KeepsItsAcquisitionRingStraightAcrossAnOutOfDateRebuild) {
  const Surface surface = configuredSurface();

  // One frame first, so the acquisition counter is not zero when the rebuild resets it. That is
  // the whole bug: an out-of-date acquisition rebuilds the swapchain mid-acquire, which replaces
  // the acquisition ring and restarts its counter, and a retry that keeps the slot it computed
  // against the destroyed ring signals one slot's semaphore while filing its fence under
  // another. With the counter already at one, those two slots differ for any ring size.
  SurfaceTexture warmup = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(warmup.texture.isValid());
  renderClear(warmup.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
  ASSERT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);

  device_->forceNextAcquireOutOfDateForTest(surface.slotIndex());
  SurfaceTexture rebuilt = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  EXPECT_EQ(rebuilt.status, SurfaceStatus::Outdated)
      << "A rebuilt swapchain still hands back a frame, with the signal to reconfigure";
  ASSERT_TRUE(rebuilt.texture.isValid());
  renderClear(rebuilt.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
  ASSERT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);

  // Deliberately not waiting for each frame: the fence filed against a ring slot is what stops
  // its semaphore being signalled again before the previous wait has run, so a fence filed under
  // the wrong slot only shows up while frames are still in flight.
  uint64_t lastSerial = 0;
  for (int frameIndex = 0; frameIndex < 12; ++frameIndex) {
    SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
    ASSERT_TRUE(frame.texture.isValid()) << "frame " << frameIndex;
    lastSerial = renderClear(frame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
    EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success)
        << "frame " << frameIndex;
  }

  EXPECT_TRUE(device_->waitForSerial(lastSerial, /*timeoutSeconds=*/30.0))
      << device_->lastErrorForTest();
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, OneSurfacesSubmissionDoesNotConsumeAnothersAcquisitionWait) {
  const Surface first = configuredSurface();
  const Surface second = configuredSurface();

  SurfaceTexture firstFrame =
      unwrap(device_->acquireCurrentTexture(first), "acquireCurrentTexture");
  SurfaceTexture secondFrame =
      unwrap(device_->acquireCurrentTexture(second), "acquireCurrentTexture");
  ASSERT_TRUE(firstFrame.texture.isValid());
  ASSERT_TRUE(secondFrame.texture.isValid());
  ASSERT_TRUE(device_->surfaceAcquisitionForTest(second.slotIndex()).owesAcquireWait);

  // Writing one surface's frame says nothing about when the other's is safe to write. A
  // submission that swept up both waits would leave the second surface's own writer carrying
  // none, and its first transition would then race the presentation engine's read of that image.
  // On one in-order queue that race cannot be observed, so what is asserted is the contract
  // itself: after a submission that names only the first frame, the second surface still owes
  // its wait.
  const uint64_t firstSerial =
      renderClear(firstFrame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);

  EXPECT_TRUE(device_->surfaceAcquisitionForTest(second.slotIndex()).owesAcquireWait)
      << "A submission that writes only the first surface's frame took the second's wait with it";
  EXPECT_FALSE(device_->surfaceAcquisitionForTest(first.slotIndex()).owesAcquireWait)
      << "The submission that writes a frame is the one that owes its wait";

  // Both surfaces' work stays in flight until here, so the two submissions overlap rather than
  // being serialised by a wait between them.
  const uint64_t secondSerial =
      renderClear(secondFrame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
  EXPECT_EQ(unwrap(device_->presentSurface(first), "presentSurface"), SurfaceStatus::Success);
  EXPECT_EQ(unwrap(device_->presentSurface(second), "presentSurface"), SurfaceStatus::Success);

  EXPECT_TRUE(device_->waitForSerial(firstSerial, /*timeoutSeconds=*/30.0))
      << device_->lastErrorForTest();
  EXPECT_TRUE(device_->waitForSerial(secondSerial, /*timeoutSeconds=*/30.0))
      << device_->lastErrorForTest();
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, FencesAFrameInTheRingSlotItsSemaphoreCameFrom) {
  const Surface surface = configuredSurface();

  // One frame first, so the acquisition counter is not zero when a rebuild resets it: that is
  // the only state in which the slot computed against the old ring and the slot the restarted
  // counter names differ.
  SurfaceTexture warmup = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(warmup.texture.isValid());
  renderClear(warmup.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
  ASSERT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);

  device_->forceNextAcquireOutOfDateForTest(surface.slotIndex());
  SurfaceTexture rebuilt = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_EQ(rebuilt.status, SurfaceStatus::Outdated);
  ASSERT_TRUE(rebuilt.texture.isValid());

  const size_t frameRingSlot =
      device_->surfaceAcquisitionForTest(surface.slotIndex()).frameRingSlot;
  renderClear(rebuilt.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
  ASSERT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);

  // The fence that says when a ring slot's semaphore may be signalled again has to be filed
  // against the slot whose semaphore this frame actually used. Filed anywhere else, a later
  // acquisition skips a wait it owes, and the only symptom is a race that a fast driver wins.
  const std::optional<size_t> fencedRingSlot =
      device_->surfaceAcquisitionForTest(surface.slotIndex()).lastFencedRingSlot;
  ASSERT_TRUE(fencedRingSlot.has_value());
  EXPECT_EQ(*fencedRingSlot, frameRingSlot)
      << "The frame's fence was filed against a ring slot whose semaphore it never used";
}

TEST_F(VulkanSurfaceTest, StaysInBoundsWhenARebuildShrinksTheAcquisitionRing) {
  const Surface surface = configuredSurface();

  // Land on the ring's last slot before rebuilding, because that is the slot a smaller new ring
  // would no longer have, and ask the rebuild for the fewest images the surface allows.
  //
  // Whether the ring actually shrinks is the driver's choice: that count is a minimum, and an
  // implementation may return more. On a software rasterizer it returns the same number every
  // time, so this case does not reproduce the out-of-bounds read on that lane - it asserts the
  // invariant that would catch it, on any driver that does hand back a smaller ring.
  const size_t ringSize = device_->surfaceAcquisitionForTest(surface.slotIndex()).ringSize;
  ASSERT_GT(ringSize, 1u);
  for (size_t frameIndex = 0; frameIndex < ringSize + 1; ++frameIndex) {
    SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
    ASSERT_TRUE(frame.texture.isValid()) << "frame " << frameIndex;
    const bool onLastSlot =
        device_->surfaceAcquisitionForTest(surface.slotIndex()).frameRingSlot == ringSize - 1;
    renderClear(frame.texture, kRedClear, nullptr, kSurfaceWidth, kSurfaceHeight);
    ASSERT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);
    if (onLastSlot) {
      break;
    }
  }

  device_->forceMinimumImageCountOnceForTest(surface.slotIndex());
  device_->forceNextAcquireOutOfDateForTest(surface.slotIndex());

  SurfaceTexture rebuilt = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  EXPECT_EQ(rebuilt.status, SurfaceStatus::Outdated);
  EXPECT_TRUE(rebuilt.texture.isValid());
  const VulkanDevice::SurfaceAcquisitionForTest acquisition =
      device_->surfaceAcquisitionForTest(surface.slotIndex());
  EXPECT_LT(acquisition.frameRingSlot, acquisition.ringSize)
      << "The retry used a slot the rebuilt ring does not have";
  EXPECT_THAT(device_->abandonCurrentTexture(surface), IsOk());
}

/// Counts the submissions a device reports, and the command buffers they carried.
class SubmissionCounter final : public DeviceObserver {
public:
  void onBufferCreated() override {}
  void onTextureCreated() override {}
  void onTextureReleased() override {}
  void onBindGroupCreated() override {}
  void onBufferWritten(uint64_t /*byteCount*/) override {}
  void onTextureWritten(uint64_t /*byteCount*/) override {}
  void onSubmitted(uint64_t commandBufferCount, uint64_t /*drawCount*/) override {
    ++submissions;
    commandBuffers += commandBufferCount;
  }

  uint64_t submissions = 0;     //!< Submissions reported.
  uint64_t commandBuffers = 0;  //!< Command buffers they carried.
};

TEST_F(VulkanSurfaceTest, AnObserverSeesTheQueueSubmissionThatEndsEachFrame) {
  const Surface surface = configuredSurface();
  SubmissionCounter counter;
  const gpu::tests::ScopedObserverInstallation observing(*device_, counter);
  ASSERT_THAT(observing.status(), IsOk());

  // Nothing is submitted through `submit` here: the only queue work is the swapchain's own
  // handover barrier, once for the presented frame and once for the abandoned one.
  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(frame.texture.isValid());
  EXPECT_EQ(unwrap(device_->presentSurface(surface), "presentSurface"), SurfaceStatus::Success);
  EXPECT_EQ(counter.submissions, 1u);

  SurfaceTexture next = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(next.texture.isValid());
  EXPECT_THAT(device_->abandonCurrentTexture(surface), IsOk());
  EXPECT_EQ(counter.submissions, 2u);
  EXPECT_EQ(counter.commandBuffers, 0u) << "a backend's own submission carries no caller buffers";
}

TEST_F(VulkanSurfaceTest, AbandonsMoreFramesThanTheSwapchainHoldsImages) {
  const Surface surface = configuredSurface();

  // Vulkan has no operation that gives an acquired image back, so discarding one is only made
  // good by replacing the swapchain. Running the discard more times than the swapchain holds
  // images is what proves that actually happens rather than the images being stranded.
  for (int frameIndex = 0; frameIndex < 8; ++frameIndex) {
    SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
    ASSERT_TRUE(frame.texture.isValid()) << "frame " << frameIndex;
    EXPECT_THAT(device_->abandonCurrentTexture(surface), IsOk()) << "frame " << frameIndex;
  }

  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, ReconfiguringHandsOutFramesAtTheNewExtent) {
  constexpr uint32_t kResizedWidth = 128;
  constexpr uint32_t kResizedHeight = 32;

  const Surface surface = configuredSurface();
  SurfaceTexture before = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(before.texture.isValid());

  ASSERT_THAT(
      device_->configureSurface(surface, configuration(surface, kResizedWidth, kResizedHeight)),
      IsOk());
  EXPECT_THAT(device_->createTextureView(before.texture, TextureViewDescriptor{"stale"}),
              IsGpuError(GpuErrorType::InvalidHandle));

  SurfaceTexture after = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_EQ(after.status, SurfaceStatus::Success);
  ASSERT_TRUE(after.texture.isValid());

  const std::vector<uint8_t> pixels =
      renderClearAndReadBack(after.texture, kRedClear, kResizedWidth, kResizedHeight);
  EXPECT_EQ(pixels.size(), size_t{kResizedWidth} * kResizedHeight * 4u)
      << "The swapchain's images follow the configuration rather than the surface's first extent";
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

TEST_F(VulkanSurfaceTest, DestroyingASurfaceReleasesItsSwapchain) {
  Surface surface = configuredSurface();
  SurfaceTexture frame = unwrap(device_->acquireCurrentTexture(surface), "acquireCurrentTexture");
  ASSERT_TRUE(frame.texture.isValid());

  ASSERT_THAT(device_->destroySurface(std::move(surface)), IsOk());

  // A new surface finds a whole swapchain available again, which it would not if destruction had
  // left the previous one holding its images or its surface object.
  const Surface replacement = configuredSurface();
  for (int frameIndex = 0; frameIndex < 4; ++frameIndex) {
    SurfaceTexture next =
        unwrap(device_->acquireCurrentTexture(replacement), "acquireCurrentTexture");
    ASSERT_EQ(next.status, SurfaceStatus::Success) << "frame " << frameIndex;
    EXPECT_THAT(device_->abandonCurrentTexture(replacement), IsOk()) << "frame " << frameIndex;
  }
  EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
}

}  // namespace
}  // namespace donner::gpu::vulkan::tests
