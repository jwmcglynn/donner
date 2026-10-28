#include "donner/svg/properties/ClipPath.h"

#include <array>
#include <cmath>
#include <optional>

#include "donner/svg/parser/LengthPercentageParser.h"

namespace donner::svg {
namespace {

using Component = css::ComponentValue;
using Position = RadialClipPath::Position;

ParseDiagnostic InvalidShape() {
  ParseDiagnostic error;
  error.reason = "Invalid basic shape clip-path";
  return error;
}

bool IsIdent(const Component& component, std::string_view expected) {
  const auto* ident = component.tryGetToken<css::Token::Ident>();
  return ident && ident->value.equalsLowercase(expected);
}

std::optional<RadialClipPath::Box> ReferenceBox(const Component& component) {
  if (IsIdent(component, "fill-box") || IsIdent(component, "content-box") ||
      IsIdent(component, "padding-box")) {
    return RadialClipPath::Box::Fill;
  }
  if (IsIdent(component, "stroke-box") || IsIdent(component, "border-box") ||
      IsIdent(component, "margin-box")) {
    return RadialClipPath::Box::Stroke;
  }
  if (IsIdent(component, "view-box")) {
    return RadialClipPath::Box::View;
  }
  return std::nullopt;
}

std::optional<Lengthd> FiniteLength(const Component& component, bool allowUserUnits) {
  auto length = parser::ParseLengthPercentage(component, allowUserUnits);
  if (length.hasError() || !std::isfinite(length.result().value)) {
    return std::nullopt;
  }
  return length.result();
}

/// Fixed storage bounds both the argument grammar and its allocation cost.
struct Arguments {
  std::array<const Component*, 7> values{};
  std::size_t size = 0;

  bool append(std::span<const Component> components) {
    for (const Component& component : components) {
      if (component.isToken<css::Token::Whitespace>()) {
        continue;
      }
      if (size == values.size()) {
        return false;
      }
      values[size++] = &component;
    }
    return true;
  }
};

/// -1 means a length or center, 0 a horizontal edge, 1 a vertical edge.
int PositionAxis(const Component& component) {
  if (IsIdent(component, "left") || IsIdent(component, "right")) {
    return 0;
  }
  if (IsIdent(component, "top") || IsIdent(component, "bottom")) {
    return 1;
  }
  return -1;
}

std::optional<Position> SimplePosition(const Component& component, int axis, bool allowUserUnits) {
  if (IsIdent(component, "center")) {
    return Position{};
  }
  if (PositionAxis(component) == axis) {
    return Position{Lengthd(0), IsIdent(component, "right") || IsIdent(component, "bottom")};
  }
  if (const auto length = FiniteLength(component, allowUserUnits)) {
    return Position{*length, false};
  }
  return std::nullopt;
}

bool ParseSinglePosition(const Component& argument, RadialClipPath& circle, bool allowUserUnits) {
  const int axis = PositionAxis(argument) == 1 ? 1 : 0;
  const auto position = SimplePosition(argument, axis, allowUserUnits);
  if (!position) {
    return false;
  }
  (axis == 0 ? circle.x : circle.y) = *position;
  return true;
}

bool ParseSimplePosition(std::span<const Component* const> args, RadialClipPath& circle,
                         bool allowUserUnits) {
  if (args.size() == 1) {
    return ParseSinglePosition(*args[0], circle, allowUserUnits);
  }
  const bool reverse = PositionAxis(*args[0]) == 1 || PositionAxis(*args[1]) == 0;
  if (reverse &&
      (FiniteLength(*args[0], allowUserUnits) || FiniteLength(*args[1], allowUserUnits))) {
    return false;
  }
  const auto x = SimplePosition(*args[reverse ? 1 : 0], 0, allowUserUnits);
  const auto y = SimplePosition(*args[reverse ? 0 : 1], 1, allowUserUnits);
  if (!x || !y) {
    return false;
  }
  circle.x = *x;
  circle.y = *y;
  return true;
}

bool ParseEdgePosition(std::span<const Component* const> args, RadialClipPath& circle,
                       bool allowUserUnits) {
  std::array<std::optional<Position>, 2> positions;
  bool center = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (IsIdent(*args[i], "center")) {
      if (center) {
        return false;
      }
      center = true;
      continue;
    }
    const int axis = PositionAxis(*args[i]);
    if (axis < 0 || positions[axis]) {
      return false;
    }
    Position position{Lengthd(0), IsIdent(*args[i], "right") || IsIdent(*args[i], "bottom")};
    if (i + 1 < args.size()) {
      if (const auto offset = FiniteLength(*args[i + 1], allowUserUnits)) {
        position.offset = *offset;
        ++i;
      }
    }
    positions[axis] = position;
  }
  if (static_cast<int>(positions[0].has_value()) + static_cast<int>(positions[1].has_value()) +
          static_cast<int>(center) !=
      2) {
    return false;
  }
  circle.x = positions[0].value_or(Position{});
  circle.y = positions[1].value_or(Position{});
  return true;
}

bool ParseRadius(const Component& radius, RadialClipPath::Radius& type, Lengthd& value,
                 bool allowUserUnits) {
  if (IsIdent(radius, "closest-side")) {
    type = RadialClipPath::Radius::ClosestSide;
  } else if (IsIdent(radius, "farthest-side")) {
    type = RadialClipPath::Radius::FarthestSide;
  } else {
    const auto length = FiniteLength(radius, allowUserUnits);
    if (!length || length->value < 0) {
      return false;
    }
    type = RadialClipPath::Radius::Length;
    value = *length;
  }
  return true;
}

bool ParseCircleArguments(const css::Function& function, CircleClipPath& circle,
                          bool allowUserUnits) {
  Arguments args;
  if (!args.append(function.values)) {
    return false;
  }
  std::size_t next = 0;
  if (next < args.size && !IsIdent(*args.values[next], "at")) {
    if (!ParseRadius(*args.values[next++], circle.radiusType, circle.radius, allowUserUnits)) {
      return false;
    }
  }

  if (next == args.size) {
    return true;
  }
  if (!IsIdent(*args.values[next++], "at") || next == args.size) {
    return false;
  }
  const auto position = std::span(args.values).subspan(next, args.size - next);
  return position.size() <= 2 ? ParseSimplePosition(position, circle, allowUserUnits)
                              : ParseEdgePosition(position, circle, allowUserUnits);
}

bool ParseEllipseRadii(const Arguments& args, EllipseClipPath& ellipse, std::size_t& next,
                       bool allowUserUnits) {
  if (args.size && !IsIdent(*args.values[0], "at")) {
    if (!ParseRadius(*args.values[0], ellipse.radiusXType, ellipse.radiusX, allowUserUnits)) {
      return false;
    }
    next = 1;
    if (next < args.size && !IsIdent(*args.values[next], "at")) {
      if (!ParseRadius(*args.values[next++], ellipse.radiusYType, ellipse.radiusY,
                       allowUserUnits)) {
        return false;
      }
    } else if (ellipse.radiusXType != RadialClipPath::Radius::Length) {
      ellipse.radiusYType = ellipse.radiusXType;
    } else {
      return false;
    }
  }
  return true;
}

bool ParseEllipseArguments(const css::Function& function, EllipseClipPath& ellipse,
                           bool allowUserUnits) {
  Arguments args;
  std::size_t next = 0;
  if (!args.append(function.values) || !ParseEllipseRadii(args, ellipse, next, allowUserUnits)) {
    return false;
  }
  if (next == args.size) {
    return true;
  }
  if (!IsIdent(*args.values[next++], "at") || next == args.size) {
    return false;
  }
  const auto position = std::span(args.values).subspan(next, args.size - next);
  return position.size() <= 2 ? ParseSimplePosition(position, ellipse, allowUserUnits)
                              : ParseEdgePosition(position, ellipse, allowUserUnits);
}

ParseResult<ClipPath> ParseRadialFunction(const css::Function& function, RadialClipPath::Box box,
                                          bool allowUserUnits) {
  if (function.name.equalsLowercase("ellipse")) {
    EllipseClipPath ellipse;
    ellipse.box = box;
    return ParseEllipseArguments(function, ellipse, allowUserUnits)
               ? ParseResult<ClipPath>(ClipPath(ellipse))
               : ParseResult<ClipPath>(InvalidShape());
  }
  CircleClipPath circle;
  circle.box = box;
  return ParseCircleArguments(function, circle, allowUserUnits)
             ? ParseResult<ClipPath>(ClipPath(circle))
             : ParseResult<ClipPath>(InvalidShape());
}

void PrintRadius(std::ostream& os, RadialClipPath::Radius type, const Lengthd& radius) {
  if (type == RadialClipPath::Radius::Length) {
    os << radius;
  } else {
    os << (type == RadialClipPath::Radius::ClosestSide ? "closest-side" : "farthest-side");
  }
}

}  // namespace

ParseResult<ClipPath> ParseBasicShapeClipPath(std::span<const Component> components,
                                              bool allowUserUnits) {
  Arguments args;
  if (!args.append(components) || args.size == 0 || args.size > 2) {
    return InvalidShape();
  }
  RadialClipPath::Box box = RadialClipPath::Box::Stroke;
  const css::Function* function = nullptr;
  for (std::size_t i = 0; i < args.size; ++i) {
    if (args.values[i]->is<css::Function>()) {
      const auto& candidate = args.values[i]->get<css::Function>();
      if (function || (!candidate.name.equalsLowercase("circle") &&
                       !candidate.name.equalsLowercase("ellipse"))) {
        return InvalidShape();
      }
      function = &candidate;
    } else if (const auto referenceBox = ReferenceBox(*args.values[i])) {
      box = *referenceBox;
    } else {
      return InvalidShape();
    }
  }
  if (!function) {
    return InvalidShape();
  }
  return ParseRadialFunction(*function, box, allowUserUnits);
}

std::ostream& operator<<(std::ostream& os, const ClipPath& clip) {
  if (const auto* reference = std::get_if<Reference>(&clip.value)) {
    return os << *reference;
  }
  const RadialClipPath* shape;
  if (const auto* circle = std::get_if<CircleClipPath>(&clip.value)) {
    shape = circle;
    os << "circle(";
    PrintRadius(os, circle->radiusType, circle->radius);
  } else {
    const auto& ellipse = std::get<EllipseClipPath>(clip.value);
    shape = &ellipse;
    os << "ellipse(";
    PrintRadius(os, ellipse.radiusXType, ellipse.radiusX);
    os << ' ';
    PrintRadius(os, ellipse.radiusYType, ellipse.radiusY);
  }
  os << " at " << (shape->x.fromFarEdge ? "right " : "left ") << shape->x.offset << ' '
     << (shape->y.fromFarEdge ? "bottom " : "top ") << shape->y.offset << ") ";
  return os << (shape->box == RadialClipPath::Box::Fill     ? "fill-box"
                : shape->box == RadialClipPath::Box::Stroke ? "stroke-box"
                                                            : "view-box");
}

}  // namespace donner::svg
