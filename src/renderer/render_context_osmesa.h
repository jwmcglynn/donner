#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Forward declaration.
typedef struct osmesa_context* OSMesaContext;

namespace donner {

class RenderContextOSMesa {
public:
  using GLFunction = void (*)();

  /**
   * Create a OSMesa (software rendering) context with a backbuffer of the given size.
   *
   * @param width Backbuffer width, in pixels.
   * @param height Backbuffer height, in pixels.
   */
  RenderContextOSMesa(size_t width, size_t height);
  ~RenderContextOSMesa();

  /**
   * Make the rendering context active on the current thread.
   *
   * @param out_errors (Optional) If an error occurs, write the message in the provided string.
   * @return true if the operation succeeded.
   */
  bool makeCurrent(std::string* out_errors);

  size_t width() const { return width_; }
  size_t height() const { return height_; }

  /**
   * Get the image backbuffer, which is a GL_RGBA buffer with 8-bit pixels, of size width*height*4.
   */
  std::span<const uint8_t> image() const;

  /**
   * Save the backbuffer to a file.
   *
   * @param path PNG file destination.
   * @return true if the save succeeded.
   */
  bool savePNG(std::string_view path) const;

  /**
   * Get the GL proc address for a given function from OSMesa, equivalent to glGetProcAddress.
   *
   * @param name Function name.
   */
  static GLFunction getProcAddress(const char* name);

private:
  size_t width_;
  size_t height_;
  std::vector<uint8_t> image_;
  OSMesaContext context_ = nullptr;
};

}  // namespace donner
