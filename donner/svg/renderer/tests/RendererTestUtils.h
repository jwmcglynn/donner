#pragma once

#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "donner/base/Vector2.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererSkia.h"
#include "donner/svg/tests/XMLTestUtils.h"

namespace donner::svg {

/**
 * Stores an uncompressed RGBA-format image.
 *
 * Loaded by \ref RendererTestUtils::readRgbaImageFromPngFile.
 */
struct Image {
  int width;                  //!< Image width in pixels.
  int height;                 //!< Image height in pixels.
  size_t strideInPixels;      //!< The stride of \ref data, in pixels.
  std::vector<uint8_t> data;  //!< Pixel data, in RGBA format. Rows are are \ref strideInPixels long
                              //!< (byte length is `strideInPixels * 4`).
};

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
   * Read an RGBA image from a PNG file.
   *
   * @param filename Path to a PNG file to load.
   */
  static std::optional<Image> readRgbaImageFromPngFile(const char* filename);

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
    SVGDocument document = instantiateSubtree(svgFragment, parser::XMLParser::Options(), size);

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
    RendererSkia renderer;
    renderer.setAntialias(false);

    AsciiImage result;
    result.generated = renderer.drawIntoAscii(document);
    return result;
  }
};

}  // namespace donner::svg
