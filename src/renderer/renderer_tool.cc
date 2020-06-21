#include <GL/gl.h>
#include <GL/osmesa.h>
#include <pathfinder/pathfinder.h>
#undef None

#include <stb/stb_image_write.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "src/svg/svg_element.h"
#include "src/svg/xml/xml_parser.h"

namespace donner {

std::string_view TypeToString(ElementType type) {
  switch (type) {
    case ElementType::SVG: return "SVG";
    case ElementType::Path: return "Path";
    case ElementType::Unknown: return "Unknown";
  }
}

void DumpTree(SVGElement element, int depth) {
  for (int i = 0; i < depth; ++i) {
    std::cout << "  ";
  }

  std::cout << TypeToString(element.type()) << ", id: '" << element.id() << "'";
  if (element.type() == ElementType::SVG) {
    if (auto viewbox = element.cast<SVGSVGElement>().viewbox()) {
      std::cout << ", viewbox: " << *viewbox;
    }
  }
  std::cout << std::endl;
  for (auto elm = element.firstChild(); elm; elm = elm->nextSibling()) {
    DumpTree(elm.value(), depth + 1);
  }
}

static const void* LoadGLFunction(const char* name, void* userdata) {
  return reinterpret_cast<const void*>(OSMesaGetProcAddress(name));
}

extern "C" int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Unexpected arg count." << std::endl;
    std::cerr << "USAGE: renderer_tool <filename>" << std::endl;
    return 1;
  }

  std::ifstream file(argv[1]);
  if (!file) {
    std::cerr << "Could not open file " << argv[1] << std::endl;
    return 2;
  }

  const size_t kWidth = 800;
  const size_t kHeight = 600;
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

  OSMesaContext ctx = OSMesaCreateContextAttribs(kAttribs, nullptr);
  if (!ctx) {
    std::cerr << "OSMesaCreateContextAttribs failed" << std::endl;
    return 3;
  }

  std::vector<uint8_t> buffer(800 * 600 * 4);
  if (!OSMesaMakeCurrent(ctx, buffer.data(), GL_UNSIGNED_BYTE, kWidth, kHeight)) {
    std::cerr << "OSMesaCreateContextExt failed" << std::endl;
    return 3;
  }

  file.seekg(0, std::ios::end);
  const size_t fileLength = file.tellg();
  file.seekg(0);

  std::vector<char> fileData(fileLength + 1);
  file.read(fileData.data(), fileLength);

  std::vector<ParseError> warnings;
  auto maybeResult = XMLParser::ParseSVG(fileData, &warnings);
  if (maybeResult.hasError()) {
    const auto& e = maybeResult.error();
    std::cerr << "Parse Error " << e.line << ":" << e.offset << ": " << e.reason << std::endl;
    return 3;
  }

  std::cout << "Parsed successfully." << std::endl;

  if (!warnings.empty()) {
    std::cout << "Warnings:" << std::endl;
    for (auto& w : warnings) {
      std::cout << "  " << w.line << ":" << w.offset << ": " << w.reason << std::endl;
    }
  }

  std::cout << "Tree:" << std::endl;
  DumpTree(maybeResult.result().svgElement(), 0);

  // Create a Pathfinder renderer.
  PFGLLoadWith(LoadGLFunction, nullptr);
  PFGLDestFramebufferRef dest_framebuffer =
      PFGLDestFramebufferCreateFullWindow(&(PFVector2I){kWidth, kHeight});
  PFGLRendererRef renderer = PFGLRendererCreate(
      PFGLDeviceCreate(PF_GL_VERSION_GL3, 0), PFFilesystemResourceLoaderLocate(), dest_framebuffer,
      &(PFRendererOptions){(PFColorF){1.0, 1.0, 1.0, 1.0},
                           PF_RENDERER_OPTIONS_FLAGS_HAS_BACKGROUND_COLOR});

  // Make a canvas. We're going to draw a house.
  PFCanvasRef canvas =
      PFCanvasCreate(PFCanvasFontContextCreateWithSystemSource(), &(PFVector2F){640.0f, 480.0f});

  // Set line width.
  PFCanvasSetLineWidth(canvas, 10.0f);

  // Draw walls.
  PFCanvasStrokeRect(canvas, &(PFRectF){{75.0f, 140.0f}, {225.0f, 250.0f}});

  // Draw door.
  PFCanvasFillRect(canvas, &(PFRectF){{130.0f, 190.0f}, {170.0f, 250.0f}});

  // Draw roof.
  PFPathRef path = PFPathCreate();
  PFPathMoveTo(path, &(PFVector2F){50.0, 140.0});
  PFPathLineTo(path, &(PFVector2F){150.0, 60.0});
  PFPathLineTo(path, &(PFVector2F){250.0, 140.0});
  PFPathClosePath(path);
  PFCanvasStrokePath(canvas, path);

  // Render the canvas to screen.
  PFSceneRef scene = PFCanvasCreateScene(canvas);
  PFSceneProxyRef scene_proxy = PFSceneProxyCreateFromSceneAndRayonExecutor(scene);
  PFSceneProxyBuildAndRenderGL(scene_proxy, renderer, PFBuildOptionsCreate());

  stbi_write_png("offscreen.png", kWidth, kHeight, 4, buffer.data() + (kWidth * 4 * (kHeight - 1)),
                 -int(kWidth) * 4);

  OSMesaDestroyContext(ctx);
  return 0;
}

}  // namespace donner