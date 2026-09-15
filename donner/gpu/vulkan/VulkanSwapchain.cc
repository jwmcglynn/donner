/// @file
/// Vulkan presentation: surface creation, swapchain configuration, frame acquisition and present.

#include "donner/gpu/vulkan/VulkanSwapchain.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <sstream>
#include <string>
#include <utility>

namespace donner::gpu::vulkan {

namespace {

/// How long an acquisition waits for the presentation engine to release a frame before reporting
/// a timeout. A frame loop retries on the next frame rather than blocking on a compositor that
/// is not handing images back.
constexpr uint64_t kAcquireTimeoutNanoseconds = 1'000'000'000;

/// How long a drain waits for this swapchain's own submissions. Only teardown and recreation use
/// it, and a device that has not finished them in this long is not going to.
constexpr uint64_t kDrainTimeoutNanoseconds = 5'000'000'000;

/// A failure that names the native operation and its result. @param what Operation name.
/// @param result Native result.
GpuError VkError(std::string_view what, VkResult result) {
  return GpuError{GpuErrorType::InvalidState,
                  std::format("{} failed with {}", what, VkResultToString(result))};
}

/// Names an enum value through its stream operator, so a refusal says which value was refused.
/// @param value Value to name.
template <typename T>
std::string Describe(T value) {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

/// The runtime format a swapchain format corresponds to, or nothing when it is not one this
/// runtime names. @param format Native format.
std::optional<TextureFormat> RuntimeFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM: return TextureFormat::BGRA8Unorm;
    case VK_FORMAT_R8G8B8A8_UNORM: return TextureFormat::RGBA8Unorm;
    default: return std::nullopt;
  }
}

/// The swapchain format a runtime format corresponds to, or nothing when it cannot be presented.
/// @param format Runtime format.
std::optional<VkFormat> PresentationFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::R8Unorm:
    case TextureFormat::RGBA32Float: return std::nullopt;
  }
  return std::nullopt;
}

/// The runtime pacing a native present mode corresponds to, or nothing when it is not one this
/// runtime names. @param mode Native present mode.
std::optional<PresentMode> RuntimePresentMode(VkPresentModeKHR mode) {
  switch (mode) {
    case VK_PRESENT_MODE_FIFO_KHR: return PresentMode::Fifo;
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return PresentMode::Immediate;
    case VK_PRESENT_MODE_MAILBOX_KHR: return PresentMode::Mailbox;
    default: return std::nullopt;
  }
}

/// The native present mode a runtime pacing corresponds to. @param mode Runtime pacing.
VkPresentModeKHR NativePresentMode(PresentMode mode) {
  switch (mode) {
    case PresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
    case PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

/// The native composite-alpha bit a runtime alpha mode corresponds to. @param mode Alpha mode.
VkCompositeAlphaFlagBitsKHR NativeCompositeAlpha(SurfaceAlphaMode mode) {
  switch (mode) {
    case SurfaceAlphaMode::Opaque: return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    case SurfaceAlphaMode::Premultiplied: return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    case SurfaceAlphaMode::Inherit: return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  }
  return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

/// Native image usage for a runtime usage set. @param usage Runtime usage flags.
VkImageUsageFlags NativeImageUsage(TextureUsage usage) {
  VkImageUsageFlags result = 0;
  if (HasAllFlags(usage, TextureUsage::RenderAttachment)) {
    result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  if (HasAllFlags(usage, TextureUsage::Sampled)) {
    result |= VK_IMAGE_USAGE_SAMPLED_BIT;
  }
  if (HasAllFlags(usage, TextureUsage::CopySrc)) {
    result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  if (HasAllFlags(usage, TextureUsage::CopyDst)) {
    result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  if (HasAllFlags(usage, TextureUsage::StorageBinding)) {
    result |= VK_IMAGE_USAGE_STORAGE_BIT;
  }
  return result;
}

/// The runtime usage set a native image usage mask covers. @param usage Native usage flags.
TextureUsage RuntimeImageUsage(VkImageUsageFlags usage) {
  TextureUsage result = TextureUsage::None;
  if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) {
    result |= TextureUsage::RenderAttachment;
  }
  if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) {
    result |= TextureUsage::Sampled;
  }
  if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
    result |= TextureUsage::CopySrc;
  }
  if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0) {
    result |= TextureUsage::CopyDst;
  }
  if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) {
    result |= TextureUsage::StorageBinding;
  }
  return result;
}

/// The surface object an embedder named in the descriptor's 64-bit payload slot.
///
/// A Vulkan non-dispatchable handle is a pointer on a 64-bit platform and a 64-bit integer on a
/// 32-bit one, so it is always exactly eight bytes but not always an integer; copying the bytes
/// is the one spelling that is correct either way.
///
/// @param value Handle the embedder passed.
VkSurfaceKHR SurfaceHandleFrom(uint64_t value) {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  static_assert(sizeof(surface) == sizeof(value), "a Vulkan handle fills the 64-bit payload slot");
  std::memcpy(&surface, &value, sizeof(surface));
  return surface;
}

/// A surface object plus whether creating it made it this runtime's to destroy.
struct CreatedSurface {
  VkSurfaceKHR surface = VK_NULL_HANDLE;  //!< The surface.
  bool owned = true;                      //!< False for a surface the embedder still owns.
};

/// Produces the surface \p descriptor names.
/// @param api Entry points. @param instance Instance to create against.
/// @param descriptor Label and platform object.
Result<CreatedSurface> SurfaceForDescriptor(const VulkanApi& api, VkInstance instance,
                                            const SurfaceDescriptor& descriptor) {
  switch (descriptor.native.kind) {
    case NativeSurfaceKind::EmbedderSurface:
      // The production path: the embedder's windowing library made the surface against the
      // instance this device exposes, and keeps it. Nothing here can check which instance it came
      // from, because a surface does not name one, so that stays the embedder's half of the
      // contract with the validation layer as the backstop.
      return CreatedSurface{SurfaceHandleFrom(descriptor.native.window), false};

    case NativeSurfaceKind::Headless: {
      if (api.vkCreateHeadlessSurfaceEXT == nullptr) {
        return GpuError{GpuErrorType::Unsupported,
                        "createSurface: the Vulkan instance does not offer headless surfaces "
                        "(VK_EXT_headless_surface)"};
      }
      VkHeadlessSurfaceCreateInfoEXT surfaceInfo = {};
      surfaceInfo.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      if (const VkResult result =
              api.vkCreateHeadlessSurfaceEXT(instance, &surfaceInfo, nullptr, &surface);
          result != VK_SUCCESS) {
        return VkError("vkCreateHeadlessSurfaceEXT", result);
      }
      return CreatedSurface{surface, true};
    }

    case NativeSurfaceKind::XlibWindow:
    case NativeSurfaceKind::WaylandSurface:
      // Making a surface from a raw window needs that window system's client headers, which this
      // build deliberately does not depend on. An embedder already linking one creates the
      // surface itself and hands it over, which the refusal says.
      return GpuError{
          GpuErrorType::Unsupported,
          std::format("createSurface: this runtime does not create a surface from {} itself; "
                      "create it with the instance from nativeInstance() and pass it as "
                      "EmbedderSurface",
                      Describe(descriptor.native.kind))};

    case NativeSurfaceKind::MetalLayer:
    case NativeSurfaceKind::CanvasSelector:
      return GpuError{GpuErrorType::Unsupported,
                      std::format("createSurface: the Vulkan backend does not present to {}",
                                  Describe(descriptor.native.kind))};
  }
  return GpuError{GpuErrorType::Unsupported, "createSurface: unknown surface kind"};
}

/// Whether a status means no frame came back, so the caller has nothing to draw.
/// @param status Status an acquisition reported.
bool CarriesNoFrame(SurfaceStatus status) {
  return status == SurfaceStatus::Lost || status == SurfaceStatus::DeviceLost ||
         status == SurfaceStatus::Timeout;
}

/// The surface formats \p surface offers. @param api Entry points.
/// @param physicalDevice Device to query. @param surface Surface to query.
Result<std::vector<VkSurfaceFormatKHR>> QuerySurfaceFormats(const VulkanApi& api,
                                                            VkPhysicalDevice physicalDevice,
                                                            VkSurfaceKHR surface) {
  uint32_t count = 0;
  if (const VkResult result =
          api.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, nullptr);
      result != VK_SUCCESS) {
    return VkError("vkGetPhysicalDeviceSurfaceFormatsKHR", result);
  }
  std::vector<VkSurfaceFormatKHR> formats(count);
  if (count == 0) {
    return formats;
  }
  if (const VkResult result =
          api.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, formats.data());
      result != VK_SUCCESS) {
    return VkError("vkGetPhysicalDeviceSurfaceFormatsKHR", result);
  }
  return formats;
}

/// The present modes \p surface offers. @param api Entry points.
/// @param physicalDevice Device to query. @param surface Surface to query.
Result<std::vector<VkPresentModeKHR>> QueryPresentModes(const VulkanApi& api,
                                                        VkPhysicalDevice physicalDevice,
                                                        VkSurfaceKHR surface) {
  uint32_t count = 0;
  if (const VkResult result =
          api.vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, nullptr);
      result != VK_SUCCESS) {
    return VkError("vkGetPhysicalDeviceSurfacePresentModesKHR", result);
  }
  std::vector<VkPresentModeKHR> modes(count);
  if (count == 0) {
    return modes;
  }
  if (const VkResult result = api.vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface,
                                                                            &count, modes.data());
      result != VK_SUCCESS) {
    return VkError("vkGetPhysicalDeviceSurfacePresentModesKHR", result);
  }
  return modes;
}

/// The runtime formats among \p nativeFormats, without duplicates.
///
/// Only the nonlinear sRGB color space: the runtime has no way to say which color space a frame
/// is in, so a format offered in another one would present colors nobody asked for.
///
/// @param nativeFormats Formats the surface offers.
std::vector<TextureFormat> RuntimeFormats(const std::vector<VkSurfaceFormatKHR>& nativeFormats) {
  std::vector<TextureFormat> formats;
  for (const VkSurfaceFormatKHR& nativeFormat : nativeFormats) {
    if (nativeFormat.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      continue;
    }
    const std::optional<TextureFormat> format = RuntimeFormat(nativeFormat.format);
    if (format.has_value() && std::ranges::find(formats, *format) == formats.end()) {
      formats.push_back(*format);
    }
  }
  return formats;
}

/// The runtime pacings among \p nativeModes, without duplicates. @param nativeModes Native modes.
std::vector<PresentMode> RuntimePresentModes(const std::vector<VkPresentModeKHR>& nativeModes) {
  std::vector<PresentMode> modes;
  for (const VkPresentModeKHR nativeMode : nativeModes) {
    const std::optional<PresentMode> mode = RuntimePresentMode(nativeMode);
    if (mode.has_value() && std::ranges::find(modes, *mode) == modes.end()) {
      modes.push_back(*mode);
    }
  }
  return modes;
}

/// The runtime alpha modes a composite-alpha mask covers. @param supported Native mask.
std::vector<SurfaceAlphaMode> RuntimeAlphaModes(VkCompositeAlphaFlagsKHR supported) {
  std::vector<SurfaceAlphaMode> modes;
  for (const auto& [bit, mode] :
       {std::pair{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, SurfaceAlphaMode::Opaque},
        std::pair{VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, SurfaceAlphaMode::Premultiplied},
        std::pair{VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, SurfaceAlphaMode::Inherit}}) {
    if ((supported & bit) != 0) {
      modes.push_back(mode);
    }
  }
  return modes;
}

/// How many images to ask a swapchain for: one more than the surface's minimum, so a frame can be
/// drawn while another is on screen, clamped to whatever maximum it names.
/// @param native Capabilities the surface reported.
uint32_t ChooseImageCount(const VkSurfaceCapabilitiesKHR& native) {
  const uint32_t preferred = native.minImageCount + 1;
  if (native.maxImageCount != 0 && preferred > native.maxImageCount) {
    return native.maxImageCount;
  }
  return preferred;
}

/// The extent a swapchain must be created at, or a refusal naming what the surface will accept.
///
/// A surface that dictates its own extent (0xFFFFFFFF means it does not) is authoritative: a
/// swapchain created at any other size is refused by the driver, so the configuration is what is
/// wrong, not the request.
///
/// @param native Capabilities the surface reported. @param configuration Requested configuration.
Result<VkExtent2D> ResolveExtent(const VkSurfaceCapabilitiesKHR& native,
                                 const SurfaceConfiguration& configuration) {
  if (native.currentExtent.width != 0xFFFFFFFFu) {
    if (native.currentExtent.width != configuration.size.width ||
        native.currentExtent.height != configuration.size.height) {
      return GpuError{GpuErrorType::InvalidDescriptor,
                      std::format("configureSurface: this surface presents at {}x{}, not {}x{}",
                                  native.currentExtent.width, native.currentExtent.height,
                                  configuration.size.width, configuration.size.height)};
    }
    return native.currentExtent;
  }

  const VkExtent2D extent = {configuration.size.width, configuration.size.height};
  if (extent.width < native.minImageExtent.width || extent.height < native.minImageExtent.height ||
      extent.width > native.maxImageExtent.width || extent.height > native.maxImageExtent.height) {
    return GpuError{
        GpuErrorType::InvalidDescriptor,
        std::format("configureSurface: {}x{} is outside the {}x{} to {}x{} this surface presents",
                    configuration.size.width, configuration.size.height,
                    native.minImageExtent.width, native.minImageExtent.height,
                    native.maxImageExtent.width, native.maxImageExtent.height)};
  }
  return extent;
}

/// The runtime status a native presentation result reports, or nothing when the result is not
/// one presentation defines. @param result Native result.
std::optional<SurfaceStatus> RuntimeStatus(VkResult result) {
  switch (result) {
    case VK_SUCCESS: return SurfaceStatus::Success;
    case VK_SUBOPTIMAL_KHR: return SurfaceStatus::Outdated;
    case VK_ERROR_OUT_OF_DATE_KHR: return SurfaceStatus::Outdated;
    case VK_ERROR_SURFACE_LOST_KHR: return SurfaceStatus::Lost;
    case VK_ERROR_DEVICE_LOST: return SurfaceStatus::DeviceLost;
    case VK_TIMEOUT:
    case VK_NOT_READY: return SurfaceStatus::Timeout;
    default: return std::nullopt;
  }
}

}  // namespace

Result<std::unique_ptr<VulkanSwapchain>> VulkanSwapchain::Create(
    const VulkanSurfaceContext& context, const SurfaceDescriptor& descriptor) {
  const VulkanApi& api = *context.api;
  if (api.vkCreateSwapchainKHR == nullptr) {
    return GpuError{GpuErrorType::Unsupported,
                    "createSurface: this device was created without presentation support"};
  }

  Result<CreatedSurface> created = SurfaceForDescriptor(api, context.instance, descriptor);
  if (created.hasError()) {
    return std::move(created).error();
  }
  const CreatedSurface surface = created.result();

  // A device whose queue cannot present to this surface would fail every acquisition later, in a
  // place that does not say why, so it is refused here where the surface is still the subject.
  // For a surface the embedder created, this is also the first call that touches it, so one made
  // against another instance fails here rather than at a frame.
  VkBool32 supported = VK_FALSE;
  const VkResult supportResult = api.vkGetPhysicalDeviceSurfaceSupportKHR(
      context.physicalDevice, context.queueFamilyIndex, surface.surface, &supported);
  if (supportResult != VK_SUCCESS || supported != VK_TRUE) {
    if (surface.owned) {
      api.vkDestroySurfaceKHR(context.instance, surface.surface, nullptr);
    }
    if (supportResult != VK_SUCCESS) {
      return VkError("vkGetPhysicalDeviceSurfaceSupportKHR", supportResult);
    }
    return GpuError{GpuErrorType::Unsupported,
                    std::format("createSurface: queue family {} cannot present to this surface",
                                context.queueFamilyIndex)};
  }

  return std::unique_ptr<VulkanSwapchain>(
      new VulkanSwapchain(context, surface.surface, surface.owned));
}

VulkanSwapchain::VulkanSwapchain(const VulkanSurfaceContext& context, VkSurfaceKHR surface,
                                 bool ownsSurface)
    : context_(context), surface_(surface), ownsSurface_(ownsSurface) {}

VulkanSwapchain::~VulkanSwapchain() {
  drainPendingSubmissions();
  destroySwapchain();
  // An embedder's surface outlives its swapchain: the library that made it destroys it, usually
  // with the window, and doing it here would destroy an object that library still tracks.
  if (surface_ != VK_NULL_HANDLE && ownsSurface_) {
    context_.api->vkDestroySurfaceKHR(context_.instance, surface_, nullptr);
  }
  surface_ = VK_NULL_HANDLE;
}

Result<SurfaceCapabilities> VulkanSwapchain::capabilities() const {
  const VulkanApi& api = *context_.api;

  VkSurfaceCapabilitiesKHR native = {};
  if (const VkResult result =
          api.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_.physicalDevice, surface_, &native);
      result != VK_SUCCESS) {
    return VkError("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
  }

  Result<std::vector<VkSurfaceFormatKHR>> nativeFormats =
      QuerySurfaceFormats(api, context_.physicalDevice, surface_);
  if (nativeFormats.hasError()) {
    return std::move(nativeFormats).error();
  }
  Result<std::vector<VkPresentModeKHR>> nativeModes =
      QueryPresentModes(api, context_.physicalDevice, surface_);
  if (nativeModes.hasError()) {
    return std::move(nativeModes).error();
  }

  SurfaceCapabilities capabilities;
  capabilities.formats = RuntimeFormats(nativeFormats.result());
  capabilities.usages = RuntimeImageUsage(native.supportedUsageFlags);
  capabilities.presentModes = RuntimePresentModes(nativeModes.result());
  capabilities.alphaModes = RuntimeAlphaModes(native.supportedCompositeAlpha);
  return capabilities;
}

Status VulkanSwapchain::configure(const SurfaceConfiguration& configuration) {
  // Checked against what this surface reports rather than against a second list, so a
  // configuration is accepted exactly when capabilities() said it would be.
  Result<SurfaceCapabilities> supported = capabilities();
  if (supported.hasError()) {
    return std::move(supported).error();
  }
  const SurfaceCapabilities& caps = supported.result();

  if (std::ranges::find(caps.formats, configuration.format) == caps.formats.end()) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("configureSurface: this surface does not present {}",
                                Describe(configuration.format))};
  }
  if (!HasAllFlags(caps.usages, configuration.usage)) {
    return GpuError{GpuErrorType::Unsupported,
                    "configureSurface: this surface's images do not carry every requested usage"};
  }
  if (std::ranges::find(caps.presentModes, configuration.presentMode) == caps.presentModes.end()) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("configureSurface: this surface does not pace frames as {}",
                                Describe(configuration.presentMode))};
  }
  if (std::ranges::find(caps.alphaModes, configuration.alphaMode) == caps.alphaModes.end()) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("configureSurface: this surface does not composite alpha as {}",
                                Describe(configuration.alphaMode))};
  }

  configuration_ = configuration;
  needsRecreation_ = false;
  if (Status status = createSwapchain(); status.hasError()) {
    configuration_.reset();  // Nothing was configured, so acquiring must say exactly that.
    return status;
  }
  return OkStatus();
}

Status VulkanSwapchain::createSwapchain() {
  const VulkanApi& api = *context_.api;
  const SurfaceConfiguration& configuration = *configuration_;

  if (swapchain_ != VK_NULL_HANDLE) {
    // The semaphores and images about to be released may still be named by submitted work, and a
    // discarded frame certainly is, so nothing is destroyed while the device could be reading it.
    if (const VkResult result = api.vkDeviceWaitIdle(context_.device); result != VK_SUCCESS) {
      return VkError("vkDeviceWaitIdle", result);
    }
    drainPendingSubmissions();
  }

  VkSurfaceCapabilitiesKHR native = {};
  if (const VkResult result =
          api.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_.physicalDevice, surface_, &native);
      result != VK_SUCCESS) {
    return VkError("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
  }

  const std::optional<VkFormat> format = PresentationFormat(configuration.format);
  if (!format.has_value()) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("configureSurface: no swapchain format presents {}",
                                Describe(configuration.format))};
  }

  Result<VkExtent2D> extent = ResolveExtent(native, configuration);
  if (extent.hasError()) {
    return std::move(extent).error();
  }

  VkSwapchainCreateInfoKHR swapchainInfo = {};
  swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapchainInfo.surface = surface_;
  swapchainInfo.minImageCount = ChooseImageCount(native);
  swapchainInfo.imageFormat = *format;
  swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  swapchainInfo.imageExtent = extent.result();
  swapchainInfo.imageArrayLayers = 1;
  swapchainInfo.imageUsage = NativeImageUsage(configuration.usage);
  swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  swapchainInfo.preTransform = native.currentTransform;
  swapchainInfo.compositeAlpha = NativeCompositeAlpha(configuration.alphaMode);
  swapchainInfo.presentMode = NativePresentMode(configuration.presentMode);
  swapchainInfo.clipped = VK_TRUE;
  swapchainInfo.oldSwapchain = swapchain_;

  VkSwapchainKHR created = VK_NULL_HANDLE;
  if (const VkResult result =
          api.vkCreateSwapchainKHR(context_.device, &swapchainInfo, nullptr, &created);
      result != VK_SUCCESS) {
    return VkError("vkCreateSwapchainKHR", result);
  }

  // Replacing the swapchain is what releases the images the old one owned, including any frame
  // that was discarded rather than presented.
  destroySwapchain();
  swapchain_ = created;
  extent_ = Extent2d{extent.result().width, extent.result().height};

  if (Status status = fetchSwapchainImages(); status.hasError()) {
    return status;
  }
  return createSyncObjects();
}

Status VulkanSwapchain::fetchSwapchainImages() {
  const VulkanApi& api = *context_.api;
  uint32_t imageCount = 0;
  if (const VkResult result =
          api.vkGetSwapchainImagesKHR(context_.device, swapchain_, &imageCount, nullptr);
      result != VK_SUCCESS) {
    return VkError("vkGetSwapchainImagesKHR", result);
  }
  images_.resize(imageCount);
  if (const VkResult result =
          api.vkGetSwapchainImagesKHR(context_.device, swapchain_, &imageCount, images_.data());
      result != VK_SUCCESS) {
    return VkError("vkGetSwapchainImagesKHR", result);
  }
  return OkStatus();
}

Status VulkanSwapchain::createSyncObjects() {
  const VulkanApi& api = *context_.api;
  VkSemaphoreCreateInfo semaphoreInfo = {};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  // One handover semaphore per image, so it is free to reuse exactly when that image comes back
  // around, and an acquisition ring one longer than the image count, so the slot being reused is
  // always one whose frame has already been presented or discarded.
  handoverSemaphores_.assign(images_.size(), VK_NULL_HANDLE);
  acquireSemaphores_.assign(images_.size() + 1, VK_NULL_HANDLE);
  for (std::vector<VkSemaphore>* group : {&handoverSemaphores_, &acquireSemaphores_}) {
    for (VkSemaphore& semaphore : *group) {
      if (const VkResult result =
              api.vkCreateSemaphore(context_.device, &semaphoreInfo, nullptr, &semaphore);
          result != VK_SUCCESS) {
        return VkError("vkCreateSemaphore", result);
      }
    }
  }
  acquireRingFences_.assign(acquireSemaphores_.size(), VK_NULL_HANDLE);
  acquireCount_ = 0;
  return OkStatus();
}

Status VulkanSwapchain::recreateSwapchain() {
  needsRecreation_ = false;
  return createSwapchain();
}

Status VulkanSwapchain::prepareForAcquire() {
  if (!configuration_.has_value() || swapchain_ == VK_NULL_HANDLE) {
    return GpuError{GpuErrorType::InvalidState,
                    "acquireCurrentTexture: the surface has not been configured"};
  }
  if (hasFrame_) {
    return GpuError{GpuErrorType::InvalidState,
                    "acquireCurrentTexture: the swapchain is still holding the frame it handed "
                    "out"};
  }

  pollPendingSubmissions();
  if (needsRecreation_) {
    return recreateSwapchain();
  }
  return OkStatus();
}

Status VulkanSwapchain::waitForAcquireRingSlot(size_t ringSlot) {
  // The slot's previous wait has to have executed before its semaphore is signalled again. The
  // ring is one longer than the image count, so in a steady frame loop this fence is already
  // signalled and the wait returns immediately.
  if (acquireRingFences_[ringSlot] == VK_NULL_HANDLE) {
    return OkStatus();
  }
  if (const VkResult result = context_.api->vkWaitForFences(
          context_.device, 1, &acquireRingFences_[ringSlot], VK_TRUE, kDrainTimeoutNanoseconds);
      result != VK_SUCCESS) {
    return VkError("vkWaitForFences (acquire ring)", result);
  }
  acquireRingFences_[ringSlot] = VK_NULL_HANDLE;
  return OkStatus();
}

Result<SurfaceStatus> VulkanSwapchain::acquire() {
  if (Status ready = prepareForAcquire(); ready.hasError()) {
    return std::move(ready).error();
  }

  const VulkanApi& api = *context_.api;
  const size_t ringSlot = static_cast<size_t>(acquireCount_ % acquireSemaphores_.size());
  if (Status ready = waitForAcquireRingSlot(ringSlot); ready.hasError()) {
    return std::move(ready).error();
  }

  uint32_t imageIndex = 0;
  VkResult result =
      std::exchange(forceNextAcquireOutOfDate_, false)
          ? VK_ERROR_OUT_OF_DATE_KHR
          : api.vkAcquireNextImageKHR(context_.device, swapchain_, kAcquireTimeoutNanoseconds,
                                      acquireSemaphores_[ringSlot], VK_NULL_HANDLE, &imageIndex);

  bool outgrown = result == VK_SUBOPTIMAL_KHR;
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    // No frame came with this result, and the contract's Outdated still carries one, so the
    // swapchain is rebuilt and the acquisition retried once. A caller that gets Outdated can
    // draw this frame and reconfigure at its own pace.
    if (Status rebuilt = recreateSwapchain(); rebuilt.hasError()) {
      return std::move(rebuilt).error();
    }
    outgrown = true;
    result = api.vkAcquireNextImageKHR(context_.device, swapchain_, kAcquireTimeoutNanoseconds,
                                       acquireSemaphores_[ringSlot], VK_NULL_HANDLE, &imageIndex);
  }

  const std::optional<SurfaceStatus> status = RuntimeStatus(result);
  if (!status.has_value()) {
    return VkError("vkAcquireNextImageKHR", result);
  }
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    return SurfaceStatus::Timeout;  // Still moving; the caller retries on the next frame.
  }
  if (CarriesNoFrame(*status)) {
    return *status;
  }

  imageIndex_ = imageIndex;
  hasFrame_ = true;
  pendingAcquireWait_ = acquireSemaphores_[ringSlot];
  ++acquireCount_;
  return outgrown ? SurfaceStatus::Outdated : SurfaceStatus::Success;
}

VkImage VulkanSwapchain::currentImage() const {
  return hasFrame_ && imageIndex_ < images_.size() ? images_[imageIndex_] : VK_NULL_HANDLE;
}

SurfaceWaitSync VulkanSwapchain::takeAcquireWait() {
  SurfaceWaitSync sync;
  if (pendingAcquireWait_ == VK_NULL_HANDLE) {
    return sync;
  }
  sync.semaphores.push_back(pendingAcquireWait_);
  sync.stages.push_back(kAcquireWaitStage);
  pendingAcquireWait_ = VK_NULL_HANDLE;
  return sync;
}

Status VulkanSwapchain::submitFrameHandover(const std::optional<TextureSyncState>& state,
                                            VkSemaphore signalSemaphore) {
  const VulkanApi& api = *context_.api;

  VkCommandBufferAllocateInfo allocateInfo = {};
  allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocateInfo.commandPool = context_.commandPool;
  allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocateInfo.commandBufferCount = 1;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  if (const VkResult result =
          api.vkAllocateCommandBuffers(context_.device, &allocateInfo, &commandBuffer);
      result != VK_SUCCESS) {
    return VkError("vkAllocateCommandBuffers (present)", result);
  }

  VkCommandBufferBeginInfo beginInfo = {};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (const VkResult result = api.vkBeginCommandBuffer(commandBuffer, &beginInfo);
      result != VK_SUCCESS) {
    api.vkFreeCommandBuffers(context_.device, context_.commandPool, 1, &commandBuffer);
    return VkError("vkBeginCommandBuffer (present)", result);
  }

  if (state.has_value()) {
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = state->access;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = state->layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images_[imageIndex_];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    // The presentation engine is outside the pipeline, so the destination scope is the bottom of
    // it with no access to make visible; the semaphore signalled below carries the rest.
    api.vkCmdPipelineBarrier(commandBuffer, state->stage, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
  }

  if (const VkResult result = api.vkEndCommandBuffer(commandBuffer); result != VK_SUCCESS) {
    api.vkFreeCommandBuffers(context_.device, context_.commandPool, 1, &commandBuffer);
    return VkError("vkEndCommandBuffer (present)", result);
  }

  VkFence fence = VK_NULL_HANDLE;
  VkFenceCreateInfo fenceInfo = {};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  if (const VkResult result = api.vkCreateFence(context_.device, &fenceInfo, nullptr, &fence);
      result != VK_SUCCESS) {
    api.vkFreeCommandBuffers(context_.device, context_.commandPool, 1, &commandBuffer);
    return VkError("vkCreateFence (present)", result);
  }

  // Whatever is left of this frame's acquisition wait rides here: if no submission ever drew
  // into the frame, this is the one that consumes the semaphore, so it is never left signalled.
  const SurfaceWaitSync wait = takeAcquireWait();

  VkSubmitInfo submitInfo = {};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.waitSemaphoreCount = static_cast<uint32_t>(wait.semaphores.size());
  submitInfo.pWaitSemaphores = wait.semaphores.empty() ? nullptr : wait.semaphores.data();
  submitInfo.pWaitDstStageMask = wait.stages.empty() ? nullptr : wait.stages.data();
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;
  submitInfo.signalSemaphoreCount = signalSemaphore == VK_NULL_HANDLE ? 0u : 1u;
  submitInfo.pSignalSemaphores = signalSemaphore == VK_NULL_HANDLE ? nullptr : &signalSemaphore;

  if (const VkResult result = api.vkQueueSubmit(context_.queue, 1, &submitInfo, fence);
      result != VK_SUCCESS) {
    api.vkDestroyFence(context_.device, fence, nullptr);
    api.vkFreeCommandBuffers(context_.device, context_.commandPool, 1, &commandBuffer);
    return VkError("vkQueueSubmit (present)", result);
  }

  // The ring slot whose semaphore this submission waited on is reusable once this fence signals.
  const size_t ringSlot = static_cast<size_t>((acquireCount_ - 1) % acquireSemaphores_.size());
  acquireRingFences_[ringSlot] = fence;
  pending_.push_back(PendingSubmission{fence, commandBuffer});
  return OkStatus();
}

Result<SurfaceStatus> VulkanSwapchain::present(const TextureSyncState& state) {
  if (!hasFrame_) {
    return GpuError{GpuErrorType::InvalidState, "presentSurface: no frame is being held"};
  }

  const VkSemaphore handover = handoverSemaphores_[imageIndex_];
  if (Status status = submitFrameHandover(state, handover); status.hasError()) {
    // The frame is still the swapchain's to reclaim, and only a new swapchain does that.
    hasFrame_ = false;
    needsRecreation_ = true;
    return std::move(status).error();
  }

  VkPresentInfoKHR presentInfo = {};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = &handover;
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = &swapchain_;
  presentInfo.pImageIndices = &imageIndex_;

  const VkResult result = context_.api->vkQueuePresentKHR(context_.queue, &presentInfo);
  hasFrame_ = false;

  const std::optional<SurfaceStatus> status = RuntimeStatus(result);
  if (!status.has_value()) {
    return VkError("vkQueuePresentKHR", result);
  }
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    needsRecreation_ = true;
  }
  return *status;
}

Status VulkanSwapchain::abandon() {
  if (!hasFrame_) {
    return OkStatus();
  }

  // Vulkan has no operation that gives an acquired image back, so the frame stays out until the
  // swapchain that owns it is replaced. The acquisition wait still has to be consumed, which is
  // what this submission is for.
  const Status status = submitFrameHandover(std::nullopt, VK_NULL_HANDLE);
  hasFrame_ = false;
  needsRecreation_ = true;
  return status;
}

void VulkanSwapchain::pollPendingSubmissions() {
  const VulkanApi& api = *context_.api;
  auto it = pending_.begin();
  while (it != pending_.end()) {
    if (api.vkGetFenceStatus(context_.device, it->fence) != VK_SUCCESS) {
      ++it;
      continue;
    }
    if (std::ranges::find(acquireRingFences_, it->fence) == acquireRingFences_.end()) {
      api.vkFreeCommandBuffers(context_.device, context_.commandPool, 1, &it->commandBuffer);
      api.vkDestroyFence(context_.device, it->fence, nullptr);
      it = pending_.erase(it);
    } else {
      ++it;  // Still naming a ring slot's reuse point; released when that slot is reused.
    }
  }
}

void VulkanSwapchain::drainPendingSubmissions() {
  const VulkanApi& api = *context_.api;
  std::vector<VkFence> fences;
  for (const PendingSubmission& submission : pending_) {
    fences.push_back(submission.fence);
  }
  if (!fences.empty()) {
    api.vkWaitForFences(context_.device, static_cast<uint32_t>(fences.size()), fences.data(),
                        VK_TRUE, kDrainTimeoutNanoseconds);
  }
  for (PendingSubmission& submission : pending_) {
    api.vkFreeCommandBuffers(context_.device, context_.commandPool, 1, &submission.commandBuffer);
    api.vkDestroyFence(context_.device, submission.fence, nullptr);
  }
  pending_.clear();
  std::ranges::fill(acquireRingFences_, VK_NULL_HANDLE);
}

void VulkanSwapchain::destroySwapchain() {
  const VulkanApi& api = *context_.api;
  for (VkSemaphore semaphore : handoverSemaphores_) {
    if (semaphore != VK_NULL_HANDLE) {
      api.vkDestroySemaphore(context_.device, semaphore, nullptr);
    }
  }
  handoverSemaphores_.clear();
  for (VkSemaphore semaphore : acquireSemaphores_) {
    if (semaphore != VK_NULL_HANDLE) {
      api.vkDestroySemaphore(context_.device, semaphore, nullptr);
    }
  }
  acquireSemaphores_.clear();
  acquireRingFences_.clear();
  images_.clear();
  hasFrame_ = false;
  pendingAcquireWait_ = VK_NULL_HANDLE;
  if (swapchain_ != VK_NULL_HANDLE) {
    api.vkDestroySwapchainKHR(context_.device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
}

}  // namespace donner::gpu::vulkan
