/**
 * @example svg_animation.cc Render animated SVG frames
 * @details This example demonstrates how to parse an SVG with animation elements and render
 * multiple frames at different time points, producing a sequence of PNG files.
 *
 * To run:
 *
 * ```sh
 * bazel run --run_under="cd $PWD &&" //examples:svg_animation -- input.svg
 * ```
 *
 * Produces frame_000.png through frame_059.png (2 seconds at 30fps).
 */

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

int main(int argc, char* argv[]) {
  using namespace donner;
  using namespace donner::svg;
  using namespace donner::svg::parser;

  if (argc != 2) {
    std::cerr << "USAGE: svg_animation <filename>\n";
    return 1;
  }

  std::ifstream file(argv[1]);
  if (!file) {
    std::cerr << "Could not open file " << argv[1] << "\n";
    return 1;
  }

  std::string fileData;
  file.seekg(0, std::ios::end);
  const size_t fileLength = file.tellg();
  file.seekg(0);
  fileData.resize(fileLength);
  file.read(fileData.data(), static_cast<std::streamsize>(fileLength));

  SVGParser::Options options;
  options.enableExperimental = true;

  ParseResult<SVGDocument> maybeDocument = SVGParser::ParseSVG(fileData, nullptr, options);
  if (maybeDocument.hasError()) {
    std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
    return 1;
  }

  SVGDocument document = std::move(maybeDocument.result());
  document.setCanvasSize(800, 600);

  // Render 2 seconds of animation at 30fps.
  constexpr double kFps = 30.0;
  constexpr double kDuration = 2.0;
  constexpr int kFrameCount = static_cast<int>(kFps * kDuration);

  for (int i = 0; i < kFrameCount; ++i) {
    const double time = static_cast<double>(i) / kFps;

    // Advance the document time. This updates all animated attribute values.
    document.setTime(time);

    // Render the frame.
    Renderer renderer;
    renderer.draw(document);

    // Save to frame_NNN.png.
    std::ostringstream filename;
    filename << "frame_" << std::setfill('0') << std::setw(3) << i << ".png";

    if (renderer.save(filename.str().c_str())) {
      std::cout << "Saved " << filename.str() << " (t=" << time << "s)\n";
    } else {
      std::cerr << "Failed to save " << filename.str() << "\n";
      return 1;
    }
  }

  std::cout << "Rendered " << kFrameCount << " frames.\n";
  return 0;
}
