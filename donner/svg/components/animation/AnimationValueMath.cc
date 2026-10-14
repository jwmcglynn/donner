#include "donner/svg/components/animation/AnimationValueMath.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

#include "donner/base/StringUtils.h"
#include "donner/base/parser/LengthParser.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"

namespace donner::svg::components {

std::optional<std::vector<Lengthd>> ParseAnimationLengths(std::string_view input) {
  donner::parser::LengthParser::Options options;
  options.unitOptional = true;
  std::vector<Lengthd> values;
  input = StringUtils::TrimWhitespace(input);
  while (!input.empty()) {
    if (values.size() >= AnimationResourceBudget::kMaximumNumbersPerValue) {
      return std::nullopt;
    }
    auto parsed = donner::parser::LengthParser::Parse(input, options);
    if (parsed.hasError() || !std::isfinite(parsed.result().length.value)) {
      return std::nullopt;
    }
    values.push_back(parsed.result().length);
    input.remove_prefix(parsed.result().consumedChars);
    const auto trimmed = StringUtils::TrimWhitespace(input);
    const bool separated = trimmed.size() != input.size();
    input = trimmed;
    if (!input.empty() && input.front() == ',') {
      input = StringUtils::TrimWhitespace(input.substr(1));
      if (input.empty()) {
        return std::nullopt;
      }
    } else if (!input.empty() && !separated && input.front() != '-' && input.front() != '+') {
      return std::nullopt;
    }
  }
  return values.empty() ? std::nullopt : std::optional(std::move(values));
}

std::optional<Lengthd> ParseAnimationLength(std::string_view attribute, std::string_view value) {
  if (const auto lengths = ParseAnimationLengths(value); lengths && lengths->size() == 1) {
    return lengths->front();
  }
  if (attribute != "refX" && attribute != "refY") {
    return std::nullopt;
  }
  const bool x = attribute == "refX";
  if (value == (x ? "left" : "top")) {
    return Lengthd(0, Lengthd::Unit::Percent);
  }
  if (value == "center") {
    return Lengthd(50, Lengthd::Unit::Percent);
  }
  if (value == (x ? "right" : "bottom")) {
    return Lengthd(100, Lengthd::Unit::Percent);
  }
  return std::nullopt;
}

namespace {

std::optional<css::RGBA> ParseColor(std::string_view input, const AnimationValueContext& context) {
  if (input.size() > AnimationResourceBudget::kMaximumPathValueBytes) {
    return std::nullopt;
  }
  auto parsed = css::parser::ColorParser::ParseString(input);
  if (parsed.hasError()) {
    return std::nullopt;
  }
  return parsed.result().isCurrentColor() ? context.currentColor : parsed.result().asRGBA();
}

double LinearColorChannel(double value) {
  value /= 255;
  return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

double EncodedColorChannel(double value) {
  return 255 * (value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1 / 2.4) - 0.055);
}

std::optional<std::string> CombineColors(css::RGBA first, css::RGBA second, double firstWeight,
                                         double secondWeight, bool linearRgb) {
  const auto channel = [firstWeight, secondWeight, linearRgb](uint8_t a, uint8_t b) {
    const double value =
        linearRgb ? EncodedColorChannel(std::clamp(
                        LinearColorChannel(a) * firstWeight + LinearColorChannel(b) * secondWeight,
                        0.0, 1.0))
                  : a * firstWeight + b * secondWeight;
    return static_cast<uint8_t>(std::round(std::clamp(value, 0.0, 255.0)));
  };
  const auto alpha = static_cast<uint8_t>(
      std::round(std::clamp(first.a * firstWeight + second.a * secondWeight, 0.0, 255.0)));
  return css::RGBA(channel(first.r, second.r), channel(first.g, second.g),
                   channel(first.b, second.b), alpha)
      .toHexString();
}

double ResolveAnimationLength(const Lengthd& length, const AnimationValueContext& context,
                              bool offset) {
  if (!context.mappedLengths) {
    return length.toPixels(context.viewBox, context.fontMetrics, context.extent);
  }
  const bool displacement = offset || context.lengthValuesAreOffsets;
  if (length.unit == Lengthd::Unit::None) {
    return length.value * context.unitlessScale + (displacement ? 0 : context.unitlessOrigin);
  }
  const double pixels = length.toPixels(context.viewBox, context.fontMetrics, context.extent);
  return pixels +
         (length.unit == Lengthd::Unit::Percent && !displacement ? context.percentageOrigin : 0);
}

std::optional<std::string> CombineLengths(const std::vector<Lengthd>& first,
                                          const std::vector<Lengthd>& second, double firstWeight,
                                          double secondWeight, const AnimationValueContext& context,
                                          bool secondIsOffset) {
  if (first.size() != second.size()) {
    return std::nullopt;
  }
  std::ostringstream output;
  output.precision(12);
  for (std::size_t i = 0; i < first.size(); ++i) {
    const bool sameUnit = first[i].unit == second[i].unit;
    const double a = sameUnit ? first[i].value : ResolveAnimationLength(first[i], context, false);
    const double b =
        sameUnit ? second[i].value : ResolveAnimationLength(second[i], context, secondIsOffset);
    const double value = a * firstWeight + b * secondWeight;
    if (!std::isfinite(value)) {
      return std::nullopt;
    }
    if (i != 0) {
      output << ' ';
    }
    output << value;
    if (sameUnit) {
      output << first[i].unit;
    } else if (context.mappedLengths) {
      output << Lengthd::Unit::Px;
    }
  }
  return output.str();
}

}  // namespace

std::optional<std::string> CombineAnimationValues(std::string_view first, std::string_view second,
                                                  double firstWeight, double secondWeight,
                                                  const AnimationValueContext& context,
                                                  bool secondIsOffset) {
  if (!std::isfinite(firstWeight) || !std::isfinite(secondWeight)) {
    return std::nullopt;
  }
  auto a = ParseAnimationLengths(first);
  auto b = ParseAnimationLengths(second);
  if (context.numberPair) {
    if (a && a->size() == 1) {
      a->push_back(a->front());
    }
    if (b && b->size() == 1) {
      b->push_back(b->front());
    }
  }
  if (a && b) {
    return CombineLengths(*a, *b, firstWeight, secondWeight, context, secondIsOffset);
  }
  const auto firstColor = ParseColor(first, context);
  const auto secondColor = ParseColor(second, context);
  if (firstColor && secondColor) {
    return CombineColors(*firstColor, *secondColor, firstWeight, secondWeight, context.linearRgb);
  }
  return std::nullopt;
}

std::optional<double> AnimationValueDistance(std::string_view first, std::string_view second,
                                             const AnimationValueContext& context) {
  auto a = ParseAnimationLengths(first);
  auto b = ParseAnimationLengths(second);
  if (context.numberPair) {
    if (a && a->size() == 1) {
      a->push_back(a->front());
    }
    if (b && b->size() == 1) {
      b->push_back(b->front());
    }
  }
  if (a && b && a->size() == b->size()) {
    double distance = 0;
    for (std::size_t i = 0; i < a->size(); ++i) {
      distance = std::hypot(distance, ResolveAnimationLength((*a)[i], context, false) -
                                          ResolveAnimationLength((*b)[i], context, false));
    }
    return distance;
  }
  const auto ca = ParseColor(first, context);
  const auto cb = ParseColor(second, context);
  if (ca && cb) {
    const auto channel = [&](uint8_t value) {
      return context.linearRgb ? LinearColorChannel(value) : static_cast<double>(value);
    };
    return std::hypot(channel(ca->r) - channel(cb->r), channel(ca->g) - channel(cb->g),
                      channel(ca->b) - channel(cb->b));
  }
  return std::nullopt;
}

}  // namespace donner::svg::components
