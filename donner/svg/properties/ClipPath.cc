#include "donner/svg/properties/ClipPath.h"

#include <array>
#include <cmath>
#include <optional>

#include "donner/css/parser/DeclarationListParser.h"
#include "donner/svg/parser/LengthPercentageParser.h"
#include "donner/svg/parser/PointsListParser.h"

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

/// Iterate without allocating a second token list.
class PolygonArguments {
public:
  explicit PolygonArguments(std::span<const Component> values) : values_(values) {}

  const Component* next() {
    while (next_ < values_.size() && values_[next_].isToken<css::Token::Whitespace>()) {
      ++next_;
    }
    return next_ < values_.size() ? &values_[next_++] : nullptr;
  }

private:
  std::span<const Component> values_;
  std::size_t next_ = 0;
};

std::optional<PolygonClipPath::Vertex> ParsePolygonVertex(const Component& x,
                                                          PolygonArguments& args,
                                                          bool allowUserUnits) {
  const Component* y = args.next();
  if (!y) {
    return std::nullopt;
  }
  const auto lengthX = FiniteLength(x, allowUserUnits);
  const auto lengthY = FiniteLength(*y, allowUserUnits);
  if (!lengthX || !lengthY) {
    return std::nullopt;
  }
  return PolygonClipPath::Vertex{*lengthX, *lengthY};
}

bool ParsePolygonRule(PolygonArguments& args, const Component*& current, PolygonClipPath& polygon) {
  if (current && (IsIdent(*current, "evenodd") || IsIdent(*current, "nonzero"))) {
    polygon.rule = IsIdent(*current, "evenodd") ? ClipRule::EvenOdd : ClipRule::NonZero;
    const Component* comma = args.next();
    if (!comma || !comma->isToken<css::Token::Comma>()) {
      return false;
    }
    current = args.next();
  }
  return true;
}

ParseResult<ClipPath> ParsePolygonFunction(const css::Function& function, RadialClipPath::Box box,
                                           bool allowUserUnits) {
  static_assert(PolygonClipPath::kMaximumVertices == parser::PointsListParser::kMaximumPoints);
  if (function.values.size() > css::parser::DeclarationListParser::kMaximumComponentValues) {
    return InvalidShape();
  }
  PolygonArguments args(function.values);
  PolygonClipPath polygon;
  polygon.box = box;
  const Component* current = args.next();
  if (!ParsePolygonRule(args, current, polygon)) {
    return InvalidShape();
  }
  while (current) {
    const auto vertex = ParsePolygonVertex(*current, args, allowUserUnits);
    if (!vertex || polygon.vertices.size() == PolygonClipPath::kMaximumVertices) {
      return InvalidShape();
    }
    polygon.vertices.push_back(*vertex);
    const Component* comma = args.next();
    if (!comma) {
      return ClipPath(std::move(polygon));
    }
    if (!comma->isToken<css::Token::Comma>()) {
      return InvalidShape();
    }
    current = args.next();
  }
  return InvalidShape();
}

ParseResult<ClipPath> ParseInsetFunction(const css::Function& function, RadialClipPath::Box box,
                                         bool allowUserUnits) {
  Arguments args;
  if (!args.append(function.values) || args.size == 0 || args.size > 4) {
    return InvalidShape();
  }
  InsetClipPath inset;
  inset.box = box;
  for (std::size_t i = 0; i < args.size; ++i) {
    const auto offset = FiniteLength(*args.values[i], allowUserUnits);
    if (!offset) {
      return InvalidShape();
    }
    inset.offsets[i] = *offset;
  }
  if (args.size == 1) {
    inset.offsets[1] = inset.offsets[0];
  }
  if (args.size < 3) {
    inset.offsets[2] = inset.offsets[0];
  }
  if (args.size < 4) {
    inset.offsets[3] = inset.offsets[1];
  }
  return ClipPath(std::move(inset));
}

bool IsSupportedShape(const css::Function& function) {
  return function.name.equalsLowercase("circle") || function.name.equalsLowercase("ellipse") ||
         function.name.equalsLowercase("polygon") || function.name.equalsLowercase("inset");
}

ParseResult<ClipPath> ParseShapeFunction(const css::Function& function, RadialClipPath::Box box,
                                         bool allowUserUnits) {
  if (function.name.equalsLowercase("polygon")) {
    return ParsePolygonFunction(function, box, allowUserUnits);
  }
  if (function.name.equalsLowercase("inset")) {
    return ParseInsetFunction(function, box, allowUserUnits);
  }
  return ParseRadialFunction(function, box, allowUserUnits);
}

std::ostream& PrintReferenceBox(std::ostream& os, RadialClipPath::Box box) {
  return os << (box == RadialClipPath::Box::Fill     ? "fill-box"
                : box == RadialClipPath::Box::Stroke ? "stroke-box"
                                                     : "view-box");
}

std::ostream& PrintPolygon(std::ostream& os, const PolygonClipPath& polygon) {
  os << "polygon(" << polygon.rule << ", ";
  bool first = true;
  for (const auto& vertex : polygon.vertices) {
    if (!first) {
      os << ", ";
    }
    os << vertex.x << ' ' << vertex.y;
    first = false;
  }
  os << ") ";
  return PrintReferenceBox(os, polygon.box);
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
      if (function || !IsSupportedShape(candidate)) {
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
  return ParseShapeFunction(*function, box, allowUserUnits);
}

std::ostream& operator<<(std::ostream& os, const ClipPath& clip) {
  if (const auto* reference = std::get_if<Reference>(&clip.value)) {
    return os << *reference;
  }
  if (const auto* polygon = std::get_if<PolygonClipPath>(&clip.value)) {
    return PrintPolygon(os, *polygon);
  }
  if (const auto* inset = std::get_if<InsetClipPath>(&clip.value)) {
    os << "inset(" << inset->offsets[0] << ' ' << inset->offsets[1] << ' ' << inset->offsets[2]
       << ' ' << inset->offsets[3] << ") ";
    return PrintReferenceBox(os, inset->box);
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
  return PrintReferenceBox(os, shape->box);
}

}  // namespace donner::svg
