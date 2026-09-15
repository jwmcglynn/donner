#pragma once
/// @file
/// \c donner::gpu::vulkan::VulkanSwapchain - a Vulkan surface, its swapchain, and the
/// synchronization one frame of presentation needs.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "donner/gpu/Descriptors.h"
#include "donner/gpu/GpuResult.h"
#include "donner/gpu/vulkan/VulkanLoader.h"
#include "donner/gpu/vulkan/VulkanResourceState.h"

namespace donner::gpu::vulkan {

/// What a swapchain borrows from the device that owns it. None of these are owned here, and all
/// of them outlive the swapchain.
struct VulkanSurfaceContext {
  const VulkanApi* api = nullptr;                    //!< Entry points, presentation group loaded.
  VkInstance instance = VK_NULL_HANDLE;              //!< Instance the surface is created on.
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;  //!< Device the surface is queried against.
  VkDevice device = VK_NULL_HANDLE;                  //!< Logical device.
  VkQueue queue = VK_NULL_HANDLE;                    //!< Queue frames are presented on.
  uint32_t queueFamilyIndex = 0;                     //!< Family of \ref queue.
  VkCommandPool commandPool = VK_NULL_HANDLE;        //!< Pool the present barrier is recorded in.
};

/// The stage an acquisition wait applies to, and therefore the earliest stage at which a frame
/// may be written.
///
/// Shared by the wait itself and by the synchronization state a freshly acquired frame starts in,
/// so the two cannot drift: a frame whose first barrier claimed an earlier source stage would be
/// writing the image outside what the wait covers.
inline constexpr VkPipelineStageFlags kAcquireWaitStage =
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

/// The synchronization state a frame is in the moment it is acquired.
///
/// Its contents are undefined, and the last thing to have touched it is the presentation engine's
/// read, which the acquisition semaphore orders against \ref kAcquireWaitStage. Recording that
/// stage rather than the top of the pipe is what places the frame's first layout transition after
/// the wait; a transition from the top of the pipe is a write the wait does not cover, which
/// synchronization validation reports as a write-after-read hazard against the presentation
/// engine.
inline TextureSyncState AcquiredFrameSyncState() {
  return TextureSyncState{VK_IMAGE_LAYOUT_UNDEFINED, kAcquireWaitStage, 0};
}

/// Semaphores a submission must wait on, with the stages that wait applies to.
///
/// A frame comes back from the presentation engine before the engine has necessarily finished
/// reading it, so the first submission that writes the frame waits here rather than assuming the
/// image is free.
struct SurfaceWaitSync {
  std::vector<VkSemaphore> semaphores;       //!< Semaphores to wait on.
  std::vector<VkPipelineStageFlags> stages;  //!< Stage each wait applies to, index-matched.

  /// Whether there is nothing to wait on.
  bool empty() const { return semaphores.empty(); }
};

/**
 * One surface and the swapchain it presents through.
 *
 * Presentation on Vulkan is three things the runtime's surface contract has to keep straight.
 *
 * The frame is acquired with a semaphore, because the image comes back before the presentation
 * engine has finished with it. Whichever submission first writes the frame waits on that
 * semaphore (\ref takeAcquireWait hands it over); if nothing ever writes the frame, the
 * submission that presents or discards it does the waiting instead, so the semaphore is consumed
 * exactly once either way and never left signalled.
 *
 * The frame is presented from a layout the presentation engine can read, which nothing else uses,
 * so \ref present records the barrier into it and submits that barrier itself. Because the
 * queue runs its submissions in order, that barrier also carries the dependency on every earlier
 * submission of the frame, which is why any number of submissions may draw into one frame
 * without each having to signal.
 *
 * A frame that is acquired cannot be handed back: Vulkan has no operation that returns an image
 * to its swapchain other than presenting it. \ref abandon therefore strands the image and marks
 * the swapchain for recreation, and the next acquisition recreates it - which is what actually
 * reclaims the stranded frame.
 */
class VulkanSwapchain {
public:
  /**
   * Creates the surface \p descriptor names. The swapchain itself follows from \ref configure.
   *
   * @param context Borrowed device objects.
   * @param descriptor Label and platform object.
   */
  static Result<std::unique_ptr<VulkanSwapchain>> Create(const VulkanSurfaceContext& context,
                                                         const SurfaceDescriptor& descriptor);

  /// Destructor; waits for the device to go idle, then destroys the swapchain, and the surface
  /// too when this object created it.
  ~VulkanSwapchain();

  VulkanSwapchain(const VulkanSwapchain&) = delete;
  VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

  /// What this surface supports, read from the physical device. Every value reported is accepted
  /// by \ref configure, and every value absent from it is refused.
  Result<SurfaceCapabilities> capabilities() const;

  /**
   * Creates the swapchain \p configuration describes, replacing any previous one.
   *
   * @param configuration Format, usage, extent, pacing and alpha compositing.
   */
  Status configure(const SurfaceConfiguration& configuration);

  /**
   * Acquires the next frame.
   *
   * A swapchain the presentation engine has outgrown is recreated and the acquisition retried
   * once, so an \ref SurfaceStatus::Outdated result still carries a frame the caller can draw
   * and present while it decides whether to reconfigure.
   */
  Result<SurfaceStatus> acquire();

  /// Image of the frame currently held, or null when none is.
  VkImage currentImage() const;

  /// Hands over the wait the next submission must carry, and marks it taken. Empty once taken,
  /// or when no frame is held.
  SurfaceWaitSync takeAcquireWait();

  /// Gives a taken wait back, for a submission that was never handed to the queue.
  ///
  /// A semaphore that was claimed for a submission the driver refused is neither waited on nor
  /// pending: it stays signalled, and the next acquisition to reuse it signals a semaphore that
  /// is already signalled. Returning it keeps it owed instead.
  ///
  /// @param semaphore Semaphore this surface handed out.
  void restoreAcquireWait(VkSemaphore semaphore);

  /// The texture slot the runtime gave this surface's frame, for deciding which submissions are
  /// writing it. Empty while no frame is held.
  const std::optional<uint32_t>& frameTextureSlot() const { return frameTextureSlot_; }

  /// Records the texture slot the runtime gave the current frame.
  /// @param textureSlot Slot the frame occupies.
  void setFrameTextureSlot(uint32_t textureSlot) { frameTextureSlot_ = textureSlot; }

  /**
   * Presents the frame currently held and releases it.
   *
   * @param state Synchronization state the frame's image was left in, which is the source scope
   *   of the barrier into the presentation engine's layout.
   */
  Result<SurfaceStatus> present(const TextureSyncState& state);

  /// Discards the frame currently held without presenting it, marking the swapchain for the
  /// recreation that reclaims it.
  Status abandon();

  /// Makes the next acquisition report the swapchain as out of date, before it asks for an image.
  ///
  /// Test seam. A presentation engine decides on its own when a swapchain has been outgrown, and
  /// a headless surface has no window to resize, so the retry path has no other way to run. The
  /// injected result stands in for one that acquires nothing and signals nothing, which is what
  /// an out-of-date acquisition does.
  void forceNextAcquireOutOfDateForTest() { forceNextAcquireOutOfDate_ = true; }

  /// Configuration the swapchain was created with, or nothing before it is configured.
  const std::optional<SurfaceConfiguration>& configuration() const { return configuration_; }

  /// Extent the swapchain was created with. Zero until \ref configure succeeds.
  Extent2d extent() const { return extent_; }

private:
  /// Constructs a swapchain bound to \p surface.
  /// @param context Borrowed objects. @param surface Created surface.
  /// @param ownsSurface Whether destroying this also destroys \p surface.
  VulkanSwapchain(const VulkanSurfaceContext& context, VkSurfaceKHR surface, bool ownsSurface);

  /// One submission this swapchain made on the caller's behalf, awaiting its fence.
  struct PendingSubmission {
    VkFence fence = VK_NULL_HANDLE;                  //!< Fence signalled when it completes.
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;  //!< Command buffer to free afterwards.
  };

  /// Creates or replaces the swapchain from \ref configuration_, leaving the surface
  /// unconfigured if anything fails once the previous swapchain has been let go.
  Status createSwapchain();

  /// The body of \ref createSwapchain, whose failures it turns into an unconfigured surface.
  Status createSwapchainUnguarded();

  /// Whether a frame is held and its index addresses this swapchain's images and semaphores.
  /// Every index of either is guarded by this, so no path assumes what another checks.
  bool hasAddressableFrame() const;

  /// Reads back the images of the newly created swapchain.
  Status fetchSwapchainImages();

  /// Creates the per-image handover semaphores and the acquisition ring.
  Status createSyncObjects();

  /// Refuses an acquisition the surface is not in a state to serve, and rebuilds the swapchain
  /// when a discarded frame or an outdated presentation engine left one owed.
  Status prepareForAcquire();

  /// Waits for the fence recorded against \p ringSlot, so its semaphore is free to signal again.
  /// @param ringSlot Acquisition ring slot about to be reused.
  Status waitForAcquireRingSlot(size_t ringSlot);

  /// Recreates the swapchain, reclaiming any stranded frame. Waits for the device to go idle
  /// first, because the images being released may still be named by submitted work.
  Status recreateSwapchain();

  /// Records the barrier into the layout the presentation engine reads, for a frame being handed
  /// over. Records nothing for a frame being discarded, which the engine never reads.
  /// @param commandBuffer Command buffer to record into. @param state Source synchronization
  ///   state, or nothing for a discard.
  void recordHandoverBarrier(VkCommandBuffer commandBuffer,
                             const std::optional<TextureSyncState>& state);

  /// Records and submits the barrier into the presentation engine's layout, plus whatever wait
  /// this frame still owes. @param state Source synchronization state, or nothing for a discard.
  Status submitFrameHandover(const std::optional<TextureSyncState>& state,
                             VkSemaphore signalSemaphore);

  /// Reaps completed submissions without blocking.
  void pollPendingSubmissions();

  /// Waits for every submission this swapchain made, then releases their objects.
  void drainPendingSubmissions();

  /// Releases the swapchain, its per-image semaphores, and the acquire ring.
  void destroySwapchain();

  VulkanSurfaceContext context_;           //!< Borrowed device objects.
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;  //!< The surface presented to.
  /// Whether \ref surface_ is this object's to destroy. False for a surface the embedder created
  /// and still owns, which its windowing library generally destroys with the window.
  bool ownsSurface_ = true;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;  //!< Owned swapchain, or null before configuring.
  std::optional<SurfaceConfiguration> configuration_;  //!< Applied configuration, if any.
  Extent2d extent_;                                    //!< Extent the swapchain was created with.

  std::vector<VkImage> images_;  //!< Swapchain images; owned by the swapchain, not by this.
  /// One semaphore per swapchain image, signalled by the submission that hands the image over
  /// and waited on by the present. Indexed by image index, so it is free to reuse exactly when
  /// that image comes back around.
  std::vector<VkSemaphore> handoverSemaphores_;
  /// Ring of acquisition semaphores, one longer than the image count so the slot being reused is
  /// always one whose frame has already been presented or discarded.
  std::vector<VkSemaphore> acquireSemaphores_;
  /// Fence recorded for each acquire-ring slot when its semaphore was last consumed; waited on
  /// before that slot is reused.
  std::vector<VkFence> acquireRingFences_;
  uint64_t acquireCount_ = 0;  //!< Total acquisitions, which selects the ring slot.
  /// Ring slot the current frame was acquired on. Carried with the frame rather than recomputed
  /// from \ref acquireCount_ when the frame ends: a rebuild restarts that counter, so recomputing
  /// would file this frame's fence under a slot whose semaphore was never signalled for it.
  size_t frameRingSlot_ = 0;

  /// Texture slot the runtime gave the current frame, so a submission can be matched to the
  /// surface whose frame it writes rather than to every surface at once.
  std::optional<uint32_t> frameTextureSlot_;

  uint32_t imageIndex_ = 0;                          //!< Index of the frame currently held.
  bool hasFrame_ = false;                            //!< Whether a frame is currently held.
  VkSemaphore pendingAcquireWait_ = VK_NULL_HANDLE;  //!< Acquire semaphore nothing has taken yet.
  bool forceNextAcquireOutOfDate_ = false;           //!< One-shot injected out-of-date acquisition.
  /// True once a frame was discarded rather than presented: Vulkan reclaims it only when the
  /// swapchain that owns it is replaced.
  bool needsRecreation_ = false;

  std::vector<PendingSubmission> pending_;  //!< Submissions awaiting their fences.
};

}  // namespace donner::gpu::vulkan
