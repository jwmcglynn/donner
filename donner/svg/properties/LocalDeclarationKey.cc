#include <bit>
#include <ranges>
#include <string>
#include <type_traits>

#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg {
namespace {

/// Process-local encoding for exact declaration equality. Field types fix the layout; variable
/// length sequences carry their size. Only scalar values are copied as bytes, never object padding.
class DeclarationKeyWriter {
public:
  explicit DeclarationKeyWriter(std::size_t maximumBytes) : maximumBytes_(maximumBytes) {}

  template <typename... T>
  void fields(const T&... values) {
    (append(values), ...);
  }

  bool oversize() const { return oversize_; }

  std::optional<std::string> finish() && {
    if (oversize_) {
      return std::nullopt;
    }
    return std::move(bytes_);
  }

private:
  void raw(std::string_view data) {
    if (data.size() > maximumBytes_ - bytes_.size()) {
      oversize_ = true;
    } else if (!oversize_) {
      bytes_.append(data);
    }
  }

  template <typename T>
    requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
  void append(T value) {
    const auto bytes = std::bit_cast<std::array<char, sizeof(T)>>(value);
    raw({bytes.data(), bytes.size()});
  }

  template <typename T>
    requires std::is_empty_v<T>
  void append(const T&) {}

  void append(const RcString& text) {
    append(text.size());
    raw(std::string_view(text));
  }

  template <typename T>
  void append(const std::optional<T>& value) {
    append(value.has_value());
    if (value) {
      append(*value);
    }
  }

  template <typename... T>
  void append(const std::variant<T...>& value) {
    append(value.index());
    std::visit([this](const auto& item) { this->append(item); }, value);
  }

  template <std::ranges::sized_range T>
  void append(const T& values) {
    append(std::ranges::size(values));
    for (const auto& item : values) {
      if (oversize_) {
        break;
      }
      append(item);
    }
  }

  void append(const css::Token& token) {
    fields(token.tokenIndex(), token.offset());
    token.visit([this](const auto& value) { this->append(value); });
  }

  void append(const Lengthd& v) { fields(v.value, v.unit); }
  void append(const Vector2d& v) { fields(v.x, v.y); }
  void append(const TransformOrigin& v) { fields(v.x, v.y); }
  void append(const css::RGBA& v) { fields(v.r, v.g, v.b, v.a); }
  void append(const css::HSLA& v) { fields(v.hDeg, v.s, v.l, v.a); }
  void append(const css::Color& v) { fields(v.value); }
  void append(const PaintServer& v) { fields(v.value); }
  void append(const PaintServer::Solid& v) { fields(v.color); }
  void append(const PaintServer::ElementReference& v) { fields(v.reference, v.fallback); }
  void append(const Reference& v) { fields(v.href); }
  void append(const PaintOrder& v) { fields(v.order); }
  void append(const TextDecorationThickness& v) { fields(v.kind, v.value); }
  void append(const Cursor& v) { fields(v.images, v.fallback); }
  void append(const CursorImage& v) { fields(v.url, v.hotspot); }
  void append(const FilterEffect& v) { fields(v.value); }
  void append(const FilterEffect::Blur& v) { fields(v.stdDeviationX, v.stdDeviationY); }
  void append(const FilterEffect::ElementReference& v) { fields(v.reference); }
  void append(const FilterEffect::HueRotate& v) { fields(v.angleDegrees); }
  void append(const FilterEffect::Brightness& v) { fields(v.amount); }
  void append(const FilterEffect::Contrast& v) { fields(v.amount); }
  void append(const FilterEffect::Grayscale& v) { fields(v.amount); }
  void append(const FilterEffect::Invert& v) { fields(v.amount); }
  void append(const FilterEffect::FilterOpacity& v) { fields(v.amount); }
  void append(const FilterEffect::Saturate& v) { fields(v.amount); }
  void append(const FilterEffect::Sepia& v) { fields(v.amount); }
  void append(const FilterEffect::DropShadow& v) {
    fields(v.offsetX, v.offsetY, v.stdDeviation, v.color);
  }
  void append(const ClipPath& v) { fields(v.value); }
  void append(const RadialClipPath::Position& v) { fields(v.offset, v.fromFarEdge); }
  void append(const CircleClipPath& v) { fields(v.box, v.x, v.y, v.radiusType, v.radius); }
  void append(const EllipseClipPath& v) {
    fields(v.box, v.x, v.y, v.radiusXType, v.radiusYType, v.radiusX, v.radiusY);
  }
  void append(const PolygonClipPath& v) { fields(v.box, v.rule, v.vertices); }
  void append(const PolygonClipPath::Vertex& v) { fields(v.x, v.y); }
  void append(const InsetClipPath& v) { fields(v.box, v.offsets, v.radiiX, v.radiiY); }
  void append(const PathClipPath& v) { fields(v.box, v.rule, v.data.commands(), v.data.points()); }
  void append(const Path::Command& v) { fields(v.verb, v.pointIndex, v.isInternal); }
  void append(const FileOffset& v) { fields(v.offset, v.lineInfo); }
  void append(const FileOffset::LineInfo& v) { fields(v.line, v.offsetOnLine); }
  void append(const SourceRange& v) { fields(v.start, v.end); }
  void append(const css::Function& v) { fields(v.name, v.values, v.sourceOffset); }
  void append(const css::SimpleBlock& v) { fields(v.associatedToken, v.values, v.sourceOffset); }
  void append(const css::ComponentValue& v) { fields(v.value); }
  void append(const css::Declaration& v) {
    fields(v.name, v.values, v.sourceOffset, v.sourceRange, v.sourceByteSize, v.important);
  }
  void append(const css::Token::Ident& v) { fields(v.value); }
  void append(const css::Token::AtKeyword& v) { fields(v.value); }
  void append(const css::Token::String& v) { fields(v.value); }
  void append(const css::Token::BadString& v) { fields(v.value); }
  void append(const css::Token::Url& v) { fields(v.value); }
  void append(const css::Token::Delim& v) { fields(v.value); }
  void append(const css::Token::Whitespace& v) { fields(v.value); }
  void append(const css::Token::Function& v) { fields(v.name); }
  void append(const css::Token::Hash& v) { fields(v.type, v.name); }
  void append(const css::Token::Number& v) { fields(v.value, v.valueString, v.type); }
  void append(const css::Token::Percentage& v) { fields(v.value, v.valueString, v.type); }
  void append(const css::Token::Dimension& v) {
    fields(v.value, v.suffixString, v.suffixUnit, v.valueString, v.type);
  }
  void append(const css::Token::ErrorToken& v) { fields(v.type); }
  void append(const css::Specificity& v) {
    fields(v.specialValue(), v.abc().a, v.abc().b, v.abc().c);
  }

  const std::size_t maximumBytes_;
  std::string bytes_;
  bool oversize_ = false;
};

}  // namespace

std::optional<std::string> PropertyRegistry::localDeclarationKey() const {
  return encodeDeclarationKey(2048);
}

bool PropertyRegistry::exactlyEquals(const PropertyRegistry& other) const {
  if (this == &other) {
    return true;
  }
  const auto limit = std::string().max_size();
  return encodeDeclarationKey(limit) == other.encodeDeclarationKey(limit);
}

std::optional<std::string> PropertyRegistry::encodeDeclarationKey(std::size_t maximumBytes) const {
  DeclarationKeyWriter key(maximumBytes);
  const auto properties = allProperties();
  forEachProperty<0, numProperties()>([&](auto index) {
    const auto& property = std::get<index.value>(properties);
    if (property.state != PropertyState::NotSet || property.value ||
        property.specificity != css::Specificity()) {
      key.fields(static_cast<std::uint8_t>(index.value), property.state, property.specificity,
                 property.value);
    }
  });
  static_assert(numProperties() + kResourceColorCount < 255);
  for (std::size_t i = 0; i < kResourceColorCount; ++i) {
    if (const auto& overlay = inlineResourceColors_[i]) {
      key.fields(static_cast<std::uint8_t>(numProperties() + i), overlay->base.state,
                 overlay->base.specificity, overlay->base.value, overlay->value.state,
                 overlay->value.specificity, overlay->value.value);
    }
  }
  for (const auto& [name, entry] : unparsedProperties) {
    if (key.oversize()) {
      break;
    }
    key.fields(std::uint8_t{255}, name, entry.specificity, entry.declaration);
  }
  return std::move(key).finish();
}

}  // namespace donner::svg
