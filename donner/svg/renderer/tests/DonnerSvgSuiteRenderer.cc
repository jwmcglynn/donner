/// @file
/// Offline SVG-to-PNG command used by the native suite and comparison tools.

#include <charconv>
#include <iostream>
#include <string>
#include <string_view>

#ifdef DONNER_TEXT_ENABLED
#include "donner/base/PackagedResource.h"
#endif

#include "donner/svg/renderer/tests/SvgSuiteRender.h"

namespace {

bool parseSize(std::string_view value, int& result) {
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  return parsed.ec == std::errc() && parsed.ptr == value.data() + value.size();
}

bool parseOption(std::string_view flag, std::string_view value,
                 donner::svg::SvgSuiteRenderSettings& settings, std::string& output) {
  if (flag == "--input") {
    settings.input = value;
  } else if (flag == "--output") {
    output = value;
  } else if (flag == "--root") {
    settings.resourceRoot = value;
  } else if (flag == "--fonts") {
    settings.fontRoot = value;
  } else if (flag == "--width") {
    return parseSize(value, settings.width);
  } else if (flag == "--height") {
    return parseSize(value, settings.height);
  } else {
    return false;
  }
  return true;
}

bool parseArguments(int argc, char** argv, donner::svg::SvgSuiteRenderSettings& settings,
                    std::string& output) {
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc) {
      std::cerr << "every option requires a value\n";
      return false;
    }
    if (!parseOption(argv[i], argv[i + 1], settings, output)) {
      std::cerr << "unknown option or invalid value: " << argv[i] << "\n";
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  donner::svg::SvgSuiteRenderSettings settings;
  std::string output;
  if (!parseArguments(argc, argv, settings, output)) {
    return 2;
  }
  if (settings.input.empty() || settings.resourceRoot.empty() || output.empty()) {
    std::cerr << "--input, --output and --root are required\n";
    return 2;
  }
#ifdef DONNER_TEXT_ENABLED
  std::string resourceError;
  if (!donner::PreparePackagedResources(&resourceError)) {
    std::cerr << "Packaged resources unavailable: " << resourceError << "\n";
    return 2;
  }
#endif

  const auto result = donner::svg::RenderSvgSuiteCase(settings);
  if (!result.image) {
    std::cerr << result.error << "\n";
    return 2;
  }
  const auto& image = *result.image;
  const auto error = donner::svg_suite::WritePng(
      output, {image.width, image.height, image.strideInPixels, image.data});
  if (!error.empty()) {
    std::cerr << error << "\n";
    return 2;
  }
  return 0;
}
