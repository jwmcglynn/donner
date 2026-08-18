#include "donner/svg/resources/SystemFontProvider.h"

#include <algorithm>

#include "donner/base/StringUtils.h"

#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>

namespace donner::svg {

namespace {

/// RAII wrapper that CFRelease()s a CoreFoundation object on scope exit.
template <typename T>
class CFRef {
public:
  explicit CFRef(T ref) : ref_(ref) {}
  ~CFRef() {
    if (ref_ != nullptr) {
      CFRelease(ref_);
    }
  }
  CFRef(const CFRef&) = delete;
  CFRef& operator=(const CFRef&) = delete;
  T get() const { return ref_; }
  explicit operator bool() const { return ref_ != nullptr; }

private:
  T ref_;
};

/// Convert a CFStringRef to a UTF-8 std::string.
std::string cfStringToUtf8(CFStringRef str) {
  if (str == nullptr) {
    return {};
  }
  const CFIndex length = CFStringGetLength(str);
  const CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<size_t>(maxSize), '\0');
  if (CFStringGetCString(str, out.data(), maxSize, kCFStringEncodingUTF8)) {
    out.resize(std::strlen(out.c_str()));
    return out;
  }
  return {};
}

void writeBE32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[3] = static_cast<uint8_t>(v & 0xFF);
}

void writeBE16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[1] = static_cast<uint8_t>(v & 0xFF);
}

/// Reconstruct a flat sfnt byte stream from a CTFont's tables. This works for fonts backed by
/// `.ttc`/`.dfont` collections, where reading the file bytes directly would not yield a single
/// usable font. Returns empty on failure.
std::vector<uint8_t> buildSfntFromCTFont(CTFontRef font) {
  CFRef<CFArrayRef> tags(CTFontCopyAvailableTables(font, kCTFontTableOptionNoOptions));
  if (!tags) {
    return {};
  }
  const CFIndex numTablesSigned = CFArrayGetCount(tags.get());
  if (numTablesSigned <= 0) {
    return {};
  }
  const uint16_t numTables = static_cast<uint16_t>(numTablesSigned);

  struct Table {
    uint32_t tag;
    std::vector<uint8_t> data;
  };
  std::vector<Table> tables;
  tables.reserve(numTables);

  bool hasCff = false;
  for (CFIndex i = 0; i < numTablesSigned; ++i) {
    const auto tag =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(CFArrayGetValueAtIndex(tags.get(), i)));
    CFRef<CFDataRef> tableData(
        CTFontCopyTable(font, static_cast<CTFontTableTag>(tag), kCTFontTableOptionNoOptions));
    if (!tableData) {
      continue;
    }
    const CFIndex len = CFDataGetLength(tableData.get());
    Table table;
    table.tag = tag;
    table.data.resize(static_cast<size_t>(len));
    if (len > 0) {
      std::memcpy(table.data.data(), CFDataGetBytePtr(tableData.get()), static_cast<size_t>(len));
    }
    if (tag == 0x43464620u /* 'CFF ' */) {
      hasCff = true;
    }
    tables.push_back(std::move(table));
  }
  if (tables.empty()) {
    return {};
  }

  const uint16_t realNumTables = static_cast<uint16_t>(tables.size());
  uint16_t searchRange = 1;
  uint16_t entrySelector = 0;
  while (searchRange * 2 <= realNumTables) {
    searchRange *= 2;
    entrySelector++;
  }
  searchRange *= 16;
  const uint16_t rangeShift = realNumTables * 16 - searchRange;

  const size_t headerSize = 12 + static_cast<size_t>(realNumTables) * 16;
  size_t totalSize = headerSize;
  for (const Table& table : tables) {
    totalSize += (table.data.size() + 3) & ~size_t{3};
  }

  std::vector<uint8_t> sfnt(totalSize, 0);
  uint8_t* out = sfnt.data();
  writeBE32(out, hasCff ? 0x4F54544Fu /* OTTO */ : 0x00010000u);
  writeBE16(out + 4, realNumTables);
  writeBE16(out + 6, searchRange);
  writeBE16(out + 8, entrySelector);
  writeBE16(out + 10, rangeShift);

  uint32_t dataOffset = static_cast<uint32_t>(headerSize);
  uint8_t* dir = out + 12;
  for (const Table& table : tables) {
    uint32_t checksum = 0;
    const size_t paddedLen = (table.data.size() + 3) & ~size_t{3};
    for (size_t i = 0; i < paddedLen; i += 4) {
      uint32_t word = 0;
      for (size_t j = 0; j < 4 && (i + j) < table.data.size(); ++j) {
        word |= static_cast<uint32_t>(table.data[i + j]) << (24 - 8 * j);
      }
      checksum += word;
    }
    writeBE32(dir, table.tag);
    writeBE32(dir + 4, checksum);
    writeBE32(dir + 8, dataOffset);
    writeBE32(dir + 12, static_cast<uint32_t>(table.data.size()));
    dir += 16;

    if (!table.data.empty()) {
      std::memcpy(out + dataOffset, table.data.data(), table.data.size());
    }
    dataOffset += static_cast<uint32_t>(paddedLen);
  }

  return sfnt;
}

/**
 * Map a CSS `font-weight` (100-900) onto CoreText's normalized `kCTFontWeightTrait` scale, where
 * -1 is thinnest, 0 is regular and 1 is heaviest. The anchors are the ones Apple documents for the
 * `NSFontWeight*` constants.
 */
double NormalizedWeight(int cssWeight) {
  struct Anchor {
    int css;
    double normalized;
  };
  static constexpr Anchor kAnchors[] = {{100, -0.80}, {200, -0.60}, {300, -0.40},
                                        {400, 0.00},  {500, 0.23},  {600, 0.30},
                                        {700, 0.40},  {800, 0.56},  {900, 0.62}};

  if (cssWeight <= kAnchors[0].css) {
    return kAnchors[0].normalized;
  }
  for (size_t i = 1; i < std::size(kAnchors); ++i) {
    if (cssWeight <= kAnchors[i].css) {
      const Anchor& low = kAnchors[i - 1];
      const Anchor& high = kAnchors[i];
      const double t = static_cast<double>(cssWeight - low.css) / (high.css - low.css);
      return low.normalized + t * (high.normalized - low.normalized);
    }
  }
  return kAnchors[std::size(kAnchors) - 1].normalized;
}

/// Map \ref FontStretch (1-9, 5 = normal) onto CoreText's normalized `kCTFontWidthTrait` scale.
double NormalizedWidth(FontStretch stretch) {
  return (static_cast<double>(static_cast<uint8_t>(stretch)) - 5.0) / 4.0;
}

/// Read a numeric entry out of a CoreText traits dictionary, or \p fallback when absent.
double TraitNumber(CFDictionaryRef traits, CFStringRef key, double fallback) {
  const auto number = static_cast<CFNumberRef>(CFDictionaryGetValue(traits, key));
  if (number == nullptr) {
    return fallback;
  }
  double value = fallback;
  if (!CFNumberGetValue(number, kCFNumberDoubleType, &value)) {
    return fallback;
  }
  return value;
}

/**
 * Pick the face within \p familyDescriptor that best matches \p request.
 *
 * Scoring follows the CSS font-matching priority (style, then stretch, then weight) with the same
 * penalty scale as the `@font-face` matcher in \ref FontManager, so document-provided and
 * system-provided families rank their faces the same way.
 *
 * @param familyDescriptor Descriptor carrying only the family name.
 * @param request Face to prefer.
 * @return A retained descriptor for the best face, or nullptr when the family cannot be expanded
 *   (the caller then falls back to the family default).
 * @see https://www.w3.org/TR/css-fonts-4/#font-style-matching
 */
CTFontDescriptorRef BestFaceInFamily(CTFontDescriptorRef familyDescriptor,
                                     const FontFaceRequest& request) {
  CFRef<CFSetRef> mandatory([] {
    const void* keys[] = {kCTFontFamilyNameAttribute};
    return CFSetCreate(kCFAllocatorDefault, keys, 1, &kCFTypeSetCallBacks);
  }());
  if (!mandatory) {
    return nullptr;
  }

  CFRef<CFArrayRef> faces(
      CTFontDescriptorCreateMatchingFontDescriptors(familyDescriptor, mandatory.get()));
  if (!faces) {
    return nullptr;
  }

  const double wantWeight = NormalizedWeight(request.weight);
  const double wantWidth = NormalizedWidth(request.stretch);
  const bool wantSlanted = request.style != FontStyle::Normal;

  CTFontDescriptorRef best = nullptr;
  double bestScore = std::numeric_limits<double>::max();

  const CFIndex count = CFArrayGetCount(faces.get());
  for (CFIndex i = 0; i < count; ++i) {
    const auto face = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(faces.get(), i));
    CFRef<CFDictionaryRef> traits(
        static_cast<CFDictionaryRef>(CTFontDescriptorCopyAttribute(face, kCTFontTraitsAttribute)));
    if (!traits) {
      continue;
    }

    const auto symbolic =
        static_cast<uint32_t>(TraitNumber(traits.get(), kCTFontSymbolicTrait, 0.0));
    const bool isSlanted = (symbolic & kCTFontTraitItalic) != 0;

    double score = 0.0;
    if (isSlanted != wantSlanted) {
      score += 10000.0;
    }
    score += std::abs(TraitNumber(traits.get(), kCTFontWidthTrait, 0.0) - wantWidth) * 1000.0;
    score += std::abs(TraitNumber(traits.get(), kCTFontWeightTrait, 0.0) - wantWeight) * 100.0;

    if (score < bestScore) {
      bestScore = score;
      best = face;
    }
  }

  return best != nullptr ? static_cast<CTFontDescriptorRef>(CFRetain(best)) : nullptr;
}

}  // namespace

bool SystemFontProvider::isSupported() {
  return true;
}

const std::vector<std::string>& SystemFontProvider::enumeratedFamilies() const {
  std::call_once(enumeratedOnce_, [this]() {
    CFRef<CFArrayRef> names(CTFontManagerCopyAvailableFontFamilyNames());
    if (!names) {
      return;
    }
    const CFIndex count = CFArrayGetCount(names.get());
    familyNames_.reserve(static_cast<size_t>(count));
    for (CFIndex i = 0; i < count; ++i) {
      const auto name = static_cast<CFStringRef>(CFArrayGetValueAtIndex(names.get(), i));
      std::string utf8 = cfStringToUtf8(name);
      // Skip hidden/system-only families (their names begin with a dot, e.g. ".SF NS").
      if (utf8.empty() || utf8.front() == '.') {
        continue;
      }
      familyNames_.push_back(std::move(utf8));
    }
    std::sort(familyNames_.begin(), familyNames_.end());
  });
  return familyNames_;
}

std::vector<FontFamilyInfo> SystemFontProvider::families() const {
  std::vector<FontFamilyInfo> result;
  for (const std::string& name : enumeratedFamilies()) {
    result.push_back(FontFamilyInfo{name, FontSource::System, FontCategory::Unknown});
  }
  return result;
}

bool SystemFontProvider::hasFamily(std::string_view family) const {
  for (const std::string& name : enumeratedFamilies()) {
    if (StringUtils::Equals<StringComparison::IgnoreCase>(name, family)) {
      return true;
    }
  }
  return false;
}

std::vector<uint8_t> SystemFontProvider::loadFamilyData(std::string_view family,
                                                        const FontFaceRequest& request) const {
  // Guard against CoreText silently substituting a fallback font for an unknown family name.
  if (!hasFamily(family)) {
    return {};
  }

  const std::string familyStr(family);
  CFRef<CFStringRef> cfFamily(
      CFStringCreateWithCString(kCFAllocatorDefault, familyStr.c_str(), kCFStringEncodingUTF8));
  if (!cfFamily) {
    return {};
  }

  const void* keys[] = {kCTFontFamilyNameAttribute};
  const void* values[] = {cfFamily.get()};
  CFRef<CFDictionaryRef> attrs(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks));
  if (!attrs) {
    return {};
  }
  CFRef<CTFontDescriptorRef> desc(CTFontDescriptorCreateWithAttributes(attrs.get()));
  if (!desc) {
    return {};
  }

  // Expand the family into its individual faces and pick the one matching `request`. Building a
  // font straight from the family-only descriptor always yields the family's default (regular
  // upright) face, which is what makes bold and italic render as regular.
  CFRef<CTFontDescriptorRef> best(BestFaceInFamily(desc.get(), request));
  CFRef<CTFontRef> font(
      CTFontCreateWithFontDescriptor(best ? best.get() : desc.get(), 0.0, nullptr));
  if (!font) {
    return {};
  }
  return buildSfntFromCTFont(font.get());
}

}  // namespace donner::svg

#else  // !__APPLE__

namespace donner::svg {

// Stub for non-Apple platforms: no system font enumeration.

bool SystemFontProvider::isSupported() {
  return false;
}

const std::vector<std::string>& SystemFontProvider::enumeratedFamilies() const {
  std::call_once(enumeratedOnce_, []() {});
  return familyNames_;
}

std::vector<FontFamilyInfo> SystemFontProvider::families() const {
  return {};
}

bool SystemFontProvider::hasFamily(std::string_view /*family*/) const {
  return false;
}

std::vector<uint8_t> SystemFontProvider::loadFamilyData(std::string_view /*family*/,
                                                        const FontFaceRequest& /*request*/) const {
  return {};
}

}  // namespace donner::svg

#endif  // __APPLE__
