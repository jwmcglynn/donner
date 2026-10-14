#pragma once
/// @file

#include <optional>
#include <string>
#include <string_view>

#include "donner/base/Path.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"

namespace donner::svg::components {

/// Serialize typed geometry with bounded allocation and round-trip coordinate precision.
/// @param path Typed underlying geometry. @param budget Shared preparation admission.
/// @return SVG data (possibly empty), or nullopt for invalid/refused geometry.
std::optional<std::string> SerializeAnimationPathGeometry(const Path& path,
                                                          AnimationResourceBudget& budget);

/** Sample authored path parameters without geometry normalization.
 * Exactly one of set and value must be non-null.
 * @param progress Current simple progress. @param iteration Completed iterations.
 * @param set Set declaration, or null. @param value Animate declaration, or null.
 * @param underlying Lower-layer path value. @param budget Shared preparation admission.
 * @return Sampled path (possibly empty), or no contribution on incompatible/refused composition.
 */
std::optional<std::string> SampleAnimationPath(double progress, double iteration,
                                               const SetAnimationComponent* set,
                                               const AnimateValueComponent* value,
                                               std::string_view underlying,
                                               AnimationResourceBudget& budget);

}  // namespace donner::svg::components
