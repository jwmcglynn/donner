#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace donner::teleport::render_demo {

/// Request payload for the Teleport M2 proof-of-life render demo.
struct RenderRequest {
  std::string svg_source;  //!< Full text of the SVG to render.
  std::int32_t width = 0;  //!< Target raster width in pixels.
  std::int32_t height = 0;  //!< Target raster height in pixels.
};

/// Response payload for the Teleport M2 proof-of-life render demo.
struct RenderResponse {
  std::vector<std::byte> png_bytes;  //!< PNG-encoded rendered output.
};

}  // namespace donner::teleport::render_demo
