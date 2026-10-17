#pragma once
/// @file

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "donner/base/RcString.h"
#include "donner/svg/SVGDocumentHandle.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::svg::components {

/**
 * Parameters for the \ref xml_image element.
 */
struct ImageComponent {
  /// URI to the image resource, which can either be a file path, URL, or data URL (e.g.
  /// "data:image/png;base64,...").
  RcString href;
};

/// Owns a decoded-raster reservation from a shared document-family budget.
class DecodedImageReservation {
public:
  /// Reserve bytes before decoding or copying pixels; null means admission was refused.
  /// @param family Shared budget owning the reservation.
  /// @param bytes Maximum decoded bytes to reserve.
  static std::shared_ptr<DecodedImageReservation> Create(
      std::shared_ptr<DocumentResourceFamilyBudget> family, std::size_t bytes) {
    if (!family || !family->reserve(DocumentResourceFamilyBudget::Kind::DecodedImage, bytes)) {
      return nullptr;
    }
    return std::shared_ptr<DecodedImageReservation>(
        new DecodedImageReservation(std::move(family), bytes));
  }

  ~DecodedImageReservation() {
    family_->release(DocumentResourceFamilyBudget::Kind::DecodedImage, bytes_);
  }
  DecodedImageReservation(const DecodedImageReservation&) = delete;
  DecodedImageReservation& operator=(const DecodedImageReservation&) = delete;

  /// Release unused decoder allowance after the actual pixel buffer is known.
  /// @param bytes Actual retained pixel count in bytes; must not exceed the reservation.
  void shrinkTo(std::size_t bytes) {
    assert(bytes <= bytes_);
    family_->release(DocumentResourceFamilyBudget::Kind::DecodedImage, bytes_ - bytes);
    bytes_ = bytes;
  }

  /// Reserve independent storage for a deep copy of the owned pixels.
  std::shared_ptr<DecodedImageReservation> duplicate() const { return Create(family_, bytes_); }

private:
  DecodedImageReservation(std::shared_ptr<DocumentResourceFamilyBudget> family, std::size_t bytes)
      : family_(std::move(family)), bytes_(bytes) {}
  std::shared_ptr<DocumentResourceFamilyBudget> family_;
  std::size_t bytes_;
};

/**
 * Loaded raster image resource, created from a \ref ImageComponent when the href references a
 * raster image (PNG, JPEG, GIF).
 */
struct LoadedImageComponent {
  LoadedImageComponent() : revision_(NextRevision()) {}
  /// Store a decoded image with a fresh revision for cache invalidation.
  /// @param loadedImage Decoded image payload to own.
  /// @param reservation Optional shared-family reservation acquired before decoding.
  explicit LoadedImageComponent(ImageResource loadedImage,
                                std::shared_ptr<DecodedImageReservation> reservation = nullptr)
      : image(std::move(loadedImage)),
        revision_(NextRevision()),
        reservation_(std::move(reservation)) {}

  /// Destroy pixel storage while its reservation is still held.
  ~LoadedImageComponent() { image.reset(); }

  /// Copy pixels only after reserving independent storage; refused copies contain no image.
  /// @param other Image to copy, including its existing family-budget policy.
  LoadedImageComponent(const LoadedImageComponent& other) : revision_(other.revision_) {
    if (other.reservation_) {
      reservation_ = other.reservation_->duplicate();
      if (!reservation_) {
        return;
      }
    }
    image = other.image;
  }
  /// Replace the image with a separately budgeted copy.
  /// @param other Image to copy.
  LoadedImageComponent& operator=(const LoadedImageComponent& other) {
    if (this != &other) {
      LoadedImageComponent copy(other);
      *this = std::move(copy);
    }
    return *this;
  }
  /// Transfer pixels and their reservation without an additional charge.
  /// @param other Image whose storage is transferred.
  LoadedImageComponent(LoadedImageComponent&& other) noexcept = default;
  /// Replace pixels by transferring their existing reservation.
  /// @param other Image whose storage is transferred.
  LoadedImageComponent& operator=(LoadedImageComponent&& other) noexcept = default;

  /// Monotonic identity for the current decoded image payload.
  std::uint64_t revision() const { return revision_; }

  /// Mark a direct mutation of the decoded image payload.
  void markImageModified() { revision_ = NextRevision(); }

  std::optional<ImageResource> image;  //!< Loaded image resource.

private:
  static std::uint64_t NextRevision() {
    static std::atomic<std::uint64_t> nextRevision{1};
    return nextRevision.fetch_add(1, std::memory_order_relaxed);
  }

  std::uint64_t revision_ = 0;
  std::shared_ptr<DecodedImageReservation> reservation_;
};

/**
 * Loaded SVG sub-document, created from a \ref ImageComponent when the href references an SVG
 * file (`image/svg+xml`). Stores the shared internal document handle used by \ref SVGDocument.
 */
struct LoadedSVGImageComponent {
  SVGDocumentHandle subDocument;  //!< Parsed external SVG sub-document handle.
};

/**
 * Loaded external SVG sub-document referenced by a `<use>` element. When a `<use>` element's
 * href points to an external SVG file, the document is loaded via \ref SubDocumentCache and
 * stored here as an \ref SVGDocumentHandle for rendering as a nested sub-document.
 */
struct ExternalUseComponent {
  SVGDocumentHandle subDocument;  //!< Parsed external SVG sub-document handle.

  /// Fragment identifier within the external document (e.g., "elementId" from
  /// "file.svg#elementId"). Empty if the whole document is referenced.
  RcString fragment;
};

}  // namespace donner::svg::components
