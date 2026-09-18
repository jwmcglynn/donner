#include <iostream>

#include "Reflection.h"

int main() {
  using namespace donner::experimental;
  Rect rect{1, 2, 30, 40};
  std::cout << "GCC " << __VERSION__ << "\n";
  std::cout << "__cpp_impl_reflection=" << __cpp_impl_reflection << "\n";
  std::cout << Describe(rect) << "\n";
  ForEachMember(rect, [](std::string_view, double& value) { value += 1; });
  std::cout << "After reflected mutation: " << Describe(rect) << "\n";
  std::cout << "Paint mode: " << EnumName(PaintMode::Stroke) << "\n";

  if (Describe(rect) != "x=2, y=3, width=31, height=41" ||
      EnumName(PaintMode::Stroke) != "Stroke" ||
      EnumName(static_cast<PaintMode>(42)) != "<unknown>") {
    std::cerr << "Reflection validation failed\n";
    return 1;
  }
  std::cout << "Reflection validation passed\n";
  return 0;
}
