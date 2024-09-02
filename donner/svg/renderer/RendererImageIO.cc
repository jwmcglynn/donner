#include "donner/svg/renderer/RendererImageIO.h"

#include <stb/stb_image_write.h>
#include <stb/stb_image.h>
#include <cassert>
#include <fstream>
#include <sstream>
#include <curl/curl.h>
#include <vector>
#include <string>
#include <stdexcept>

namespace donner::svg {

bool RendererImageIO::writeRgbaPixelsToPngFile(const char* filename,
                                               std::span<const uint8_t> rgbaPixels, int width,
                                               int height, size_t strideInPixels) {
  struct Context {
    std::ofstream output;
  };

  assert(width > 0);
  assert(height > 0);
  assert(strideInPixels > 0);
  assert(strideInPixels <= std::numeric_limits<int>::max() / 4);
  assert(rgbaPixels.size() == static_cast<size_t>(width) * height * 4);

  Context context;
  context.output = std::ofstream(filename, std::ofstream::out | std::ofstream::binary);
  if (!context.output) {
    return false;
  }

  stbi_write_png_to_func(
      [](void* context, void* data, int len) {
        Context* contextObj = static_cast<Context*>(context);
        contextObj->output.write(static_cast<const char*>(data), len);
      },
      &context, width, height, 4, rgbaPixels.data(),
      /* stride in bytes */ strideInPixels ? static_cast<int>(strideInPixels * 4) : width * 4);

  return context.output.good();
}

std::vector<uint8_t> RendererImageIO::fetchExternalResource(const std::string& url) {
  CURL* curl;
  CURLcode res;
  std::vector<uint8_t> data;

  curl = curl_easy_init();
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
      size_t totalSize = size * nmemb;
      std::vector<uint8_t>* data = static_cast<std::vector<uint8_t>*>(userp);
      data->insert(data->end(), static_cast<uint8_t*>(contents), static_cast<uint8_t*>(contents) + totalSize);
      return totalSize;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
      throw std::runtime_error("Failed to fetch external resource: " + std::string(curl_easy_strerror(res)));
    }
  }

  return data;
}

std::vector<uint8_t> RendererImageIO::decodeBase64Data(const std::string& base64String) {
  std::string base64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<uint8_t> decodedData;
  int val = 0, valb = -8;
  for (unsigned char c : base64String) {
    if (base64Chars.find(c) == std::string::npos) break;
    val = (val << 6) + base64Chars.find(c);
    valb += 6;
    if (valb >= 0) {
      decodedData.push_back((val >> valb) & 0xFF);
      valb -= 8;
    }
  }
  return decodedData;
}

std::vector<uint8_t> RendererImageIO::loadImage(const std::string& source, int& width, int& height) {
  std::vector<uint8_t> imageData;
  if (source.find("data:image/") == 0 && source.find(";base64,") != std::string::npos) {
    std::string base64Data = source.substr(source.find(";base64,") + 8);
    imageData = decodeBase64Data(base64Data);
  } else {
    imageData = fetchExternalResource(source);
  }

  int channels;
  uint8_t* data = stbi_load_from_memory(imageData.data(), imageData.size(), &width, &height, &channels, 4);
  if (!data) {
    throw std::runtime_error("Failed to load image from source: " + source);
  }

  std::vector<uint8_t> rgbaData(data, data + (width * height * 4));
  stbi_image_free(data);

  return rgbaData;
}

}  // namespace donner::svg
