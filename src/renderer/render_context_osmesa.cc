#include "src/renderer/render_context_osmesa.h"

#include <GL/osmesa.h>
#include <stb/stb_image_write.h>

#include <fstream>

namespace donner {

RenderContextOSMesa::RenderContextOSMesa(size_t width, size_t height)
    : width_(width), height_(height), image_(width * height * 4) {}

RenderContextOSMesa::~RenderContextOSMesa() {
  if (context_) {
    OSMesaDestroyContext(context_);
  }
}

bool RenderContextOSMesa::makeCurrent(std::string* out_errors) {
  static const int kAttribs[] = {OSMESA_FORMAT,
                                 OSMESA_RGBA,
                                 OSMESA_DEPTH_BITS,
                                 32,
                                 OSMESA_STENCIL_BITS,
                                 0,
                                 OSMESA_ACCUM_BITS,
                                 0,
                                 OSMESA_PROFILE,
                                 OSMESA_CORE_PROFILE,
                                 OSMESA_CONTEXT_MAJOR_VERSION,
                                 3,
                                 OSMESA_CONTEXT_MINOR_VERSION,
                                 2,
                                 0};

  context_ = OSMesaCreateContextAttribs(kAttribs, nullptr);
  if (!context_) {
    if (out_errors) {
      *out_errors = "OSMesaCreateContextAttribs failed";
    }
    return false;
  }

  std::vector<uint8_t> buffer(800 * 600 * 4);
  if (!OSMesaMakeCurrent(context_, image_.data(), GL_UNSIGNED_BYTE, width_, height_)) {
    if (out_errors) {
      *out_errors = "OSMesaCreateContextExt failed";
    }
    return false;
  }

  return true;
}

std::span<const uint8_t> RenderContextOSMesa::image() const {
  return image_;
}

bool RenderContextOSMesa::savePNG(std::string_view path) const {
  struct Context {
    std::ofstream output;
  };

  Context context;
  context.output = std::ofstream(path, std::ofstream::out | std::ofstream::binary);
  if (!context.output) {
    return false;
  }

  stbi_write_png_to_func(
      [](void* context, void* data, int len) {
        Context* contextObj = reinterpret_cast<Context*>(context);
        contextObj->output.write(static_cast<const char*>(data), len);
      },
      &context, width_, height_, 4, image().data() + (width_ * 4 * (height_ - 1)),
      -int(width_) * 4);

  return context.output.good();
}

RenderContextOSMesa::GLFunction RenderContextOSMesa::getProcAddress(const char* name) {
  return OSMesaGetProcAddress(name);
}

}  // namespace donner