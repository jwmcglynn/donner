#include "donner/editor/FontPreviewCache.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace donner::editor {
namespace {

constexpr std::size_t kMaxEntries = 256;
constexpr std::size_t kMaxSvgBytes = 256 * 1024;
constexpr std::size_t kMaxBitmapBytes = 256 * 1024;
constexpr std::size_t kMaxIdentityBytes = 4096;
constexpr std::string_view kMagic = "DFPV3\n";

std::string EntryIdentity(std::string_view build, std::string_view family,
                          std::string_view content) {
  std::string identity(build);
  identity += '\0';
  identity += family;
  identity += '\0';
  identity += content;
  return identity;
}

struct ScopedDirectory {
  std::filesystem::path path;
  ~ScopedDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

std::uint64_t Hash(std::string_view bytes) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (unsigned char byte : bytes) {
    hash = (hash ^ byte) * 1099511628211ULL;
  }
  return hash;
}

std::string Hex(std::uint64_t value) {
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << value;
  return output.str();
}

bool ValidBitmap(const svg::RendererBitmap& bitmap) {
  if (bitmap.dimensions.x <= 0 || bitmap.dimensions.y <= 0 || bitmap.dimensions.x > 4096 ||
      bitmap.dimensions.y > 4096 ||
      bitmap.rowBytes < static_cast<std::size_t>(bitmap.dimensions.x) * 4 ||
      bitmap.rowBytes > kMaxBitmapBytes) {
    return false;
  }
  return bitmap.pixels.size() == bitmap.rowBytes * static_cast<std::size_t>(bitmap.dimensions.y) &&
         bitmap.pixels.size() <= kMaxBitmapBytes;
}

bool ValidOutlinedSvg(std::string_view svg) {
  return !svg.empty() && svg.size() <= kMaxSvgBytes && svg.find("<svg") != std::string_view::npos &&
         svg.find("<path") != std::string_view::npos && svg.find("<text") == std::string_view::npos;
}

bool ValidStoreInputs(const std::filesystem::path& directory, std::string_view buildIdentity,
                      std::string_view contentIdentity, std::string_view outlinedSvg,
                      const svg::RendererBitmap& bitmap) {
  return !directory.empty() && !buildIdentity.empty() && !contentIdentity.empty() &&
         ValidOutlinedSvg(outlinedSvg) && ValidBitmap(bitmap);
}

std::optional<std::string> ReadOutlinedSvg(const std::filesystem::path& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size > kMaxSvgBytes) {
    return std::nullopt;
  }
  std::ifstream file(path, std::ios::binary);
  std::string svg(static_cast<std::size_t>(size), '\0');
  file.read(svg.data(), static_cast<std::streamsize>(svg.size()));
  if (!file || file.peek() != std::char_traits<char>::eof() || !ValidOutlinedSvg(svg)) {
    return std::nullopt;
  }
  return svg;
}

struct SidecarHeader {
  std::size_t rowBytes = 0;
  svg::AlphaType alphaType = svg::AlphaType::Unpremultiplied;
  std::uint64_t pixelHash = 0;
  std::size_t pixelBytes = 0;
};

std::optional<SidecarHeader> ReadSidecarHeader(std::ifstream& file, std::string_view identity,
                                               Vector2i dimensions, std::uint64_t svgHash) {
  std::string magic(kMagic.size(), '\0');
  file.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  std::string line;
  line.reserve(128);
  char next = '\0';
  while (line.size() < 128 && file.get(next) && next != '\n') {
    line.push_back(next);
  }
  if (!file || magic != kMagic || next != '\n') {
    return std::nullopt;
  }
  std::istringstream input(line);
  int width = 0;
  int height = 0;
  std::size_t rowBytes = 0;
  int alphaType = -1;
  std::size_t identitySize = 0;
  std::uint64_t savedSvgHash = 0;
  std::uint64_t pixelHash = 0;
  input >> width >> height >> rowBytes >> alphaType >> identitySize >> savedSvgHash >> pixelHash;
  std::string trailing;
  if (!input || (input >> trailing) || width != dimensions.x || height != dimensions.y ||
      width <= 0 || height <= 0 || width > 4096 || height > 4096 || rowBytes > kMaxBitmapBytes ||
      rowBytes < static_cast<std::size_t>(width) * 4 || identitySize != identity.size() ||
      savedSvgHash != svgHash ||
      (alphaType != static_cast<int>(svg::AlphaType::Premultiplied) &&
       alphaType != static_cast<int>(svg::AlphaType::Unpremultiplied)) ||
      static_cast<std::size_t>(height) > kMaxBitmapBytes / rowBytes) {
    return std::nullopt;
  }
  return SidecarHeader{rowBytes, static_cast<svg::AlphaType>(alphaType), pixelHash,
                       rowBytes * static_cast<std::size_t>(height)};
}

std::optional<svg::RendererBitmap> ReadBitmapSidecar(const std::filesystem::path& path,
                                                     std::string_view identity, Vector2i dimensions,
                                                     std::uint64_t svgHash) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size > kMaxBitmapBytes + 128 + kMaxIdentityBytes) {
    return std::nullopt;
  }
  std::ifstream file(path, std::ios::binary);
  const auto header = ReadSidecarHeader(file, identity, dimensions, svgHash);
  if (!header.has_value()) {
    return std::nullopt;
  }
  std::string savedIdentity(identity.size(), '\0');
  file.read(savedIdentity.data(), static_cast<std::streamsize>(savedIdentity.size()));
  if (!file || savedIdentity != identity) {
    return std::nullopt;
  }
  svg::RendererBitmap bitmap;
  bitmap.dimensions = dimensions;
  bitmap.rowBytes = header->rowBytes;
  bitmap.alphaType = header->alphaType;
  bitmap.pixels.resize(header->pixelBytes);
  file.read(reinterpret_cast<char*>(bitmap.pixels.data()),
            static_cast<std::streamsize>(bitmap.pixels.size()));
  if (!file || file.peek() != std::char_traits<char>::eof() ||
      Hash(std::string_view(reinterpret_cast<const char*>(bitmap.pixels.data()),
                            bitmap.pixels.size())) != header->pixelHash) {
    return std::nullopt;
  }
  return bitmap;
}

bool AcquireEntryLock(const std::filesystem::path& path) {
  std::error_code error;
  if (std::filesystem::create_directory(path, error)) {
    return true;
  }
  error.clear();
  const auto modified = std::filesystem::last_write_time(path, error);
  if (error || std::filesystem::file_time_type::clock::now() - modified < std::chrono::hours(1)) {
    return false;
  }
  std::filesystem::remove_all(path, error);
  return !error && std::filesystem::create_directory(path, error);
}

bool WriteOutlinedSvg(const std::filesystem::path& path, std::string_view svg) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(svg.data(), static_cast<std::streamsize>(svg.size()));
  return static_cast<bool>(file);
}

bool WriteBitmapSidecar(const std::filesystem::path& path, std::string_view identity,
                        std::string_view outlinedSvg, const svg::RendererBitmap& bitmap) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  const std::string_view pixels(reinterpret_cast<const char*>(bitmap.pixels.data()),
                                bitmap.pixels.size());
  file << kMagic << bitmap.dimensions.x << ' ' << bitmap.dimensions.y << ' ' << bitmap.rowBytes
       << ' ' << static_cast<int>(bitmap.alphaType) << ' ' << identity.size() << ' '
       << Hash(outlinedSvg) << ' ' << Hash(pixels) << '\n';
  file.write(identity.data(), static_cast<std::streamsize>(identity.size()));
  file.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
  return static_cast<bool>(file);
}

bool PublishEntry(const std::filesystem::path& base, const std::filesystem::path& svgTemp,
                  const std::filesystem::path& bitmapTemp) {
  std::error_code error;
  std::filesystem::rename(svgTemp, base.string() + ".svg", error);
  if (error) {
    return false;
  }
  std::filesystem::rename(bitmapTemp, base.string() + ".rgba", error);
  if (error) {
    std::filesystem::remove(base.string() + ".svg", error);
    return false;
  }
  return true;
}

}  // namespace

FontPreviewCache::FontPreviewCache(std::filesystem::path directory, std::string buildIdentity)
    : directory_(std::move(directory)), buildIdentity_(std::move(buildIdentity)) {}

std::filesystem::path FontPreviewCache::stem(std::string_view family,
                                             std::string_view contentIdentity,
                                             Vector2i dimensions) const {
  std::string key = EntryIdentity(buildIdentity_, family, contentIdentity);
  key += '\0';
  key += std::to_string(dimensions.x) + "x" + std::to_string(dimensions.y);
  return directory_ / Hex(Hash(key));
}

std::optional<svg::RendererBitmap> FontPreviewCache::load(std::string_view family,
                                                          std::string_view contentIdentity,
                                                          Vector2i dimensions) const {
  const std::string identity = EntryIdentity(buildIdentity_, family, contentIdentity);
  if (directory_.empty() || buildIdentity_.empty() || contentIdentity.empty() ||
      identity.size() > kMaxIdentityBytes) {
    return std::nullopt;
  }
  const auto base = stem(family, contentIdentity, dimensions);
  const auto svg = ReadOutlinedSvg(base.string() + ".svg");
  if (!svg.has_value()) {
    return std::nullopt;
  }
  return ReadBitmapSidecar(base.string() + ".rgba", identity, dimensions, Hash(*svg));
}

void FontPreviewCache::store(std::string_view family, std::string_view contentIdentity,
                             std::string_view outlinedSvg,
                             const svg::RendererBitmap& bitmap) const {
  if (!ValidStoreInputs(directory_, buildIdentity_, contentIdentity, outlinedSvg, bitmap)) {
    return;
  }
  const std::string identity = EntryIdentity(buildIdentity_, family, contentIdentity);
  if (identity.size() > kMaxIdentityBytes ||
      load(family, contentIdentity, bitmap.dimensions).has_value()) {
    return;
  }
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  if (error) {
    return;
  }
  const auto base = stem(family, contentIdentity, bitmap.dimensions);
  const std::filesystem::path lockPath = base.string() + ".lock";
  if (!AcquireEntryLock(lockPath)) {
    return;
  }
  const ScopedDirectory lock{lockPath};
  if (load(family, contentIdentity, bitmap.dimensions).has_value()) {
    return;
  }
  const auto svgTemp = lockPath / "preview.svg";
  const auto rgbaTemp = lockPath / "preview.rgba";
  if (!WriteOutlinedSvg(svgTemp, outlinedSvg) ||
      !WriteBitmapSidecar(rgbaTemp, identity, outlinedSvg, bitmap)) {
    return;
  }
  if (!PublishEntry(base, svgTemp, rgbaTemp)) {
    return;
  }
  trim();
}

void FontPreviewCache::trim() const {
  std::error_code error;
  std::vector<std::filesystem::directory_entry> entries;
  for (std::filesystem::directory_iterator it(directory_, error), end; !error && it != end;
       it.increment(error)) {
    if (it->path().extension() == ".svg") {
      entries.push_back(*it);
    }
  }
  if (error || entries.size() <= kMaxEntries) {
    return;
  }
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    std::error_code leftError;
    std::error_code rightError;
    return left.last_write_time(leftError) < right.last_write_time(rightError);
  });
  for (std::size_t index = 0; index < entries.size() - kMaxEntries; ++index) {
    auto base = entries[index].path();
    base.replace_extension("");
    std::filesystem::remove(base.string() + ".svg", error);
    std::filesystem::remove(base.string() + ".rgba", error);
  }
}

}  // namespace donner::editor
