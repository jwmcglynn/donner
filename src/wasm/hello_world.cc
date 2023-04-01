#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <iostream>

class HelloClass {
public:
  HelloClass() = default;

  void doThing() { std::cout << "Did thing" << std::endl; }

  static std::string SayHello() { return "Hello World"; };
};

EMSCRIPTEN_BINDINGS(HelloWorld) {
  emscripten::class_<HelloClass>("HelloClass")
      .constructor<>()
      .class_function("SayHello", &HelloClass::SayHello)
      .function("doThing", &HelloClass::doThing);
}

int main() {
  const auto document = emscripten::val::global("document");
  const auto canvas = document.call<emscripten::val, std::string>("querySelector", "canvas");

  auto ctx = canvas.call<emscripten::val, std::string>("getContext", "2d");

  ctx.set("fillStyle", "green");
  ctx.call<void>("fillRect", 10, 10, 150, 100);
  return 0;
}
