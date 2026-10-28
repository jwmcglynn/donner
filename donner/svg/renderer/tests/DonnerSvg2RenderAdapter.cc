/// @file
/// Legacy JSON request entry point for the shared native suite renderer.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#ifdef DONNER_TEXT_ENABLED
#include "donner/base/PackagedResource.h"
#endif

#include "donner/svg/renderer/tests/SvgSuiteRender.h"
#include "nlohmann/json.hpp"

namespace donner::svg {
namespace {

std::optional<std::string> readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return std::nullopt;
  }
  const auto size = file.tellg();
  if (size < 0 || size > 1024 * 1024) {
    return std::nullopt;
  }
  std::string data(static_cast<size_t>(size), '\0');
  file.seekg(0);
  if (!file.read(data.data(), size)) {
    return std::nullopt;
  }
  return data;
}

void writeResponse(const std::string& path, const nlohmann::json& document) {
  std::ofstream out(path, std::ios::binary);
  out << document.dump(2) << "\n";
}

std::optional<int> readDimension(const nlohmann::json& value) {
  if (value.is_number_unsigned()) {
    const auto number = value.get<uint64_t>();
    if (number > 0 && number <= 16384) {
      return static_cast<int>(number);
    }
  } else if (value.is_number_integer()) {
    const auto number = value.get<int64_t>();
    if (number > 0 && number <= 16384) {
      return static_cast<int>(number);
    }
  }
  return std::nullopt;
}

std::optional<std::string> NormalizeFontRoot(const std::string& root) {
  if (root.empty()) {
    return root;
  }
  std::error_code error;
  const bool empty = std::filesystem::is_empty(root, error);
  if (error) {
    return std::nullopt;
  }
  return empty ? std::string() : root;
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
  const nlohmann::json request =
      nlohmann::json::parse(*requestText, nullptr, /*allow_exceptions=*/false);
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
  std::string fontRoot = request.value("font_root", std::string());
  const auto width = readDimension(request["width"]);
  const auto height = readDimension(request["height"]);
  if (!width || !height) {
    return requestError("invalid bounded dimensions");
  }
  const auto normalizedFontRoot = NormalizeFontRoot(fontRoot);
  if (!normalizedFontRoot) {
    return requestError("cannot inspect staged font directory");
  }

  const std::filesystem::path resourceDir = resourceRoot.empty()
                                                ? std::filesystem::path(inputPath).parent_path()
                                                : std::filesystem::path(resourceRoot);
#ifdef DONNER_TEXT_ENABLED
  std::string resourceError;
  if (!donner::PreparePackagedResources(&resourceError)) {
    return requestError("Packaged resources unavailable: " + resourceError);
  }
#endif

  const auto rendered =
      RenderSvgSuiteCase({inputPath, resourceDir, *normalizedFontRoot, *width, *height,
                          std::filesystem::path(requestPath).parent_path()});
  if (!rendered.image) {
    return requestError(rendered.error);
  }
  const auto& image = *rendered.image;
  const int renderedWidth = image.width;
  const int renderedHeight = image.height;
  const auto writeError = svg_suite::WritePng(
      outputPath, {image.width, image.height, image.strideInPixels, image.data});
  if (!writeError.empty()) {
    return requestError(writeError);
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
