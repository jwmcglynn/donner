#pragma once

#include <variant>
#include <vector>

#include "src/base/length.h"
#include "src/base/transform.h"

namespace donner::svg {

class CssTransform {
public:
  CssTransform() = default;
  explicit CssTransform(Transformd transform) { addTransform(std::move(transform)); }

  struct Simple {
    Transformd transform;
  };

  struct Translate {
    Lengthd x;
    Lengthd y;
  };

  using Element = std::variant<Simple, Translate>;

  Transformd compute(const Boxd& viewbox, FontMetrics fontMetrics) const {
    Transformd result;
    for (const auto& element : elements_) {
      if (const auto* simple = std::get_if<Simple>(&element)) {
        result = simple->transform * result;
      } else if (const auto* translate = std::get_if<Translate>(&element)) {
        result = Transformd::Translate(
                     Vector2d(translate->x.toPixels(viewbox, fontMetrics, Lengthd::Extent::X),
                              translate->y.toPixels(viewbox, fontMetrics, Lengthd::Extent::Y))) *
                 result;
      }
    }

    return result;
  }

  void addTransform(Transformd transform) {
    if (!elements_.empty()) {
      if (Simple* s = std::get_if<Simple>(&elements_.back())) {
        s->transform = s->transform * transform;
        return;
      }
    }

    elements_.emplace_back(Simple{transform});
  }

  void addTranslate(Lengthd x, Lengthd y) { elements_.emplace_back(Translate{x, y}); }

private:
  std::vector<Element> elements_;
};

}  // namespace donner::svg
