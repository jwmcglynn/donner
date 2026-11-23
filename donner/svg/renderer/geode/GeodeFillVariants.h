#pragma once
/// @file
/// Number of fill and stroke encode variants an entity keeps, shared by the CPU encode cache and
/// the GPU residence so neither header has to include the other.

#include <cstddef>

namespace donner::geode {

/// Number of fill encodes an entity keeps, one per device-derived tolerance bucket. See
/// `GeodePathCacheComponent::FillVariant`; `GeodeResidentPathSlots` keeps one resident fill slot
/// per variant.
inline constexpr std::size_t kGeodeFillVariantCount = 2;

/// Number of stroke outlines an entity keeps, one per stroke key (stroke style, device-derived
/// flattening tolerance, and host-space transform). See `GeodePathCacheComponent::StrokeVariant`;
/// `GeodeResidentPathSlots` keeps resident stroke slots per variant.
inline constexpr std::size_t kGeodeStrokeVariantCount = 8;

}  // namespace donner::geode
