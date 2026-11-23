#pragma once
/// @file
/// Per-entity GPU residence for a decoded raster image's texture.

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/Handles.h"
#include "donner/svg/renderer/geode/GeodeHandleRetirement.h"
#include "donner/svg/renderer/geode/GeodePerDevice.h"
#include "donner/svg/renderer/geode/GeodeResidentPathComponent.h"
#include "donner/svg/renderer/geode/GeodeResourceBudget.h"

namespace donner::geode {

/**
 * One decoded image's pixels on one device, premultiplied in an RGBA8 texture that draws sample
 * across frames.
 *
 * The texture is written once, when it is created, so every draw that names it samples the pixels
 * of the image revision it was uploaded from. Its bytes are charged to the document's resident
 * limit (\ref GeodeDocumentGeometryBudget::reserveImageBytes) for as long as it exists, and its
 * handle is released on the thread of the context that created it.
 */
struct GeodeResidentImage {
  gpu::Texture texture;      //!< Premultiplied RGBA8 pixels.
  std::uint64_t revision{};  //!< `LoadedImageComponent::revision()` of the uploaded pixels.
  int width{};               //!< Image width in pixels.
  int height{};              //!< Image height in pixels.
  /// Document budget charged for the texture, and the bytes charged: width * height * 4.
  std::shared_ptr<GeodeDocumentGeometryBudget> chargedBudget;
  std::uint64_t chargedBytes{};  //!< Bytes charged to \ref chargedBudget.
  /// Retirement of the context whose device created the texture.
  std::weak_ptr<GeodeHandleRetirement> owner;

  GeodeResidentImage() = default;
  ~GeodeResidentImage() {
    if (chargedBudget) {
      chargedBudget->releaseImageBytes(chargedBytes);
    }
    if (const std::shared_ptr<GeodeHandleRetirement> retirement = owner.lock()) {
      std::vector<gpu::Texture> textures;
      textures.push_back(std::move(texture));
      UTILS_RELEASE_ASSERT_MSG(retirement->retireTextures(textures) == 0,
                               "a resident image held another device's texture");
    }
  }
  GeodeResidentImage(const GeodeResidentImage&) = delete;
  GeodeResidentImage& operator=(const GeodeResidentImage&) = delete;

  /// Bytes of an RGBA8 texture of \p width x \p height pixels.
  /// @param width Width in pixels; positive.
  /// @param height Height in pixels; positive.
  [[nodiscard]] static std::uint64_t TextureBytes(int width, int height) {
    return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4u;
  }
};

/**
 * One device's resident texture for an image entity.
 *
 * Dropping or replacing the texture parks it in the document's resident slab for that device,
 * which releases it at the first frame boundary no open frame on the device precedes, so a draw an
 * open frame recorded never samples a released texture.
 */
class GeodeImageResidency {
public:
  GeodeImageResidency() = default;
  ~GeodeImageResidency() { reset(); }
  GeodeImageResidency(const GeodeImageResidency&) = delete;
  GeodeImageResidency& operator=(const GeodeImageResidency&) = delete;

  /// The resident texture, or null when none is kept.
  [[nodiscard]] const GeodeResidentImage* image() const { return image_.get(); }

  /// Slab the texture is parked in when it is dropped, or null.
  [[nodiscard]] const std::shared_ptr<GeodeResidentSlab>& slab() const { return slab_; }

  /// Park the current texture, if any, in its slab and keep \p image instead, parking future
  /// drops in \p slab.
  /// @param slab This device's resident slab for the document.
  /// @param image Texture to keep, or null to keep none.
  void replace(std::shared_ptr<GeodeResidentSlab> slab,
               std::shared_ptr<const GeodeResidentImage> image) {
    reset();
    slab_ = std::move(slab);
    image_ = std::move(image);
  }

  /// Park the current texture, if any, in its slab.
  void reset() {
    if (image_ && slab_) {
      slab_->retireImage(std::move(image_));
    }
    image_.reset();
  }

private:
  std::shared_ptr<GeodeResidentSlab> slab_;
  std::shared_ptr<const GeodeResidentImage> image_;
};

/**
 * Resident textures of an entity that draws a decoded raster image (`LoadedImageComponent`), one
 * per device that draws it.
 *
 * Installed lazily by `RendererGeode::drawImage` on the image's source entity, and removed by the
 * `LoadedImageComponent` update and destroy listener, so new pixels or a removed image drop the
 * texture. Every draw also compares the kept texture's revision and size with the image being
 * drawn and uploads again on a mismatch, which covers a direct pixel edit that marks a new
 * revision without a signal.
 */
struct GeodeResidentImageComponent {
  GeodePerDevice<GeodeImageResidency> devices;  //!< Resident texture per drawing device.
};
static_assert(!std::is_copy_constructible_v<GeodeResidentImageComponent> &&
                  std::is_nothrow_move_constructible_v<GeodeResidentImageComponent>,
              "The registry must move the component, never copy its GPU handles.");

}  // namespace donner::geode
