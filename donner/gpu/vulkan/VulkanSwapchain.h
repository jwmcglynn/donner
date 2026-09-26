#pragma once
/// @file
/// \c donner::gpu::vulkan::VulkanSwapchain - a Vulkan surface, its swapchain, and the
/// synchronization one frame of presentation needs.

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "donner/gpu/Descriptors.h"
#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/GpuResult.h"
#include "donner/gpu/vulkan/VulkanLoader.h"
#include "donner/gpu/vulkan/VulkanResourceState.h"

namespace donner::gpu::vulkan {

class VulkanSwapchainTestAccess;
class VulkanSurfaceRetirement;

/// Preallocated owner/child teardown state shared by every surface of one device.
///
/// A child whose native completion cannot be proved poisons the state without allocating. The
/// owner observes that poison and retains the complete device graph, including every prerequisite
/// of leaked native handles. The live count catches a child that escaped the owner's containers.
struct VulkanSurfaceLifetime {
  std::atomic<bool> unproven{
      false};  //!< Whether outstanding surface work lacks a completion proof.
  std::atomic<size_t> liveChildren{
      0};  //!< Number of live swapchain children retaining this surface lifetime.
};

/// Converts native surface formats into the formats the runtime can present.
/// Exposed for deterministic capability tests whose driver cannot advertise the wildcard.
/// @param nativeFormats Formats reported by the presentation engine.
std::vector<TextureFormat> RuntimeSurfaceFormatsForTest(
    const std::vector<VkSurfaceFormatKHR>& nativeFormats);

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
  std::shared_ptr<VulkanSurfaceLifetime> lifetime;   //!< Shared owner-retention state.
  std::mutex* queueMutex = nullptr;  //!< Shared VkQueue call lock; null for fake test contexts.
  std::shared_ptr<DeviceLostState> rootLoss;  //!< Shared loss condition of the owning root.
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
/// read, which the acquisition semaphore orders against \ref donner::gpu::vulkan::kAcquireWaitStage
/// "kAcquireWaitStage". Recording that stage rather than the top of the pipe is what places the
/// frame's first layout transition after the wait; a transition from the top of the pipe is a write
/// the wait does not cover, which synchronization validation reports as a write-after-read hazard
/// against the presentation engine.
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

  /// Destructor; proves this surface's native work complete, then destroys the swapchain and the
  /// surface too when this object created it. A failed proof poisons the shared lifetime state so
  /// the owner retains all native prerequisites fail-closed.
  ~VulkanSwapchain();

  VulkanSwapchain(const VulkanSwapchain&) = delete;
  VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

  /// Proves that every native object owned by this surface is no longer in use.
  ///
  /// This is the first phase of teardown: it may enqueue and wait for the submission needed to
  /// consume an outstanding acquisition semaphore, but it does not destroy or reset native
  /// objects. The owner can therefore prepare every sibling before destroying any of them.
  Status prepareForDestruction();

  /// Binds the exact external-surface retirement record after backend acceptance.
  /// @param retirement One-shot record owned by the shared root and embedder.
  void setRetirementSignal(std::shared_ptr<VulkanSurfaceRetirement> retirement) {
    retirement_ = std::move(retirement);
  }

  /// Marks platform prerequisites permanently retained after destruction proof fails.
  void markRetirementUnproven();

  /// Retains \p next behind this surface while failed teardown quarantines a whole owner.
  void retainBefore(std::unique_ptr<VulkanSwapchain> next) { retainedNext_ = std::move(next); }

  /// Unlinks and returns the next retained surface.
  std::unique_ptr<VulkanSwapchain> retainedNext() { return std::move(retainedNext_); }

  /// Borrows the next retained surface while the owner prepares the complete chain.
  VulkanSwapchain* retainedNextForPreparation() const { return retainedNext_.get(); }

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
  void setFrameTextureSlot(uint32_t textureSlot) {
    preparedForDestruction_ = false;
    frameTextureSlot_ = textureSlot;
  }

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

  /**
   * Has \p callback called after each queue submission of the swapchain's own that the queue
   * accepted. That is the handover barrier that ends a frame, presented or abandoned, including
   * the one that only consumes an acquisition nothing drew into. The owning device reports each as
   * queue work it did not submit through `Device::submit`.
   *
   * @param callback Callback, or empty for none.
   */
  void setQueueSubmissionCallback(std::function<void()> callback) {
    queueSubmissionCallback_ = std::move(callback);
  }

  /// Makes the next acquisition report the swapchain as out of date, before it asks for an image.
  ///
  /// Test seam. A presentation engine decides on its own when a swapchain has been outgrown, and
  /// a headless surface has no window to resize, so the retry path has no other way to run. The
  /// injected result stands in for one that acquires nothing and signals nothing, which is what
  /// an out-of-date acquisition does.
  void forceNextAcquireOutOfDateForTest() { forceNextAcquireOutOfDate_ = true; }

  /// Makes the next swapchain creation ask for the fewest images this surface allows, so that a
  /// rebuild may produce a smaller acquisition ring than the one it replaces.
  ///
  /// Test seam, and only ever an ask: the count handed to a swapchain is a minimum, so an
  /// implementation is free to return more, and one that always returns the same number gives the
  /// same ring back. Measured on a software rasterizer it does exactly that, which is why the
  /// case using this asserts the bounds invariant rather than claiming to have shrunk anything.
  void forceMinimumImageCountOnceForTest() { forceMinimumImageCount_ = true; }

  /// Whether this surface still owes the acquisition wait for the frame it holds.
  ///
  /// Test accessor. Which submission takes that wait is the contract, and a submission that took
  /// one it had no business taking leaves no other trace on a single in-order queue.
  [[nodiscard]] bool owesAcquireWaitForTest() const {
    return pendingAcquireWait_ != VK_NULL_HANDLE;
  }

  /// The acquisition-ring slot the frame currently held was acquired on. Test accessor.
  [[nodiscard]] size_t frameRingSlotForTest() const { return frameRingSlot_; }

  /// The ring slot the most recent frame handover filed its fence under, or nothing when none
  /// has. Test accessor: that slot has to be the one whose semaphore the frame used, and the two
  /// drifting apart has only a timing-dependent symptom.
  [[nodiscard]] const std::optional<size_t>& lastFencedRingSlotForTest() const {
    return lastFencedRingSlot_;
  }

  /// Number of slots in the acquisition ring. Test accessor, for a test that has to land on a
  /// particular slot without knowing how many images the driver gave this swapchain.
  [[nodiscard]] size_t acquireRingSizeForTest() const { return acquireSemaphores_.size(); }

  /// Configuration the swapchain was created with, or nothing before it is configured.
  const std::optional<SurfaceConfiguration>& configuration() const { return configuration_; }

  /// Extent the swapchain was created with. Zero until \ref configure succeeds.
  Extent2d extent() const { return extent_; }

private:
  friend class VulkanSwapchainTestAccess;

  /// Constructs a swapchain bound to \p surface.
  /// @param context Borrowed objects. @param surface Created surface.
  /// @param ownsSurface Whether destroying this also destroys \p surface.
  VulkanSwapchain(const VulkanSurfaceContext& context, VkSurfaceKHR surface, bool ownsSurface);

  /// Publishes a driver-reported loss on the shared root after the caller records any native
  /// ownership it must retain. @param result Native result. @param reason Diagnostic if lost.
  void declareDeviceLoss(VkResult result, const char* reason) const;

  /// One submission this swapchain made on the caller's behalf, awaiting its fence.
  struct PendingSubmission {
    VkFence fence = VK_NULL_HANDLE;                  //!< Fence signalled when it completes.
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;  //!< Command buffer to free afterwards.
  };

  /// One native acquisition attempt after any out-of-date rebuild and retry.
  struct AcquireAttempt {
    VkResult result = VK_SUCCESS;
    uint32_t imageIndex = 0;
    size_t ringSlot = 0;
    bool outgrown = false;
  };

  /// Creates or replaces the swapchain from \ref configuration_, leaving the surface
  /// unconfigured if anything fails once the previous swapchain has been let go.
  Status createSwapchain();

  /// The body of \ref createSwapchain, whose failures it turns into an unconfigured surface.
  Status createSwapchainUnguarded();

  /// Proves the current generation idle and releases its submission objects before replacement.
  Status retireSwapchainGeneration();

  /// Whether a frame is held and its index addresses this swapchain's images and semaphores.
  /// Every index of either is guarded by this, so no path assumes what another checks.
  bool hasAddressableFrame() const;

  /// Reads back the images of the newly created swapchain.
  Status fetchSwapchainImages();

  /// Creates the per-image handover semaphores and the acquisition ring.
  Status createSyncObjects();

  /// Waits and resets the presentation fence for an image before that fence is reused.
  Status waitForPresentFence(uint32_t imageIndex);

  /// Waits every fence associated with an accepted presentation operation.
  Status drainPresentFences();

  /// Refuses an acquisition the surface is not in a state to serve, and rebuilds the swapchain
  /// when a discarded frame or an outdated presentation engine left one owed.
  Status prepareForAcquire();

  /// Waits for the fence recorded against \p ringSlot, so its semaphore is free to signal again.
  /// @param ringSlot Acquisition ring slot about to be reused.
  Status waitForAcquireRingSlot(size_t ringSlot);

  /// Acquires once, rebuilding and retrying once when the presentation engine is out of date.
  Result<AcquireAttempt> acquireImage();

  /// Recreates the swapchain, reclaiming any stranded frame. Waits for the device to go idle
  /// first, because the images being released may still be named by submitted work.
  Status recreateSwapchain();

  /// Records the barrier into the layout the presentation engine reads, for a frame being handed
  /// over. Records nothing for a frame being discarded, which the engine never reads.
  /// @param commandBuffer Command buffer to record into. @param state Source synchronization
  ///   state, or nothing for a discard.
  void recordHandoverBarrier(VkCommandBuffer commandBuffer,
                             const std::optional<TextureSyncState>& state);

  /// Records a handover command buffer and creates the fence that will own its completion.
  Result<PendingSubmission> recordHandoverSubmission(const std::optional<TextureSyncState>& state);

  /// Associates a possibly accepted handover submission with its acquisition-ring slot.
  void associateHandoverWithAcquireRing(VkFence fence);

  /// Applies queue-result ownership rules to a recorded handover submission.
  Status finishHandoverSubmission(VkResult result, const SurfaceWaitSync& wait,
                                  PendingSubmission submission);

  /// Records and submits the barrier into the presentation engine's layout, plus whatever wait
  /// this frame still owes. @param state Source synchronization state, or nothing for a discard.
  Status submitFrameHandover(const std::optional<TextureSyncState>& state,
                             VkSemaphore signalSemaphore);

  /// Reaps completed submissions without blocking.
  void pollPendingSubmissions();

  /// Waits for every submission this swapchain made, then releases their objects.
  Status drainPendingSubmissions();

  /// Waits for all pending submissions without releasing or resetting their native objects.
  Status provePendingSubmissionsComplete();

  /// Releases submission objects after completion has already been proved.
  void releasePreparedSubmissions();

  /// Releases the swapchain, its per-image semaphores, and the acquire ring.
  void destroySwapchain();

  /// See \ref setQueueSubmissionCallback.
  std::function<void()> queueSubmissionCallback_;

  VulkanSurfaceContext context_;  //!< Borrowed device objects.
  std::shared_ptr<VulkanSurfaceRetirement> retirement_;
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
  std::vector<VkFence> presentFences_;     //!< Completion fence for each image's latest present.
  std::vector<bool> presentFencePending_;  //!< Whether the corresponding fence was enqueued.

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
  bool forceMinimumImageCount_ = false;              //!< One-shot smallest-allowed image count.
  std::optional<size_t> lastFencedRingSlot_;         //!< Ring slot the last handover fenced.

  /// True once a frame was discarded rather than presented: Vulkan reclaims it only when the
  /// swapchain that owns it is replaced.
  bool needsRecreation_ = false;

  std::vector<PendingSubmission> pending_;  //!< Submissions awaiting their fences.
  bool preparationBlocked_ = false;      //!< An ambiguous native result prevents safe destruction.
  bool preparedForDestruction_ = false;  //!< Every native use has completion proof.
  std::unique_ptr<VulkanSwapchain> retainedNext_;  //!< Quarantined sibling chain.
};

}  // namespace donner::gpu::vulkan
