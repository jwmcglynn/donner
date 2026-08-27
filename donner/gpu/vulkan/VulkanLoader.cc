/// @file
/// Runtime loading of the Vulkan loader library and its entry points.

#include "donner/gpu/vulkan/VulkanLoader.h"

#include <format>
#include <string>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace donner::gpu::vulkan {

namespace {

/// Names the platform's Vulkan loader is published under, most specific first.
///
/// The versioned name is the one an installed runtime always provides and the one an
/// application is expected to load; the unversioned name is a development symlink that some
/// minimal environments ship instead, so it is a fallback rather than the primary.
constexpr const char* kLoaderNames[] = {
#if defined(_WIN32)
    "vulkan-1.dll",
#elif defined(__APPLE__)
    "libvulkan.1.dylib",
    "libvulkan.dylib",
#else
    "libvulkan.so.1",
    "libvulkan.so",
#endif
};

/// Opens a shared library by name, or returns null. @param name Library name to open.
void* OpenLibrary(const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(::LoadLibraryA(name));
#else
  // Resolve eagerly and keep the symbols private to this handle: the loader is an
  // implementation detail of this backend, and a lazily bound missing symbol would surface as a
  // crash at an arbitrary later call instead of a failure here.
  return ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

/// Closes a shared library opened by \ref OpenLibrary. @param library Handle to close.
void CloseLibrary(void* library) {
#if defined(_WIN32)
  ::FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
  ::dlclose(library);
#endif
}

/// Looks up a symbol in an opened library, or returns null.
/// @param library Opened library handle.
/// @param name Symbol name.
void* LibrarySymbol(void* library, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
  return ::dlsym(library, name);
#endif
}

/// Collects the first entry point a resolution pass could not find.
///
/// Reporting the first missing name rather than a count is what makes the failure actionable:
/// a loader too old for this backend is missing a specific function, and its name says which.
class MissingEntryPoint {
public:
  /// Stores \p function, recording \p name when the lookup came back null.
  /// @param function Table slot to fill.
  /// @param name Entry point name, used for the diagnostic.
  /// @param resolved Raw pointer the lookup returned, possibly null.
  template <typename Fn>
  void store(Fn& function, const char* name, PFN_vkVoidFunction resolved) {
    function = reinterpret_cast<std::remove_reference_t<Fn>>(resolved);
    if (resolved == nullptr) {
      record(name);
    }
  }

  /// True when every lookup so far succeeded.
  bool complete() const { return name_.empty(); }

  /// Name of the first entry point that was missing, or an empty string.
  const std::string& name() const { return name_; }

private:
  /// Records \p name as the first failure. @param name Missing entry point name.
  void record(const char* name) {
    if (name_.empty()) {
      name_ = name;
    }
  }

  std::string name_;
};

}  // namespace

Result<std::shared_ptr<VulkanLoader>> VulkanLoader::Open() {
  void* library = nullptr;
  for (const char* name : kLoaderNames) {
    library = OpenLibrary(name);
    if (library != nullptr) {
      break;
    }
  }
  if (library == nullptr) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("no Vulkan loader could be opened (tried {})", kLoaderNames[0])};
  }

  // Wrap the handle before any further failure so the library is closed on every exit path.
  std::shared_ptr<VulkanLoader> loader(new VulkanLoader(library));

  // The loader exports exactly one symbol an application is guaranteed to find by name;
  // everything else is reached through it.
  loader->api_.vkGetInstanceProcAddr =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(LibrarySymbol(library, "vkGetInstanceProcAddr"));
  if (loader->api_.vkGetInstanceProcAddr == nullptr) {
    return GpuError{GpuErrorType::Unsupported,
                    "the Vulkan loader does not export vkGetInstanceProcAddr"};
  }

  // Global entry points: the ones that exist before any instance does, resolved with a null
  // instance as the specification prescribes.
  VulkanApi& api = loader->api_;
  const auto global = [&api](const char* name) {
    return api.vkGetInstanceProcAddr(VK_NULL_HANDLE, name);
  };
  MissingEntryPoint missing;
  missing.store(api.vkEnumerateInstanceVersion, "vkEnumerateInstanceVersion",
                global("vkEnumerateInstanceVersion"));
  missing.store(api.vkEnumerateInstanceLayerProperties, "vkEnumerateInstanceLayerProperties",
                global("vkEnumerateInstanceLayerProperties"));
  missing.store(api.vkEnumerateInstanceExtensionProperties,
                "vkEnumerateInstanceExtensionProperties",
                global("vkEnumerateInstanceExtensionProperties"));
  missing.store(api.vkCreateInstance, "vkCreateInstance", global("vkCreateInstance"));
  if (!missing.complete()) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("the Vulkan loader does not provide {}", missing.name())};
  }
  return loader;
}

VulkanLoader::~VulkanLoader() {
  if (library_ != nullptr) {
    CloseLibrary(library_);
    library_ = nullptr;
  }
}

Status VulkanLoader::loadInstance(VkInstance instance, bool debugUtilsEnabled) {
  const auto resolve = [this, instance](const char* name) {
    return api_.vkGetInstanceProcAddr(instance, name);
  };
  MissingEntryPoint missing;
  missing.store(api_.vkDestroyInstance, "vkDestroyInstance", resolve("vkDestroyInstance"));
  missing.store(api_.vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices",
                resolve("vkEnumeratePhysicalDevices"));
  missing.store(api_.vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties",
                resolve("vkGetPhysicalDeviceProperties"));
  missing.store(api_.vkGetPhysicalDeviceFeatures, "vkGetPhysicalDeviceFeatures",
                resolve("vkGetPhysicalDeviceFeatures"));
  missing.store(api_.vkGetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties",
                resolve("vkGetPhysicalDeviceMemoryProperties"));
  missing.store(api_.vkGetPhysicalDeviceQueueFamilyProperties,
                "vkGetPhysicalDeviceQueueFamilyProperties",
                resolve("vkGetPhysicalDeviceQueueFamilyProperties"));
  missing.store(api_.vkCreateDevice, "vkCreateDevice", resolve("vkCreateDevice"));
  missing.store(api_.vkGetDeviceProcAddr, "vkGetDeviceProcAddr", resolve("vkGetDeviceProcAddr"));
  if (!missing.complete()) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("the Vulkan instance does not provide {}", missing.name())};
  }

  // The messenger is a diagnostic, not a requirement: leaving these null when the extension is
  // off keeps the caller's "no messenger" path a null check rather than a special case.
  if (debugUtilsEnabled) {
    api_.vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        resolve("vkCreateDebugUtilsMessengerEXT"));
    api_.vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        resolve("vkDestroyDebugUtilsMessengerEXT"));
  }
  return OkStatus();
}

Status VulkanLoader::loadDevice(VkDevice device) {
  const auto resolve = [this, device](const char* name) {
    return api_.vkGetDeviceProcAddr(device, name);
  };
  MissingEntryPoint missing;
  missing.store(api_.vkDestroyDevice, "vkDestroyDevice", resolve("vkDestroyDevice"));
  missing.store(api_.vkDeviceWaitIdle, "vkDeviceWaitIdle", resolve("vkDeviceWaitIdle"));
  missing.store(api_.vkGetDeviceQueue, "vkGetDeviceQueue", resolve("vkGetDeviceQueue"));
  missing.store(api_.vkQueueSubmit, "vkQueueSubmit", resolve("vkQueueSubmit"));
  missing.store(api_.vkAllocateMemory, "vkAllocateMemory", resolve("vkAllocateMemory"));
  missing.store(api_.vkFreeMemory, "vkFreeMemory", resolve("vkFreeMemory"));
  missing.store(api_.vkMapMemory, "vkMapMemory", resolve("vkMapMemory"));
  missing.store(api_.vkCreateBuffer, "vkCreateBuffer", resolve("vkCreateBuffer"));
  missing.store(api_.vkDestroyBuffer, "vkDestroyBuffer", resolve("vkDestroyBuffer"));
  missing.store(api_.vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements",
                resolve("vkGetBufferMemoryRequirements"));
  missing.store(api_.vkBindBufferMemory, "vkBindBufferMemory", resolve("vkBindBufferMemory"));
  missing.store(api_.vkCreateImage, "vkCreateImage", resolve("vkCreateImage"));
  missing.store(api_.vkDestroyImage, "vkDestroyImage", resolve("vkDestroyImage"));
  missing.store(api_.vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements",
                resolve("vkGetImageMemoryRequirements"));
  missing.store(api_.vkBindImageMemory, "vkBindImageMemory", resolve("vkBindImageMemory"));
  missing.store(api_.vkCreateImageView, "vkCreateImageView", resolve("vkCreateImageView"));
  missing.store(api_.vkDestroyImageView, "vkDestroyImageView", resolve("vkDestroyImageView"));
  missing.store(api_.vkCreateSampler, "vkCreateSampler", resolve("vkCreateSampler"));
  missing.store(api_.vkDestroySampler, "vkDestroySampler", resolve("vkDestroySampler"));
  missing.store(api_.vkCreateDescriptorSetLayout, "vkCreateDescriptorSetLayout",
                resolve("vkCreateDescriptorSetLayout"));
  missing.store(api_.vkDestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout",
                resolve("vkDestroyDescriptorSetLayout"));
  missing.store(api_.vkCreateDescriptorPool, "vkCreateDescriptorPool",
                resolve("vkCreateDescriptorPool"));
  missing.store(api_.vkDestroyDescriptorPool, "vkDestroyDescriptorPool",
                resolve("vkDestroyDescriptorPool"));
  missing.store(api_.vkAllocateDescriptorSets, "vkAllocateDescriptorSets",
                resolve("vkAllocateDescriptorSets"));
  missing.store(api_.vkUpdateDescriptorSets, "vkUpdateDescriptorSets",
                resolve("vkUpdateDescriptorSets"));
  missing.store(api_.vkCreatePipelineLayout, "vkCreatePipelineLayout",
                resolve("vkCreatePipelineLayout"));
  missing.store(api_.vkDestroyPipelineLayout, "vkDestroyPipelineLayout",
                resolve("vkDestroyPipelineLayout"));
  missing.store(api_.vkCreateShaderModule, "vkCreateShaderModule", resolve("vkCreateShaderModule"));
  missing.store(api_.vkDestroyShaderModule, "vkDestroyShaderModule",
                resolve("vkDestroyShaderModule"));
  missing.store(api_.vkCreateRenderPass, "vkCreateRenderPass", resolve("vkCreateRenderPass"));
  missing.store(api_.vkDestroyRenderPass, "vkDestroyRenderPass", resolve("vkDestroyRenderPass"));
  missing.store(api_.vkCreateFramebuffer, "vkCreateFramebuffer", resolve("vkCreateFramebuffer"));
  missing.store(api_.vkDestroyFramebuffer, "vkDestroyFramebuffer", resolve("vkDestroyFramebuffer"));
  missing.store(api_.vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines",
                resolve("vkCreateGraphicsPipelines"));
  missing.store(api_.vkCreateComputePipelines, "vkCreateComputePipelines",
                resolve("vkCreateComputePipelines"));
  missing.store(api_.vkDestroyPipeline, "vkDestroyPipeline", resolve("vkDestroyPipeline"));
  missing.store(api_.vkCreateCommandPool, "vkCreateCommandPool", resolve("vkCreateCommandPool"));
  missing.store(api_.vkDestroyCommandPool, "vkDestroyCommandPool", resolve("vkDestroyCommandPool"));
  missing.store(api_.vkAllocateCommandBuffers, "vkAllocateCommandBuffers",
                resolve("vkAllocateCommandBuffers"));
  missing.store(api_.vkFreeCommandBuffers, "vkFreeCommandBuffers", resolve("vkFreeCommandBuffers"));
  missing.store(api_.vkBeginCommandBuffer, "vkBeginCommandBuffer", resolve("vkBeginCommandBuffer"));
  missing.store(api_.vkEndCommandBuffer, "vkEndCommandBuffer", resolve("vkEndCommandBuffer"));
  missing.store(api_.vkCreateFence, "vkCreateFence", resolve("vkCreateFence"));
  missing.store(api_.vkDestroyFence, "vkDestroyFence", resolve("vkDestroyFence"));
  missing.store(api_.vkGetFenceStatus, "vkGetFenceStatus", resolve("vkGetFenceStatus"));
  missing.store(api_.vkWaitForFences, "vkWaitForFences", resolve("vkWaitForFences"));
  missing.store(api_.vkCmdBeginRenderPass, "vkCmdBeginRenderPass", resolve("vkCmdBeginRenderPass"));
  missing.store(api_.vkCmdEndRenderPass, "vkCmdEndRenderPass", resolve("vkCmdEndRenderPass"));
  missing.store(api_.vkCmdBindPipeline, "vkCmdBindPipeline", resolve("vkCmdBindPipeline"));
  missing.store(api_.vkCmdBindDescriptorSets, "vkCmdBindDescriptorSets",
                resolve("vkCmdBindDescriptorSets"));
  missing.store(api_.vkCmdBindVertexBuffers, "vkCmdBindVertexBuffers",
                resolve("vkCmdBindVertexBuffers"));
  missing.store(api_.vkCmdSetViewport, "vkCmdSetViewport", resolve("vkCmdSetViewport"));
  missing.store(api_.vkCmdSetScissor, "vkCmdSetScissor", resolve("vkCmdSetScissor"));
  missing.store(api_.vkCmdDraw, "vkCmdDraw", resolve("vkCmdDraw"));
  missing.store(api_.vkCmdDispatch, "vkCmdDispatch", resolve("vkCmdDispatch"));
  missing.store(api_.vkCmdPipelineBarrier, "vkCmdPipelineBarrier", resolve("vkCmdPipelineBarrier"));
  missing.store(api_.vkCmdCopyBufferToImage, "vkCmdCopyBufferToImage",
                resolve("vkCmdCopyBufferToImage"));
  missing.store(api_.vkCmdCopyImageToBuffer, "vkCmdCopyImageToBuffer",
                resolve("vkCmdCopyImageToBuffer"));
  missing.store(api_.vkCmdCopyImage, "vkCmdCopyImage", resolve("vkCmdCopyImage"));
  if (!missing.complete()) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("the Vulkan device does not provide {}", missing.name())};
  }
  return OkStatus();
}

}  // namespace donner::gpu::vulkan
