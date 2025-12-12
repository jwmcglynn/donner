#pragma once
/// @file

#include <map>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"

namespace donner::svg {

/**
 * Resolve a typeface using an ordered list of font families, renderer-cached typefaces, the
 * platform font manager, and a fallback font.
 */
sk_sp<SkTypeface> ResolveTypeface(
    const SmallVector<RcString, 1>& families, const SkFontStyle& fontStyle,
    const std::map<std::string, std::vector<sk_sp<SkTypeface>>>& typefaces, SkFontMgr& fontManager,
    sk_sp<SkTypeface> fallbackTypeface);

/**
 * Create a default fallback typeface from the embedded Public Sans font. Falls back to the platform
 * default if embedding fails.
 */
sk_sp<SkTypeface> CreateEmbeddedFallbackTypeface(SkFontMgr& fontManager);

}  // namespace donner::svg
