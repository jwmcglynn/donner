#pragma once
/// @file
/// \c donner::editor::UiTextureRegistry - validated UI texture registrations for ImGui draw data.

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/GpuResult.h"
#include "donner/gpu/Handles.h"

namespace donner::gpu {
class Device;
}  // namespace donner::gpu

namespace donner::editor {

/// How a registered UI texture's color channels relate to its alpha channel.
enum class UiTextureAlphaMode : uint8_t {
  Premultiplied,  //!< Color channels are already multiplied by alpha.
  Straight,       //!< Color channels are independent of alpha and multiplied when sampled.
};

/**
 * Ostream output operator for \ref donner::editor::UiTextureAlphaMode "UiTextureAlphaMode", e.g.
 * `Premultiplied`.
 *
 * @param os Output stream.
 * @param value Alpha mode to output.
 */
std::ostream& operator<<(std::ostream& os, UiTextureAlphaMode value);

/**
 * Opaque identifier for a UI texture registration, in the form ImGui carries through draw data.
 *
 * The value is a registry slot plus the generation that slot held when the registration was
 * made. It is deliberately not a backend object address: a value reaching the registry from
 * recorded draw data is untrusted, and a slot/generation pair lets a recycled slot reject a token
 * minted for the registration that used to live there. Slot generations start at one, so every
 * valid identifier is nonzero and zero keeps ImGui's "no texture" meaning.
 */
class UiTextureId {
public:
  /// Constructs a null identifier.
  UiTextureId() = default;

  /// Returns true if this identifier is non-null. A non-null identifier may still be stale; only
  /// \ref UiTextureRegistry::lookup is authoritative.
  bool isValid() const { return generation_ != 0; }

  /// Registry slot index this identifier names.
  uint32_t slotIndex() const { return slotIndex_; }

  /// Registry slot generation at registration time. Zero for null identifiers.
  uint32_t generation() const { return generation_; }

  /// Returns the value to hand ImGui, which returns it unchanged in \c ImDrawCmd. Zero for null
  /// identifiers.
  ImTextureID imTextureId() const {
    return (static_cast<ImTextureID>(generation_) << 32) | static_cast<ImTextureID>(slotIndex_);
  }

  /**
   * Reconstructs an identifier from a value ImGui carried through draw data. The value is not
   * trusted: the resulting identifier is only meaningful once \ref UiTextureRegistry::lookup
   * accepts it.
   *
   * @param value Value previously returned by \ref imTextureId.
   */
  static UiTextureId FromImTextureId(ImTextureID value) {
    UiTextureId id;
    id.slotIndex_ = static_cast<uint32_t>(value & 0xFFFFFFFFu);
    id.generation_ = static_cast<uint32_t>(value >> 32);
    return id;
  }

  /**
   * Mints an identifier for a registry slot; called by \ref UiTextureRegistry only.
   *
   * @param slotIndex Registry slot index.
   * @param generation Slot generation at registration time; must be nonzero.
   */
  static UiTextureId CreateForRegistry(uint32_t slotIndex, uint32_t generation) {
    UiTextureId id;
    id.slotIndex_ = slotIndex;
    id.generation_ = generation;
    return id;
  }

  /// Equality operator. @param other Identifier to compare against.
  bool operator==(const UiTextureId& other) const = default;

private:
  uint32_t slotIndex_ = 0;
  uint32_t generation_ = 0;
};

/**
 * gtest PrintTo support: prints `uiTexture#<slot>@<generation>` or `uiTexture(null)`.
 *
 * @param id Identifier to print.
 * @param os Output stream.
 */
void PrintTo(const UiTextureId& id, std::ostream* os);

/// What a UI texture registration carries beyond the view it names.
struct UiTextureDescriptor {
  gpu::TextureViewRef view;  //!< View sampled when the texture is drawn. Texture needs
                             //!< \ref gpu::TextureUsage::Sampled.
  gpu::Extent2d size;        //!< Sampled extent in texels. Must be nonzero.
  UiTextureAlphaMode alphaMode = UiTextureAlphaMode::Premultiplied;  //!< Alpha interpretation.
};

/// The validated result of a registration lookup.
struct UiTextureBinding {
  gpu::TextureViewRef view;                                          //!< Validated view.
  gpu::Extent2d size;                                                //!< Sampled extent in texels.
  UiTextureAlphaMode alphaMode = UiTextureAlphaMode::Premultiplied;  //!< Alpha interpretation.

  /// Equality operator; views compare by the identity the device validates, since
  /// \ref gpu::HandleRef has no comparison of its own.
  /// @param other Binding to compare against.
  bool operator==(const UiTextureBinding& other) const {
    return view.slotIndex() == other.view.slotIndex() &&
           view.generation() == other.view.generation() &&
           view.deviceId() == other.view.deviceId() && size == other.size &&
           alphaMode == other.alphaMode;
  }
};

/**
 * gtest PrintTo support for readable failures on a binding mismatch.
 *
 * @param binding Binding to print.
 * @param os Output stream.
 */
void PrintTo(const UiTextureBinding& binding, std::ostream* os);

/**
 * Registrations of runtime textures that UI draw data may sample, keyed by an opaque identifier.
 *
 * A registration records the view, its sampled extent and its alpha interpretation, and binds all
 * three to one device. Registering does not take ownership of the backing: the producer keeps the
 * texture alive, and the registry reports when a retired registration's frames have passed so the
 * producer can release it.
 *
 * Every lookup revalidates, in release builds too, because the identifier arrives from recorded
 * draw data rather than from the caller that registered it: a null identifier, an identifier whose
 * slot was reused for a later registration, an identifier minted against another device, and a
 * retired registration all fail closed instead of reaching a backend. A view destroyed while its
 * registration is live is caught where the returned \ref UiTextureBinding is consumed, by the
 * generation check the device already runs on every bind group it builds.
 *
 * Retirement is deferred by frames rather than immediate. \ref retire makes a registration
 * unusable for new draw data at once, but keeps its slot occupied for
 * \ref retirementFrames() calls to \ref advanceFrame, so a frame already recorded against it can
 * still be submitted before the producer releases the backing.
 */
class UiTextureRegistry {
public:
  /// Maximum simultaneously live or retiring registrations.
  static constexpr size_t kMaxRegistrations = 16384;
  /// Presentation frames a retired registration occupies its slot for, covering the frames a
  /// recorded draw can still be in flight.
  static constexpr uint32_t kDefaultRetirementFrames = 3;

  /**
   * Constructs a registry bound to \p device.
   *
   * @param device Device every registered view must belong to; must outlive the registry.
   * @param retirementFrames Frames a retired registration occupies its slot for.
   */
  explicit UiTextureRegistry(const gpu::Device& device,
                             uint32_t retirementFrames = kDefaultRetirementFrames);

  /// Destructor. Registrations do not own their backing, so nothing is released here.
  ~UiTextureRegistry();

  UiTextureRegistry(const UiTextureRegistry&) = delete;
  UiTextureRegistry& operator=(const UiTextureRegistry&) = delete;
  UiTextureRegistry(UiTextureRegistry&&) = delete;
  UiTextureRegistry& operator=(UiTextureRegistry&&) = delete;

  /**
   * Registers \p descriptor and returns the identifier to hand ImGui. Fails closed with
   * \ref gpu::GpuErrorType::InvalidHandle for a null view, \ref gpu::GpuErrorType::DeviceMismatch
   * for a view of another device, and \ref gpu::GpuErrorType::InvalidDescriptor for a zero extent.
   *
   * @param descriptor View, extent and alpha interpretation to register.
   */
  gpu::Result<UiTextureId> registerTexture(const UiTextureDescriptor& descriptor);

  /**
   * Resolves \p id to its registration. Fails closed with
   * \ref gpu::GpuErrorType::InvalidHandle when \p id is null, names a slot this registry never
   * handed out, or carries the generation of a registration whose slot has since been reused, and
   * with \ref gpu::GpuErrorType::InvalidState when the registration is retired.
   *
   * @param id Identifier carried through draw data.
   */
  gpu::Result<UiTextureBinding> lookup(UiTextureId id) const;

  /**
   * Retires the registration \p id names. Later lookups fail, and the slot is released after
   * \ref retirementFrames() calls to \ref advanceFrame. Fails closed exactly like \ref lookup,
   * except that retiring an already-retired registration is an error rather than a no-op so a
   * double release is reported at its source.
   *
   * @param id Identifier to retire.
   */
  gpu::Status retire(UiTextureId id);

  /**
   * Advances one presentation frame and returns the identifiers whose retirement frames have
   * passed, in registration order. Their backing is no longer reachable through this registry, so
   * the producer may release it. Their slots are reusable, with a bumped generation, from the next
   * \ref registerTexture.
   */
  std::vector<UiTextureId> advanceFrame();

  /// Number of registrations that lookups still accept.
  size_t liveCount() const;

  /// Number of retired registrations still occupying a slot.
  size_t retiredCount() const;

  /// Frames a retired registration occupies its slot for.
  uint32_t retirementFrames() const { return retirementFrames_; }

private:
  /// One registry slot. An unused slot has `alive` false and is on \ref freeSlots_.
  struct Slot {
    uint32_t generation = 0;  //!< Generation of the registration in this slot; nonzero when used.
    bool alive = false;       //!< Whether this slot holds a registration.
    bool retired = false;     //!< Whether that registration is retired.
    uint32_t framesSinceRetired = 0;  //!< Completed frames since \ref retire.
    UiTextureBinding binding;         //!< Registered view, extent and alpha interpretation.
  };

  /// Resolves \p id to a live slot, applying every validation \ref lookup documents except the
  /// view liveness check.
  /// @param id Identifier to resolve. @param operation Operation name for diagnostics.
  [[gnu::noinline]] gpu::Result<const Slot*> resolveSlot(UiTextureId id,
                                                         std::string_view operation) const;

  const gpu::Device* device_;
  uint32_t retirementFrames_;
  std::vector<Slot> slots_;
  std::vector<uint32_t> freeSlots_;
};

}  // namespace donner::editor
