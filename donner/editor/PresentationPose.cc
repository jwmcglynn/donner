#include "donner/editor/PresentationPose.h"

#include <algorithm>
#include <cmath>

namespace donner::editor {

bool PresentationIdentity::sameContent(const PresentationIdentity& other) const {
  return documentGeneration == other.documentGeneration &&
         geometryRevision == other.geometryRevision &&
         fontResourceRevision == other.fontResourceRevision &&
         presentationEpoch == other.presentationEpoch;
}

bool PresentationIdentity::sameScene(const PresentationIdentity& other) const {
  return sameContent(other) && documentRevision == other.documentRevision &&
         version == other.version;
}

bool FinitePresentationTransform(const Transform2d& documentFromElement) {
  return std::ranges::all_of(documentFromElement.data,
                             [](double coefficient) { return std::isfinite(coefficient); });
}

bool SamePresentationTransform(const Transform2d& lhs, const Transform2d& rhs) {
  for (std::size_t i = 0; i < 6; ++i) {
    if (std::abs(lhs.data[i] - rhs.data[i]) > 1e-8) {
      return false;
    }
  }
  return FinitePresentationTransform(lhs) && FinitePresentationTransform(rhs);
}

std::optional<Transform2d> ResolvePresentationTransform(
    std::span<const PresentationPose> captured, std::span<const PresentationPose> requested) {
  if (captured.empty() || captured.size() != requested.size()) {
    return std::nullopt;
  }
  std::optional<Transform2d> presentedDocumentFromCapturedDocument;
  for (std::size_t i = 0; i < captured.size(); ++i) {
    const auto& source = captured[i];
    const auto& target = requested[i];
    if (source.entity != target.entity ||
        !FinitePresentationTransform(source.documentFromElement) ||
        !FinitePresentationTransform(target.documentFromElement) ||
        std::abs(source.documentFromElement.determinant()) < 1e-12) {
      return std::nullopt;
    }
    const Transform2d presentedFromCaptured =
        source.documentFromElement.inverse() * target.documentFromElement;
    if (presentedDocumentFromCapturedDocument.has_value() &&
        !SamePresentationTransform(*presentedDocumentFromCapturedDocument, presentedFromCaptured)) {
      return std::nullopt;
    }
    presentedDocumentFromCapturedDocument = presentedFromCaptured;
  }
  return presentedDocumentFromCapturedDocument;
}

}  // namespace donner::editor
