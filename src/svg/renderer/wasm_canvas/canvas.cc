#include "src/svg/renderer/wasm_canvas/canvas.h"

#include <emscripten.h>
#include <emscripten/val.h>

#include <string>

#include "src/base/math_utils.h"

namespace donner::canvas {

namespace {

struct ToCStr {
  explicit ToCStr(std::string_view str) : view_(str) {
    if (!view_.empty() && view_[view_.size() - 1] != '\0') {
      nullTerminatedCopy_ = std::string(view_);
    }
  }

  operator const char*() const {
    return !nullTerminatedCopy_.empty() ? nullTerminatedCopy_.c_str() : view_.data();
  }

  std::string_view view_;
  std::string nullTerminatedCopy_;
};

// Fill functions.
EM_JS(void, js_setFillStyle, (emscripten::EM_VAL ctx, const char* value),
      { Emval.toValue(ctx).fillStyle = UTF8ToString(value); });
EM_JS(void, js_fill, (emscripten::EM_VAL ctx), { Emval.toValue(ctx).fill(); });
EM_JS(void, js_fillRect, (emscripten::EM_VAL ctx, int x, int y, int width, int height),
      { Emval.toValue(ctx).fillRect(x, y, width, height); });

// Stroke functions.
EM_JS(void, js_setStrokeStyle, (emscripten::EM_VAL ctx, const char* value),
      { Emval.toValue(ctx).strokeStyle = UTF8ToString(value); });
EM_JS(void, js_stroke, (emscripten::EM_VAL ctx), { Emval.toValue(ctx).stroke(); });

// Path functions.
EM_JS(void, js_beginPath, (emscripten::EM_VAL ctx), { Emval.toValue(ctx).beginPath(); });
EM_JS(void, js_moveTo, (emscripten::EM_VAL ctx, double x, double y),
      { Emval.toValue(ctx).moveTo(x, y); });
EM_JS(void, js_lineTo, (emscripten::EM_VAL ctx, double x, double y),
      { Emval.toValue(ctx).lineTo(x, y); });
EM_JS(void, js_bezierCurveTo,
      (emscripten::EM_VAL ctx, double c0x, double c0y, double c1x, double c1y, double endx,
       double endy),
      { Emval.toValue(ctx).bezierCurveTo(c0x, c0y, c1x, c1y, endx, endy); });
EM_JS(void, js_closePath, (emscripten::EM_VAL ctx), { Emval.toValue(ctx).closePath(); });

EM_JS(void, js_scale, (emscripten::EM_VAL ctx, double x, double y),
      { Emval.toValue(ctx).scale(x, y); });

}  // namespace

class CanvasRenderingContext2D::Impl {
public:
  Impl(emscripten::val&& ctx, double pixelRatio) : ctx_(std::move(ctx)), pixelRatio_(pixelRatio) {
    if (pixelRatio_ > 1.0) {
      js_scale(ctx_.as_handle(), pixelRatio_, pixelRatio_);
    }
  }

  emscripten::EM_VAL ctx() { return ctx_.as_handle(); }

  void setPath(const svg::PathSpline& path) {
    js_beginPath(ctx());

    const std::vector<Vector2d>& points = path.points();

    for (const svg::PathSpline::Command& command : path.commands()) {
      switch (command.type) {
        case svg::PathSpline::CommandType::MoveTo: {
          auto pt = points[command.pointIndex];
          js_moveTo(ctx(), pt.x, pt.y);
          break;
        }
        case svg::PathSpline::CommandType::LineTo: {
          auto pt = points[command.pointIndex];
          js_lineTo(ctx(), pt.x, pt.y);
          break;
        }
        case svg::PathSpline::CommandType::CurveTo: {
          auto c0 = points[command.pointIndex];
          auto c1 = points[command.pointIndex + 1];
          auto end = points[command.pointIndex + 2];
          js_bezierCurveTo(ctx(), c0.x, c0.y, c1.x, c1.y, end.x, end.y);
          break;
        }
        case svg::PathSpline::CommandType::ClosePath: {
          js_closePath(ctx());
          break;
        }
      }
    }
  }

  emscripten::val ctx_;
  double pixelRatio_ = 1.0;

  friend class CanvasRenderingContext2D;
};

CanvasRenderingContext2D::CanvasRenderingContext2D(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

CanvasRenderingContext2D::~CanvasRenderingContext2D() = default;

void CanvasRenderingContext2D::setFillStyle(std::string_view style) {
  js_setFillStyle(impl_->ctx(), ToCStr(style));
}

void CanvasRenderingContext2D::fill(const svg::PathSpline& path) {
  impl_->setPath(path);
  js_fill(impl_->ctx());
}

void CanvasRenderingContext2D::fillRect(int x, int y, int width, int height) {
  js_fillRect(impl_->ctx(), x, y, width, height);
}

void CanvasRenderingContext2D::setStrokeStyle(std::string_view style) {
  js_setStrokeStyle(impl_->ctx(), ToCStr(style));
}

void CanvasRenderingContext2D::stroke(const svg::PathSpline& path) {
  impl_->setPath(path);
  js_stroke(impl_->ctx());
}

class Canvas::Impl {
public:
  Impl(emscripten::val&& canvas, double devicePixelRatio)
      : canvas_(std::move(canvas)), pixelRatio_(devicePixelRatio) {}

  emscripten::val canvas_;
  double pixelRatio_ = 1.0;

  friend class Canvas;
};

Canvas::Canvas(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Canvas::~Canvas() = default;

EM_JS(double, js_canvas_configureHiDpi, (emscripten::EM_VAL canvasHandle), {
  const canvas = Emval.toValue(canvasHandle);
  const devicePixelRatio = window.devicePixelRatio || 1;

  // Determine the "backing store ratio" of the canvas context.
  const context = canvas.getContext("2d");
  const backingStoreRatio =
      (context.webkitBackingStorePixelRatio || context.mozBackingStorePixelRatio ||
       context.msBackingStorePixelRatio || context.oBackingStorePixelRatio ||
       context.backingStorePixelRatio || 1);

  const ratio = devicePixelRatio / backingStoreRatio;
  const rect = canvas.getBoundingClientRect();

  // Upscale the canvas if the two ratios don't match.
  if (devicePixelRatio != backingStoreRatio) {
    canvas.width = rect.width * ratio;
    canvas.height = rect.height * ratio;

    canvas.style.width = rect.width + "px";
    canvas.style.height = rect.height + "px";
  }

  return ratio;
});

Canvas Canvas::Create(std::string_view canvasSelector) {
  const emscripten::val document = emscripten::val::global("document");
  emscripten::val canvas =
      document.call<emscripten::val, std::string>("querySelector", std::string(canvasSelector));
  // TODO: Return error here.

  const double pixelRatio = js_canvas_configureHiDpi(canvas.as_handle());

  return Canvas(std::make_unique<Impl>(std::move(canvas), pixelRatio));
}

EM_JS(emscripten::EM_VAL, js_canvas_getSize, (emscripten::EM_VAL canvasHandle), {
  const rect = Emval.toValue(canvasHandle).getBoundingClientRect();
  return Emval.newObject({width : rect.width, height : rect.height});
});

Vector2i Canvas::size() const {
  const emscripten::val size =
      emscripten::val::take_ownership(js_canvas_getSize(impl_->canvas_.as_handle()));
  return Vector2i(size["width"].as<int>(), size["height"].as<int>());
}

EM_JS(void, js_canvas_setSize,
      (emscripten::EM_VAL canvasHandle, double ratio, int width, int height), {
        let canvas = Emval.toValue(canvasHandle);

        canvas.style.width = width + "px";
        canvas.style.height = height + "px";
        canvas.width = width * ratio;
        canvas.height = height * ratio;
      });

void Canvas::setSize(Vector2i size) {
  js_canvas_setSize(impl_->canvas_.as_handle(), impl_->pixelRatio_, size.x, size.y);
}

CanvasRenderingContext2D Canvas::getContext2D() {
  return CanvasRenderingContext2D(std::make_unique<CanvasRenderingContext2D::Impl>(
      impl_->canvas_.call<emscripten::val, std::string>("getContext", "2d"), impl_->pixelRatio_));
}

}  // namespace donner::canvas
