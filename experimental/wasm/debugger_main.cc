
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <iostream>

#include "donner/svg/SVG.h"
#include "donner/svg/renderer/RendererWasmCanvas.h"
#include "donner/svg/renderer/wasm_canvas/Canvas.h"
#include "donner/svg/xml/SVGParser.h"

using namespace donner::svg;

void DumpTree(SVGElement element, int depth) {
  for (int i = 0; i < depth; ++i) {
    std::cout << "  ";
  }

  std::cout << element.type() << ", " << element.entityHandle().entity() << ", id: '"
            << element.id() << "'";
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

class HelloClass {
public:
  HelloClass() = default;

  void doThing() { std::cout << "Did thing" << std::endl; }

  static std::string SayHello() { return "Hello World"; };
};

class DonnerBindings {
public:
  DonnerBindings() = default;

  bool loadSVG(const std::string& canvasId, const std::string& svg) {
    fileData_.resize(svg.size());
    fileData_.assign(svg.begin(), svg.end());

    std::vector<donner::base::parser::ParseError> warnings;
    auto maybeResult = parser::SVGParser::ParseSVG(fileData_, &warnings);

    if (maybeResult.hasError()) {
      const auto& e = maybeResult.error();
      std::cerr << "Parse Error " << e << std::endl;
      return false;
    }

    std::cout << "Parsed successfully." << std::endl;

    if (!warnings.empty()) {
      std::cout << "Warnings:" << std::endl;
      for (auto& w : warnings) {
        std::cout << "  " << w << std::endl;
      }
    }

    SVGDocument document = std::move(maybeResult.result());

    std::cout << "Tree:" << std::endl;
    DumpTree(document.svgElement(), 0);

    RendererWasmCanvas renderer("#secondCanvas");
    renderer.draw(document);

    return true;
  }

private:
  parser::SVGParser::InputBuffer fileData_;
};

EMSCRIPTEN_BINDINGS(Donner) {
  emscripten::class_<DonnerBindings>("Donner").constructor<>().function("loadSVG",
                                                                        &DonnerBindings::loadSVG);
}

int main() {
  using namespace donner::canvas;

  Canvas canvas = Canvas::Create("#mainCanvas");
  CanvasRenderingContext2D ctx = canvas.getContext2D();

  ctx.setFillStyle("red");
  ctx.fillRect(10, 10, 150, 100);

  return 0;
}
