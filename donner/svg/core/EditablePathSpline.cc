#include "donner/svg/core/EditablePathSpline.h"

#include "donner/base/MathUtils.h"
#include "donner/base/Utils.h"

namespace donner::svg {

namespace {

void appendSegment(PathSpline& spline, const EditablePathAnchor& start,
                   const EditablePathAnchor& end) {
  if (start.outgoingHandle.has_value() || end.incomingHandle.has_value()) {
    spline.curveTo(start.outgoingHandle.value_or(start.position),
                   end.incomingHandle.value_or(end.position), end.position);
  } else {
    spline.lineTo(end.position);
  }
}

}  // namespace

EditablePathSpline EditablePathSpline::FromPathSpline(const PathSpline& spline) {
  EditablePathSpline result;
  EditablePathContour* currentContour = nullptr;

  for (const PathSpline::Command& command : spline.commands()) {
    switch (command.type) {
      case PathSpline::CommandType::MoveTo: {
        PathContourId contourId = result.allocateContourId();
        result.contours_.push_back(EditablePathContour{.id = contourId});
        currentContour = &result.contours_.back();
        currentContour->anchors.push_back(EditablePathAnchor{
            .id = result.allocateAnchorId(),
            .position = spline.points()[command.pointIndex],
        });
        break;
      }
      case PathSpline::CommandType::LineTo: {
        UTILS_RELEASE_ASSERT_MSG(currentContour != nullptr,
                                 "PathSpline contains LineTo before MoveTo");
        currentContour->anchors.push_back(EditablePathAnchor{
            .id = result.allocateAnchorId(),
            .position = spline.points()[command.pointIndex],
        });
        break;
      }
      case PathSpline::CommandType::CurveTo: {
        UTILS_RELEASE_ASSERT_MSG(currentContour != nullptr,
                                 "PathSpline contains CurveTo before MoveTo");
        UTILS_RELEASE_ASSERT_MSG(!currentContour->anchors.empty(),
                                 "PathSpline contour missing starting anchor");

        EditablePathAnchor& previousAnchor = currentContour->anchors.back();
        previousAnchor.outgoingHandle = spline.points()[command.pointIndex];
        currentContour->anchors.push_back(EditablePathAnchor{
            .id = result.allocateAnchorId(),
            .position = spline.points()[command.pointIndex + 2],
            .incomingHandle = spline.points()[command.pointIndex + 1],
        });
        break;
      }
      case PathSpline::CommandType::ClosePath: {
        UTILS_RELEASE_ASSERT_MSG(currentContour != nullptr,
                                 "PathSpline contains ClosePath before MoveTo");
        currentContour->closed = true;
        break;
      }
    }
  }

  for (EditablePathContour& contour : result.contours_) {
    for (EditablePathAnchor& anchor : contour.anchors) {
      anchor.mode = inferMode(anchor);
    }
  }

  return result;
}

PathSpline EditablePathSpline::toPathSpline() const {
  PathSpline spline;

  for (const EditablePathContour& contour : contours_) {
    if (contour.anchors.empty()) {
      continue;
    }

    spline.moveTo(contour.anchors.front().position);
    for (size_t i = 1; i < contour.anchors.size(); ++i) {
      appendSegment(spline, contour.anchors[i - 1], contour.anchors[i]);
    }

    if (contour.closed) {
      if (contour.anchors.size() > 1u) {
        const EditablePathAnchor& lastAnchor = contour.anchors.back();
        const EditablePathAnchor& firstAnchor = contour.anchors.front();
        if (lastAnchor.outgoingHandle.has_value() || firstAnchor.incomingHandle.has_value()) {
          appendSegment(spline, lastAnchor, firstAnchor);
        }
      }

      spline.closePath();
    }
  }

  return spline;
}

EditablePathContour* EditablePathSpline::findContour(PathContourId contourId) {
  for (EditablePathContour& contour : contours_) {
    if (contour.id == contourId) {
      return &contour;
    }
  }

  return nullptr;
}

const EditablePathContour* EditablePathSpline::findContour(PathContourId contourId) const {
  for (const EditablePathContour& contour : contours_) {
    if (contour.id == contourId) {
      return &contour;
    }
  }

  return nullptr;
}

EditablePathAnchor* EditablePathSpline::findAnchor(PathAnchorId anchorId) {
  for (EditablePathContour& contour : contours_) {
    for (EditablePathAnchor& anchor : contour.anchors) {
      if (anchor.id == anchorId) {
        return &anchor;
      }
    }
  }

  return nullptr;
}

const EditablePathAnchor* EditablePathSpline::findAnchor(PathAnchorId anchorId) const {
  for (const EditablePathContour& contour : contours_) {
    for (const EditablePathAnchor& anchor : contour.anchors) {
      if (anchor.id == anchorId) {
        return &anchor;
      }
    }
  }

  return nullptr;
}

PathContourId EditablePathSpline::addContour(bool closed) {
  const PathContourId contourId = allocateContourId();
  contours_.push_back(EditablePathContour{.id = contourId, .closed = closed});
  return contourId;
}

PathAnchorId EditablePathSpline::appendAnchor(PathContourId contourId, const Vector2d& position,
                                              std::optional<Vector2d> incomingHandle,
                                              std::optional<Vector2d> outgoingHandle) {
  EditablePathContour* contour = findContour(contourId);
  UTILS_RELEASE_ASSERT_MSG(contour != nullptr, "appendAnchor called with unknown contour id");

  EditablePathAnchor anchor{
      .id = allocateAnchorId(),
      .position = position,
      .incomingHandle = incomingHandle,
      .outgoingHandle = outgoingHandle,
  };
  anchor.mode = inferMode(anchor);
  contour->anchors.push_back(anchor);
  return anchor.id;
}

void EditablePathSpline::setContourClosed(PathContourId contourId, bool closed) {
  EditablePathContour* contour = findContour(contourId);
  UTILS_RELEASE_ASSERT_MSG(contour != nullptr, "setContourClosed called with unknown contour id");
  contour->closed = closed;
}

void EditablePathSpline::moveAnchor(PathAnchorId anchorId, const Vector2d& newPosition) {
  EditablePathAnchor* anchor = findAnchor(anchorId);
  UTILS_RELEASE_ASSERT_MSG(anchor != nullptr, "moveAnchor called with unknown anchor id");

  const Vector2d delta = newPosition - anchor->position;
  anchor->position = newPosition;
  if (anchor->incomingHandle) {
    *anchor->incomingHandle = *anchor->incomingHandle + delta;
  }
  if (anchor->outgoingHandle) {
    *anchor->outgoingHandle = *anchor->outgoingHandle + delta;
  }
}

void EditablePathSpline::moveHandle(PathAnchorId anchorId, PathHandleType handleType,
                                    std::optional<Vector2d> newPosition) {
  EditablePathAnchor* anchor = findAnchor(anchorId);
  UTILS_RELEASE_ASSERT_MSG(anchor != nullptr, "moveHandle called with unknown anchor id");

  std::optional<Vector2d>& handle =
      handleType == PathHandleType::Incoming ? anchor->incomingHandle : anchor->outgoingHandle;
  handle = std::move(newPosition);

  if (!anchor->incomingHandle.has_value() || !anchor->outgoingHandle.has_value()) {
    anchor->mode = PathAnchorMode::Corner;
    return;
  }

  enforceMode(*anchor, handleType);
}

void EditablePathSpline::setAnchorHandles(PathAnchorId anchorId,
                                          std::optional<Vector2d> incomingHandle,
                                          std::optional<Vector2d> outgoingHandle,
                                          std::optional<PathAnchorMode> mode) {
  EditablePathAnchor* anchor = findAnchor(anchorId);
  UTILS_RELEASE_ASSERT_MSG(anchor != nullptr, "setAnchorHandles called with unknown anchor id");

  anchor->incomingHandle = std::move(incomingHandle);
  anchor->outgoingHandle = std::move(outgoingHandle);

  if (mode.has_value()) {
    anchor->mode = *mode;
    enforceMode(*anchor, std::nullopt);
  } else {
    anchor->mode = inferMode(*anchor);
  }
}

PathAnchorId EditablePathSpline::insertAnchorAfter(PathAnchorId afterAnchorId, double t) {
  UTILS_RELEASE_ASSERT_MSG(t > 0.0 && t < 1.0, "insertAnchorAfter t must be in (0, 1)");

  // Find the contour and index of the "after" anchor.
  for (EditablePathContour& contour : contours_) {
    for (size_t i = 0; i < contour.anchors.size(); ++i) {
      if (contour.anchors[i].id != afterAnchorId) {
        continue;
      }

      // Determine the next anchor index, wrapping for closed contours.
      const size_t nextIndex =
          (i + 1 < contour.anchors.size()) ? (i + 1) : (contour.closed ? 0 : contour.anchors.size());
      UTILS_RELEASE_ASSERT_MSG(nextIndex < contour.anchors.size(),
                               "insertAnchorAfter called on the last anchor of an open contour");

      EditablePathAnchor& a = contour.anchors[i];
      EditablePathAnchor& b = contour.anchors[nextIndex];

      const bool isCubic = a.outgoingHandle.has_value() || b.incomingHandle.has_value();
      EditablePathAnchor newAnchor{.id = allocateAnchorId()};

      if (isCubic) {
        // De Casteljau subdivision of cubic P0, P1, P2, P3 at parameter t.
        const Vector2d p0 = a.position;
        const Vector2d p1 = a.outgoingHandle.value_or(a.position);
        const Vector2d p2 = b.incomingHandle.value_or(b.position);
        const Vector2d p3 = b.position;

        const Vector2d q0 = p0 + (p1 - p0) * t;
        const Vector2d q1 = p1 + (p2 - p1) * t;
        const Vector2d q2 = p2 + (p3 - p2) * t;

        const Vector2d r0 = q0 + (q1 - q0) * t;
        const Vector2d r1 = q1 + (q2 - q1) * t;

        const Vector2d s = r0 + (r1 - r0) * t;

        a.outgoingHandle = q0;
        newAnchor.position = s;
        newAnchor.incomingHandle = r0;
        newAnchor.outgoingHandle = r1;
        newAnchor.mode = PathAnchorMode::Smooth;
        b.incomingHandle = q2;
      } else {
        // Line segment: simple linear interpolation.
        newAnchor.position = a.position + (b.position - a.position) * t;
        newAnchor.mode = PathAnchorMode::Corner;
      }

      const PathAnchorId newId = newAnchor.id;
      // Insert after position i (before nextIndex). For closed contour wrap-around where
      // nextIndex == 0, we append at the end.
      const size_t insertPos = (nextIndex == 0) ? contour.anchors.size() : nextIndex;
      contour.anchors.insert(contour.anchors.begin() + static_cast<ptrdiff_t>(insertPos),
                             std::move(newAnchor));
      return newId;
    }
  }

  UTILS_RELEASE_ASSERT_MSG(false, "insertAnchorAfter called with unknown anchor id");
  return PathAnchorId{0};  // Unreachable.
}

void EditablePathSpline::setAnchorMode(PathAnchorId anchorId, PathAnchorMode mode) {
  EditablePathAnchor* anchor = findAnchor(anchorId);
  UTILS_RELEASE_ASSERT_MSG(anchor != nullptr, "setAnchorMode called with unknown anchor id");

  anchor->mode = mode;
  enforceMode(*anchor, std::nullopt);
}

PathAnchorMode EditablePathSpline::inferMode(const EditablePathAnchor& anchor) {
  if (!anchor.incomingHandle.has_value() || !anchor.outgoingHandle.has_value()) {
    return PathAnchorMode::Corner;
  }

  const Vector2d incoming = anchor.position - *anchor.incomingHandle;
  const Vector2d outgoing = *anchor.outgoingHandle - anchor.position;
  if (NearZero(incoming.lengthSquared(), 1e-12) || NearZero(outgoing.lengthSquared(), 1e-12)) {
    return PathAnchorMode::Corner;
  }

  const double cross = incoming.x * outgoing.y - incoming.y * outgoing.x;
  const double dot = incoming.x * outgoing.x + incoming.y * outgoing.y;
  if (!NearZero(cross, 1e-6) || dot <= 0.0) {
    return PathAnchorMode::Corner;
  }

  if (NearEquals(incoming.length(), outgoing.length(), 1e-6)) {
    return PathAnchorMode::Symmetric;
  }

  return PathAnchorMode::Smooth;
}

void EditablePathSpline::enforceMode(EditablePathAnchor& anchor,
                                     std::optional<PathHandleType> movedHandle) {
  if (anchor.mode == PathAnchorMode::Corner || !anchor.incomingHandle.has_value() ||
      !anchor.outgoingHandle.has_value()) {
    return;
  }

  std::optional<Vector2d>& primaryHandle =
      (!movedHandle.has_value() || *movedHandle == PathHandleType::Outgoing) ? anchor.outgoingHandle
                                                                             : anchor.incomingHandle;
  std::optional<Vector2d>& secondaryHandle =
      (!movedHandle.has_value() || *movedHandle == PathHandleType::Outgoing) ? anchor.incomingHandle
                                                                             : anchor.outgoingHandle;

  const Vector2d primaryVector =
      *primaryHandle - anchor.position;
  if (NearZero(primaryVector.lengthSquared(), 1e-12)) {
    return;
  }

  if (anchor.mode == PathAnchorMode::Symmetric) {
    *secondaryHandle = anchor.position - primaryVector;
    return;
  }

  const double secondaryLength = (*secondaryHandle - anchor.position).length();
  const Vector2d direction = primaryVector.normalize();
  *secondaryHandle = anchor.position - direction * secondaryLength;
}

namespace {

Vector2d evalCubicBezier(const Vector2d& p0, const Vector2d& p1, const Vector2d& p2,
                         const Vector2d& p3, double t) {
  const double u = 1.0 - t;
  return p0 * (u * u * u) + p1 * (3.0 * u * u * t) + p2 * (3.0 * u * t * t) + p3 * (t * t * t);
}

}  // namespace

std::optional<PathAnchorId> EditablePathSpline::hitTestAnchor(const Vector2d& worldPoint,
                                                              const HitTestParams& params) const {
  for (const auto& contour : contours_) {
    for (const auto& anchor : contour.anchors) {
      const Vector2d wp = params.localToWorld.transformPosition(anchor.position);
      if ((wp - worldPoint).length() <= params.anchorRadius) {
        return anchor.id;
      }
    }
  }
  return std::nullopt;
}

std::optional<EditablePathSpline::HandleHit> EditablePathSpline::hitTestHandle(
    const Vector2d& worldPoint, const HitTestParams& params) const {
  for (const auto& contour : contours_) {
    for (const auto& anchor : contour.anchors) {
      if (anchor.incomingHandle.has_value()) {
        const Vector2d wp = params.localToWorld.transformPosition(*anchor.incomingHandle);
        if ((wp - worldPoint).length() <= params.handleRadius) {
          return HandleHit{anchor.id, PathHandleType::Incoming};
        }
      }
      if (anchor.outgoingHandle.has_value()) {
        const Vector2d wp = params.localToWorld.transformPosition(*anchor.outgoingHandle);
        if ((wp - worldPoint).length() <= params.handleRadius) {
          return HandleHit{anchor.id, PathHandleType::Outgoing};
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<EditablePathSpline::SegmentHit> EditablePathSpline::hitTestSegment(
    const Vector2d& worldPoint, const HitTestParams& params) const {
  double bestDist = params.segmentRadius;
  std::optional<SegmentHit> bestHit;

  for (const auto& contour : contours_) {
    const size_t anchorCount = contour.anchors.size();
    const size_t segmentCount =
        contour.closed ? anchorCount : (anchorCount > 0 ? anchorCount - 1 : 0);

    for (size_t seg = 0; seg < segmentCount; ++seg) {
      const auto& a = contour.anchors[seg];
      const auto& b = contour.anchors[(seg + 1) % anchorCount];
      const bool isCubic = a.outgoingHandle.has_value() || b.incomingHandle.has_value();

      double closestT = 0.5;
      double closestDist = bestDist + 1.0;

      for (int s = 1; s < params.segmentSamples; ++s) {
        const double t = static_cast<double>(s) / params.segmentSamples;
        Vector2d sampleLocal;
        if (isCubic) {
          sampleLocal = evalCubicBezier(a.position, a.outgoingHandle.value_or(a.position),
                                        b.incomingHandle.value_or(b.position), b.position, t);
        } else {
          sampleLocal = a.position + (b.position - a.position) * t;
        }

        const Vector2d sampleWorld = params.localToWorld.transformPosition(sampleLocal);
        const double d = (sampleWorld - worldPoint).length();
        if (d < closestDist) {
          closestDist = d;
          closestT = t;
        }
      }

      if (closestDist < bestDist) {
        bestDist = closestDist;
        bestHit = SegmentHit{a.id, closestT};
      }
    }
  }

  return bestHit;
}

PathContourId EditablePathSpline::allocateContourId() {
  return PathContourId{nextContourId_++};
}

PathAnchorId EditablePathSpline::allocateAnchorId() {
  return PathAnchorId{nextAnchorId_++};
}

}  // namespace donner::svg
