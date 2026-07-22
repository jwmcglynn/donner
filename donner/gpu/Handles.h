#pragma once
/// @file
/// Move-only RAII, generation-checked resource handles for \c donner::gpu.
///
/// Destroying a live handle releases its resource through the owning device (design 0053 "Core
/// types and ownership"). Teardown ordering is safe in every direction: an explicit
/// `Device::destroy*(std::move(handle))` call makes the RAII release a no-op, and a handle
/// destroyed after its device holds an expired device-alive token, so the release is skipped
/// (device teardown already freed everything that remained). Backend objects referenced by
/// in-flight submissions are additionally kept alive by the device's deferred-destruction queue,
/// so an RAII release never destroys a backend object the GPU is still using.

#include <cstdint>
#include <memory>
#include <ostream>

namespace donner::gpu {

class Device;

/**
 * Identity of a resource at a point in time: slot plus generation, so slot reuse cannot alias
 * two different resources. Recorded commands and dependent-resource records store identities and
 * re-validate them on every use.
 */
struct ResourceIdentity {
  uint32_t slotIndex = 0;   //!< Resource slot index.
  uint32_t generation = 0;  //!< Slot generation at capture time.

  /// Equality operator. @param other Identity to compare against.
  bool operator==(const ResourceIdentity& other) const = default;
};

namespace details {

/**
 * Releases the resource identified by (\p slotIndex, \p generation) on \p device if it is still
 * alive; silently a no-op when the identity is already stale (e.g. the resource was destroyed
 * explicitly). Called by `~Handle` only; specialized per tag in Device.cc.
 *
 * @param device Owning device.
 * @param slotIndex Resource slot index.
 * @param generation Slot generation the handle carries.
 */
template <typename Tag>
void ReleaseHandleFromRaii(Device& device, uint32_t slotIndex, uint32_t generation);

}  // namespace details

/**
 * Move-only RAII handle to a device resource.
 *
 * A handle carries a (slot, generation) pair plus the identity of the owning device. The device
 * validates all three on every use, so a null handle, a stale handle (the resource was destroyed,
 * possibly with its slot reused), or a handle from a different device fails closed with
 * \ref GpuErrorType::InvalidHandle / \ref GpuErrorType::DeviceMismatch instead of reaching a
 * backend. Validation runs in release builds too.
 *
 * Destroying a live handle releases the resource through the owning device (see the file comment
 * for the teardown-ordering contract). A default-constructed or moved-from handle is null
 * (\ref isValid() returns false) and releases nothing.
 *
 * @tparam Tag Tag type identifying the resource type; provides `Tag::kName` for diagnostics.
 */
template <typename Tag>
class Handle {
public:
  /// Constructs a null handle.
  Handle() = default;

  /// Destructor; releases the resource if this handle still owns it and the device is alive.
  ~Handle() { releaseIfOwned(); }

  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  /**
   * Move constructor; \p other becomes null.
   *
   * @param other Handle to move from.
   */
  Handle(Handle&& other) noexcept
      : slotIndex_(other.slotIndex_),
        generation_(other.generation_),
        deviceId_(other.deviceId_),
        deviceAlive_(std::move(other.deviceAlive_)) {
    other.resetToNull();
  }

  /**
   * Move assignment; releases the currently owned resource (if any), then takes ownership from
   * \p other, which becomes null.
   *
   * @param other Handle to move from.
   */
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      releaseIfOwned();
      slotIndex_ = other.slotIndex_;
      generation_ = other.generation_;
      deviceId_ = other.deviceId_;
      deviceAlive_ = std::move(other.deviceAlive_);
      other.resetToNull();
    }
    return *this;
  }

  /// Returns true if this handle is non-null. The device generation check is authoritative for
  /// whether the resource is still alive.
  bool isValid() const { return generation_ != 0; }

  /// Resource table slot index, for backend use.
  uint32_t slotIndex() const { return slotIndex_; }

  /// Slot generation at creation time, for backend use. Zero for null handles.
  uint32_t generation() const { return generation_; }

  /// Identity of the owning device (see `Device::deviceId()`), for backend use. Zero for null
  /// handles.
  uint64_t deviceId() const { return deviceId_; }

  /**
   * Mints a live handle; called by device backends only.
   *
   * @param slotIndex Resource table slot index.
   * @param generation Slot generation at creation time; must be nonzero.
   * @param deviceId Identity of the owning device.
   * @param deviceAlive Device-alive token (see `Device`); when empty, the handle is inert - its
   *   destructor never self-releases. `Device` always supplies the token; the empty default
   *   exists for handle-shape unit tests that have no device.
   */
  static Handle CreateForBackend(uint32_t slotIndex, uint32_t generation, uint64_t deviceId,
                                 std::weak_ptr<Device*> deviceAlive = {}) {
    Handle handle;
    handle.slotIndex_ = slotIndex;
    handle.generation_ = generation;
    handle.deviceId_ = deviceId;
    handle.deviceAlive_ = std::move(deviceAlive);
    return handle;
  }

  /**
   * Compares against null: true if the handle is null.
   *
   * @param handle Handle to test.
   */
  friend bool operator==(const Handle& handle, std::nullptr_t) { return !handle.isValid(); }

private:
  void releaseIfOwned() {
    if (generation_ == 0) {
      return;
    }
    if (std::shared_ptr<Device*> device = deviceAlive_.lock()) {
      details::ReleaseHandleFromRaii<Tag>(**device, slotIndex_, generation_);
    }
  }

  void resetToNull() {
    slotIndex_ = 0;
    generation_ = 0;
    deviceId_ = 0;
    deviceAlive_.reset();
  }

  uint32_t slotIndex_ = 0;
  uint32_t generation_ = 0;
  uint64_t deviceId_ = 0;
  std::weak_ptr<Device*> deviceAlive_;
};

/**
 * Copyable non-owning reference to a \ref Handle, used inside descriptor structs.
 *
 * Handles are move-only, so descriptors reference them through this identity-only view instead.
 * The device re-validates slot, generation, and device identity when the descriptor is consumed,
 * so a stale or foreign reference fails closed exactly like a stale or foreign handle.
 *
 * @tparam Tag Tag type identifying the resource type.
 */
template <typename Tag>
class HandleRef {
public:
  /// Constructs a null reference.
  HandleRef() = default;

  /**
   * Implicitly references \p handle. The reference does not keep the resource alive.
   *
   * @param handle Handle to reference.
   */
  /* implicit */ HandleRef(const Handle<Tag>& handle)
      : slotIndex_(handle.slotIndex()),
        generation_(handle.generation()),
        deviceId_(handle.deviceId()) {}

  /// Returns true if this reference is non-null.
  bool isValid() const { return generation_ != 0; }

  /// Resource table slot index, for backend use.
  uint32_t slotIndex() const { return slotIndex_; }

  /// Slot generation at creation time, for backend use. Zero for null references.
  uint32_t generation() const { return generation_; }

  /// Identity of the owning device, for backend use. Zero for null references.
  uint64_t deviceId() const { return deviceId_; }

  /**
   * Compares against null: true if the reference is null.
   *
   * @param ref Reference to test.
   */
  friend bool operator==(const HandleRef& ref, std::nullptr_t) { return !ref.isValid(); }

private:
  uint32_t slotIndex_ = 0;
  uint32_t generation_ = 0;
  uint64_t deviceId_ = 0;
};

/// Tag for \ref Buffer handles.
struct BufferTag {
  static constexpr const char* kName = "buffer";  //!< Resource name for diagnostics.
};
/// Tag for \ref Texture handles.
struct TextureTag {
  static constexpr const char* kName = "texture";  //!< Resource name for diagnostics.
};
/// Tag for \ref TextureView handles.
struct TextureViewTag {
  static constexpr const char* kName = "textureView";  //!< Resource name for diagnostics.
};
/// Tag for \ref Sampler handles.
struct SamplerTag {
  static constexpr const char* kName = "sampler";  //!< Resource name for diagnostics.
};
/// Tag for \ref BindGroupLayout handles.
struct BindGroupLayoutTag {
  static constexpr const char* kName = "bindGroupLayout";  //!< Resource name for diagnostics.
};
/// Tag for \ref BindGroup handles.
struct BindGroupTag {
  static constexpr const char* kName = "bindGroup";  //!< Resource name for diagnostics.
};
/// Tag for \ref PipelineLayout handles.
struct PipelineLayoutTag {
  static constexpr const char* kName = "pipelineLayout";  //!< Resource name for diagnostics.
};
/// Tag for \ref ShaderModule handles.
struct ShaderModuleTag {
  static constexpr const char* kName = "shaderModule";  //!< Resource name for diagnostics.
};
/// Tag for \ref RenderPipeline handles.
struct RenderPipelineTag {
  static constexpr const char* kName = "renderPipeline";  //!< Resource name for diagnostics.
};
/// Tag for \ref CommandBuffer handles.
struct CommandBufferTag {
  static constexpr const char* kName = "commandBuffer";  //!< Resource name for diagnostics.
};

using Buffer = Handle<BufferTag>;                    //!< Buffer handle.
using Texture = Handle<TextureTag>;                  //!< Texture handle.
using TextureView = Handle<TextureViewTag>;          //!< Texture view handle.
using Sampler = Handle<SamplerTag>;                  //!< Sampler handle.
using BindGroupLayout = Handle<BindGroupLayoutTag>;  //!< Bind group layout handle.
using BindGroup = Handle<BindGroupTag>;              //!< Bind group handle.
using PipelineLayout = Handle<PipelineLayoutTag>;    //!< Pipeline layout handle.
using ShaderModule = Handle<ShaderModuleTag>;        //!< Shader module handle.
using RenderPipeline = Handle<RenderPipelineTag>;    //!< Render pipeline handle.
using CommandBuffer = Handle<CommandBufferTag>;      //!< Finished command buffer handle.

using BufferRef = HandleRef<BufferTag>;                    //!< Buffer reference.
using TextureRef = HandleRef<TextureTag>;                  //!< Texture reference.
using TextureViewRef = HandleRef<TextureViewTag>;          //!< Texture view reference.
using SamplerRef = HandleRef<SamplerTag>;                  //!< Sampler reference.
using BindGroupLayoutRef = HandleRef<BindGroupLayoutTag>;  //!< Bind group layout reference.
using BindGroupRef = HandleRef<BindGroupTag>;              //!< Bind group reference.
using PipelineLayoutRef = HandleRef<PipelineLayoutTag>;    //!< Pipeline layout reference.
using ShaderModuleRef = HandleRef<ShaderModuleTag>;        //!< Shader module reference.
using RenderPipelineRef = HandleRef<RenderPipelineTag>;    //!< Render pipeline reference.

/**
 * gtest PrintTo support: prints `<name>#<slot>@<generation>` or `<name>(null)`.
 *
 * @param handle Handle to print.
 * @param os Output stream.
 */
template <typename Tag>
void PrintTo(const Handle<Tag>& handle, std::ostream* os) {
  if (handle.isValid()) {
    *os << Tag::kName << "#" << handle.slotIndex() << "@" << handle.generation();
  } else {
    *os << Tag::kName << "(null)";
  }
}

/**
 * gtest PrintTo support: prints `<name>#<slot>@<generation>` or `<name>(null)`.
 *
 * @param ref Reference to print.
 * @param os Output stream.
 */
template <typename Tag>
void PrintTo(const HandleRef<Tag>& ref, std::ostream* os) {
  if (ref.isValid()) {
    *os << Tag::kName << "#" << ref.slotIndex() << "@" << ref.generation();
  } else {
    *os << Tag::kName << "(null)";
  }
}

}  // namespace donner::gpu
