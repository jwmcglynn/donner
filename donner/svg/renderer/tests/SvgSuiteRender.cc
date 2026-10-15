#include "donner/svg/renderer/tests/SvgSuiteRender.h"

#include <picosha2.h>

#include <algorithm>
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
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/renderer/tests/SvgSuiteReferenceFonts.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/resources/FontMetadata.h"
#include "donner/svg/resources/SandboxedFileResourceLoader.h"

namespace donner::svg {
namespace {

struct LoadedFont {
  std::optional<css::FontFace> face;
  size_t byteCount = 0;
  std::string error;
};

LoadedFont loadFont(const std::filesystem::path& path, size_t remainingBytes) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || std::filesystem::is_symlink(path, error) ||
      error) {
    return {{}, 0, "font input is not a regular non-symlink file"};
  }
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return {{}, 0, "cannot read required font"};
  }
  const auto size = file.tellg();
  if (size <= 0 || size > 32 * 1024 * 1024 || static_cast<size_t>(size) > remainingBytes) {
    return {{}, 0, "required fonts exceed input budget"};
  }
  file.seekg(0);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
    return {{}, 0, "required font was truncated"};
  }
  const auto metadata = ParseFontMetadata(data);
  if (!metadata) {
    return {{}, 0, "required font metadata is invalid"};
  }
  // Preserve the reference fixture's regular/bold font selection.
  if (metadata->fontWeight != 400 && metadata->fontWeight != 700) {
    return {{}, static_cast<size_t>(size), {}};
  }
  css::FontFaceSource source;
  source.kind = css::FontFaceSource::Kind::Data;
  const std::string digest = picosha2::hash256_hex_string(data.begin(), data.end());
  source.payload = std::make_shared<const std::vector<uint8_t>>(std::move(data));
  source.trusted =
      std::find(kSvgSuiteReferenceFontHashes.begin(), kSvgSuiteReferenceFontHashes.end(), digest) !=
      kSvgSuiteReferenceFontHashes.end();
  css::FontFace face;
  face.familyName = RcString(metadata->familyName);
  face.fontWeight = metadata->fontWeight;
  face.fontStyle = metadata->fontStyle;
  face.fontStretch = metadata->fontStretch;
  face.sources.push_back(std::move(source));
  return {std::move(face), static_cast<size_t>(size), {}};
}

std::string loadFontsFromDirectory(const std::filesystem::path& fontsDir,
                                   std::vector<css::FontFace>& faces) {
  size_t remainingBytes = 128u * 1024u * 1024u;
  std::error_code error;
  std::filesystem::directory_iterator entry(fontsDir, error);
  while (!error && entry != std::filesystem::directory_iterator()) {
    const auto path = entry->path();
    const auto extension = path.extension();
    if (extension == ".ttf" || extension == ".otf") {
      auto font = loadFont(path, remainingBytes);
      if (!font.error.empty()) {
        return font.error;
      }
      remainingBytes -= font.byteCount;
      if (font.face) {
        faces.push_back(std::move(*font.face));
      }
    }
    entry.increment(error);
  }
  if (error || faces.empty()) {
    return "font directory is unreadable or has no supported regular/bold fonts";
  }
  return {};
}

std::string registerFontsFromDirectory(SVGDocument& document,
                                       const std::filesystem::path& fontsDir) {
  std::vector<css::FontFace> faces;
  const auto error = loadFontsFromDirectory(fontsDir, faces);
  if (!error.empty()) {
    return error;
  }
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
  return {};
}

std::optional<std::string> readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  file.seekg(0, std::ios::end);
  const std::streamsize length = file.tellg();
  file.seekg(0);
  if (length < 0 || length > 32 * 1024 * 1024) {
    return std::nullopt;
  }
  std::string data(static_cast<size_t>(length), '\0');
  if (!file.read(data.data(), length)) {
    return std::nullopt;
  }
  return data;
}

struct ResolvedInput {
  std::filesystem::path root;
  std::filesystem::path input;
  std::string error;
};

ResolvedInput resolveInput(const SvgSuiteRenderSettings& settings) {
  if (settings.width <= 0 || settings.height <= 0 || settings.width > 16384 ||
      settings.height > 16384 ||
      static_cast<uint64_t>(settings.width) * settings.height > 64u * 1024u * 1024u) {
    return {{}, {}, "unsupported render dimensions"};
  }
  std::error_code error;
  const auto root = std::filesystem::canonical(settings.resourceRoot, error);
  if (error || !std::filesystem::is_directory(root, error)) {
    return {{}, {}, "resource root is not a directory"};
  }
  const auto input = std::filesystem::canonical(settings.input, error);
  if (error) {
    return {{}, {}, "input cannot be resolved"};
  }
  const auto inputRoot =
      settings.inputRoot.empty() ? root : std::filesystem::canonical(settings.inputRoot, error);
  if (error) {
    return {{}, {}, "input root cannot be resolved"};
  }
  const auto relative = input.lexically_relative(inputRoot);
  if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
    return {{}, {}, "input escapes resource root"};
  }
  return {root, input, {}};
}

}  // namespace

svg_suite::ImageResult RenderSvgSuiteCase(const SvgSuiteRenderSettings& settings) {
  const auto resolved = resolveInput(settings);
  if (!resolved.error.empty()) {
    return {std::nullopt, resolved.error};
  }
  const auto svgData = readFile(resolved.input.string());
  if (!svgData) {
    return {std::nullopt, "cannot read bounded SVG input"};
  }
  SVGDocument::Settings documentSettings;
  documentSettings.resourceLoader =
      std::make_unique<SandboxedFileResourceLoader>(resolved.root, resolved.input);
  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  parser::SVGParser::Options parserOptions;
  auto parsed = parser::SVGParser::ParseSVG(*svgData, warningSink, parserOptions,
                                            std::move(documentSettings));
  if (parsed.hasError()) {
    std::ostringstream message;
    message << "parse error: " << parsed.error();
    return {std::nullopt, message.str()};
  }
  SVGDocument document = std::move(parsed.result());
  if (!settings.fontRoot.empty()) {
    std::error_code error;
    if (!std::filesystem::is_directory(settings.fontRoot, error) || error) {
      return {std::nullopt, "font root is not a directory"};
    }
    const auto fontError = registerFontsFromDirectory(document, settings.fontRoot);
    if (!fontError.empty()) {
      return {std::nullopt, fontError};
    }
  }
  document.setCanvasSize(settings.width, settings.height);
  RegisterTinySkiaBackend();
  const RendererBitmap bitmap = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  svg_suite::Image image{bitmap.dimensions.x, bitmap.dimensions.y, bitmap.rowBytes / 4u,
                         bitmap.pixels};
  const std::string imageError =
      svg_suite::ValidateImage({image.width, image.height, image.strideInPixels, image.data});
  if (!imageError.empty()) {
    return {std::nullopt, imageError};
  }
  return {std::move(image), {}};
}

}  // namespace donner::svg
