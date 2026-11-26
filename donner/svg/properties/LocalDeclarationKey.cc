#include <bit>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>

#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg {
namespace {

struct AggregateInitializer {
  template <typename T>
  operator T() const;
};

template <typename T, typename... Members>
constexpr bool HasAggregateLayout = std::is_aggregate_v<T> && requires {
  T{std::declval<Members>()...};
} && !requires { T{std::declval<Members>()..., AggregateInitializer()}; };

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

  // An invalid URL has no semantic payload. Its authored closure only controls source edits.
  void append(const css::Token::BadUrl&) {}

  void append(const css::Token& token) {
    fields(token.tokenIndex(), token.offset());
    token.visit([this](const auto& value) { this->append(value); });
  }

  // Decomposing each structure makes a field addition a build error until its encoding is updated.
  template <std::size_t N, typename T>
  void appendMembers(const T& value) {
    static_assert(N >= 1 && N <= 6);
    if constexpr (N == 1) {
      const auto& [a] = value;
      fields(a);
    } else if constexpr (N == 2) {
      const auto& [a, b] = value;
      fields(a, b);
    } else if constexpr (N == 3) {
      const auto& [a, b, c] = value;
      fields(a, b, c);
    } else if constexpr (N == 4) {
      const auto& [a, b, c, d] = value;
      fields(a, b, c, d);
    } else if constexpr (N == 5) {
      const auto& [a, b, c, d, e] = value;
      fields(a, b, c, d, e);
    } else {
      const auto& [a, b, c, d, e, f] = value;
      fields(a, b, c, d, e, f);
    }
  }

  void append(const Lengthd& value) { appendMembers<2>(value); }
  void append(const Vector2d& value) { appendMembers<2>(value); }
  void append(const TransformOrigin& value) { appendMembers<2>(value); }
  void append(const css::RGBA& value) { appendMembers<4>(value); }
  void append(const css::HSLA& value) { appendMembers<4>(value); }
  void append(const css::Color& value) { appendMembers<1>(value); }
  void append(const PaintServer& value) { appendMembers<1>(value); }
  void append(const PaintServer::Solid& value) { appendMembers<1>(value); }
  void append(const PaintServer::ElementReference& value) { appendMembers<2>(value); }
  void append(const Reference& value) { appendMembers<1>(value); }
  void append(const PaintOrder& value) { appendMembers<1>(value); }
  void append(const TextDecorationThickness& value) { appendMembers<2>(value); }
  void append(const Cursor& value) { appendMembers<2>(value); }
  void append(const CursorImage& value) { appendMembers<2>(value); }
  void append(const FilterEffect& value) { appendMembers<1>(value); }
  void append(const FilterEffect::Blur& value) { appendMembers<2>(value); }
  void append(const FilterEffect::ElementReference& value) { appendMembers<1>(value); }
  void append(const FilterEffect::HueRotate& value) { appendMembers<1>(value); }
  void append(const FilterEffect::Brightness& value) { appendMembers<1>(value); }
  void append(const FilterEffect::Contrast& value) { appendMembers<1>(value); }
  void append(const FilterEffect::Grayscale& value) { appendMembers<1>(value); }
  void append(const FilterEffect::Invert& value) { appendMembers<1>(value); }
  void append(const FilterEffect::FilterOpacity& value) { appendMembers<1>(value); }
  void append(const FilterEffect::Saturate& value) { appendMembers<1>(value); }
  void append(const FilterEffect::Sepia& value) { appendMembers<1>(value); }
  void append(const FilterEffect::DropShadow& value) { appendMembers<4>(value); }
  void append(const ClipPath& value) { appendMembers<1>(value); }
  void append(const RadialClipPath::Position& value) { appendMembers<2>(value); }

  void append(const PolygonClipPath& value) { appendMembers<3>(value); }
  void append(const PolygonClipPath::Vertex& value) { appendMembers<2>(value); }
  void append(const InsetClipPath& value) { appendMembers<4>(value); }
  void append(const PathClipPath& value) { appendMembers<3>(value); }
  void append(const Path::Command& value) { appendMembers<3>(value); }
  void append(const FileOffset& value) { appendMembers<2>(value); }
  void append(const FileOffset::LineInfo& value) { appendMembers<2>(value); }
  void append(const SourceRange& value) { appendMembers<2>(value); }
  void append(const css::Function& value) {
    // EOF closure affects source-preserving edits, not the function's computed value.
    fields(value.name, value.values, value.sourceOffset);
  }
  void append(const css::SimpleBlock& value) {
    // EOF closure affects source-preserving edits, not the block's computed value.
    fields(value.associatedToken, value.values, value.sourceOffset);
  }
  void append(const css::ComponentValue& value) { appendMembers<1>(value); }
  void append(const css::Declaration& value) {
    const auto& [name, values, offset, range, bytes, nameSourceByteLength, important, references] =
        value;
    // Authored name widths are queried from the source declaration, never a cached style.
    static_cast<void>(nameSourceByteLength);
    // Pack the two flags in the existing final byte of the key.
    const auto flags = static_cast<std::uint8_t>((static_cast<unsigned>(references) << 1) |
                                                 static_cast<unsigned>(important));
    fields(name, values, offset, range, bytes, flags);
  }
  void append(const css::Token::Ident& value) {
    const auto& [identifier, sourceByteLength] = value;
    static_cast<void>(sourceByteLength);
    append(identifier);
  }
  void append(const css::Token::AtKeyword& value) { appendMembers<1>(value); }
  void append(const css::Token::String& value) { appendMembers<1>(value); }
  void append(const css::Token::BadString& value) { appendMembers<1>(value); }
  void append(const css::Token::Url& value) { appendMembers<1>(value); }
  void append(const css::Token::Delim& value) { appendMembers<1>(value); }
  void append(const css::Token::Whitespace& value) { appendMembers<1>(value); }
  void append(const css::Token::Function& value) { appendMembers<1>(value); }
  void append(const css::Token::Hash& value) { appendMembers<2>(value); }
  void append(const css::Token::Number& value) { appendMembers<3>(value); }
  void append(const css::Token::Percentage& value) { appendMembers<3>(value); }
  void append(const css::Token::Dimension& value) { appendMembers<5>(value); }
  void append(const css::Token::ErrorToken& value) { appendMembers<1>(value); }

  void append(const RadialClipPath& value) { appendMembers<3>(value); }

  // These shapes split their data between a base and their own members.
  void append(const CircleClipPath& value) {
    static_assert(
        HasAggregateLayout<CircleClipPath, RadialClipPath, RadialClipPath::Radius, Lengthd>);
    fields(static_cast<const RadialClipPath&>(value), value.radiusType, value.radius);
  }

  void append(const EllipseClipPath& value) {
    static_assert(HasAggregateLayout<EllipseClipPath, RadialClipPath, RadialClipPath::Radius,
                                     RadialClipPath::Radius, Lengthd, Lengthd>);
    fields(static_cast<const RadialClipPath&>(value), value.radiusXType, value.radiusYType,
           value.radiusX, value.radiusY);
  }

  void append(const Path& path) { fields(path.commands(), path.points()); }

  void append(const css::Specificity& value) {
    const auto [a, b, c] = value.abc();
    // Preserve the legacy 32-bit origin field for the common zero-rank representation.
    const auto priority = static_cast<std::uint32_t>(value.specialValue()) |
                          (static_cast<std::uint32_t>(value.layerRank()) << 8);
    fields(priority, a, b, c);
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
    key.fields(std::uint8_t{255}, name, entry.specificity, entry.sourceDeclaration());
    if (entry.presentationVariable) {
      key.fields(std::uint8_t{254}, false, name, *entry.presentationVariable);
    }
    if (entry.substituted) {
      key.fields(std::uint8_t{254}, true, name);
    }
  }
  return std::move(key).finish();
}

}  // namespace donner::svg
