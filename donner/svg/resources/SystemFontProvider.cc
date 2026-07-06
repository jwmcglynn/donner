#include "donner/svg/resources/SystemFontProvider.h"

#include <algorithm>

#include "donner/base/StringUtils.h"

#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <cstdint>
#include <cstring>

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
  CFRef<CFArrayRef> tags(
      CTFontCopyAvailableTables(font, kCTFontTableOptionNoOptions));
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
    const auto tag = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
        CFArrayGetValueAtIndex(tags.get(), i)));
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

std::vector<uint8_t> SystemFontProvider::loadFamilyData(std::string_view family) const {
  // Guard against CoreText silently substituting a fallback font for an unknown family name.
  if (!hasFamily(family)) {
    return {};
  }

  const std::string familyStr(family);
  CFRef<CFStringRef> cfFamily(CFStringCreateWithCString(kCFAllocatorDefault, familyStr.c_str(),
                                                        kCFStringEncodingUTF8));
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
  CFRef<CTFontRef> font(CTFontCreateWithFontDescriptor(desc.get(), 0.0, nullptr));
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

std::vector<uint8_t> SystemFontProvider::loadFamilyData(std::string_view /*family*/) const {
  return {};
}

}  // namespace donner::svg

#endif  // __APPLE__
