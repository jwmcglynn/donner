#pragma once
/// @file
/// Stable object coordinates used by both raster and selection presentation.

#include <cstdint>
#include <optional>
#include <span>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"

namespace donner::editor {

/// Identity of the document content consumed by one renderer capture.
struct PresentationIdentity {
  std::uint64_t captureId = 0;             //!< Immutable worker capture, never a gesture number.
  std::uint64_t documentGeneration = 0;    //!< Document lifetime.
  std::uint64_t documentRevision = 0;      //!< Actual guarded DOM mutation revision.
  std::uint64_t version = 0;               //!< Committed document mutation version.
  std::uint64_t geometryRevision = 0;      //!< Non-transform document content.
  std::uint64_t fontResourceRevision = 0;  //!< Adopted font resources.
  std::uint64_t presentationEpoch = 0;     //!< Renderer configuration.

  /// Compare geometry/resource provenance while allowing different object poses and captures.
  /// @param other Candidate source identity.
  [[nodiscard]] bool sameContent(const PresentationIdentity& other) const;
  /// Compare the exact consumed document/resource revision, independent of capture allocation.
  /// @param other Candidate source identity.
  [[nodiscard]] bool sameScene(const PresentationIdentity& other) const;
  /// Compare all members for value equality.
  /// @param lhs Value to compare.
  /// @param rhs Value to compare.
  friend bool operator==(const PresentationIdentity& lhs,
                         const PresentationIdentity& rhs) = default;
};

/// An object's absolute pose in the document, independent of mouse gestures.
struct PresentationPose {
  Entity entity = entt::null;       //!< Object within the identity's document generation.
  Transform2d documentFromElement;  //!< Actual captured or requested object pose.
};

/// Validate every coefficient of a presentation mapping.
/// @param documentFromElement Mapping to inspect.
[[nodiscard]] bool FinitePresentationTransform(const Transform2d& documentFromElement);

/// Compare two presentation mappings without changing their reference space.
/// @param lhs First mapping.
/// @param rhs Second mapping in the same coordinate spaces.
[[nodiscard]] bool SamePresentationTransform(const Transform2d& lhs, const Transform2d& rhs);

/**
 * Resolve one affine mapping shared by all selected participants.
 * @param captured Poses belonging to the raster and captured selection geometry.
 * @param requested Absolute poses chosen by the current input transaction.
 * @return Captured-document to presented-document mapping, or nullopt when no common mapping
 * exists.
 */
[[nodiscard]] std::optional<Transform2d> ResolvePresentationTransform(
    std::span<const PresentationPose> captured, std::span<const PresentationPose> requested);

}  // namespace donner::editor
