#include "donner/svg/properties/ClipPath.h"

#include <array>
#include <cmath>
#include <optional>

#include "donner/svg/parser/LengthPercentageParser.h"

namespace donner::svg {
namespace {

using Component = css::ComponentValue;
using Position = CircleClipPath::Position;

ParseDiagnostic InvalidCircle() {
  ParseDiagnostic error;
  error.reason = "Invalid circle clip-path";
  return error;
}

bool IsIdent(const Component& component, std::string_view expected) {
  const auto* ident = component.tryGetToken<css::Token::Ident>();
  return ident && ident->value.equalsLowercase(expected);
}

std::optional<CircleClipPath::Box> ReferenceBox(const Component& component) {
  if (IsIdent(component, "fill-box") || IsIdent(component, "content-box") ||
      IsIdent(component, "padding-box")) {
    return CircleClipPath::Box::Fill;
  }
  if (IsIdent(component, "stroke-box") || IsIdent(component, "border-box") ||
      IsIdent(component, "margin-box")) {
    return CircleClipPath::Box::Stroke;
  }
  if (IsIdent(component, "view-box")) {
    return CircleClipPath::Box::View;
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
  std::array<const Component*, 6> values{};
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

bool ParseSimplePosition(std::span<const Component* const> args, CircleClipPath& circle,
                         bool allowUserUnits) {
  if (args.size() == 1) {
    const int axis = PositionAxis(*args[0]) == 1 ? 1 : 0;
    const auto position = SimplePosition(*args[0], axis, allowUserUnits);
    if (!position) {
      return false;
    }
    (axis == 0 ? circle.x : circle.y) = *position;
    return true;
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

bool ParseEdgePosition(std::span<const Component* const> args, CircleClipPath& circle,
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

bool ParseCircleArguments(const css::Function& function, CircleClipPath& circle,
                          bool allowUserUnits) {
  Arguments args;
  if (!args.append(function.values)) {
    return false;
  }
  std::size_t next = 0;
  if (next < args.size && !IsIdent(*args.values[next], "at")) {
    const Component& radius = *args.values[next++];
    if (IsIdent(radius, "closest-side")) {
      circle.radiusType = CircleClipPath::Radius::ClosestSide;
    } else if (IsIdent(radius, "farthest-side")) {
      circle.radiusType = CircleClipPath::Radius::FarthestSide;
    } else {
      const auto length = FiniteLength(radius, allowUserUnits);
      if (!length || length->value < 0) {
        return false;
      }
      circle.radiusType = CircleClipPath::Radius::Length;
      circle.radius = *length;
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

}  // namespace

ParseResult<ClipPath> ParseCircleClipPath(std::span<const Component> components,
                                          bool allowUserUnits) {
  Arguments args;
  if (!args.append(components) || args.size == 0 || args.size > 2) {
    return InvalidCircle();
  }
  CircleClipPath circle;
  const css::Function* function = nullptr;
  for (std::size_t i = 0; i < args.size; ++i) {
    if (args.values[i]->is<css::Function>()) {
      const auto& candidate = args.values[i]->get<css::Function>();
      if (function || !candidate.name.equalsLowercase("circle")) {
        return InvalidCircle();
      }
      function = &candidate;
    } else if (const auto box = ReferenceBox(*args.values[i])) {
      circle.box = *box;
    } else {
      return InvalidCircle();
    }
  }
  if (!function || !ParseCircleArguments(*function, circle, allowUserUnits)) {
    return InvalidCircle();
  }
  return ClipPath(circle);
}

std::ostream& operator<<(std::ostream& os, const ClipPath& clip) {
  if (const auto* reference = std::get_if<Reference>(&clip.value)) {
    return os << *reference;
  }
  const auto& circle = std::get<CircleClipPath>(clip.value);
  os << "circle(";
  if (circle.radiusType == CircleClipPath::Radius::Length) {
    os << circle.radius;
  } else {
    os << (circle.radiusType == CircleClipPath::Radius::ClosestSide ? "closest-side"
                                                                    : "farthest-side");
  }
  os << " at " << (circle.x.fromFarEdge ? "right " : "left ") << circle.x.offset << ' '
     << (circle.y.fromFarEdge ? "bottom " : "top ") << circle.y.offset << ") ";
  return os << (circle.box == CircleClipPath::Box::Fill     ? "fill-box"
                : circle.box == CircleClipPath::Box::Stroke ? "stroke-box"
                                                            : "view-box");
}

}  // namespace donner::svg
