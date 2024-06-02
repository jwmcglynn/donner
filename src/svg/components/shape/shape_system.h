#pragma once
/// @file

#include "src/svg/components/shape/circle_component.h"
#include "src/svg/components/shape/computed_path_component.h"
#include "src/svg/components/shape/ellipse_component.h"
#include "src/svg/components/shape/line_component.h"
#include "src/svg/components/shape/path_component.h"
#include "src/svg/components/shape/poly_component.h"
#include "src/svg/components/shape/rect_component.h"
#include "src/svg/components/style/computed_style_component.h"
#include "src/svg/components/style/style_system.h"
#include "src/svg/registry/registry.h"

namespace donner::svg::components {

class ShapeSystem {
public:
  template <typename T>
  const ComputedPathComponent* createComputedPath(EntityHandle handle, const T& component,
                                                  const FontMetrics& fontMetrics,
                                                  std::vector<ParseError>* outWarnings) {
    const ComputedStyleComponent& style = StyleSystem().computeProperties(handle);
    return createComputedShapeWithStyle(handle, component, style, fontMetrics, outWarnings);
  }

  void instantiateAllComputedPaths(Registry& registry, std::vector<ParseError>* outWarnings);

private:
  using AllShapes = entt::type_list<CircleComponent, EllipseComponent, LineComponent, PathComponent,
                                    PolyComponent, RectComponent>;

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const CircleComponent& circle,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const EllipseComponent& ellipse,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const LineComponent& line,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const PathComponent& path,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const PolyComponent& poly,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);

  const ComputedPathComponent* createComputedShapeWithStyle(EntityHandle handle,
                                                            const RectComponent& rect,
                                                            const ComputedStyleComponent& style,
                                                            const FontMetrics& fontMetrics,
                                                            std::vector<ParseError>* outWarnings);
};

}  // namespace donner::svg::components
