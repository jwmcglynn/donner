#include "donner/svg/resources/EmbeddedFontProvider.h"

#include <algorithm>
#include <array>
#include <span>

#include "donner/base/StringUtils.h"
#include "embed_resources/GoogleFontsData.h"

namespace donner::svg {

namespace {

struct EmbeddedEntry {
  std::string_view family;
  FontCategory category;
  std::span<const unsigned char> data;
};

/// The embedded family table, generated from GOOGLE_FONTS (fonts.bzl). Built once on first use; the
/// spans reference `extern` byte arrays, so this cannot be `constexpr`.
const std::array<EmbeddedEntry, 12>& entries() {
  static const std::array<EmbeddedEntry, 12> kEntries = {{
#define DONNER_GF_ENTRY(familyName, categoryLeaf, span) \
  EmbeddedEntry{familyName, FontCategory::categoryLeaf, span},
#include "embed_resources/GoogleFontsCatalog.inc"
#undef DONNER_GF_ENTRY
  }};
  return kEntries;
}

}  // namespace

std::vector<FontFamilyInfo> EmbeddedFontProvider::families() const {
  std::vector<FontFamilyInfo> result;
  result.reserve(entries().size());
  for (const EmbeddedEntry& entry : entries()) {
    result.push_back(FontFamilyInfo{std::string(entry.family), FontSource::Embedded, entry.category});
  }
  std::sort(result.begin(), result.end(),
            [](const FontFamilyInfo& a, const FontFamilyInfo& b) { return a.family < b.family; });
  return result;
}

bool EmbeddedFontProvider::hasFamily(std::string_view family) const {
  for (const EmbeddedEntry& entry : entries()) {
    if (StringUtils::Equals<StringComparison::IgnoreCase>(entry.family, family)) {
      return true;
    }
  }
  return false;
}

std::vector<uint8_t> EmbeddedFontProvider::loadFamilyData(std::string_view family) const {
  for (const EmbeddedEntry& entry : entries()) {
    if (StringUtils::Equals<StringComparison::IgnoreCase>(entry.family, family)) {
      return std::vector<uint8_t>(entry.data.begin(), entry.data.end());
    }
  }
  return {};
}

}  // namespace donner::svg
