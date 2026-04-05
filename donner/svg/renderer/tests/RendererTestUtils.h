/// @file
#pragma once

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/Vector2.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {

/**
 * Stores an ASCII representation of a rendered image, and supports diffing it to another image.
 */
struct AsciiImage {
  std::string generated;  //!< ASCII art of generated image, with lines separated by `\n`

  /**
   * Compare the rendered ASCII image to a golden ASCII string, and output the image differences if
   * any.
   *
   * Example:
   * ```
   * const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
   *       <rect width="8" height="8" fill="white" />
   *       )");
   *
   * EXPECT_TRUE(generatedAscii.matches(R"(
   *       @@@@@@@@........
   *       @@@@@@@@........
   *       @@@@@@@@........
   *       @@@@@@@@........
   *       @@@@@@@@........
   *       @@@@@@@@........
   *       @@@@@@@@........
   *       @@@@@@@@........
   *       ................
   *       ................
   *       ................
   *       ................
   *       ................
   *       ................
   *       ................
   *       ................
   *       )"));
   * ```
   *
   * @param golden The golden ASCII image to compare the rendered output to, which should be a
   * multiline string, whitespaces and the first newlines are removed.
   * @return true if the image matches.
   */
  bool matches(std::string_view golden) const {
    // Remove spaces and newlines at the beginning of the golden image.
    while (!golden.empty() && (std::isspace(golden.front()) || golden.front() == '\n')) {
      golden.remove_prefix(1);
    }

    std::ostringstream diffStream;
    int lineNum = 1;
    bool hasDifferences = false;

    auto genBegin = generated.begin();
    auto goldBegin = golden.begin();

    while (genBegin != generated.end() || goldBegin != golden.end()) {
      auto genEnd = std::find(genBegin, generated.end(), '\n');
      auto goldEnd = std::find(goldBegin, golden.end(), '\n');

      // Remove whitespace at the beginning of the golden image line.
      while (goldBegin != goldEnd && std::isspace(*goldBegin)) {
        ++goldBegin;
      }

      std::string_view genLine(genBegin, genEnd);
      std::string_view goldLine(goldBegin, goldEnd);

      if (genLine != goldLine) {
        hasDifferences = true;
        diffStream << "Line " << lineNum << ":\n";
        diffStream << "Generated: " << genLine << "\n";
        diffStream << "Expected:  " << goldLine << "\n\n";
      }

      if (genEnd != generated.end()) {
        ++genEnd;  // Skip newline character
      }
      if (goldEnd != golden.end()) {
        ++goldEnd;  // Skip newline character
      }

      genBegin = genEnd;
      goldBegin = goldEnd;
      ++lineNum;
    }

    if (hasDifferences) {
      std::cerr << "ASCII outputs differ:\n" << diffStream.str();
      std::cerr << "\nGenerated image:\n--------\n" << generated << "--------\n";
    }

    return !hasDifferences;
  }
};

/**
 * Test utilities for rendering and saving SVGs in tests.
 */
class RendererTestUtils {
public:
  /**
   * Returns true when the active test renderer backend is tiny-skia.
   */
  static bool isTinySkiaBackend() { return ActiveRendererBackend() == RendererBackend::TinySkia; }

  /**
   * Convert a snapshot bitmap to ASCII art, mapping grayscale intensity to ten glyph levels.
   *
   * @param snapshot Renderer snapshot in RGBA format.
   * @return ASCII art with one newline-terminated row per image row.
   */
  static std::string snapshotToAscii(const RendererBitmap& snapshot) {
    if (snapshot.empty() || snapshot.rowBytes == 0) {
      return "";
    }

    static constexpr std::string_view kGrayscaleTable = ".,:-=+*#%@";
    const std::size_t width = static_cast<std::size_t>(snapshot.dimensions.x);
    const std::size_t height = static_cast<std::size_t>(snapshot.dimensions.y);
    const std::size_t requiredRowBytes = width * 4u;
    if (snapshot.rowBytes < requiredRowBytes) {
      return "";
    }
    if (snapshot.pixels.size() < snapshot.rowBytes * height) {
      return "";
    }

    std::string asciiArt;
    asciiArt.reserve(width * height + height);

    for (std::size_t y = 0; y < height; ++y) {
      const std::size_t rowStart = y * snapshot.rowBytes;
      for (std::size_t x = 0; x < width; ++x) {
        const std::size_t pixelIndex = rowStart + x * 4u;
        const uint8_t r = snapshot.pixels[pixelIndex];
        const uint8_t g = snapshot.pixels[pixelIndex + 1];
        const uint8_t b = snapshot.pixels[pixelIndex + 2];
        const uint8_t intensity = static_cast<uint8_t>(
            (static_cast<uint32_t>(r) + static_cast<uint32_t>(g) + static_cast<uint32_t>(b)) / 3u);
        const std::size_t tableIndex =
            static_cast<std::size_t>(intensity) * (kGrayscaleTable.size() - 1u) / 255u;
        asciiArt.push_back(kGrayscaleTable[tableIndex]);
      }
      asciiArt.push_back('\n');
    }

    return asciiArt;
  }

  /**
   * Render the given SVG fragment into ASCII art. The generated image is of the given size, and has
   * a black background.
   *
   * Colors will be mapped to ASCII characters, with `@` white all the way to `.` black, with ten
   * shades of gray.
   *
   * To compare the generated ASCII image to a golden ASCII string, use `matches` on the returned
   * \ref AsciiImage object.
   *
   * Example:
   * ```
   * const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"(
   *       <rect width="8" height="8" fill="white" />
   *       )");
   * ```
   *
   * @param svgFragment The SVG fragment to render.
   * @param size The size of the generated image.
   */
  static AsciiImage renderToAsciiImage(std::string_view svgFragment,
                                       Vector2i size = kTestSvgDefaultSize) {
    SVGDocument document = instantiateSubtree(svgFragment, parser::SVGParser::Options(), size);

    return renderToAsciiImage(document);
  }

  /**
   * Render the given \ref SVGDocument into ASCII art. The generated image is of given size, and has
   * a black background.
   *
   * Colors will be mapped to ASCII characters, with `@` white all the way to `.` black, with ten
   * shades of gray.
   *
   * To compare the generated ASCII image to a golden ASCII string, use `matches` on the returned
   * \ref AsciiImage object.
   *
   * @param document SVG document to render, of max size 64x64.
   */
  static AsciiImage renderToAsciiImage(SVGDocument document) {
    AsciiImage result;
    result.generated = snapshotToAscii(RenderDocumentWithActiveBackendForAscii(document));
    return result;
  }
};

}  // namespace donner::svg
