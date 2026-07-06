#pragma once
/// @file
///
/// Transform `transform=` writeback that preserves the author's chosen function
/// syntax. Forward transform edits (inspector fields and canvas drags) used to
/// canonicalize every author form into `matrix(a,b,c,d,e,f)` via
/// \ref donner::toSVGTransformString. This module re-expresses an edited
/// \ref Transform2d in the author's original function list where the edit is
/// representable as an update to that list (a rotation change on `rotate(45)`
/// writes `rotate(60)`, a move updates `translate(x, y)`, and so on), falling
/// back to `matrix()` only when the edit cannot be represented in the author's
/// structure.

#include <optional>
#include <string_view>

#include "donner/base/RcString.h"
#include "donner/base/Transform.h"

namespace donner::editor {

/// Attempt to re-serialize @p target into the author's function-list syntax
/// parsed from @p authorSource.
///
/// Returns the author-form string (e.g. `rotate(60)`) when the edit is
/// expressible as a parameter update to the author's transform functions, or
/// `std::nullopt` when it is not (a decomposed rotation change against a skew
/// matrix, a move on a rotate-only element, etc.). The returned string always
/// re-parses to @p target within a tight tolerance; callers that receive
/// `std::nullopt` should emit `toSVGTransformString(target)`.
[[nodiscard]] std::optional<RcString> reserializeTransformPreservingSyntax(
    std::string_view authorSource, const Transform2d& target);

/// Serialize @p target for a `transform=` writeback, preserving the author's
/// syntax from @p authorSource where possible and otherwise emitting the
/// canonical \ref donner::toSVGTransformString form. An empty result means the
/// attribute should be removed (the transform is the identity).
[[nodiscard]] RcString serializeTransformForWriteback(const std::optional<RcString>& authorSource,
                                                      const Transform2d& target);

}  // namespace donner::editor
