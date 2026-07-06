#pragma once
/// @file
/// Standard-CSS stringification for computed property values.
///
/// The property value types' `operator<<` overloads emit a debug-oriented
/// syntax (e.g. `PaintServer(solid rgba(255, 0, 0, 255))`) intended for test
/// diagnostics and logging. The editor's computed-style inspector instead needs
/// each value's standard CSS text (`#ff0000`). `CssSerialize` provides that
/// second, non-debug stringification pass: one overload per property value
/// type, reusing the engine's canonical serializers (`RGBA::toHexString`,
/// `Length::toRcString`, `detail::FormatNumberForSVG`).

#include <string>

#include "donner/base/Length.h"
#include "donner/css/Color.h"
#include "donner/svg/core/Display.h"
#include "donner/svg/core/Visibility.h"
#include "donner/svg/properties/PaintServer.h"

namespace donner::svg {

/// Serialize a `display` value to standard CSS (`inline`, `none`, ...).
[[nodiscard]] std::string CssSerialize(Display value);

/// Serialize a `visibility` value to standard CSS (`visible`, `hidden`, `collapse`).
[[nodiscard]] std::string CssSerialize(Visibility value);

/// Serialize a numeric value (opacity, fill-opacity, stroke-opacity) to
/// standard CSS with minimal round-tripping precision (`1`, `0.5`).
[[nodiscard]] std::string CssSerialize(double value);

/// Serialize a length (e.g. `stroke-width`) to canonical CSS (`1`, `2px`, `50%`).
[[nodiscard]] std::string CssSerialize(const Lengthd& value);

/// Serialize a color to standard CSS: `currentColor`, or lowercase hex
/// (`#rrggbb`, or `#rrggbbaa` when translucent).
[[nodiscard]] std::string CssSerialize(const css::Color& value);

/// Serialize a paint value to standard CSS: `none`, `context-fill`,
/// `context-stroke`, a color (`#rrggbb`), `url(#id)`, or `url(#id) <fallback>`.
[[nodiscard]] std::string CssSerialize(const PaintServer& value);

}  // namespace donner::svg
