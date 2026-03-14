#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/svg/core/PathSpline.h"

namespace donner::svg {

/// Stable identifier for an editable contour.
struct PathContourId {
  uint32_t value = 0;

  friend bool operator==(const PathContourId&, const PathContourId&) = default;
};

/// Stable identifier for an editable anchor.
struct PathAnchorId {
  uint32_t value = 0;

  friend bool operator==(const PathAnchorId&, const PathAnchorId&) = default;
};

/// Identifies which Bézier handle is being edited.
enum class PathHandleType : uint8_t {
  Incoming,
  Outgoing,
};

/// Interaction mode for an anchor's handles.
enum class PathAnchorMode : uint8_t {
  Corner,
  Smooth,
  Symmetric,
};

/// An editable anchor point with optional incoming and outgoing Bézier handles.
struct EditablePathAnchor {
  PathAnchorId id;
  Vector2d position;
  std::optional<Vector2d> incomingHandle;
  std::optional<Vector2d> outgoingHandle;
  PathAnchorMode mode = PathAnchorMode::Corner;

  friend bool operator==(const EditablePathAnchor&, const EditablePathAnchor&) = default;
};

/// A single editable contour composed of ordered anchors.
struct EditablePathContour {
  PathContourId id;
  bool closed = false;
  std::vector<EditablePathAnchor> anchors;

  friend bool operator==(const EditablePathContour&, const EditablePathContour&) = default;
};

/**
 * Editable topology view layered on top of \ref PathSpline.
 *
 * This keeps \ref PathSpline as the canonical rendered/serialized geometry while exposing a stable
 * contour/anchor/handle model suitable for interactive editing.
 */
class EditablePathSpline {
public:
  EditablePathSpline() = default;

  /// Convert from a rendered spline into editable contours and anchors.
  static EditablePathSpline FromPathSpline(const PathSpline& spline);

  /// Convert the editable representation back into a \ref PathSpline.
  PathSpline toPathSpline() const;

  /// Returns true if there are no contours.
  bool empty() const { return contours_.empty(); }

  /// Read-only view of contours.
  std::span<const EditablePathContour> contours() const { return contours_; }

  /// Mutable view of contours.
  std::span<EditablePathContour> contours() { return contours_; }

  /// Find a contour by id.
  EditablePathContour* findContour(PathContourId contourId);
  /// Find a contour by id.
  const EditablePathContour* findContour(PathContourId contourId) const;

  /// Find an anchor by id.
  EditablePathAnchor* findAnchor(PathAnchorId anchorId);
  /// Find an anchor by id.
  const EditablePathAnchor* findAnchor(PathAnchorId anchorId) const;

  /// Add a new contour and return its id.
  PathContourId addContour(bool closed = false);

  /// Append an anchor to the given contour and return its id.
  PathAnchorId appendAnchor(PathContourId contourId, const Vector2d& position,
                            std::optional<Vector2d> incomingHandle = std::nullopt,
                            std::optional<Vector2d> outgoingHandle = std::nullopt);

  /// Set whether a contour is open or closed.
  void setContourClosed(PathContourId contourId, bool closed);

  /// Move an anchor and preserve its handle offsets.
  void moveAnchor(PathAnchorId anchorId, const Vector2d& newPosition);

  /// Move or clear one of an anchor's handles.
  void moveHandle(PathAnchorId anchorId, PathHandleType handleType,
                  std::optional<Vector2d> newPosition);

  /// Set both handles on an anchor at once and optionally force a mode.
  void setAnchorHandles(PathAnchorId anchorId, std::optional<Vector2d> incomingHandle,
                        std::optional<Vector2d> outgoingHandle,
                        std::optional<PathAnchorMode> mode = std::nullopt);

  /// Insert a new anchor on the segment that starts at \p afterAnchorId, at parameter \p t in
  /// [0,1]. For cubic segments this uses de Casteljau subdivision to preserve the curve shape.
  /// Returns the new anchor's id.
  PathAnchorId insertAnchorAfter(PathAnchorId afterAnchorId, double t);

  /// Update the anchor mode and normalize handles to match it when possible.
  void setAnchorMode(PathAnchorId anchorId, PathAnchorMode mode);

  // --- Hit testing ---

  /// Result of a handle hit test.
  struct HandleHit {
    PathAnchorId anchorId;
    PathHandleType handleType;
  };

  /// Result of a segment hit test.
  struct SegmentHit {
    PathAnchorId afterAnchorId;  ///< The anchor at the start of the hit segment.
    double t;                    ///< Parameter along the segment [0,1].
  };

  /// Parameters for hit testing.
  struct HitTestParams {
    Transformd localToWorld;   ///< Transform from path-local to world coordinates.
    double anchorRadius = 6.0; ///< Hit radius for anchors in world units.
    double handleRadius = 5.0; ///< Hit radius for handles in world units.
    double segmentRadius = 4.0; ///< Hit radius for segments in world units.
    int segmentSamples = 32;   ///< Number of samples per segment for segment hit testing.
  };

  /// Hit-test anchors. Returns the id of the closest anchor within \p params.anchorRadius of
  /// \p worldPoint, or nullopt.
  std::optional<PathAnchorId> hitTestAnchor(const Vector2d& worldPoint,
                                            const HitTestParams& params) const;

  /// Hit-test handles. Returns the anchor id and handle type of the closest handle within
  /// \p params.handleRadius of \p worldPoint, or nullopt.
  std::optional<HandleHit> hitTestHandle(const Vector2d& worldPoint,
                                         const HitTestParams& params) const;

  /// Hit-test segments by sampling. Returns the anchor id at the start of the closest segment
  /// and the parameter t, or nullopt if no segment is within \p params.segmentRadius.
  std::optional<SegmentHit> hitTestSegment(const Vector2d& worldPoint,
                                           const HitTestParams& params) const;

private:
  static PathAnchorMode inferMode(const EditablePathAnchor& anchor);
  static void enforceMode(EditablePathAnchor& anchor, std::optional<PathHandleType> movedHandle);
  PathContourId allocateContourId();
  PathAnchorId allocateAnchorId();

private:
  std::vector<EditablePathContour> contours_;
  uint32_t nextContourId_ = 1;
  uint32_t nextAnchorId_ = 1;
};

}  // namespace donner::svg
