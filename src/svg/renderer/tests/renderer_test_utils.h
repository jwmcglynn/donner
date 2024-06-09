#pragma once

#include <gtest/gtest.h>
#include <stb/stb_image.h>

#include <optional>
#include <vector>

#include "src/base/vector2.h"
#include "src/svg/renderer/renderer_skia.h"
#include "src/svg/svg_document.h"
#include "src/svg/tests/xml_test_utils.h"

namespace donner::svg {

struct Image {
  int width;
  int height;
  size_t strideInPixels;
  std::vector<uint8_t> data;
};

struct AsciiImage {
  std::string generated;

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

class RendererTestUtils {
public:
  static std::optional<Image> readRgbaImageFromPngFile(const char* filename) {
    int width, height, channels;
    auto data = stbi_load(filename, &width, &height, &channels, 4);
    if (!data) {
      ADD_FAILURE() << "Failed to load image: " << filename;
      return std::nullopt;
    }

    Image result{width, height, static_cast<size_t>(width),
                 std::vector<uint8_t>(data, data + static_cast<ptrdiff_t>(width * height * 4))};
    stbi_image_free(data);
    return result;
  }

  /**
   * Render the given SVG fragment into ASCII art. The generated image is of the given size, and has
   * a black background.
   *
   * Colors will be mapped to ASCII characters, with `.` white all the way to `@` black, with ten
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
  static AsciiImage renderToAsciiImage(std::string_view svgFragment, Vector2i size = {16, 16}) {
    SVGDocument document = instantiateSubtree(svgFragment, XMLParser::Options(), size);

    RendererSkia renderer;

    AsciiImage result;
    result.generated = renderer.drawIntoAscii(document);
    return result;
  }
};

}  // namespace donner::svg
