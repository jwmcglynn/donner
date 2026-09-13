/// @file

#include <woff2/decode.h>
#include <woff2/encode.h>
#include <woff2/output.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace donner {
namespace {

constexpr size_t kMaximumFontBytes = 2 * 1024 * 1024;
constexpr size_t kMaximumBrotliBytes = 8 * 1024 * 1024;

bool ReadFile(const char* path, std::vector<uint8_t>& result) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  const std::streamoff length = stream.tellg();
  if (!stream || length <= 0 || static_cast<size_t>(length) > kMaximumFontBytes) {
    return false;
  }
  result.resize(static_cast<size_t>(length));
  stream.seekg(0);
  return static_cast<bool>(stream.read(reinterpret_cast<char*>(result.data()), length));
}

bool WriteFile(const char* path, const uint8_t* data, size_t length) {
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(length));
  stream.close();
  return static_cast<bool>(stream);
}

int Convert(const char* sourcePath, const char* encodedPath, const char* decodedPath) {
  std::vector<uint8_t> source;
  if (!ReadFile(sourcePath, source)) {
    std::cerr << "Cannot read bounded sfnt input\n";
    return 1;
  }

  woff2::WOFF2Params params;
  params.brotli_quality = 11;
  params.allow_transforms = true;
  size_t encodedSize = woff2::MaxWOFF2CompressedSize(source.data(), source.size());
  if (encodedSize == 0 || encodedSize > 2 * kMaximumFontBytes) {
    std::cerr << "Invalid compression bound\n";
    return 1;
  }

  std::vector<uint8_t> encoded(encodedSize);
  if (!woff2::ConvertTTFToWOFF2(source.data(), source.size(), encoded.data(), &encodedSize,
                              params) ||
      encodedSize > kMaximumFontBytes) {
    std::cerr << "WOFF2 encoding failed or exceeds the catalog bound\n";
    return 1;
  }
  encoded.resize(encodedSize);

  const size_t decodedSize = woff2::ComputeWOFF2FinalSize(encoded.data(), encoded.size());
  if (decodedSize == 0 || decodedSize > kMaximumFontBytes) {
    std::cerr << "Invalid expanded font bound\n";
    return 1;
  }
  std::string decoded;
  woff2::WOFF2StringOut output(&decoded);
  output.SetMaxSize(decodedSize);
  if (!woff2::ConvertWOFF2ToTTF(encoded.data(), encoded.size(), &output, kMaximumBrotliBytes)) {
    std::cerr << "Generated WOFF2 cannot be decoded\n";
    return 1;
  }
  if (!WriteFile(encodedPath, encoded.data(), encoded.size()) ||
      !WriteFile(decodedPath, reinterpret_cast<const uint8_t*>(decoded.data()), decoded.size())) {
    std::cerr << "Cannot write generated font\n";
    return 1;
  }
  return 0;
}

}  // namespace
}  // namespace donner

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: catalog_woff2_tool INPUT.ttf OUTPUT.woff2 DECODED.ttf\n";
    return 1;
  }
  return donner::Convert(argv[1], argv[2], argv[3]);
}
