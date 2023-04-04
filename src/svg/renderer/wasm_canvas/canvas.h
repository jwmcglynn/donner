/// @file
#pragma once

#include <string>
#include <string_view>

#include "src/base/vector2.h"
#include "src/svg/core/path_spline.h"

namespace donner::canvas {

// Forward declarations.
class Canvas;

struct WasmHandle {};

class CanvasRenderingContext2D {
public:
  ~CanvasRenderingContext2D();

  void setFillStyle(std::string_view style);
  void fill(const svg::PathSpline& path);
  void fillRect(int x, int y, int width, int height);

  void setStrokeStyle(std::string_view style);
  void stroke(const svg::PathSpline& path);

private:
  friend class Canvas;
  class Impl;

  explicit CanvasRenderingContext2D(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

class Canvas {
public:
  static Canvas Create(std::string_view canvasSelector);
  ~Canvas();

  void setSize(Vector2i size);
  Vector2i size() const;

  CanvasRenderingContext2D getContext2D();

private:
  class Impl;

  explicit Canvas(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::canvas
