/// @file
/// Donner render adapter for the portable SVG2 test suite (design 0057,
/// "Adapter protocol" and Rollout step 4).
///
/// The portable reference runner invokes a render adapter as an executable plus
/// an argument array (never a shell string):
///
///   donner_svg2_render_adapter render --request request.json --response response.json
///
/// This adapter renders one case through Donner's existing tiny-skia renderer
/// configuration, the same RenderDocumentWithBackend(..., TinySkia) path the
/// resvg C++ fixture uses, and applies the same font wiring, so its output is
/// identical to what donner/svg/renderer/tests/resvg_test_suite.cc renders.
/// It writes an 8-bit RGBA PNG and a typed response document. It does not
/// compare images; comparison is a separate step in the runner (which reuses
/// Donner's pixelmatch via svg2_image_compare).

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/RcString.h"
#include "donner/css/FontFace.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/resources/FontMetadata.h"
#include "donner/svg/resources/SandboxedFileResourceLoader.h"
#include "nlohmann/json.hpp"

namespace donner::svg {
namespace {

// Load all regular/bold TTF/OTF fonts from a directory and register them as
// @font-face rules on the document, plus the resvg-suite generic-family
// mappings. This mirrors the wiring in ImageComparisonTestFixture so text cases
// render with the same fonts the resvg fixture uses.
std::vector<css::FontFace> loadFontsFromDirectory(const std::filesystem::path& fontsDir) {
  std::vector<css::FontFace> faces;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(fontsDir, error)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto extension = entry.path().extension().string();
    if (extension != ".ttf" && extension != ".otf") {
      continue;
    }

    std::ifstream fontFile(entry.path(), std::ios::binary);
    if (!fontFile) {
      continue;
    }
    fontFile.seekg(0, std::ios::end);
    const auto size = fontFile.tellg();
    fontFile.seekg(0);
    std::vector<uint8_t> fontData(static_cast<size_t>(size));
    fontFile.read(reinterpret_cast<char*>(fontData.data()), size);

    const auto metadata = ParseFontMetadata(fontData);
    if (!metadata.has_value()) {
      continue;
    }

    // Only register regular (400) and bold (700) weights; other weights use
    // glyph shapes the stb_truetype renderer cannot match to the reference.
    if (metadata->fontWeight != 400 && metadata->fontWeight != 700) {
      continue;
    }

    css::FontFaceSource source;
    source.kind = css::FontFaceSource::Kind::Data;
    source.payload = std::make_shared<const std::vector<uint8_t>>(std::move(fontData));

    css::FontFace face;
    face.familyName = RcString(metadata->familyName);
    face.fontWeight = metadata->fontWeight;
    face.fontStyle = metadata->fontStyle;
    face.fontStretch = metadata->fontStretch;
    face.sources.push_back(std::move(source));
    faces.push_back(std::move(face));
  }
  return faces;
}

void registerFontsFromDirectory(SVGDocument& document, const std::filesystem::path& fontsDir) {
  const std::vector<css::FontFace> faces = loadFontsFromDirectory(fontsDir);
  if (!faces.empty()) {
    auto& resourceManager = document.registry().ctx().get<components::ResourceManagerContext>();
    resourceManager.addFontFaces(faces);
  }

  auto& registry = document.registry();
  auto& fontManager = registry.ctx().contains<FontManager>()
                          ? registry.ctx().get<FontManager>()
                          : registry.ctx().emplace<FontManager>(registry);
  fontManager.setGenericFamilyMapping("serif", "Noto Serif");
  fontManager.setGenericFamilyMapping("sans-serif", "Noto Sans");
  fontManager.setGenericFamilyMapping("monospace", "Noto Mono");
  fontManager.setGenericFamilyMapping("cursive", "Yellowtail");
  fontManager.setGenericFamilyMapping("fantasy", "Sedgwick Ave Display");
}

std::optional<std::string> readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  file.seekg(0, std::ios::end);
  const std::streamsize length = file.tellg();
  file.seekg(0);
  if (length < 0) {
    return std::nullopt;
  }
  std::string data(static_cast<size_t>(length), '\0');
  file.read(data.data(), length);
  return data;
}

void writeResponse(const std::string& path, const nlohmann::json& document) {
  std::ofstream out(path, std::ios::binary);
  out << document.dump(2) << "\n";
}

int runRender(const std::string& requestPath, const std::string& responsePath) {
  const auto requestText = readFile(requestPath);
  if (!requestText) {
    nlohmann::json response;
    response["status"] = "error";
    response["diagnostics"] = "cannot read request file: " + requestPath;
    writeResponse(responsePath, response);
    return 1;
  }

  // Exceptions are disabled in this build, so parse without throwing and check
  // every field explicitly rather than relying on json::at().
  const nlohmann::json request = nlohmann::json::parse(*requestText, nullptr, /*allow_exceptions=*/false);
  auto requestError = [&](const std::string& message) {
    nlohmann::json response;
    response["status"] = "error";
    response["diagnostics"] = message;
    writeResponse(responsePath, response);
    return 1;
  };
  if (request.is_discarded() || !request.is_object()) {
    return requestError("request is not a valid JSON object");
  }
  if (!request.contains("input") || !request["input"].is_string() || !request.contains("output") ||
      !request["output"].is_string() || !request.contains("width") ||
      !request["width"].is_number_integer() || !request.contains("height") ||
      !request["height"].is_number_integer()) {
    return requestError("request is missing required fields (input, output, width, height)");
  }

  if ((request.contains("resource_root") && !request["resource_root"].is_string()) ||
      (request.contains("font_root") && !request["font_root"].is_string())) {
    return requestError("resource_root and font_root must be strings when present");
  }

  const std::string inputPath = request["input"].get<std::string>();
  const std::string outputPath = request["output"].get<std::string>();
  const std::string resourceRoot = request.value("resource_root", std::string());
  const std::string fontRoot = request.value("font_root", std::string());
  const int width = request["width"].get<int>();
  const int height = request["height"].get<int>();

  RegisterTinySkiaBackend();

  const auto svgData = readFile(inputPath);
  if (!svgData) {
    nlohmann::json response;
    response["status"] = "error";
    response["diagnostics"] = "cannot read input SVG: " + inputPath;
    writeResponse(responsePath, response);
    return 1;
  }

  // Resource lookups (external images, etc.) are rooted at the runner-provided
  // resource root, matching the resvg fixture's SandboxedFileResourceLoader.
  const std::filesystem::path resourceDir =
      resourceRoot.empty() ? std::filesystem::path(inputPath).parent_path()
                           : std::filesystem::path(resourceRoot);

  SVGDocument::Settings settings;
  settings.resourceLoader =
      std::make_unique<SandboxedFileResourceLoader>(resourceDir, std::filesystem::path(inputPath));

  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  parser::SVGParser::Options parserOptions;
  auto parseResult =
      parser::SVGParser::ParseSVG(*svgData, warningSink, parserOptions, std::move(settings));
  if (parseResult.hasError()) {
    std::ostringstream diagnostics;
    diagnostics << "parse error: " << parseResult.error();
    nlohmann::json response;
    response["status"] = "error";
    response["diagnostics"] = diagnostics.str();
    writeResponse(responsePath, response);
    return 1;
  }

  SVGDocument document = std::move(parseResult.result());

  if (!fontRoot.empty() && std::filesystem::is_directory(fontRoot)) {
    registerFontsFromDirectory(document, fontRoot);
  }

  document.setCanvasSize(width, height);

  const RendererBitmap snapshot = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  const int renderedWidth = snapshot.dimensions.x;
  const int renderedHeight = snapshot.dimensions.y;
  const size_t strideInPixels = snapshot.rowBytes / 4u;

  if (!RendererImageIO::writeRgbaPixelsToPngFile(outputPath.c_str(), snapshot.pixels, renderedWidth,
                                                 renderedHeight, strideInPixels)) {
    nlohmann::json response;
    response["status"] = "error";
    response["diagnostics"] = "failed to write output PNG: " + outputPath;
    writeResponse(responsePath, response);
    return 1;
  }

  nlohmann::json response;
  response["status"] = "ok";
  response["width"] = renderedWidth;
  response["height"] = renderedHeight;
  response["format"] = "rgba8";
  writeResponse(responsePath, response);
  return 0;
}

}  // namespace
}  // namespace donner::svg

int main(int argc, char** argv) {
  std::string operation;
  std::string requestPath;
  std::string responsePath;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--request") {
      if (i + 1 >= argc) {
        std::cerr << "missing value for --request\n";
        return 2;
      }
      requestPath = argv[++i];
    } else if (arg == "--response") {
      if (i + 1 >= argc) {
        std::cerr << "missing value for --response\n";
        return 2;
      }
      responsePath = argv[++i];
    } else if (!arg.empty() && arg[0] != '-') {
      operation = arg;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 2;
    }
  }

  if (operation != "render") {
    std::cerr << "only the 'render' operation is supported\n";
    return 2;
  }
  if (requestPath.empty() || responsePath.empty()) {
    std::cerr << "both --request and --response are required\n";
    return 2;
  }

  return donner::svg::runRender(requestPath, responsePath);
}
