/// @file

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_TRUETYPE_TABLES_H

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

namespace donner {
namespace {

struct LibraryDeleter {
  void operator()(FT_Library library) const { FT_Done_FreeType(library); }
};

struct FaceDeleter {
  void operator()(FT_Face face) const { FT_Done_Face(face); }
};

using Library = std::unique_ptr<FT_LibraryRec_, LibraryDeleter>;
using Face = std::unique_ptr<FT_FaceRec_, FaceDeleter>;

bool ReadFont(const char* path, std::vector<uint8_t>& bytes) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  const std::streamoff length = stream.tellg();
  if (!stream || length <= 0 || length > 2 * 1024 * 1024) {
    return false;
  }
  bytes.resize(static_cast<size_t>(length));
  stream.seekg(0);
  return static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()), length));
}

bool CompareGlyph(FT_Face original, FT_Face decoded, FT_UInt glyph) {
  constexpr FT_Int32 kLoadFlags = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
  if (FT_Load_Glyph(original, glyph, kLoadFlags) || FT_Load_Glyph(decoded, glyph, kLoadFlags)) {
    return false;
  }
  const FT_GlyphSlot before = original->glyph;
  const FT_GlyphSlot after = decoded->glyph;
  if (before->format != after->format || before->metrics.width != after->metrics.width ||
      before->metrics.height != after->metrics.height ||
      before->metrics.horiBearingX != after->metrics.horiBearingX ||
      before->metrics.horiBearingY != after->metrics.horiBearingY ||
      before->metrics.horiAdvance != after->metrics.horiAdvance ||
      before->metrics.vertBearingX != after->metrics.vertBearingX ||
      before->metrics.vertBearingY != after->metrics.vertBearingY ||
      before->metrics.vertAdvance != after->metrics.vertAdvance ||
      before->outline.n_points != after->outline.n_points ||
      before->outline.n_contours != after->outline.n_contours) {
    return false;
  }
  for (int i = 0; i < before->outline.n_points; ++i) {
    if (before->outline.points[i].x != after->outline.points[i].x ||
        before->outline.points[i].y != after->outline.points[i].y ||
        before->outline.tags[i] != after->outline.tags[i]) {
      return false;
    }
  }
  for (int i = 0; i < before->outline.n_contours; ++i) {
    if (before->outline.contours[i] != after->outline.contours[i]) {
      return false;
    }
  }
  return true;
}

bool CompareAtCoordinates(FT_Face original, FT_Face decoded,
                          std::vector<FT_Fixed> coordinates) {
  if (!coordinates.empty() &&
      (FT_Set_Var_Design_Coordinates(original, static_cast<FT_UInt>(coordinates.size()),
                                    coordinates.data()) ||
       FT_Set_Var_Design_Coordinates(decoded, static_cast<FT_UInt>(coordinates.size()),
                                    coordinates.data()))) {
    std::cerr << "Cannot set variation coordinates\n";
    return false;
  }
  for (FT_Long glyph = 0; glyph < original->num_glyphs; ++glyph) {
    if (!CompareGlyph(original, decoded, static_cast<FT_UInt>(glyph))) {
      std::cerr << "Outline or metric mismatch at glyph " << glyph << ", coordinates";
      for (FT_Fixed coordinate : coordinates) {
        std::cerr << ' ' << coordinate;
      }
      std::cerr << '\n';
      return false;
    }
  }
  return true;
}

bool CompareFaces(FT_Library library, FT_Face original, FT_Face decoded) {
  if (original->num_glyphs != decoded->num_glyphs ||
      original->units_per_EM != decoded->units_per_EM ||
      original->num_charmaps != decoded->num_charmaps) {
    std::cerr << "Font glyph count, em size, or charmaps changed\n";
    return false;
  }
  for (int map = 0; map < original->num_charmaps; ++map) {
    // Format 14 is a supplementary map, covered by the byte-identical cmap table check.
    if (FT_Get_CMap_Format(original->charmaps[map]) == 14) {
      continue;
    }
    if (FT_Set_Charmap(original, original->charmaps[map]) ||
        FT_Set_Charmap(decoded, decoded->charmaps[map])) {
      std::cerr << "Cannot select character map\n";
      return false;
    }
    FT_UInt originalGlyph = 0;
    FT_UInt decodedGlyph = 0;
    FT_ULong originalChar = FT_Get_First_Char(original, &originalGlyph);
    FT_ULong decodedChar = FT_Get_First_Char(decoded, &decodedGlyph);
    while (originalGlyph != 0 || decodedGlyph != 0) {
      if (originalChar != decodedChar || originalGlyph != decodedGlyph) {
        std::cerr << "Character mapping changed\n";
        return false;
      }
      originalChar = FT_Get_Next_Char(original, originalChar, &originalGlyph);
      decodedChar = FT_Get_Next_Char(decoded, decodedChar, &decodedGlyph);
    }
  }

  if (!CompareAtCoordinates(original, decoded, {})) {
    return false;
  }
  if (!FT_HAS_MULTIPLE_MASTERS(original)) {
    return !FT_HAS_MULTIPLE_MASTERS(decoded);
  }
  FT_MM_Var* variation = nullptr;
  if (FT_Get_MM_Var(original, &variation)) {
    std::cerr << "Cannot read variable font axes\n";
    return false;
  }
  std::vector<FT_Fixed> defaults;
  std::vector<FT_Fixed> minimums;
  std::vector<FT_Fixed> maximums;
  for (FT_UInt index = 0; index < variation->num_axis; ++index) {
    defaults.push_back(variation->axis[index].def);
    minimums.push_back(variation->axis[index].minimum);
    maximums.push_back(variation->axis[index].maximum);
  }
  FT_Done_MM_Var(library, variation);
  for (size_t index = 0; index < defaults.size(); ++index) {
    for (FT_Fixed endpoint : {minimums[index], maximums[index]}) {
      std::vector<FT_Fixed> coordinates = defaults;
      coordinates[index] = endpoint;
      if (!CompareAtCoordinates(original, decoded, coordinates)) {
        return false;
      }
    }
  }
  return CompareAtCoordinates(original, decoded, minimums) &&
         CompareAtCoordinates(original, decoded, maximums);
}

int Compare(const char* originalPath, const char* decodedPath) {
  std::vector<uint8_t> originalBytes;
  std::vector<uint8_t> decodedBytes;
  if (!ReadFont(originalPath, originalBytes) || !ReadFont(decodedPath, decodedBytes)) {
    std::cerr << "Cannot read bounded comparison inputs\n";
    return 1;
  }
  FT_Library rawLibrary = nullptr;
  if (FT_Init_FreeType(&rawLibrary)) {
    return 1;
  }
  Library library(rawLibrary);
  FT_Face rawOriginal = nullptr;
  if (FT_New_Memory_Face(library.get(), originalBytes.data(),
                         static_cast<FT_Long>(originalBytes.size()), 0, &rawOriginal)) {
    return 1;
  }
  Face original(rawOriginal);
  FT_Face rawDecoded = nullptr;
  if (FT_New_Memory_Face(library.get(), decodedBytes.data(),
                         static_cast<FT_Long>(decodedBytes.size()), 0, &rawDecoded)) {
    return 1;
  }
  Face decoded(rawDecoded);
  return CompareFaces(library.get(), original.get(), decoded.get()) ? 0 : 1;
}

}  // namespace
}  // namespace donner

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: catalog_font_compare ORIGINAL.ttf DECODED.ttf\n";
    return 1;
  }
  return donner::Compare(argv[1], argv[2]);
}
