
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <iostream>

#include "src/svg/svg.h"

using namespace donner::svg;

void DumpTree(SVGElement element, int depth) {
  for (int i = 0; i < depth; ++i) {
    std::cout << "  ";
  }

  std::cout << TypeToString(element.type()) << ", " << element.entity() << ", id: '" << element.id()
            << "'";
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

  bool loadSVG(const std::string& svg) {
    fileData_.resize(svg.size() + 1);
    std::memcpy(fileData_.data(), svg.data(), svg.size());

    std::vector<donner::ParseError> warnings;
    auto maybeResult = XMLParser::ParseSVG(fileData_, &warnings);

    if (maybeResult.hasError()) {
      const auto& e = maybeResult.error();
      std::cerr << "Parse Error " << e.line << ":" << e.offset << ": " << e.reason << std::endl;
      return false;
    }

    std::cout << "Parsed successfully." << std::endl;

    if (!warnings.empty()) {
      std::cout << "Warnings:" << std::endl;
      for (auto& w : warnings) {
        std::cout << "  " << w.line << ":" << w.offset << ": " << w.reason << std::endl;
      }
    }

    SVGDocument document = std::move(maybeResult.result());

    std::cout << "Tree:" << std::endl;
    DumpTree(document.svgElement(), 0);

    return true;
  }

private:
  std::vector<char> fileData_;
};

EMSCRIPTEN_BINDINGS(Donner) {
  emscripten::class_<DonnerBindings>("Donner").constructor<>().function("loadSVG",
                                                                        &DonnerBindings::loadSVG);
}

int main() {
  const auto document = emscripten::val::global("document");
  const auto canvas = document.call<emscripten::val, std::string>("querySelector", "canvas");

  auto ctx = canvas.call<emscripten::val, std::string>("getContext", "2d");

  ctx.set("fillStyle", "green");
  ctx.call<void>("fillRect", 10, 10, 150, 100);
  return 0;
}
