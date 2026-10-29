#include "donner/svg/text/TextEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>

#include "donner/base/MathUtils.h"
#include "donner/base/Utf8.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/FontMetricDependenciesComponent.h"
#include "donner/svg/components/FontPaintDependenciesComponent.h"
#include "donner/svg/components/GeometryPreparationResourceBudget.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationBaseValue.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationValueSampler.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextInvalidation.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/components/text/TextSystem.h"
#include "donner/svg/core/DominantBaseline.h"
#include "donner/svg/text/TextBackendSimple.h"
#include "donner/svg/text/TextEngineHelpers.h"
#include "donner/svg/text/TextPathWarp.h"
#include "donner/svg/text/TextTransform.h"
#ifdef DONNER_TEXT_FULL
#include "donner/svg/text/TextBackendFull.h"
#endif
#include "donner/svg/core/WritingMode.h"

namespace donner::svg {

namespace {

bool ConsumeShadowStyleWork(std::size_t* remainingWork) {
  if (!remainingWork) {
    return true;
  }
  if (*remainingWork == 0u) {
    return false;
  }
  --*remainingWork;
  return true;
}

bool AppendShadowStyleChildren(Registry& registry, const donner::components::TreeComponent& tree,
                               std::size_t admitted, std::size_t* remainingWork,
                               std::vector<Entity>& pending) {
  for (Entity child = tree.firstChild(); child != entt::null;) {
    if (pending.size() >= admitted || !ConsumeShadowStyleWork(remainingWork)) {
      return false;
    }
    pending.push_back(child);
    const auto* childTree = registry.try_get<donner::components::TreeComponent>(child);
    if (!childTree) {
      return false;
    }
    child = childTree->nextSibling();
  }
  return true;
}

bool PopulateShadowStyleContext(Registry& registry, Entity root, std::size_t admitted,
                                std::size_t* remainingWork, TextStyleContext& result) {
  if (!ConsumeShadowStyleWork(remainingWork)) {
    return false;
  }
  std::vector<Entity> pending{root};
  std::size_t visited = 0;
  while (!pending.empty()) {
    if (++visited > admitted) {
      return false;
    }
    const Entity entity = pending.back();
    pending.pop_back();
    const auto* shadow = registry.try_get<components::ShadowEntityComponent>(entity);
    if (!shadow) {
      return false;
    }
    if (entity != root && registry.any_of<components::TextRootComponent>(shadow->lightEntity)) {
      continue;
    }
    if (!ConsumeShadowStyleWork(remainingWork) ||
        !result.entities.emplace(shadow->lightEntity, entity).second) {
      return false;
    }
    const auto* tree = registry.try_get<donner::components::TreeComponent>(entity);
    if (!tree) {
      return false;
    }
    if (!AppendShadowStyleChildren(registry, *tree, admitted, remainingWork, pending)) {
      return false;
    }
  }
  return true;
}

bool ShadowStyleContextCoversText(const components::ComputedTextComponent& text,
                                  std::size_t* remainingWork, const TextStyleContext& result) {
  for (const auto& span : text.spans) {
    if (!ConsumeShadowStyleWork(remainingWork) ||
        (span.sourceEntity != entt::null && !result.entities.contains(span.sourceEntity))) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::optional<TextStyleContext> BuildShadowTextStyleContext(
    Registry& registry, Entity root, const components::ComputedTextComponent& text,
    std::size_t* remainingWork) {
  TextStyleContext result;
  result.rootEntity = root;
  const std::size_t admitted = registry.view<components::ShadowEntityComponent>().size();
  if (!PopulateShadowStyleContext(registry, root, admitted, remainingWork, result) ||
      !ShadowStyleContextCoversText(text, remainingWork, result)) {
    return std::nullopt;
  }
  return result;
}

namespace {

/// Converts a computed font size only when the result is representable by the shaping API.
float CheckedFontSizePx(double sizePx) {
  if (!std::isfinite(sizePx) || sizePx <= 0.0 ||
      sizePx > static_cast<double>(std::numeric_limits<float>::max())) {
    return 0.0f;
  }
  const float result = static_cast<float>(sizePx);
  return std::isfinite(result) ? result : 0.0f;
}

/// Resolve a computed zero independently of the default unresolved span-size sentinel.
double ResolvedSpanFontSizePx(const components::ComputedTextComponent::TextSpan& span,
                              const TextLayoutParams& params) {
  const Lengthd& size =
      span.hasResolvedFontSize || span.fontSize.value != 0.0 ? span.fontSize : params.fontSize;
  return size.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Mixed);
}

/// Zero or unrepresentable used sizes consume positioning without producing glyphs.
bool HasRenderableSpanText(std::string_view text, float usedSizePx) {
  return !text.empty() && usedSizePx > 0.0f && std::isfinite(usedSizePx);
}

/// Decode a single UTF-8 codepoint, advancing \p i past the consumed bytes.
uint32_t decodeUtf8(const std::string_view str, size_t& i) {
  const auto [cp, length] = Utf8::NextCodepoint(str.substr(i));
  i += static_cast<size_t>(length);
  return static_cast<uint32_t>(cp);
}

/// Returns a glyph's positioning group without changing its source identity.
uint32_t GetPositioningCluster(const TextGlyph& glyph) {
  return glyph.positioningCluster.value_or(glyph.cluster);
}

/// Read a rendered codepoint while retaining the span's original byte addressing.
uint32_t SpanLayoutCodepoint(const components::ComputedTextComponent::TextSpan& span, size_t byte) {
  const std::string_view text(span.text.data() + span.start, span.end - span.start);
  const uint32_t codepoint = byte < text.size() ? decodeUtf8(text, byte) : 0;
  return span.textTransform == TextTransform::FullWidth
             ? text_transform::FullWidthCodepoint(
                   codepoint, span.preservesSpaces || PreservesSpaces(span.whiteSpace))
             : codepoint;
}

/// Encode a single Unicode codepoint as UTF-8.
std::string encodeUtf8(uint32_t cp) {
  std::string result;
  Utf8::Append(static_cast<char32_t>(cp), std::back_inserter(result));
  return result;
}

/// Returns true if \p font shapes \p cp to a non-.notdef glyph.
bool fontSupportsCodepoint(const TextBackend& backend, FontHandle font, float fontSizePx,
                           uint32_t cp) {
  if (!font || cp == 0) {
    return false;
  }

  const std::string utf8 = encodeUtf8(cp);
  const auto shaped =
      backend.shapeRun(font, fontSizePx, utf8, 0, utf8.size(), false, FontVariant::Normal, false);
  return !shaped.glyphs.empty() && shaped.glyphs.front().glyphIndex != 0;
}

/**
 * Returns the face for the first family in @p families that a registered `@font-face` rule or the
 * font provider actually supplies.
 *
 * The availability test is separate from the lookup because \ref FontManager::findFont answers an
 * unknown family with the embedded fallback rather than an invalid handle; without it a list like
 * `Invalid, Noto Sans` would stop at its first entry and never reach the family that exists.
 *
 * @param fontManager Font manager to query.
 * @param families Font families in cascade order, highest priority first.
 * @param weight CSS font-weight value (100-900, 400=normal, 700=bold).
 * @param style CSS font-style value (0=normal, 1=italic, 2=oblique).
 * @param stretch CSS font-stretch value (1-9, 5=normal, matching FontStretch).
 * @return The first available face, or an invalid handle when no family is available.
 */
FontHandle FindFirstAvailableFont(FontManager& fontManager, std::span<const RcString> families,
                                  int weight = 400, int style = 0, int stretch = 5) {
  for (const RcString& family : families) {
    if (!fontManager.hasFamily(family)) {
      continue;
    }
    if (const FontHandle candidate = fontManager.findFont(family, weight, style, stretch)) {
      return candidate;
    }
  }

  return {};
}

/// Substitute the trusted embedded fallback before a backend can see unsupported font bytes.
FontHandle selectBackendSafeFont(const TextBackend& backend, FontManager& fontManager,
                                 FontHandle font) {
  if (font && backend.requiresTrustedFontData() && !fontManager.isTrustedFont(font)) {
    return fontManager.fallbackFont();
  }
  return font;
}

/**
 * Existing nonspacing classification used to keep shaping input intact at
 * positioning boundaries. These codepoints still consume addressable source slots.
 * Includes combining marks, zero-width joiners, and variation selectors.
 */
bool isNonSpacing(uint32_t cp) {
  // Combining marks (General Category M).
  if ((cp >= 0x0300 && cp <= 0x036F) ||  // Combining Diacritical Marks
      (cp >= 0x0483 && cp <= 0x0489) ||  // Cyrillic combining
      (cp >= 0x0591 && cp <= 0x05C7) ||  // Hebrew combining
      (cp >= 0x0610 && cp <= 0x061A) ||  // Arabic combining
      (cp >= 0x064B && cp <= 0x065F) ||  // Arabic combining
      (cp >= 0x0670 && cp == 0x0670) ||  // Arabic superscript alef
      (cp >= 0x06D6 && cp <= 0x06ED) ||  // Arabic combining
      (cp >= 0x0730 && cp <= 0x074A) ||  // Syriac combining
      (cp >= 0x0E31 && cp == 0x0E31) ||  // Thai combining
      (cp >= 0x0E34 && cp <= 0x0E3A) ||  // Thai combining
      (cp >= 0x0EB1 && cp == 0x0EB1) ||  // Lao combining
      (cp >= 0x0EB4 && cp <= 0x0EBC) ||  // Lao combining
      (cp >= 0x1AB0 && cp <= 0x1AFF) ||  // Combining Diacritical Marks Extended
      (cp >= 0x1DC0 && cp <= 0x1DFF) ||  // Combining Diacritical Marks Supplement
      (cp >= 0x20D0 && cp <= 0x20FF) ||  // Combining Diacritical Marks for Symbols
      (cp >= 0x3099 && cp <= 0x309A) ||  // Kana combining voicing marks
      (cp >= 0xFE20 && cp <= 0xFE2F)) {  // Combining Half Marks
    return true;
  }

  // Zero-width joiners and format characters used in emoji/ligature sequences.
  if (cp == 0x200C ||  // Zero Width Non-Joiner
      cp == 0x200D ||  // Zero Width Joiner (emoji ZWJ sequences)
      cp == 0x034F) {  // Combining Grapheme Joiner
    return true;
  }

  // Variation selectors (emoji style selectors, ideographic variation sequences).
  if ((cp >= 0xFE00 && cp <= 0xFE0F) ||    // Variation Selectors (VS1-VS16)
      (cp >= 0xE0100 && cp <= 0xE01EF)) {  // Variation Selectors Supplement
    return true;
  }

  return false;
}

Path transformPath(const Path& spline, const Transform2d& transform) {
  PathBuilder builder;
  const auto& points = spline.points();

  for (const auto& command : spline.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo:
        builder.moveTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::LineTo:
        builder.lineTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::QuadTo:
        builder.quadTo(transform.transformPosition(points[command.pointIndex]),
                       transform.transformPosition(points[command.pointIndex + 1]));
        break;
      case Path::Verb::CurveTo:
        builder.curveTo(transform.transformPosition(points[command.pointIndex]),
                        transform.transformPosition(points[command.pointIndex + 1]),
                        transform.transformPosition(points[command.pointIndex + 2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  }

  return builder.build();
}

Entity findTextRootEntity(EntityHandle handle) {
  Entity current = handle.entity();
  Registry& registry = *handle.registry();

  while (current != entt::null) {
    if (registry.any_of<components::TextRootComponent>(current)) {
      return current;
    }

    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      break;
    }
    current = tree->parent();
  }

  return entt::null;
}

bool isDescendantOf(Registry& registry, Entity entity, Entity ancestor) {
  Entity current = entity;
  while (current != entt::null) {
    if (current == ancestor) {
      return true;
    }

    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      break;
    }
    current = tree->parent();
  }

  return false;
}

TextLayoutParams buildTextLayoutParams(Registry& registry, EntityHandle handle,
                                       const components::ComputedStyleComponent& style,
                                       const components::TextComponent& textComp) {
  TextLayoutParams params;
  const auto& properties = style.properties.value();

  params.fontFamilies = properties.fontFamily.get().value();
  params.fontSize = properties.fontSize.get().value();
  params.viewBox = components::LayoutSystem().getViewBox(handle);

  const FontMetrics baseFontMetrics = FontMetrics::DefaultsWithFontSize(12.0);
  const double fontSizePx =
      params.fontSize.toPixels(params.viewBox, baseFontMetrics, Lengthd::Extent::Mixed);
  params.fontMetrics = FontMetrics::DefaultsWithFontSize(fontSizePx);

  params.textAnchor = properties.textAnchor.get().value();
  params.writingMode = properties.writingMode.get().value();
  params.fontKerning = properties.fontKerning.get().value();
  params.fontSizeAdjust = properties.fontSizeAdjust.get().value();
  params.letterSpacingPx = properties.letterSpacing.get().value().toPixels(
      params.viewBox, params.fontMetrics, Lengthd::Extent::X);
  params.wordSpacingPx = properties.wordSpacing.get().value().toPixels(
      params.viewBox, params.fontMetrics, Lengthd::Extent::X);
  params.textLength = textComp.textLength;
  params.lengthAdjust = textComp.lengthAdjust;

  // SVG2 inline-size: resolve the length to pixels in the inline (X) axis. A value <= 0 means no
  // wrapping area. inline-size is non-inherited and applies to the <text> root, so it is read from
  // the root element's computed style here.
  const Lengthd inlineSize = properties.inlineSize.get().value();
  params.inlineSizePx = inlineSize.toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::X);
  return params;
}

constexpr std::array<std::string_view, 4> kTextLengthListNames = {"x", "y", "dx", "dy"};

struct SampledTextPositions {
  std::array<std::optional<std::vector<Lengthd>>, kTextLengthListNames.size()> lengths;
  std::optional<std::vector<double>> rotations;
};

size_t TextSpanCodepointCount(const components::ComputedTextComponent::TextSpan& span) {
  if (span.hidden) {
    return 0;
  }
  const std::string_view text(span.text.data() + span.start, span.end - span.start);
  size_t count = 0;
  size_t offset = 0;
  while (offset < text.size()) {
    const auto [codepoint, bytes] = Utf8::NextCodepoint(text.substr(offset));
    (void)codepoint;
    offset += static_cast<size_t>(bytes);
    ++count;
  }
  return count;
}

std::string SerializeTextPositionLengths(std::span<const Lengthd> values) {
  std::ostringstream output;
  output.precision(std::numeric_limits<double>::max_digits10);
  for (size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ' ';
    }
    output << values[index];
  }
  return output.str();
}

std::string SerializeTextPositionRotations(std::span<const double> values) {
  std::ostringstream output;
  output.precision(std::numeric_limits<double>::max_digits10);
  for (size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ' ';
    }
    output << values[index];
  }
  return output.str();
}

template <typename Declaration>
std::optional<size_t> TextPositionDeclarationArity(const Declaration& declaration) {
  const auto parse = [](const std::optional<std::string>& value) -> std::optional<size_t> {
    if (!value) {
      return 0;
    }
    const auto parsed = components::ParseAnimationLengths(*value);
    return parsed ? std::optional(parsed->size()) : std::nullopt;
  };
  if constexpr (std::is_same_v<Declaration, components::SetAnimationComponent>) {
    return parse(declaration.to);
  } else {
    if (!declaration.values.empty()) {
      size_t result = 0;
      for (const std::string& value : declaration.values) {
        const auto parsed = parse(std::optional<std::string>(value));
        if (!parsed) {
          return std::nullopt;
        }
        result = std::max(result, *parsed);
      }
      return result;
    }
    const auto first = parse(declaration.from);
    if (!first) {
      return std::nullopt;
    }
    const auto second = declaration.to ? parse(declaration.to) : parse(declaration.by);
    return second ? std::optional(std::max(*first, *second)) : std::nullopt;
  }
}

std::optional<std::string> SampleTextPositionRecipe(
    const components::ContextualLengthRecipe& recipe, std::string underlying,
    const components::AnimationValueContext& context, bool nonAdditive, bool neutralEmpty) {
  return components::SampleContextualAnimationRecipe(
      recipe, std::move(underlying), [&](const auto& sample, const std::string& lower) {
        const auto sampled = std::visit(
            [&](const auto& declaration) {
              const auto arity = TextPositionDeclarationArity(declaration);
              if (!arity || *arity == 0) {
                return std::optional<std::pair<std::string, std::string>>{};
              }
              std::string layerUnderlying = lower;
              if (neutralEmpty && layerUnderlying.empty()) {
                std::vector<Lengthd> zeros(*arity);
                layerUnderlying = SerializeTextPositionLengths(zeros);
              }
              using Declaration = std::decay_t<decltype(declaration)>;
              if constexpr (std::is_same_v<Declaration, components::SetAnimationComponent>) {
                return components::SampleAnimationValue(sample.progress, sample.iteration,
                                                        &declaration, nullptr, nullptr,
                                                        layerUnderlying, context);
              } else {
                components::AnimateValueComponent effective = declaration;
                if (nonAdditive) {
                  effective.additive = false;
                  effective.accumulate = false;
                }
                return components::SampleAnimationValue(sample.progress, sample.iteration, nullptr,
                                                        &effective, nullptr, layerUnderlying,
                                                        context);
              }
            },
            sample.declaration);
        return sampled && components::ParseAnimationLengths(sampled->second)
                   ? std::optional(sampled->second)
                   : std::nullopt;
      });
}

std::optional<size_t> TextPositionRecipeArity(const components::ContextualLengthRecipe& recipe) {
  size_t arity = 0;
  bool hasValidLayer = false;
  for (const auto& sample : recipe.samples) {
    if (sample.frozenUnderlying) {
      if (const auto frozenArity = TextPositionRecipeArity(*sample.frozenUnderlying)) {
        hasValidLayer = true;
        arity = std::max(arity, *frozenArity);
      }
    }
    const auto layerArity = std::visit(
        [](const auto& declaration) { return TextPositionDeclarationArity(declaration); },
        sample.declaration);
    if (!layerArity || *layerArity == 0) {
      continue;
    }
    hasValidLayer = true;
    arity = std::max(arity, *layerArity);
  }
  return hasValidLayer ? std::optional(arity) : std::nullopt;
}

template <typename Values>
const Values* TextPositionValues(Registry& registry, Entity owner, size_t attributeIndex);

template <>
const SmallVector<Lengthd, 1>* TextPositionValues(Registry& registry, Entity owner,
                                                  size_t attributeIndex) {
  const auto* positioning = registry.try_get<components::TextPositioningComponent>(owner);
  if (!positioning || registry.any_of<components::TextPathComponent>(owner)) {
    return nullptr;
  }
  switch (attributeIndex) {
    case 0: return &positioning->x;
    case 1: return &positioning->y;
    case 2: return &positioning->dx;
    case 3: return &positioning->dy;
    default: return nullptr;
  }
}

template <>
const SmallVector<double, 1>* TextPositionValues(Registry& registry, Entity owner, size_t) {
  const auto* positioning = registry.try_get<components::TextPositioningComponent>(owner);
  return positioning && !registry.any_of<components::TextPathComponent>(owner)
             ? &positioning->rotateDegrees
             : nullptr;
}

template <typename Value>
std::optional<Value> ResolveSpanTextPosition(
    Registry& registry, const components::ComputedTextComponent& text,
    std::span<const size_t> spanPrefix,
    const std::unordered_map<Entity, SampledTextPositions>& sampled, size_t spanIndex,
    size_t characterIndex, size_t attributeIndex, bool repeatLast) {
  Entity current = text.spans[spanIndex].sourceEntity;
  while (current != entt::null && registry.valid(current)) {
    const auto range = text.ownerSpanRanges.find(current);
    if (range == text.ownerSpanRanges.end() || range->second.begin >= spanPrefix.size()) {
      break;
    }
    const size_t index = spanPrefix[spanIndex] - spanPrefix[range->second.begin] + characterIndex;
    if (const auto replacement = sampled.find(current); replacement != sampled.end()) {
      if constexpr (std::is_same_v<Value, double>) {
        if (replacement->second.rotations) {
          const auto& values = *replacement->second.rotations;
          if (index < values.size()) {
            return values[index];
          }
          if (repeatLast && !values.empty()) {
            return values.back();
          }
          current = registry.any_of<components::TextRootComponent>(current)
                        ? entt::null
                        : registry.get<donner::components::TreeComponent>(current).parent();
          continue;
        }
      } else {
        const auto& values = replacement->second.lengths[attributeIndex];
        if (values) {
          if (index < values->size()) {
            return (*values)[index];
          }
          current = registry.any_of<components::TextRootComponent>(current)
                        ? entt::null
                        : registry.get<donner::components::TreeComponent>(current).parent();
          continue;
        }
      }
    }
    if (const auto* values =
            TextPositionValues<SmallVector<Value, 1>>(registry, current, attributeIndex);
        values && !values->empty()) {
      if (index < values->size()) {
        return (*values)[index];
      }
      if (repeatLast) {
        return values->back();
      }
    }
    if (registry.any_of<components::TextRootComponent>(current)) {
      break;
    }
    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    current = tree ? tree->parent() : entt::null;
  }
  return std::nullopt;
}

void ResolveAnimatedTextPositions(Registry& registry, components::ComputedTextComponent& text,
                                  const Box2d& viewBox, const FontMetrics& fontMetrics) {
  std::vector<size_t> spanPrefix(text.spans.size() + 1);
  for (size_t index = 0; index < text.spans.size(); ++index) {
    spanPrefix[index + 1] = spanPrefix[index] + TextSpanCodepointCount(text.spans[index]);
  }

  struct Recipe {
    Entity owner = entt::null;
    size_t attributeIndex = 0;
    bool rotate = false;
    std::shared_ptr<const components::ContextualLengthRecipe> value;
  };
  std::vector<Recipe> recipes;
  size_t evaluationWork = 0;
  for (const auto& [owner, range] : text.ownerSpanRanges) {
    const auto* animated = registry.try_get<components::AnimatedValuesComponent>(owner);
    if (!animated) {
      continue;
    }
    if (range.end >= spanPrefix.size()) {
      return;
    }
    for (size_t attributeIndex = 0; attributeIndex < kTextLengthListNames.size();
         ++attributeIndex) {
      const auto found =
          animated->contextualLengths.find(std::string(kTextLengthListNames[attributeIndex]));
      if (found == animated->contextualLengths.end()) {
        continue;
      }
      if (found->second->evaluationWork > std::numeric_limits<size_t>::max() - evaluationWork) {
        return;
      }
      const auto arity = TextPositionRecipeArity(*found->second);
      if (!arity || *arity > components::AnimationResourceBudget::kMaximumNumbersPerValue ||
          *arity > std::numeric_limits<size_t>::max() / 16) {
        continue;
      }
      const size_t serializationWork = *arity * 16;
      evaluationWork += found->second->evaluationWork;
      if (serializationWork > std::numeric_limits<size_t>::max() - evaluationWork) {
        return;
      }
      evaluationWork += serializationWork;
      recipes.push_back({owner, attributeIndex, false, found->second});
    }
    if (const auto found = animated->contextualLengths.find("rotate");
        found != animated->contextualLengths.end()) {
      if (found->second->evaluationWork > std::numeric_limits<size_t>::max() - evaluationWork) {
        return;
      }
      const auto arity = TextPositionRecipeArity(*found->second);
      if (!arity || *arity > components::AnimationResourceBudget::kMaximumNumbersPerValue ||
          *arity > std::numeric_limits<size_t>::max() / 16) {
        continue;
      }
      const size_t serializationWork = *arity * 16;
      evaluationWork += found->second->evaluationWork;
      if (serializationWork > std::numeric_limits<size_t>::max() - evaluationWork) {
        return;
      }
      evaluationWork += serializationWork;
      recipes.push_back({owner, 0, true, found->second});
    }
  }
  if (recipes.empty()) {
    return;
  }

  for (size_t spanIndex = 0; spanIndex < text.spans.size(); ++spanIndex) {
    const size_t characterCount = spanPrefix[spanIndex + 1] - spanPrefix[spanIndex];
    if (characterCount == 0 || text.spans[spanIndex].sourceEntity == entt::null) {
      continue;
    }
    size_t depth = 0;
    Entity current = text.spans[spanIndex].sourceEntity;
    while (current != entt::null && registry.valid(current)) {
      ++depth;
      if (registry.any_of<components::TextRootComponent>(current)) {
        break;
      }
      const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
      current = tree ? tree->parent() : entt::null;
    }
    if (depth > (std::numeric_limits<size_t>::max() - 4) / 5 ||
        characterCount > std::numeric_limits<size_t>::max() / (depth * 5 + 4)) {
      return;
    }
    const size_t projectionWork = characterCount * (depth * 5 + 4);
    if (projectionWork > std::numeric_limits<size_t>::max() - evaluationWork) {
      return;
    }
    evaluationWork += projectionWork;
  }

  auto* budget = registry.ctx().find<components::AnimationResourceBudget>();
  if (!budget || !budget->reserveContextualWork(evaluationWork)) {
    return;
  }

  std::unordered_map<Entity, SampledTextPositions> sampled;
  for (const Recipe& recipe : recipes) {
    const auto range = text.ownerSpanRanges.find(recipe.owner);
    if (range == text.ownerSpanRanges.end() || range->second.end >= spanPrefix.size()) {
      return;
    }
    components::AnimationValueContext context;
    context.viewBox = viewBox;
    context.fontMetrics = fontMetrics;
    if (const auto fontSize = text.textLengthOwnerFontSizes.find(recipe.owner);
        fontSize != text.textLengthOwnerFontSizes.end()) {
      context.fontMetrics.fontSize =
          fontSize->second.toPixels(viewBox, fontMetrics, Lengthd::Extent::Y);
    }

    if (recipe.rotate) {
      std::vector<double> underlying;
      if (const auto* local =
              TextPositionValues<SmallVector<double, 1>>(registry, recipe.owner, 0)) {
        if (local->size() > components::AnimationResourceBudget::kMaximumNumbersPerValue) {
          continue;
        }
        underlying.assign(local->begin(), local->end());
      }
      const auto result = SampleTextPositionRecipe(
          *recipe.value, SerializeTextPositionRotations(underlying), context, true, false);
      const auto parsed = result ? components::ParseAnimationLengths(*result) : std::nullopt;
      if (!parsed) {
        continue;
      }
      std::vector<double> rotations;
      rotations.reserve(parsed->size());
      bool valid = true;
      for (const Lengthd value : *parsed) {
        if (value.unit != Lengthd::Unit::None) {
          valid = false;
          break;
        }
        rotations.push_back(value.value);
      }
      if (valid) {
        sampled[recipe.owner].rotations = std::move(rotations);
      }
      continue;
    }

    context.extent = recipe.attributeIndex % 2 == 0 ? Lengthd::Extent::X : Lengthd::Extent::Y;
    std::vector<Lengthd> underlying;
    if (const auto* local = TextPositionValues<SmallVector<Lengthd, 1>>(registry, recipe.owner,
                                                                        recipe.attributeIndex)) {
      if (local->size() > components::AnimationResourceBudget::kMaximumNumbersPerValue) {
        continue;
      }
      underlying.assign(local->begin(), local->end());
    }
    const auto result =
        SampleTextPositionRecipe(*recipe.value, SerializeTextPositionLengths(underlying), context,
                                 false, recipe.attributeIndex >= 2);
    auto parsed = result ? components::ParseAnimationLengths(*result) : std::nullopt;
    if (parsed) {
      sampled[recipe.owner].lengths[recipe.attributeIndex] = std::move(*parsed);
    }
  }

  for (size_t spanIndex = 0; spanIndex < text.spans.size(); ++spanIndex) {
    auto& span = text.spans[spanIndex];
    const size_t count = spanPrefix[spanIndex + 1] - spanPrefix[spanIndex];
    if (span.hidden || span.sourceEntity == entt::null) {
      continue;
    }
    span.xList.resize(count);
    span.yList.resize(count);
    span.dxList.resize(count);
    span.dyList.resize(count);
    for (size_t index = 0; index < count; ++index) {
      span.xList[index] = ResolveSpanTextPosition<Lengthd>(registry, text, spanPrefix, sampled,
                                                           spanIndex, index, 0, false);
      span.yList[index] = ResolveSpanTextPosition<Lengthd>(registry, text, spanPrefix, sampled,
                                                           spanIndex, index, 1, false);
      span.dxList[index] = ResolveSpanTextPosition<Lengthd>(registry, text, spanPrefix, sampled,
                                                            spanIndex, index, 2, false);
      span.dyList[index] = ResolveSpanTextPosition<Lengthd>(registry, text, spanPrefix, sampled,
                                                            spanIndex, index, 3, false);
    }
    span.rotateList.clear();
    for (size_t index = 0; index < count; ++index) {
      const auto rotation = ResolveSpanTextPosition<double>(registry, text, spanPrefix, sampled,
                                                            spanIndex, index, 0, true);
      if (rotation) {
        span.rotateList.push_back(*rotation);
      }
    }
    const bool textPath = span.pathSpline.has_value();
    span.startsNewChunk = !textPath && !span.xList.empty() &&
                          (span.xList.front().has_value() || span.yList.front().has_value());
  }
}

void ResolvePerSpanLayoutStyles(Registry& registry, components::ComputedTextComponent& text,
                                const Box2d& viewBox, const FontMetrics& fontMetrics,
                                const TextStyleContext* styleContext = nullptr) {
  using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;

  for (auto& [owner, fontSize] : text.textLengthOwnerFontSizes) {
    const Entity styleEntity = styleContext ? styleContext->styleEntityFor(owner) : owner;
    if (styleEntity != entt::null) {
      if (const auto* style = registry.try_get<components::ComputedStyleComponent>(styleEntity);
          style && style->properties) {
        fontSize = style->properties->fontSize.get().value();
      }
    }
  }

  for (auto& span : text.spans) {
    if (span.sourceEntity == entt::null) {
      continue;
    }

    Entity styleEntity =
        styleContext ? styleContext->styleEntityFor(span.sourceEntity) : span.sourceEntity;
    auto* style = registry.try_get<components::ComputedStyleComponent>(styleEntity);
    if ((!style || !style->properties) &&
        registry.all_of<donner::components::TreeComponent>(styleEntity)) {
      const Entity parent = registry.get<donner::components::TreeComponent>(styleEntity).parent();
      if (parent != entt::null) {
        style = registry.try_get<components::ComputedStyleComponent>(parent);
        styleEntity = parent;
      }
    }

    if (!style || !style->properties) {
      continue;
    }

    span.textAnchor = style->properties->textAnchor.get().value();
    span.textDecoration = style->properties->textDecoration.get().value();
    span.textOrientation = style->properties->textOrientation.get().value();
    span.baselineShift = style->properties->baselineShift.get().value();

    // Effective baseline alignment (matching resvg): a non-auto `alignment-baseline`
    // overrides; otherwise the span's (inherited) `dominant-baseline` applies.
    {
      DominantBaseline dominantBaseline = style->properties->dominantBaseline.get().value();
      if (dominantBaseline == DominantBaseline::NoChange) {
        // `no-change` uses the parent element's dominant baseline (one level, matching
        // resvg). A residual NoChange (parent is also no-change, or there is no parent
        // style) behaves like auto in computeBaselineShift.
        if (registry.all_of<donner::components::TreeComponent>(styleEntity)) {
          const Entity parent =
              registry.get<donner::components::TreeComponent>(styleEntity).parent();
          if (const auto* parentStyle =
                  parent != entt::null
                      ? registry.try_get<components::ComputedStyleComponent>(parent)
                      : nullptr;
              parentStyle && parentStyle->properties) {
            dominantBaseline = parentStyle->properties->dominantBaseline.get().value();
          }
        }
      }

      const DominantBaseline alignmentBaseline = style->properties->alignmentBaseline.get().value();
      span.alignmentBaseline =
          alignmentBaseline != DominantBaseline::Auto ? alignmentBaseline : dominantBaseline;
    }

    span.fontWeight = style->properties->fontWeight.get().value();
    span.fontStyle = style->properties->fontStyle.get().value();
    span.fontStretch = static_cast<FontStretch>(style->properties->fontStretch.get().value());
    span.fontVariant = style->properties->fontVariant.get().value();
    span.fontSynthesisSmallCaps = style->properties->fontSynthesisSmallCaps.get().value();
    span.textTransform = style->properties->textTransform.get().value();
    span.fontKerning = style->properties->fontKerning.get().value();
    span.fontSizeAdjust.emplace(style->properties->fontSizeAdjust.get().value());
    span.fontSize = style->properties->fontSize.get().value();
    span.hasResolvedFontSize = true;
    span.fontFamilies = style->properties->fontFamily.get().value();
    span.visibility = style->properties->visibility.get().value();
    span.letterSpacingPx = style->properties->letterSpacing.get().value().toPixels(
        viewBox, fontMetrics, Lengthd::Extent::X);
    span.wordSpacingPx = style->properties->wordSpacing.get().value().toPixels(viewBox, fontMetrics,
                                                                               Lengthd::Extent::X);

    span.baselineShiftKeyword = BSK::Length;
    span.ancestorBaselineShifts.clear();
    const bool isTextRoot = styleContext
                                ? styleEntity == styleContext->rootEntity
                                : registry.any_of<components::TextRootComponent>(styleEntity);
    if (isTextRoot) {
      span.baselineShift = Lengthd(0, Lengthd::Unit::None);
    } else {
      if (span.baselineShift.unit == Lengthd::Unit::Em && span.baselineShift.value == -0.33) {
        span.baselineShiftKeyword = BSK::Sub;
      } else if (span.baselineShift.unit == Lengthd::Unit::Em && span.baselineShift.value == 0.4) {
        span.baselineShiftKeyword = BSK::Super;
      }

      Entity ancestor = styleEntity;
      while (registry.all_of<donner::components::TreeComponent>(ancestor)) {
        ancestor = registry.get<donner::components::TreeComponent>(ancestor).parent();
        if (ancestor == entt::null || (styleContext && ancestor == styleContext->rootEntity) ||
            registry.any_of<components::TextRootComponent>(ancestor)) {
          break;
        }

        auto* ancestorStyle = registry.try_get<components::ComputedStyleComponent>(ancestor);
        if (!ancestorStyle || !ancestorStyle->properties) {
          continue;
        }

        const Lengthd ancestorShift = ancestorStyle->properties->baselineShift.get().value();
        const double ancestorFontSizePx =
            ancestorStyle->properties->fontSize.get().value().toPixels(viewBox, fontMetrics,
                                                                       Lengthd::Extent::Mixed);
        BSK ancestorKeyword = BSK::Length;
        if (ancestorShift.unit == Lengthd::Unit::Em && ancestorShift.value == -0.33) {
          ancestorKeyword = BSK::Sub;
        } else if (ancestorShift.unit == Lengthd::Unit::Em && ancestorShift.value == 0.4) {
          ancestorKeyword = BSK::Super;
        }

        span.ancestorBaselineShifts.push_back({ancestorKeyword, ancestorShift, ancestorFontSizePx});
      }
    }

    if (auto* textComp = registry.try_get<components::TextComponent>(span.sourceEntity)) {
      if (textComp->textLength.has_value()) {
        span.textLength = textComp->textLength;
        span.lengthAdjust = textComp->lengthAdjust;
      }
    }
  }

  ResolveAnimatedTextPositions(registry, text, viewBox, fontMetrics);
}

}  // namespace

namespace text_engine_detail {

double computeBaselineShift(DominantBaseline baseline, const FontVMetrics& vm, float scale) {
  // Keyword → offset mapping matches resvg's usvg text layout
  // (ResolvedFont::alignment_baseline_shift), which in turn matches Chrome's hardcoded
  // baseline table. Positive values move glyphs down (+Y in SVG coordinates).
  switch (baseline) {
    case DominantBaseline::Auto:
    case DominantBaseline::Alphabetic:
    case DominantBaseline::UseScript:  // Deprecated SVG 1.1 keyword; behaves like auto.
    case DominantBaseline::NoChange:   // Resolved to the parent's value before layout; a
                                       // residual no-change behaves like auto.
    case DominantBaseline::ResetSize:  // Deprecated SVG 1.1 keyword; behaves like auto.
      return 0.0;
    case DominantBaseline::Middle: {
      // Half the x-height. Fonts without an OS/2 sxHeight fall back to 45% of
      // ascent−descent (what Firefox and resvg use).
      const double xHeight = vm.xHeight > 0 ? static_cast<double>(vm.xHeight)
                                            : static_cast<double>(vm.ascent - vm.descent) * 0.45;
      return xHeight * 0.5 * scale;
    }
    case DominantBaseline::Central:
      return static_cast<double>(vm.ascent + vm.descent) * 0.5 * scale;
    case DominantBaseline::Hanging: return static_cast<double>(vm.ascent) * 0.8 * scale;
    case DominantBaseline::Mathematical: return static_cast<double>(vm.ascent) * 0.5 * scale;
    case DominantBaseline::TextTop: return static_cast<double>(vm.ascent) * scale;
    case DominantBaseline::TextBottom:
    case DominantBaseline::Ideographic: return static_cast<double>(vm.descent) * scale;
  }
  return 0.0;
}

std::vector<ChunkRange> findChunkRanges(std::string_view spanText,
                                        const SmallVector<std::optional<Lengthd>, 1>& xList,
                                        const SmallVector<std::optional<Lengthd>, 1>& yList) {
  std::vector<ChunkRange> chunkRanges;
  size_t scanPos = 0;
  unsigned int codepointIndex = 0;
  size_t currentChunkStart = 0;
  bool previousWasJoiner = false;

  while (scanPos < spanText.size()) {
    const size_t byteStart = scanPos;
    const uint32_t cp = decodeUtf8(spanText, scanPos);
    const bool middle = codepointIndex != 0 && (isNonSpacing(cp) || previousWasJoiner);
    const bool hasAbsX = codepointIndex < xList.size() && xList[codepointIndex].has_value();
    const bool hasAbsY = codepointIndex < yList.size() && yList[codepointIndex].has_value();
    if (!middle && (hasAbsX || hasAbsY) && byteStart != currentChunkStart) {
      chunkRanges.push_back({currentChunkStart, byteStart});
      currentChunkStart = byteStart;
    }
    previousWasJoiner = cp == 0x200D;
    ++codepointIndex;
  }
  chunkRanges.push_back({currentChunkStart, spanText.size()});
  return chunkRanges;
}

ByteIndexMappings buildByteIndexMappings(std::string_view spanText) {
  ByteIndexMappings result;
  result.byteToCharIdx.resize(spanText.size(), 0);
  unsigned int codepointIndex = 0;
  size_t byteIndex = 0;
  while (byteIndex < spanText.size()) {
    const size_t startIndex = byteIndex;
    decodeUtf8(spanText, byteIndex);
    for (size_t i = startIndex; i < byteIndex; ++i) {
      result.byteToCharIdx[i] = codepointIndex;
    }
    ++codepointIndex;
  }
  return result;
}

/// A typographic cluster keeps glyph-local shaping offsets separate from path placement.
struct TextPathCluster {
  size_t runIndex;
  size_t glyphStart;
  size_t glyphEnd;
  unsigned int charIndex;
  double pathOffset;
  double baselineOffset;
  double advance = 0.0;
  TextAnchor circuitAnchor = TextAnchor::Start;
};

/// Reusable path pen and cluster storage for the current textPath scope.
struct TextPathStagingState {
  double advance = 0.0;
  double dy = 0.0;
  std::vector<TextPathCluster> clusters;
};

struct DeferredTextPath {
  size_t firstRun = 0;
  size_t endRun = 0;
  Path::MeasuredPath measuredPath;
  bool closed = false;
  std::vector<TextPathCluster> clusters;
  std::shared_ptr<TextPathMeasurementReservation> measurementReservation;
};

constexpr size_t kMaximumDeferredTextPathBytes = Path::kMaximumMeasuredPathRetainedBytes;

std::shared_ptr<components::DocumentResourceFamilyBudget> TextPathResourceFamily(
    Registry& registry) {
  auto& context = registry.ctx();
  if (!context.contains<components::DocumentResourceFamilyContext>()) {
    context.emplace<components::DocumentResourceFamilyContext>();
  }
  auto& familyContext = context.get<components::DocumentResourceFamilyContext>();
  if (!familyContext.budget) {
    familyContext.budget = std::make_shared<components::DocumentResourceFamilyBudget>();
  }
  return familyContext.budget;
}

class ScopedTextPathMeasurementWorkEpoch {
public:
  explicit ScopedTextPathMeasurementWorkEpoch(Registry& registry)
      : family_(TextPathResourceFamily(registry)) {
    family_->beginTextPathMeasurementWorkEpoch();
  }
  ~ScopedTextPathMeasurementWorkEpoch() { family_->endTextPathMeasurementWorkEpoch(); }

  ScopedTextPathMeasurementWorkEpoch(const ScopedTextPathMeasurementWorkEpoch&) = delete;
  ScopedTextPathMeasurementWorkEpoch& operator=(const ScopedTextPathMeasurementWorkEpoch&) = delete;

private:
  std::shared_ptr<components::DocumentResourceFamilyBudget> family_;
};

struct TextPathMeasurementAdmission {
  std::shared_ptr<components::DocumentResourceFamilyBudget> family;
  std::shared_ptr<TextPathMeasurementReservation> reservation;
  size_t workUnits = 0;
};

std::optional<size_t> TextPathSegmentEndBytes(const Path& sourcePath) {
  if (sourcePath.commands().size() > std::numeric_limits<size_t>::max() / sizeof(double)) {
    return std::nullopt;
  }
  return sourcePath.commands().size() * sizeof(double);
}

TextPathMeasurementAdmission AdmitTextPathMeasurement(Registry& registry, const Path& sourcePath,
                                                      size_t localAvailableBytes,
                                                      size_t mappingOverhead) {
  TextPathMeasurementAdmission result;
  result.family = TextPathResourceFamily(registry);
  const auto segmentEndBytes = TextPathSegmentEndBytes(sourcePath);
  const size_t familyAvailable =
      result.family->availableBytes(components::DocumentResourceFamilyBudget::Kind::Geometry);
  const size_t availableBytes = std::min(localAvailableBytes, familyAvailable);
  if (!segmentEndBytes || *segmentEndBytes > availableBytes ||
      mappingOverhead > availableBytes - *segmentEndBytes) {
    return result;
  }
  const size_t measurementBytesAvailable = availableBytes - *segmentEndBytes - mappingOverhead;
  result.workUnits =
      std::min(sourcePath.measurementWorkLimitForRetainedBytes(measurementBytesAvailable),
               result.family->availableTextPathMeasurementWork());
  const auto measurementUpper = sourcePath.measurementRetainedBytesUpperBound(result.workUnits);
  if (!measurementUpper || *measurementUpper > measurementBytesAvailable ||
      result.workUnits == 0u) {
    (void)result.family->reserveTextPathMeasurementWork(1u);
    result.workUnits = 0u;
    return result;
  }
  if (!result.family->reserveTextPathMeasurementWork(result.workUnits)) {
    result.workUnits = 0u;
    return result;
  }
  const size_t reservationBytes = *measurementUpper + *segmentEndBytes + mappingOverhead;
  result.reservation = TextPathMeasurementReservation::Create(result.family, reservationBytes);
  if (!result.reservation) {
    result.family->releaseUnusedTextPathMeasurementWork(result.workUnits);
    result.workUnits = 0u;
  }
  return result;
}

bool FinalizeTextPathMeasurementAdmission(TextPathMeasurementAdmission& admission,
                                          const Path::MeasuredPath& measuredPath,
                                          size_t mappingOverhead,
                                          std::optional<size_t>& measuredBytes) {
  if (!admission.reservation) {
    return false;
  }
  const size_t consumedWork = measuredPath.measurementWorkUnits();
  admission.family->releaseUnusedTextPathMeasurementWork(
      consumedWork < admission.workUnits ? admission.workUnits - consumedWork : 0u);
  if (!measuredPath.valid() && consumedWork >= admission.workUnits) {
    admission.family->rejectTextPathMeasurementWork();
  }
  measuredBytes = measuredPath.retainedBytes();
  if (!measuredPath.valid() || !measuredBytes ||
      measuredPath.segmentCount() >
          (std::numeric_limits<size_t>::max() - mappingOverhead) / sizeof(double)) {
    return false;
  }
  const size_t measuredMappingBytes =
      *measuredBytes + measuredPath.segmentCount() * sizeof(double) + mappingOverhead;
  admission.reservation->shrink(measuredMappingBytes);
  return true;
}

/// Resolves a per-character displacement, returning zero when it is absent.
double TextPathDisplacement(const SmallVector<std::optional<Lengthd>, 1>& positions,
                            unsigned int charIndex, const TextLayoutParams& params,
                            Lengthd::Extent extent) {
  return charIndex < positions.size() && positions[charIndex]
             ? positions[charIndex]->toPixels(params.viewBox, params.fontMetrics, extent)
             : 0.0;
}

/// Returns whether this glyph ends its run-local typographic cluster.
bool EndsTextPathCluster(const TextRun& run, size_t glyphIndex, unsigned int charIndex,
                         const ByteIndexMappings& mappings) {
  if (glyphIndex + 1 == run.glyphs.size()) {
    return true;
  }
  const size_t nextByte = GetPositioningCluster(run.glyphs[glyphIndex + 1]);
  return nextByte >= mappings.byteToCharIdx.size() || mappings.byteToCharIdx[nextByte] != charIndex;
}

/// Preserves glyph-local offsets while advancing one run's logical path clusters.
void StageTextPathRun(TextRun& run, size_t runIndex,
                      const components::ComputedTextComponent::TextSpan& span,
                      std::string_view shapingText, const ByteIndexMappings& mappings,
                      const TextLayoutParams& params, const TextBackend& backend, double defaultY,
                      const SmallVector<std::optional<Lengthd>, 1>& dxList,
                      const SmallVector<std::optional<Lengthd>, 1>& dyList, double boundaryKern,
                      TextPathStagingState& state) {
  state.advance += boundaryKern;
  const std::string_view spanText = shapingText;
  const bool vertical = isVertical(params.writingMode);
  for (size_t gi = 0; gi < run.glyphs.size(); ++gi) {
    TextGlyph& glyph = run.glyphs[gi];
    const uint32_t positioningCluster = GetPositioningCluster(glyph);
    const unsigned int charIndex = positioningCluster < mappings.byteToCharIdx.size()
                                       ? mappings.byteToCharIdx[positioningCluster]
                                       : 0;
    if (gi > 0) {
      state.advance += glyph.xKern;
    }
    if (gi == 0 || charIndex != state.clusters.back().charIndex) {
      state.advance += TextPathDisplacement(dxList, charIndex, params, Lengthd::Extent::X);
      state.dy += TextPathDisplacement(dyList, charIndex, params, Lengthd::Extent::Y);
      state.clusters.push_back(
          {runIndex, gi, gi + 1, charIndex, state.advance, defaultY + state.dy});
    }
    TextPathCluster& cluster = state.clusters.back();
    glyph.xPosition = state.advance - cluster.pathOffset + (vertical ? 0.0 : glyph.xPosition);
    glyph.yPosition = vertical ? 0.0 : glyph.yPosition;
    state.advance += glyph.xAdvance;
    cluster.glyphEnd = gi + 1;
    cluster.advance = state.advance - cluster.pathOffset;
    if (EndsTextPathCluster(run, gi, charIndex, mappings)) {
      size_t byteIndex = GetPositioningCluster(run.glyphs[cluster.glyphStart]);
      const uint32_t codepoint = decodeUtf8(spanText, byteIndex);
      if (!backend.isCursive(codepoint)) {
        state.advance += span.letterSpacingPx;
      }
      if (codepoint == 0x20) {
        state.advance += span.wordSpacingPx;
      }
    }
  }
}

/// Returns whether a cluster starts a new horizontal path chunk.
bool HasAbsoluteTextPathX(const components::ComputedTextComponent& text,
                          const TextPathCluster& cluster) {
  const auto& positions = text.spans[cluster.runIndex].xList;
  return cluster.charIndex < positions.size() && positions[cluster.charIndex].has_value();
}

/// Applies absolute path coordinates without discarding the same cluster's relative displacement.
void ResetTextPathCoordinates(const components::ComputedTextComponent& text,
                              const TextLayoutParams& params,
                              std::vector<TextPathCluster>& clusters) {
  double xShift = 0.0;
  for (TextPathCluster& cluster : clusters) {
    const auto& span = text.spans[cluster.runIndex];
    if (HasAbsoluteTextPathX(text, cluster)) {
      const double dx =
          TextPathDisplacement(span.dxList, cluster.charIndex, params, Lengthd::Extent::X);
      xShift = span.xList[cluster.charIndex]->toPixels(params.viewBox, params.fontMetrics,
                                                       Lengthd::Extent::X) +
               dx - cluster.pathOffset;
    }
    cluster.pathOffset += xShift;
  }
}

/// Anchors each horizontal path chunk against all of its typographic extents.
void AnchorTextPathClusters(const components::ComputedTextComponent& text,
                            std::vector<TextPathCluster>& clusters) {
  for (size_t first = 0; first < clusters.size();) {
    size_t end = first + 1;
    while (end < clusters.size() && !HasAbsoluteTextPathX(text, clusters[end])) {
      ++end;
    }
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    for (size_t ci = first; ci < end; ++ci) {
      const TextPathCluster& cluster = clusters[ci];
      minimum = std::min({minimum, cluster.pathOffset, cluster.pathOffset + cluster.advance});
      maximum = std::max({maximum, cluster.pathOffset, cluster.pathOffset + cluster.advance});
    }
    const TextAnchor anchor = text.spans[clusters[first].runIndex].textAnchor;
    const double alignment = anchor == TextAnchor::Middle ? (minimum + maximum) / 2.0
                             : anchor == TextAnchor::End  ? maximum
                                                          : minimum;
    const double shift = clusters[first].pathOffset - alignment;
    for (size_t ci = first; ci < end; ++ci) {
      clusters[ci].pathOffset += shift;
      clusters[ci].circuitAnchor = anchor;
    }
    first = end;
  }
}

/// Only one closed contour can be sampled as one equivalent circuit.
bool IsSingleClosedTextPath(const Path& source) {
  const auto commands = source.commands();
  if (commands.empty() || commands.size() > Path::kMaximumGeometryQueryWork ||
      commands.front().verb != Path::Verb::MoveTo ||
      commands.back().verb != Path::Verb::ClosePath) {
    return false;
  }
  size_t moves = 0;
  size_t closes = 0;
  for (const auto& command : commands) {
    moves += command.verb == Path::Verb::MoveTo;
    closes += command.verb == Path::Verb::ClosePath;
  }
  return moves == 1 && closes == 1;
}

bool IsStraightTextPath(const Path& source) {
  const auto commands = source.commands();
  return commands.size() == 2u && commands[0].verb == Path::Verb::MoveTo &&
         commands[1].verb == Path::Verb::LineTo;
}

/// Normalize finite distances without changing the open-path clipping policy.
double NormalizeTextPathDistance(double distance, double length) {
  const double normalized = std::fmod(distance, length);
  return normalized < 0.0 ? normalized + length : normalized;
}

/// Closed text is admitted for one circuit about its anchored start, not arbitrary repeats.
bool IsWithinClosedTextCircuit(double midpoint, double length, TextAnchor anchor) {
  const double start = anchor == TextAnchor::Middle ? -length * 0.5
                       : anchor == TextAnchor::End  ? -length
                                                    : 0.0;
  return midpoint >= start && midpoint <= start + length;
}

/// Sample a midpoint, wrapping only admitted finite single-contour positions.
Path::PointOnPath SampleTextPathCluster(const Path::MeasuredPath& path, double startOffset,
                                        double midpoint, bool closed, TextAnchor anchor) {
  const double length = path.pathLength();
  if (!closed) {
    return path.pointAtArcLength(startOffset + midpoint);
  }
  // This midpoint already excludes startOffset; authored x still counts toward the circuit.
  if (!std::isfinite(startOffset) || !std::isfinite(midpoint) ||
      !IsWithinClosedTextCircuit(midpoint, length, anchor)) {
    return {.valid = false};
  }
  const double start = NormalizeTextPathDistance(startOffset, length);
  const double offset = NormalizeTextPathDistance(midpoint, length);
  // Add two normalized distances without overflowing a large finite path length.
  const double distance = offset >= length - start ? offset - (length - start) : start + offset;
  return path.pointAtArcLength(distance);
}

/// Samples each cluster and places all of its glyphs with their shaped offsets.
void PlaceTextPathClusters(const Path::MeasuredPath& path, double startOffset, bool closed,
                           std::vector<TextRun>& runs, const std::vector<TextPathCluster>& clusters,
                           components::TextPathMethod method) {
  for (const TextPathCluster& cluster : clusters) {
    const double halfAdvance = cluster.advance * 0.5;
    const auto sample = SampleTextPathCluster(path, startOffset, cluster.pathOffset + halfAdvance,
                                              closed, cluster.circuitAnchor);
    auto& run = runs[cluster.runIndex];
    if (sample.valid) {
      const Vector2d origin(sample.point.x - halfAdvance * std::cos(sample.angle) -
                                std::sin(sample.angle) * cluster.baselineOffset,
                            sample.point.y - halfAdvance * std::sin(sample.angle) +
                                std::cos(sample.angle) * cluster.baselineOffset);
      for (size_t gi = cluster.glyphStart; gi < cluster.glyphEnd; ++gi) {
        TextGlyph& glyph = run.glyphs[gi];
        const double angle = sample.angle + glyph.rotateDegrees * MathConstants<double>::kDegToRad;
        const double localX = glyph.xPosition;
        const double localY = glyph.yPosition;
        if (method == components::TextPathMethod::Stretch) {
          // Turn the shaped offset by the authored rotate in path-local space, matching align's
          // placement; the warp then rotates only the outline about this origin.
          const double localRotate = glyph.rotateDegrees * MathConstants<double>::kDegToRad;
          const double pathLocalX = localX * std::cos(localRotate) - localY * std::sin(localRotate);
          const double pathLocalY = localX * std::sin(localRotate) + localY * std::cos(localRotate);
          glyph.textPathOrigin = cluster.pathOffset + pathLocalX;
          glyph.textPathNormalOffset = cluster.baselineOffset + pathLocalY;
          glyph.textPathLocalRotateDegrees = glyph.rotateDegrees;
        }
        glyph.xPosition = origin.x + localX * std::cos(angle) - localY * std::sin(angle);
        glyph.yPosition = origin.y + localX * std::sin(angle) + localY * std::cos(angle);
        glyph.rotateDegrees += sample.angle * MathConstants<double>::kRadToDeg;
      }
    } else {
      for (size_t gi = cluster.glyphStart; gi < cluster.glyphEnd; ++gi) {
        run.glyphs[gi].glyphIndex = 0;
      }
    }
  }
}

/// Places shaped clusters and returns geometry-based continuation independent of glyph visibility.
std::optional<Vector2d> TextPathContinuation(const Path::MeasuredPath& path, bool closed,
                                             double startOffset) {
  const double length = path.pathLength();
  if (closed && !std::isfinite(startOffset)) {
    return std::nullopt;
  }
  const double distance = closed ? NormalizeTextPathDistance(startOffset, length) : length;
  const auto end = path.pointAtArcLength(distance);
  return end.valid ? std::optional(end.point) : std::nullopt;
}

void placeTextPath(const components::ComputedTextComponent& text, const TextLayoutParams& params,
                   DeferredTextPath& path, std::vector<TextRun>& runs) {
  ResetTextPathCoordinates(text, params, path.clusters);
  AnchorTextPathClusters(text, path.clusters);
  const auto& firstSpan = text.spans[path.firstRun];
  PlaceTextPathClusters(path.measuredPath, firstSpan.pathStartOffset, path.closed, runs,
                        path.clusters, firstSpan.textPathMethod);
  if (firstSpan.textPathMethod == components::TextPathMethod::Stretch) {
    std::vector<double> segmentEnds = path.measuredPath.segmentEndDistances();
    auto mapping = std::make_shared<StretchedTextPath>(StretchedTextPath{
        std::move(path.measuredPath), std::move(segmentEnds), firstSpan.pathStartOffset,
        path.closed, IsStraightTextPath(*firstSpan.pathSpline),
        std::move(path.measurementReservation)});
    for (size_t runIndex = path.firstRun; runIndex < path.endRun; ++runIndex) {
      runs[runIndex].stretchedTextPath = mapping;
    }
  }
}

/// A retired owning path establishes an endpoint for the next unanchored flat character.
struct PostTextPathBoundary {
  size_t runIndex;
  std::optional<Vector2d> endpoint;
};

struct PostTextPathTranslation {
  std::optional<Vector2d> endpoint;
  std::optional<Vector2d> origin;

  void clear() {
    endpoint.reset();
    origin.reset();
  }
};

bool IsFiniteTextPosition(const Vector2d& position) {
  return std::isfinite(position.x) && std::isfinite(position.y);
}

/// A quarantined flat glyph must not enter logical metrics, ink, or object bounds.
bool HasFiniteFlatGlyphGeometry(const TextGlyph& glyph) {
  return std::isfinite(glyph.xPosition) && std::isfinite(glyph.yPosition) &&
         std::isfinite(glyph.xAdvance) && std::isfinite(glyph.yAdvance) &&
         std::isfinite(glyph.rotateDegrees) && std::isfinite(glyph.xPosition + glyph.xAdvance) &&
         std::isfinite(glyph.yPosition + glyph.yAdvance);
}

/// Keep subtraction local to the nominal origin, avoiding cancellation at the first character.
bool TranslatePostPathGlyph(TextGlyph& glyph, const PostTextPathTranslation& state) {
  const Vector2d position =
      *state.endpoint + (Vector2d(glyph.xPosition, glyph.yPosition) - *state.origin);
  if (!IsFiniteTextPosition(position)) {
    return false;
  }
  glyph.xPosition = position.x;
  glyph.yPosition = position.y;
  return HasFiniteFlatGlyphGeometry(glyph);
}

/// Absolute x or y belongs to a typographic positioning group, not a UTF-16 API alias.
bool HasAbsoluteFlatPosition(const components::ComputedTextComponent::TextSpan& span,
                             const ByteIndexMappings& mappings, const TextGlyph& glyph) {
  const size_t byte = GetPositioningCluster(glyph);
  if (byte >= mappings.byteToCharIdx.size()) {
    return false;
  }
  const size_t index = mappings.byteToCharIdx[byte];
  return (index < span.xList.size() && span.xList[index].has_value()) ||
         (index < span.yList.size() && span.yList[index].has_value());
}

/// Find the first anchored source group even when unpositioned shaping uses visual glyph order.
std::optional<uint32_t> FirstAnchoredFlatCluster(
    const TextRun& run, const components::ComputedTextComponent::TextSpan& span,
    const ByteIndexMappings& mappings) {
  std::optional<uint32_t> first;
  for (const auto& glyph : run.glyphs) {
    if (HasAbsoluteFlatPosition(span, mappings, glyph)) {
      const uint32_t cluster = GetPositioningCluster(glyph);
      first = first ? std::min(*first, cluster) : cluster;
    }
  }
  return first;
}

/// Preserve finite glyph order while translating and quarantining in existing storage.
void TranslateAndQuarantineFlatPrefix(TextRun& run, const PostTextPathTranslation& state,
                                      std::optional<uint32_t> anchored) {
  size_t retained = 0;
  for (size_t index = 0; index < run.glyphs.size(); ++index) {
    TextGlyph& glyph = run.glyphs[index];
    const bool anchoredSuffix = anchored && GetPositioningCluster(glyph) >= *anchored;
    const bool keep = anchoredSuffix ? HasFiniteFlatGlyphGeometry(glyph)
                                     : IsFiniteTextPosition(*state.origin) &&
                                           TranslatePostPathGlyph(glyph, state);
    if (keep) {
      if (retained != index) {
        run.glyphs[retained] = std::move(glyph);
      }
      ++retained;
    }
  }
  run.glyphs.resize(retained);
}

/// Translate an unanchored source prefix once; an anchored group retires both components.
void TranslateAfterTextPathRun(TextRun& run,
                               const components::ComputedTextComponent::TextSpan& span,
                               PostTextPathTranslation& state) {
  if (run.glyphs.empty() || !state.endpoint) {
    return;
  }
  const std::string_view spanText(span.text.data() + span.start, span.end - span.start);
  const auto mappings = buildByteIndexMappings(spanText);
  const auto anchored = FirstAnchoredFlatCluster(run, span, mappings);
  if (!state.origin) {
    const auto first = std::min_element(
        run.glyphs.begin(), run.glyphs.end(), [](const TextGlyph& a, const TextGlyph& b) {
          return GetPositioningCluster(a) < GetPositioningCluster(b);
        });
    state.origin = Vector2d(first->xPosition, first->yPosition);
  }
  TranslateAndQuarantineFlatPrefix(run, state, anchored);
  if (anchored) {
    state.clear();
  }
}

/// Walk retired owners and flat glyphs in order after ordinary length/anchor adjustments.
void ApplyPostTextPathTranslations(std::vector<TextRun>& runs,
                                   const components::ComputedTextComponent& text,
                                   const std::vector<PostTextPathBoundary>& boundaries) {
  PostTextPathTranslation state;
  size_t boundary = 0;
  for (size_t run = 0; run < runs.size() && run < text.spans.size(); ++run) {
    while (boundary < boundaries.size() && boundaries[boundary].runIndex == run) {
      state.endpoint = boundaries[boundary].endpoint;
      state.origin.reset();
      ++boundary;
    }
    if (runs[run].onPath) {
      state.clear();
    } else {
      TranslateAfterTextPathRun(runs[run], text.spans[run], state);
    }
  }
}

/// A glyph's pen position on the inline axis. An upright vertical glyph is drawn past its pen
/// position by its face's vertical origin.
double InlinePenPosition(const TextGlyph& glyph, bool vertical) {
  return vertical ? glyph.yPosition - glyph.verticalOriginOffset : glyph.xPosition;
}

/**
 * Moves a glyph's pen position to \p pen and scales its inline advance by \p scale. The stretch
 * of an upright vertical glyph along the inline axis also stretches its offset from the pen.
 */
void PlaceStretchedGlyph(TextGlyph& glyph, double pen, double scale, bool vertical) {
  if (vertical) {
    glyph.verticalOriginOffset *= scale;
    glyph.yPosition = pen + glyph.verticalOriginOffset;
    glyph.yAdvance *= scale;
  } else {
    glyph.xPosition = pen;
    glyph.xAdvance *= scale;
  }
}

/**
 * Stretches a glyph's outline by \p scale along the inline axis for
 * `lengthAdjust="spacingAndGlyphs"`. The outline is stretched before the glyph's rotation, so a
 * sideways glyph in vertical text stretches its own x axis.
 *
 * @param glyph Glyph to stretch.
 * @param scale Inline-axis scale factor.
 * @param vertical True for vertical writing modes.
 */
void StretchGlyphAlongInlineAxis(TextGlyph& glyph, double scale, bool vertical) {
  if (vertical && !glyph.sideways) {
    glyph.stretchScaleY *= NarrowToFloat(scale);
  } else {
    glyph.stretchScaleX *= NarrowToFloat(scale);
  }
}

struct OrdinaryLengthScope {
  struct Item {
    size_t index = 0;
    bool scope = false;
    bool path = false;
    bool absoluteReset = false;
    size_t group = 0;
    size_t glyphBegin = 0;
    size_t glyphEnd = 0;
    double start = 0;
    double end = 0;
    double glyphOrigin = 0;
    double position = 0;
    size_t spacingIndex = 0;
  };
  Entity owner = entt::null;
  std::optional<Lengthd> authoredTarget;
  std::shared_ptr<const components::ContextualLengthRecipe> recipe;
  LengthAdjust adjust = LengthAdjust::Default;
  FontMetrics metrics;
  std::vector<Item> items;
  double start = 0;
  double end = 0;
  double length = 0;
  double scale = 1;
  double spacing = 0;
  /// Advance that the text after this scope inherits past its end: a spacing scope's per-gap
  /// adjustment, plus the trailing advance of a child scope that ends this one. SVG 2's text-length
  /// loop accrues that adjustment after the last character but leaves following text undefined;
  /// Chrome shifts it by the accrued amount, and the resvg references agree.
  double trailingAdvance = 0;
  double position = 0;
  bool absoluteReset = false;
};

/// Advance that the text after \p item inherits past the item's end. Only a child scope has one.
double TrailingAdvanceOf(const std::vector<OrdinaryLengthScope>& scopes,
                         const OrdinaryLengthScope::Item& item) {
  return item.scope ? scopes[item.index].trailingAdvance : 0.0;
}

template <typename Declaration>
bool HasNegativeAbsoluteTextLength(const Declaration& declaration) {
  const auto negative = [](const std::optional<std::string>& value) {
    if (!value) {
      return false;
    }
    const auto parsed = components::ParseAnimationLength("textLength", *value);
    return parsed && parsed->value < 0;
  };
  if constexpr (std::is_same_v<Declaration, components::SetAnimationComponent>) {
    return negative(declaration.to);
  } else {
    if (!declaration.values.empty()) {
      return !declaration.additive &&
             std::any_of(declaration.values.begin(), declaration.values.end(),
                         [&](const std::string& value) {
                           return negative(std::optional<std::string>(value));
                         });
    }
    const bool toOnly = declaration.values.empty() && !declaration.from && declaration.to;
    if (!declaration.additive && (negative(declaration.from) || negative(declaration.to))) {
      return true;
    }
    return toOnly && negative(declaration.to);
  }
}

std::optional<Lengthd> SampleValidTextLengthRecipe(
    const components::ContextualLengthRecipe& recipe, Lengthd underlying,
    const components::AnimationValueContext& context) {
  return components::SampleContextualAnimationRecipe(
      recipe, underlying, [&](const auto& sample, const Lengthd& lower) {
        const auto sampled = std::visit(
            [&](const auto& declaration) -> std::optional<std::pair<std::string, std::string>> {
              if (HasNegativeAbsoluteTextLength(declaration)) {
                return std::nullopt;
              }
              using Declaration = std::decay_t<decltype(declaration)>;
              if constexpr (std::is_same_v<Declaration, components::SetAnimationComponent>) {
                return components::SampleAnimationValue(sample.progress, sample.iteration,
                                                        &declaration, nullptr, nullptr,
                                                        lower.toRcString().str(), context);
              } else {
                return components::SampleAnimationValue(sample.progress, sample.iteration, nullptr,
                                                        &declaration, nullptr,
                                                        lower.toRcString().str(), context);
              }
            },
            sample.declaration);
        if (!sampled) {
          return std::optional<Lengthd>{};
        }
        const auto parsed = components::ParseAnimationLength(sampled->first, sampled->second);
        return parsed && parsed->value >= 0 ? parsed : std::nullopt;
      });
}

bool ApplyTextLengthOwnerPlan(Registry& registry, std::vector<TextRun>& runs,
                              const components::ComputedTextComponent& text,
                              const std::vector<RunPenExtent>& runExtents,
                              const TextLayoutParams& params, bool vertical,
                              std::vector<DeferredTextPath>& paths) {
  struct PathClusterRange {
    size_t group = 0;
    size_t begin = 0;
    size_t end = 0;
  };

  std::vector<std::optional<PathClusterRange>> pathClustersByRun(runs.size());
  for (size_t group = 0; group < paths.size(); ++group) {
    const auto& clusters = paths[group].clusters;
    for (size_t begin = 0; begin < clusters.size();) {
      const size_t runIndex = clusters[begin].runIndex;
      size_t end = begin + 1;
      while (end < clusters.size() && clusters[end].runIndex == runIndex) {
        ++end;
      }
      if (runIndex < pathClustersByRun.size()) {
        pathClustersByRun[runIndex] = PathClusterRange{group, begin, end};
      }
      begin = end;
    }
  }

  std::vector<OrdinaryLengthScope> scopes(1);
  std::unordered_map<Entity, size_t> ownerScopes;
  bool hasManagedScope = false;
  size_t structuralWork = 0;
  bool hasLogicalEnd = false;
  double logicalEnd = 0;
  std::optional<size_t> previousPathGroup;
  double previousPathLocalEnd = 0;

  const auto initializeScope = [&](OrdinaryLengthScope& scope, Entity owner,
                                   const components::ComputedTextComponent::TextSpan& span) {
    scope.owner = owner;
    scope.metrics = params.fontMetrics;
    std::optional<Lengthd> ownerFontSize;
    if (const auto occurrence = text.textLengthOwnerFontSizes.find(owner);
        occurrence != text.textLengthOwnerFontSizes.end()) {
      ownerFontSize = occurrence->second;
    } else if (span.sourceEntity == owner && span.hasResolvedFontSize) {
      ownerFontSize = span.fontSize;
    } else if (const auto* style = registry.try_get<components::ComputedStyleComponent>(owner);
               style && style->properties) {
      ownerFontSize = style->properties->fontSize.get().value();
    }
    if (ownerFontSize) {
      const double size =
          ownerFontSize->toPixels(params.viewBox, params.fontMetrics, Lengthd::Extent::Y);
      if (std::isfinite(size) && size >= 0) {
        scope.metrics.fontSize = size;
      }
    }
    if (const auto* component = registry.try_get<components::TextComponent>(owner)) {
      scope.authoredTarget = component->textLength;
      scope.adjust = component->lengthAdjust;
    }
    scope.recipe = components::ContextualLengthFor(registry, owner, "textLength");
    hasManagedScope |= scope.authoredTarget.has_value() || scope.recipe != nullptr;
  };

  for (size_t runIndex = 0; runIndex < runs.size() && runIndex < text.spans.size(); ++runIndex) {
    const auto& span = text.spans[runIndex];
    size_t owner = 0;
    if (span.sourceEntity == entt::null) {
      if (span.textLength) {
        OrdinaryLengthScope candidate;
        candidate.authoredTarget = span.textLength;
        candidate.adjust = span.lengthAdjust;
        candidate.metrics = params.fontMetrics;
        const size_t child = scopes.size();
        scopes[owner].items.push_back({.index = child, .scope = true});
        scopes.push_back(std::move(candidate));
        owner = child;
        hasManagedScope = true;
      }
    } else {
      Entity current = span.sourceEntity;
      std::vector<Entity> ancestors;
      while (current != entt::null && registry.valid(current) && !ownerScopes.contains(current)) {
        ancestors.push_back(current);
        if (structuralWork == std::numeric_limits<size_t>::max()) {
          return false;
        }
        ++structuralWork;
        const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
        current = tree ? tree->parent() : entt::null;
      }

      owner = current != entt::null && ownerScopes.contains(current) ? ownerScopes[current] : 0;
      for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        OrdinaryLengthScope candidate;
        initializeScope(candidate, *it, span);
        if (candidate.authoredTarget || candidate.recipe) {
          const size_t child = scopes.size();
          scopes[owner].items.push_back({.index = child, .scope = true});
          scopes.push_back(std::move(candidate));
          owner = child;
        }
        ownerScopes.emplace(*it, owner);
      }
    }

    if (runs[runIndex].glyphs.empty()) {
      continue;
    }
    if (const auto& range = pathClustersByRun[runIndex]) {
      for (size_t clusterIndex = range->begin; clusterIndex < range->end; ++clusterIndex) {
        const auto& cluster = paths[range->group].clusters[clusterIndex];
        double start = hasLogicalEnd ? logicalEnd : cluster.pathOffset;
        if (previousPathGroup && *previousPathGroup == range->group) {
          start += cluster.pathOffset - previousPathLocalEnd;
        }
        const double end = start + cluster.advance;
        scopes[owner].items.push_back({.index = clusterIndex,
                                       .path = true,
                                       .group = range->group,
                                       .start = start,
                                       .end = end});
        logicalEnd = end;
        hasLogicalEnd = true;
        previousPathGroup = range->group;
        previousPathLocalEnd = cluster.pathOffset + cluster.advance;
      }
    } else {
      const auto& extent = runExtents[runIndex];
      const auto& inlinePositions = vertical ? span.yList : span.xList;
      const std::string_view spanText(span.text.data() + span.start, span.end - span.start);
      const auto mappings = buildByteIndexMappings(spanText);
      std::vector<size_t> boundaries{0};
      for (size_t glyphIndex = 1; glyphIndex < runs[runIndex].glyphs.size(); ++glyphIndex) {
        const size_t byte = GetPositioningCluster(runs[runIndex].glyphs[glyphIndex]);
        const size_t character =
            byte < mappings.byteToCharIdx.size() ? mappings.byteToCharIdx[byte] : 0;
        const size_t previousByte = GetPositioningCluster(runs[runIndex].glyphs[glyphIndex - 1]);
        const size_t previousCharacter =
            previousByte < mappings.byteToCharIdx.size() ? mappings.byteToCharIdx[previousByte] : 0;
        if (character != 0 && character != previousCharacter &&
            character < inlinePositions.size() && inlinePositions[character]) {
          boundaries.push_back(glyphIndex);
        }
      }
      boundaries.push_back(runs[runIndex].glyphs.size());
      for (size_t segment = 0; segment + 1 < boundaries.size(); ++segment) {
        const size_t glyphBegin = boundaries[segment];
        const size_t glyphEnd = boundaries[segment + 1];
        const auto& firstGlyph = runs[runIndex].glyphs[glyphBegin];
        const auto& lastGlyph = runs[runIndex].glyphs[glyphEnd - 1];
        const double start = glyphBegin == 0 ? (vertical ? extent.startY : extent.startX)
                                             : InlinePenPosition(firstGlyph, vertical);
        const double end = glyphEnd == runs[runIndex].glyphs.size()
                               ? (vertical ? extent.endY : extent.endX)
                               : InlinePenPosition(lastGlyph, vertical) +
                                     (vertical ? lastGlyph.yAdvance : lastGlyph.xAdvance);
        const bool absoluteReset =
            segment > 0 || (!inlinePositions.empty() && inlinePositions.front());
        scopes[owner].items.push_back({.index = runIndex,
                                       .absoluteReset = absoluteReset,
                                       .glyphBegin = glyphBegin,
                                       .glyphEnd = glyphEnd,
                                       .start = start,
                                       .end = end,
                                       .glyphOrigin = InlinePenPosition(firstGlyph, vertical)});
        logicalEnd = end;
      }
      hasLogicalEnd = true;
      previousPathGroup.reset();
    }
  }

  if (!hasManagedScope) {
    return false;
  }

  auto* budget = registry.ctx().find<components::AnimationResourceBudget>();
  size_t evaluationWork = structuralWork;
  bool admitted = budget != nullptr;
  for (const auto& scope : scopes) {
    if (scope.recipe &&
        scope.recipe->evaluationWork > std::numeric_limits<size_t>::max() - evaluationWork) {
      admitted = false;
      break;
    }
    if (scope.recipe) {
      evaluationWork += scope.recipe->evaluationWork;
    }
  }
  admitted = admitted && budget->reserveContextualWork(evaluationWork);

  const auto plan = [&](bool sampleRecipes) {
    for (size_t scopeIndex = scopes.size(); scopeIndex-- > 0;) {
      auto& scope = scopes[scopeIndex];
      std::erase_if(scope.items, [&](const OrdinaryLengthScope::Item& item) {
        return item.scope && scopes[item.index].items.empty();
      });
      scope.start = 0;
      scope.end = 0;
      scope.length = 0;
      scope.scale = 1;
      scope.spacing = 0;
      scope.trailingAdvance = 0;
      scope.absoluteReset = false;
      if (scope.items.empty()) {
        continue;
      }

      double fixedLength = 0;
      double cursor = 0;
      double previousEnd = 0;
      double previousTrailing = 0;
      size_t spacingUnits = 0;
      for (size_t itemIndex = 0; itemIndex < scope.items.size(); ++itemIndex) {
        auto& item = scope.items[itemIndex];
        if (item.scope) {
          item.start = scopes[item.index].start;
          item.end = scopes[item.index].end;
          item.absoluteReset = scopes[item.index].absoluteReset;
        }
        if (itemIndex == 0) {
          scope.start = item.start;
          scope.absoluteReset = item.absoluteReset;
        } else if (item.absoluteReset) {
          cursor = item.start - scope.start;
        } else {
          cursor += item.start - previousEnd + previousTrailing;
          fixedLength += previousTrailing;
        }
        item.position = cursor;
        item.spacingIndex = spacingUnits;
        const double length = item.scope ? scopes[item.index].length : item.end - item.start;
        cursor += length;
        fixedLength += item.scope ? length : 0;
        spacingUnits += item.scope || item.path ? 1 : item.glyphEnd - item.glyphBegin;
        previousEnd = item.end;
        previousTrailing = TrailingAdvanceOf(scopes, item);
      }
      scope.end = previousEnd;
      scope.length = cursor;

      std::optional<Lengthd> target = scope.authoredTarget;
      if (sampleRecipes && scope.recipe) {
        components::AnimationValueContext context;
        context.viewBox = params.viewBox;
        context.fontMetrics = scope.metrics;
        context.extent = vertical ? Lengthd::Extent::Y : Lengthd::Extent::X;
        const Lengthd underlying = target.value_or(Lengthd(scope.length));
        const auto sampled = SampleValidTextLengthRecipe(*scope.recipe, underlying, context);
        if (sampled) {
          const double pixels = sampled->toPixels(params.viewBox, scope.metrics, context.extent);
          if (std::isfinite(pixels) && pixels >= 0) {
            target = sampled;
          }
        }
      }

      if (target && cursor > 0) {
        const double pixels = target->toPixels(params.viewBox, scope.metrics,
                                               vertical ? Lengthd::Extent::Y : Lengthd::Extent::X);
        const double adjustableLength = cursor - fixedLength;
        if (std::isfinite(pixels) && pixels >= 0 && scope.adjust == LengthAdjust::Spacing &&
            spacingUnits > 1) {
          scope.spacing = (pixels - cursor) / static_cast<double>(spacingUnits - 1);
        } else if (std::isfinite(pixels) && pixels >= 0 &&
                   scope.adjust == LengthAdjust::SpacingAndGlyphs && adjustableLength > 0) {
          const double scale = std::max(0.0, pixels - fixedLength) / adjustableLength;
          if (std::isfinite(scale) && scale <= std::numeric_limits<float>::max()) {
            scope.scale = scale;
          }
        }
      }

      cursor = 0;
      previousEnd = scope.start;
      previousTrailing = 0;
      for (auto& item : scope.items) {
        if (item.absoluteReset) {
          cursor = item.start - scope.start;
        } else {
          cursor += (item.start - previousEnd) * scope.scale + previousTrailing;
        }
        item.position = cursor;
        const double length = item.scope ? scopes[item.index].length : item.end - item.start;
        cursor += item.scope ? length : length * scope.scale;
        if (!item.scope && !item.path && item.glyphEnd - item.glyphBegin > 1) {
          cursor += scope.spacing * static_cast<double>(item.glyphEnd - item.glyphBegin - 1);
        }
        cursor += scope.spacing;
        previousEnd = item.end;
        previousTrailing = TrailingAdvanceOf(scopes, item);
      }
      scope.length = cursor - scope.spacing;
      scope.trailingAdvance = scope.spacing + previousTrailing;
    }
  };

  plan(admitted);

  scopes[0].position = scopes[0].start;
  struct PathOutputOrigin {
    double output = 0;
    double localOffset = 0;
  };
  std::vector<std::vector<std::optional<double>>> pathOutputPositions;
  pathOutputPositions.reserve(paths.size());
  for (const auto& path : paths) {
    pathOutputPositions.emplace_back(path.clusters.size());
  }
  for (auto& scope : scopes) {
    for (const auto& item : scope.items) {
      const double position = scope.position + item.position;
      if (item.scope) {
        scopes[item.index].position = position;
        continue;
      }
      if (item.path) {
        pathOutputPositions[item.group][item.index] = position;
        continue;
      }

      auto& run = runs[item.index];
      const double glyphBase = position + item.glyphOrigin - item.start;
      for (size_t glyphIndex = item.glyphBegin; glyphIndex < item.glyphEnd; ++glyphIndex) {
        auto& glyph = run.glyphs[glyphIndex];
        const double pen = glyphBase +
                           (InlinePenPosition(glyph, vertical) - item.glyphOrigin) * scope.scale +
                           scope.spacing * static_cast<double>(glyphIndex - item.glyphBegin);
        PlaceStretchedGlyph(glyph, pen, scope.scale, vertical);
        StretchGlyphAlongInlineAxis(glyph, scope.scale, vertical);
      }
    }
  }

  std::vector<std::optional<PathOutputOrigin>> pathOrigins(paths.size());
  for (size_t group = 0; group < paths.size(); ++group) {
    if (!paths[group].clusters.empty() && pathOutputPositions[group][0]) {
      pathOrigins[group] =
          PathOutputOrigin{*pathOutputPositions[group][0], paths[group].clusters[0].pathOffset};
    }
  }
  for (const auto& scope : scopes) {
    for (const auto& item : scope.items) {
      if (!item.path || !pathOrigins[item.group] || !pathOutputPositions[item.group][item.index]) {
        continue;
      }
      auto& cluster = paths[item.group].clusters[item.index];
      const auto& origin = *pathOrigins[item.group];
      cluster.pathOffset =
          origin.localOffset + *pathOutputPositions[item.group][item.index] - origin.output;
      cluster.advance *= scope.scale;
      auto& run = runs[cluster.runIndex];
      for (size_t glyphIndex = cluster.glyphStart; glyphIndex < cluster.glyphEnd; ++glyphIndex) {
        auto& glyph = run.glyphs[glyphIndex];
        glyph.xPosition *= scope.scale;
        glyph.xAdvance *= scope.scale;
        glyph.stretchScaleX *= NarrowToFloat(scope.scale);
      }
    }
  }
  return true;
}

void applyTextLength(std::vector<TextRun>& runs, const components::ComputedTextComponent& text,
                     const std::vector<RunPenExtent>& runExtents, const TextLayoutParams& params,
                     bool vertical, double currentPenX, double currentPenY,
                     TextLengthTraversalStats* traversalStats) {
  // Check if any span has per-span textLength.
  bool anySpanHasTextLength = false;
  for (const auto& span : text.spans) {
    if (span.textLength.has_value()) {
      anySpanHasTextLength = true;
      break;
    }
  }

  // ── Per-span textLength ───────────────────────────────────────────────
  if (anySpanHasTextLength) {
    double carriedAdvanceDelta = 0.0;
    bool carryActive = false;

    for (size_t i = 0; i < runs.size() && i < text.spans.size(); ++i) {
      if (traversalStats) {
        ++traversalStats->runVisits;
      }
      auto& run = runs[i];
      const auto& span = text.spans[i];

      if (run.onPath) {
        carryActive = false;
        carriedAdvanceDelta = 0.0;
        if (run.textLengthAppliedOnPath) {
          continue;
        }
      } else if (carryActive) {
        const auto& inlinePositions = vertical ? span.yList : span.xList;
        std::optional<size_t> resetCharIndex;
        for (size_t charIndex = 0; charIndex < inlinePositions.size(); ++charIndex) {
          if (traversalStats) {
            ++traversalStats->inlinePositionVisits;
          }
          if (inlinePositions[charIndex].has_value()) {
            resetCharIndex = charIndex;
            break;
          }
        }

        std::optional<ByteIndexMappings> mappings;
        if (resetCharIndex.has_value() && !run.glyphs.empty()) {
          const std::string_view spanText(span.text.data() + span.start, span.end - span.start);
          if (traversalStats) {
            traversalStats->mappedTextBytes += spanText.size();
          }
          mappings = buildByteIndexMappings(spanText);
          std::vector<bool> groupStarts(inlinePositions.size(), false);
          for (const auto& glyph : run.glyphs) {
            if (traversalStats) {
              ++traversalStats->glyphVisits;
            }
            const uint32_t positioningCluster = GetPositioningCluster(glyph);
            if (positioningCluster < mappings->byteToCharIdx.size()) {
              const size_t index = mappings->byteToCharIdx[positioningCluster];
              if (index < groupStarts.size()) {
                groupStarts[index] = true;
              }
            }
          }
          resetCharIndex.reset();
          for (size_t index = 0; index < inlinePositions.size(); ++index) {
            if (traversalStats) {
              ++traversalStats->inlinePositionVisits;
            }
            if (groupStarts[index] && inlinePositions[index]) {
              resetCharIndex = index;
              break;
            }
          }
        }

        for (auto& glyph : run.glyphs) {
          if (traversalStats) {
            ++traversalStats->glyphVisits;
          }
          if (resetCharIndex.has_value()) {
            const uint32_t positioningCluster = GetPositioningCluster(glyph);
            const size_t charIndex = positioningCluster < mappings->byteToCharIdx.size()
                                         ? mappings->byteToCharIdx[positioningCluster]
                                         : 0;
            if (charIndex >= *resetCharIndex) {
              continue;
            }
          }

          if (vertical) {
            glyph.yPosition += carriedAdvanceDelta;
          } else {
            glyph.xPosition += carriedAdvanceDelta;
          }
        }

        if (resetCharIndex.has_value()) {
          carryActive = false;
          carriedAdvanceDelta = 0.0;
        }
      }

      if (!span.textLength.has_value() || run.glyphs.empty()) {
        continue;
      }

      const double runStartPos = InlinePenPosition(run.glyphs[0], vertical);
      const auto& extent = runExtents[i];
      double naturalLength =
          vertical ? (extent.endY - extent.startY) : (extent.endX - extent.startX);

      if (naturalLength <= 0.0) {
        continue;
      }

      const double targetLength = span.textLength->toPixels(
          params.viewBox, params.fontMetrics, vertical ? Lengthd::Extent::Y : Lengthd::Extent::X);

      if (targetLength < 0.0) {
        continue;
      }

      if (span.lengthAdjust == LengthAdjust::Spacing) {
        const size_t numGaps = run.glyphs.size() > 1 ? run.glyphs.size() - 1 : 1;
        const double extraPerGap = (targetLength - naturalLength) / static_cast<double>(numGaps);
        for (size_t gi = 0; gi < run.glyphs.size(); ++gi) {
          if (traversalStats) {
            ++traversalStats->glyphVisits;
          }
          if (vertical) {
            run.glyphs[gi].yPosition += extraPerGap * static_cast<double>(gi);
          } else {
            run.glyphs[gi].xPosition += extraPerGap * static_cast<double>(gi);
          }
        }
      } else {
        const double scaleFactor = targetLength / naturalLength;
        for (auto& g : run.glyphs) {
          if (traversalStats) {
            ++traversalStats->glyphVisits;
          }
          PlaceStretchedGlyph(
              g, runStartPos + (InlinePenPosition(g, vertical) - runStartPos) * scaleFactor,
              scaleFactor, vertical);
          StretchGlyphAlongInlineAxis(g, scaleFactor, vertical);
        }
      }

      if (!run.onPath) {
        const double advanceDelta = targetLength - naturalLength;
        if (carryActive) {
          carriedAdvanceDelta += advanceDelta;
        } else {
          carriedAdvanceDelta = advanceDelta;
        }
        carryActive = true;
      }
    }
  }

  // ── Global textLength ────────────���────────────────────────────────────
  double globalStartX = currentPenX;
  double globalStartY = currentPenY;
  for (const auto& r : runs) {
    if (!r.glyphs.empty()) {
      globalStartX = r.glyphs[0].xPosition;
      globalStartY = InlinePenPosition(r.glyphs[0], true);
      break;
    }
  }

  const double globalNaturalLength =
      vertical ? (currentPenY - globalStartY) : (currentPenX - globalStartX);

  if (!anySpanHasTextLength && params.textLength.has_value() && globalNaturalLength > 0.0) {
    const double targetLength = params.textLength->toPixels(
        params.viewBox, params.fontMetrics, vertical ? Lengthd::Extent::Y : Lengthd::Extent::X);

    if (targetLength >= 0.0) {
      size_t totalGlyphs = 0;
      for (const auto& r : runs) {
        totalGlyphs += r.glyphs.size();
      }

      if (params.lengthAdjust == LengthAdjust::Spacing) {
        const size_t numGaps = totalGlyphs > 1 ? totalGlyphs - 1 : 1;
        const double extraPerGap =
            (targetLength - globalNaturalLength) / static_cast<double>(numGaps);
        size_t glyphIdx = 0;
        for (auto& r : runs) {
          for (auto& g : r.glyphs) {
            if (vertical) {
              g.yPosition += extraPerGap * static_cast<double>(glyphIdx);
            } else {
              g.xPosition += extraPerGap * static_cast<double>(glyphIdx);
            }
            ++glyphIdx;
          }
        }
      } else {
        const double scaleFactor = targetLength / globalNaturalLength;
        for (auto& r : runs) {
          for (auto& g : r.glyphs) {
            const double start = vertical ? globalStartY : globalStartX;
            PlaceStretchedGlyph(g, start + (InlinePenPosition(g, vertical) - start) * scaleFactor,
                                scaleFactor, vertical);
            StretchGlyphAlongInlineAxis(g, scaleFactor, vertical);
          }
        }
      }
    }
  }
}

/// Anchoring uses the full pen extent, including backwards displacements and advances.
struct TextAnchorBounds {
  double initial = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;
  bool initialized = false;

  void startAt(double position) {
    initial = minimum = maximum = position;
    initialized = true;
  }

  void include(double position, double advance) {
    if (!initialized) {
      startAt(position);
    }
    minimum = std::min({minimum, position, position + advance});
    maximum = std::max({maximum, position, position + advance});
  }

  double shift(TextAnchor anchor) const {
    const double alignment = anchor == TextAnchor::Middle ? minimum / 2.0 + maximum / 2.0
                             : anchor == TextAnchor::End  ? maximum
                                                          : minimum;
    return initial - alignment;
  }
};

struct TextAnchorRange {
  ChunkBoundary begin;
  ChunkBoundary end;
};

std::pair<size_t, size_t> AnchorRunRange(const TextAnchorRange& range, size_t runIndex,
                                         size_t size) {
  return {std::min(runIndex == range.begin.runIndex ? range.begin.glyphIndex : 0, size),
          std::min(runIndex == range.end.runIndex ? range.end.glyphIndex : size, size)};
}

template <typename Fn>
void ForEachAnchorGlyph(std::vector<TextRun>& runs, const TextAnchorRange& range, Fn&& fn) {
  if (runs.empty() || range.begin.runIndex >= runs.size()) {
    return;
  }
  for (size_t ri = range.begin.runIndex; ri <= std::min(range.end.runIndex, runs.size() - 1);
       ++ri) {
    if (runs[ri].onPath) {
      continue;
    }
    const auto [begin, end] = AnchorRunRange(range, ri, runs[ri].glyphs.size());
    for (size_t gi = begin; gi < end; ++gi) {
      fn(runs[ri].glyphs[gi], ri);
    }
  }
}

void MarkRejectedAnchorRange(const std::vector<TextRun>& runs, const TextAnchorRange& range,
                             std::vector<std::vector<bool>>& rejected) {
  if (rejected.empty()) {
    rejected.resize(runs.size());
  }
  if (runs.empty() || range.begin.runIndex >= runs.size()) {
    return;
  }
  for (size_t ri = range.begin.runIndex; ri <= std::min(range.end.runIndex, runs.size() - 1);
       ++ri) {
    if (runs[ri].onPath) {
      continue;
    }
    const auto [begin, end] = AnchorRunRange(range, ri, runs[ri].glyphs.size());
    if (rejected[ri].empty()) {
      rejected[ri].resize(runs[ri].glyphs.size(), false);
    }
    std::fill(rejected[ri].begin() + begin, rejected[ri].begin() + end, true);
  }
}

void CompactRejectedAnchorGlyphs(std::vector<TextRun>& runs,
                                 const std::vector<std::vector<bool>>& rejected) {
  for (size_t ri = 0; ri < rejected.size(); ++ri) {
    if (rejected[ri].empty()) {
      continue;
    }
    auto& glyphs = runs[ri].glyphs;
    size_t retained = 0;
    for (size_t gi = 0; gi < glyphs.size(); ++gi) {
      if (!rejected[ri][gi]) {
        if (retained != gi) {
          glyphs[retained] = std::move(glyphs[gi]);
        }
        ++retained;
      }
    }
    glyphs.resize(retained);
  }
}

void ShiftAnchorGlyph(TextGlyph& glyph, double shift, bool vertical) {
  if (vertical) {
    glyph.yPosition += shift;
  } else {
    glyph.xPosition += shift;
  }
}

bool IsExcludedAnchorGlyph(const AnchorExclusions& exclusions, size_t runIndex,
                           const TextGlyph& glyph) {
  const size_t cluster = GetPositioningCluster(glyph);
  return runIndex < exclusions.size() && cluster < exclusions[runIndex].size() &&
         exclusions[runIndex][cluster];
}

bool AnchorTextRange(std::vector<TextRun>& runs, const TextAnchorRange& range, bool vertical,
                     const AnchorExclusions& exclusions) {
  TextAnchorBounds bounds;
  bool finite = true;
  ForEachAnchorGlyph(runs, range, [&](const TextGlyph& glyph, size_t runIndex) {
    finite &= HasFiniteFlatGlyphGeometry(glyph);
    if (!IsExcludedAnchorGlyph(exclusions, runIndex, glyph)) {
      bounds.include(InlinePenPosition(glyph, vertical),
                     vertical ? glyph.yAdvance : glyph.xAdvance);
    }
  });
  if (!bounds.initialized) {
    return finite;
  }
  const double shift = bounds.shift(range.begin.textAnchor);
  finite &= std::isfinite(shift);
  ForEachAnchorGlyph(runs, range, [&](const TextGlyph& glyph, size_t) {
    auto translated = glyph;
    ShiftAnchorGlyph(translated, shift, vertical);
    finite &= HasFiniteFlatGlyphGeometry(translated);
  });
  if (!finite) {
    return false;
  }
  ForEachAnchorGlyph(runs, range,
                     [&](TextGlyph& glyph, size_t) { ShiftAnchorGlyph(glyph, shift, vertical); });
  return true;
}

/// Retain original chunk indices until all groups are validated, then compact each run once.
void applyTextAnchor(std::vector<TextRun>& runs, std::vector<ChunkBoundary>& chunkBoundaries,
                     const components::ComputedTextComponent& text, bool vertical,
                     const AnchorExclusions& exclusions) {
  for (auto& chunk : chunkBoundaries) {
    for (size_t ri = chunk.runIndex; ri < runs.size(); ++ri) {
      const size_t begin = ri == chunk.runIndex ? chunk.glyphIndex : 0;
      if (begin < runs[ri].glyphs.size()) {
        chunk.textAnchor = text.spans[ri].textAnchor;
        break;
      }
    }
  }
  std::vector<std::vector<bool>> rejected;
  for (size_t ci = 0; ci < chunkBoundaries.size(); ++ci) {
    const ChunkBoundary end =
        ci + 1 < chunkBoundaries.size() ? chunkBoundaries[ci + 1] : ChunkBoundary{runs.size(), 0};
    const TextAnchorRange range{chunkBoundaries[ci], end};
    if (!AnchorTextRange(runs, range, vertical, exclusions)) {
      MarkRejectedAnchorRange(runs, range, rejected);
    }
  }
  CompactRejectedAnchorGlyphs(runs, rejected);
}

/// Compute per-span baseline-shift in pixels, including OS/2 sub/super metrics
/// and ancestor baseline-shift accumulation.
double computeSpanBaselineShiftPx(const TextBackend& backend,
                                  const components::ComputedTextComponent::TextSpan& span,
                                  FontHandle spanFont, float spanScale,
                                  const TextLayoutParams& params) {
  using BSK = components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;

  FontMetrics spanFontMetrics = params.fontMetrics;
  const float spanFontSizePx = CheckedFontSizePx(ResolvedSpanFontSizePx(span, params));
  spanFontMetrics.fontSize = spanFontSizePx;

  double spanBaselineShiftPx;
  if (span.baselineShiftKeyword == BSK::Sub || span.baselineShiftKeyword == BSK::Super) {
    const auto subSuper = backend.subSuperMetrics(spanFont);
    if (subSuper.has_value()) {
      if (span.baselineShiftKeyword == BSK::Sub) {
        spanBaselineShiftPx = -static_cast<double>(subSuper->subscriptYOffset) * spanScale;
      } else {
        spanBaselineShiftPx = static_cast<double>(subSuper->superscriptYOffset) * spanScale;
      }
    } else {
      spanBaselineShiftPx =
          span.baselineShift.toPixels(params.viewBox, spanFontMetrics, Lengthd::Extent::Y);
    }
  } else {
    spanBaselineShiftPx =
        span.baselineShift.toPixels(params.viewBox, spanFontMetrics, Lengthd::Extent::Y);
  }

  // Resolve ancestor baseline-shifts.
  {
    const auto subSuper = backend.subSuperMetrics(spanFont);
    for (const auto& ancestor : span.ancestorBaselineShifts) {
      if (subSuper.has_value() && ancestor.keyword == BSK::Sub) {
        const float ancestorScale =
            backend.scaleForEmToPixels(spanFont, CheckedFontSizePx(ancestor.fontSizePx));
        spanBaselineShiftPx += -static_cast<double>(subSuper->subscriptYOffset) * ancestorScale;
      } else if (subSuper.has_value() && ancestor.keyword == BSK::Super) {
        const float ancestorScale =
            backend.scaleForEmToPixels(spanFont, CheckedFontSizePx(ancestor.fontSizePx));
        spanBaselineShiftPx += static_cast<double>(subSuper->superscriptYOffset) * ancestorScale;
      } else {
        FontMetrics ancestorFm = params.fontMetrics;
        ancestorFm.fontSize = ancestor.fontSizePx;
        spanBaselineShiftPx +=
            ancestor.shift.toPixels(params.viewBox, ancestorFm, Lengthd::Extent::Y);
      }
    }
  }

  return spanBaselineShiftPx;
}

struct WrappingGlyph {
  size_t run;
  size_t glyph;
  double originalX;
  double originalY;
  double originalAdvance;
  double advance;
  uint32_t codepoint;
  WhiteSpace mode;
  WhiteSpace breakMode;
  double halfCh;
  TextAnchor textAnchor;
  bool resetsInline;
  bool resetsBlock;
  size_t chunk = 0;
  size_t anchorGroup = 0;
  size_t block = 0;
  size_t line = 0;
  size_t lineOffset = 0;
  double origin = 0;
  double inlineDisplacement = 0;

  double x = 0;

  bool isSpace() const { return codepoint == ' ' || codepoint == '\t' || codepoint == 0x3000; }
  bool isBreak() const { return codepoint == '\n' && PreservesSegmentBreaks(mode); }
};

double HalfChAdvance(const TextBackend* backend, const TextRun& run, bool vertical) {
  if (!backend) {
    return 0.0;
  }
  const auto shaped = backend->shapeRun(run.font, run.usedFontSizePx, "0", 0, 1, vertical,
                                        FontVariant::Normal, true);
  if (shaped.glyphs.empty()) {
    return 0.0;
  }
  const auto& glyph = shaped.glyphs.front();
  return (vertical ? glyph.yAdvance : glyph.xAdvance) * 0.5;
}

struct WrappingChunkBoundary {
  size_t index;
  bool absoluteX = false;
  bool absoluteY = false;
};

WrappingChunkBoundary AdvanceWrappingChunk(std::span<const ChunkBoundary> chunks, size_t chunk,
                                           size_t run, size_t glyph) {
  WrappingChunkBoundary result{chunk};
  while (result.index < chunks.size() &&
         (chunks[result.index].runIndex < run ||
          (chunks[result.index].runIndex == run && chunks[result.index].glyphIndex <= glyph))) {
    result.absoluteX |= chunks[result.index].absoluteX;
    result.absoluteY |= chunks[result.index].absoluteY;
    ++result.index;
  }
  return result;
}

std::pair<bool, bool> WrappingAxisResets(const WrappingChunkBoundary& boundary, bool vertical) {
  return vertical ? std::pair(boundary.absoluteY, boundary.absoluteX)
                  : std::pair(boundary.absoluteX, boundary.absoluteY);
}

std::vector<WrappingGlyph> CollectWrappingGlyphs(const std::vector<TextRun>& runs,
                                                 const components::ComputedTextComponent& text,
                                                 bool vertical, const TextBackend* backend,
                                                 std::span<const ChunkBoundary> chunks) {
  std::vector<WrappingGlyph> flat;
  size_t block = 0;
  size_t chunk = 0;
  for (size_t ri = 0; ri < runs.size() && ri < text.spans.size(); ++ri) {
    if (runs[ri].onPath) {
      ++block;
      continue;
    }
    const auto& span = text.spans[ri];
    const std::string_view spanText(span.text.data() + span.start, span.end - span.start);
    const double halfCh = spanText.find('\t') != std::string_view::npos
                              ? HalfChAdvance(backend, runs[ri], vertical)
                              : 0.0;
    for (size_t gi = 0; gi < runs[ri].glyphs.size(); ++gi) {
      const auto& glyph = runs[ri].glyphs[gi];
      const auto boundary = AdvanceWrappingChunk(chunks, chunk, ri, gi);
      chunk = boundary.index;
      const auto [resetsInline, resetsBlock] = WrappingAxisResets(boundary, vertical);
      size_t byte = GetPositioningCluster(glyph);
      const uint32_t cp = SpanLayoutCodepoint(span, byte);
      const double advance = vertical ? glyph.yAdvance : glyph.xAdvance;
      flat.push_back(
          {ri, gi, InlinePenPosition(glyph, vertical), vertical ? glyph.xPosition : glyph.yPosition,
           advance, advance, cp, span.whiteSpace,
           gi + 1 == runs[ri].glyphs.size() ? span.boundaryWhiteSpace.value_or(span.whiteSpace)
                                            : span.whiteSpace,
           halfCh, span.textAnchor, resetsInline, resetsBlock, chunk, 0, block});
    }
  }
  return flat;
}

/// Consume only metadata for the current run/glyph, skipping retired path runs.
double TakeInlineDisplacement(std::span<const InlineDisplacement> values, size_t& index, size_t run,
                              size_t glyph) {
  while (index < values.size() &&
         (values[index].runIndex < run ||
          (values[index].runIndex == run && values[index].glyphIndex < glyph))) {
    ++index;
  }
  return index < values.size() && values[index].runIndex == run && values[index].glyphIndex == glyph
             ? values[index++].distance
             : 0.0;
}

/// SVG relative positions accumulate after CSS layout; immutable origins survive overflow retries.
void ResolveWrappingOrigins(std::vector<WrappingGlyph>& flat,
                            std::span<const InlineDisplacement> displacements) {
  size_t next = 0;
  double origin = 0.0;
  for (size_t i = 0; i < flat.size(); ++i) {
    auto& item = flat[i];
    item.inlineDisplacement = TakeInlineDisplacement(displacements, next, item.run, item.glyph);
    if (i == 0 || item.block != flat[i - 1].block || item.resetsInline) {
      origin = item.originalX;
    } else {
      origin += item.inlineDisplacement;
    }
    item.origin = origin;
  }
}

/// Resolves tab stops from the line start, retaining glyph-local kerning between ordinary letters.
double PlaceWrappingGlyph(std::vector<WrappingGlyph>& flat, size_t index, size_t start,
                          double width, size_t line) {
  auto& item = flat[index];
  const double gap = index == start
                         ? 0.0
                         : item.originalX -
                               (flat[index - 1].originalX + flat[index - 1].originalAdvance) -
                               item.inlineDisplacement;
  item.x = width + gap;
  item.line = line;
  if (item.isBreak()) {
    item.advance = 0.0;
  } else if (item.codepoint == '\t' && item.originalAdvance > 0.0) {
    const double stop = item.originalAdvance * 8.0;
    item.advance = stop - std::fmod(std::max(0.0, item.x), stop);
    if (item.advance < item.halfCh) {
      item.advance += stop;
    }
  }
  return item.x + item.advance;
}

bool IsWrappingOverflow(const WrappingGlyph& item, double width, double measurePx,
                        std::optional<size_t> lastBreak, size_t start) {
  return measurePx > 0.0 && width > measurePx + 1e-3 && lastBreak && *lastBreak > start &&
         (!item.isSpace() || item.mode == WhiteSpace::BreakSpaces);
}

void TrimSoftBreakSpaces(std::vector<WrappingGlyph>& flat, size_t start, size_t end) {
  for (size_t trim = end; trim > start && flat[trim - 1].isSpace(); --trim) {
    if (!PreservesSpaces(flat[trim - 1].mode) && flat[trim - 1].codepoint != 0x3000) {
      flat[trim - 1].advance = 0.0;
    }
  }
}

bool IsSoftBreakAfter(const std::vector<WrappingGlyph>& flat, size_t index) {
  const auto& item = flat[index];
  const bool endOfSpaces = index + 1 == flat.size() || !flat[index + 1].isSpace();
  return item.isSpace() && AllowsSoftWrap(item.breakMode) &&
         (item.mode == WhiteSpace::BreakSpaces || endOfSpaces);
}

/// Greedy line selection never breaks an unbreakable word or a pre/nowrap span.
size_t AssignWrappingLines(std::vector<WrappingGlyph>& flat, double measurePx) {
  size_t line = 0;
  size_t blockLine = 0;
  size_t block = flat.front().block;
  size_t start = 0;
  size_t index = 0;
  std::optional<size_t> lastBreak;
  double width = 0.0;
  while (index < flat.size()) {
    auto& item = flat[index];
    if (item.block != block) {
      block = item.block;
      blockLine = ++line;
      start = index;
      width = 0.0;
      lastBreak.reset();
    }
    // An overflow retry visits the authored reset again on its newly assigned line.
    if (item.resetsBlock) {
      blockLine = line;
    }
    if (item.resetsInline) {
      start = index;
      width = 0.0;
      lastBreak.reset();
    }
    width = PlaceWrappingGlyph(flat, index, start, width, line);
    item.lineOffset = line - blockLine;
    if (item.isBreak()) {
      start = ++index;
      ++line;
      width = 0.0;
      lastBreak.reset();
      continue;
    }
    if (IsWrappingOverflow(item, width, measurePx, lastBreak, start)) {
      TrimSoftBreakSpaces(flat, start, *lastBreak);
      start = *lastBreak;
      index = start;
      ++line;
      width = 0.0;
      lastBreak.reset();
      continue;
    }
    if (IsSoftBreakAfter(flat, index)) {
      lastBreak = index + 1;
    }
    ++index;
  }
  return line + 1;
}

/// Preserved end spaces hang after a soft wrap; before a forced break they hang only on overflow.
bool CountsForLineWidth(const WrappingGlyph& item, bool forcedEnd, double measurePx) {
  if (item.isBreak()) {
    return false;
  }
  if (!item.isSpace() || item.mode == WhiteSpace::Pre || item.mode == WhiteSpace::BreakSpaces) {
    return true;
  }
  return item.mode == WhiteSpace::PreWrap && forcedEnd &&
         (!(measurePx > 0.0) || item.x + item.advance <= measurePx);
}

std::vector<bool> ForcedWrappingEnds(const std::vector<WrappingGlyph>& flat, size_t numLines) {
  std::vector<bool> result(numLines, false);
  for (size_t i = 0; i < flat.size(); ++i) {
    const auto& item = flat[i];
    result[item.line] = result[item.line] || item.isBreak() || i + 1 == flat.size() ||
                        item.block != flat[i + 1].block;
  }
  return result;
}

/// Only a line's trailing whitespace can hang; its CSS decision precedes SVG textLength.
std::vector<bool> ExcludedWrappingItems(const std::vector<WrappingGlyph>& flat, size_t numLines,
                                        double measurePx) {
  const auto forcedEnds = ForcedWrappingEnds(flat, numLines);
  std::vector<bool> result(flat.size(), false);
  size_t line = flat.back().line;
  size_t block = flat.back().block;
  bool trailing = true;
  for (size_t i = flat.size(); i-- > 0;) {
    const auto& item = flat[i];
    if (item.line != line || item.block != block) {
      line = item.line;
      block = item.block;
      trailing = true;
    }
    if (item.isBreak()) {
      result[i] = true;
    } else if (!item.isSpace()) {
      trailing = false;
    } else {
      result[i] = trailing && !CountsForLineWidth(item, forcedEnds[item.line], measurePx);
    }
  }
  return result;
}

void RecordAnchorExclusions(const std::vector<TextRun>& runs,
                            const components::ComputedTextComponent& text,
                            const std::vector<WrappingGlyph>& flat,
                            const std::vector<bool>& excluded, AnchorExclusions* output) {
  if (!output) {
    return;
  }
  auto& result = *output;
  for (size_t i = 0; i < flat.size(); ++i) {
    if (!excluded[i]) {
      continue;
    }
    if (result.empty()) {
      result.resize(runs.size());
    }
    const auto& item = flat[i];
    auto& mask = result[item.run];
    if (mask.empty()) {
      mask.resize(text.spans[item.run].end - text.spans[item.run].start, false);
    }
    const size_t cluster = GetPositioningCluster(runs[item.run].glyphs[item.glyph]);
    if (cluster < mask.size()) {
      mask[cluster] = true;
    }
  }
}

struct WrappingAnchorGroup {
  TextAnchor anchor = TextAnchor::Start;
  TextAnchorBounds bounds;
};

void AccumulateWrappingAnchor(WrappingAnchorGroup& group, const WrappingGlyph& item,
                              bool contributesWidth) {
  if (!group.bounds.initialized && !item.isBreak()) {
    group.anchor = item.textAnchor;
    group.bounds.startAt(item.origin + item.x);
  }
  if (contributesWidth) {
    group.bounds.include(item.origin + item.x, item.advance);
  }
}

/// A line start or authored x/y reset starts an anchored chunk; a style-only span does not.
std::vector<double> WrappingAnchorShifts(std::vector<WrappingGlyph>& flat,
                                         const std::vector<bool>& excluded) {
  std::vector<WrappingAnchorGroup> groups;
  for (size_t i = 0; i < flat.size(); ++i) {
    auto& item = flat[i];
    if (i == 0 || item.line != flat[i - 1].line || item.chunk != flat[i - 1].chunk) {
      groups.emplace_back();
    }
    item.anchorGroup = groups.size() - 1;
    AccumulateWrappingAnchor(groups.back(), item, !excluded[i]);
  }
  std::vector<double> shifts;
  shifts.reserve(groups.size());
  for (const auto& group : groups) {
    shifts.push_back(group.bounds.shift(group.anchor));
  }
  return shifts;
}

void PlaceWrappedGlyph(TextGlyph& glyph, const WrappingGlyph& item, const TextLayoutParams& params,
                       double shift, double lineHeightPx) {
  const double lineOffset = static_cast<double>(item.lineOffset) * lineHeightPx;
  if (isVertical(params.writingMode)) {
    glyph.yPosition = item.origin + item.x + shift + glyph.verticalOriginOffset;
    glyph.xPosition =
        item.originalY + (params.writingMode == WritingMode::VerticalRl ? -lineOffset : lineOffset);
    glyph.yAdvance = item.advance;
  } else {
    glyph.xPosition = item.origin + item.x + shift;
    glyph.yPosition = item.originalY + lineOffset;
    glyph.xAdvance = item.advance;
  }
}

std::vector<bool> FiniteWrappingGroups(const std::vector<TextRun>& runs,
                                       const std::vector<WrappingGlyph>& flat,
                                       const TextLayoutParams& params,
                                       const std::vector<double>& shifts, double lineHeightPx) {
  std::vector<bool> finite(shifts.size(), true);
  for (const auto& item : flat) {
    auto translated = runs[item.run].glyphs[item.glyph];
    PlaceWrappedGlyph(translated, item, params, shifts[item.anchorGroup], lineHeightPx);
    finite[item.anchorGroup] = finite[item.anchorGroup] && HasFiniteFlatGlyphGeometry(translated);
  }
  return finite;
}

void DiscardNonfiniteWrappingGroups(std::vector<TextRun>& runs,
                                    const std::vector<WrappingGlyph>& flat,
                                    const std::vector<bool>& finite) {
  size_t index = 0;
  for (size_t ri = 0; ri < runs.size(); ++ri) {
    if (runs[ri].onPath) {
      continue;
    }
    auto& glyphs = runs[ri].glyphs;
    size_t retained = 0;
    for (size_t gi = 0; gi < glyphs.size(); ++gi) {
      bool keep = true;
      if (index < flat.size() && flat[index].run == ri && flat[index].glyph == gi) {
        keep = finite[flat[index].anchorGroup];
        ++index;
      }
      if (keep) {
        if (retained != gi) {
          glyphs[retained] = std::move(glyphs[gi]);
        }
        ++retained;
      }
    }
    glyphs.resize(retained);
  }
}

bool HasWrappingLines(const std::vector<WrappingGlyph>& flat) {
  size_t block = flat.front().block;
  size_t line = flat.front().line;
  for (const auto& item : flat) {
    if (item.isBreak()) {
      return true;
    }
    if (item.block != block) {
      block = item.block;
      line = item.line;
    } else if (item.line != line) {
      return true;
    }
  }
  return false;
}

bool applyInlineSizeWrap(std::vector<TextRun>& runs, const components::ComputedTextComponent& text,
                         const TextLayoutParams& params, double measurePx, double lineHeightPx,
                         const TextBackend* backend, std::span<const ChunkBoundary> chunks,
                         std::span<const InlineDisplacement> displacements,
                         AnchorExclusions* exclusions) {
  auto flat = CollectWrappingGlyphs(runs, text, isVertical(params.writingMode), backend, chunks);
  if (flat.empty()) {
    return false;
  }
  ResolveWrappingOrigins(flat, displacements);
  const size_t numLines = AssignWrappingLines(flat, measurePx);
  const bool hasControls = std::any_of(flat.begin(), flat.end(), [](const WrappingGlyph& item) {
    return item.isBreak() || item.codepoint == '\t';
  });
  const bool wrapped = HasWrappingLines(flat);
  const auto excluded = ExcludedWrappingItems(flat, numLines, measurePx);
  if (!wrapped) {
    RecordAnchorExclusions(runs, text, flat, excluded, exclusions);
  }
  if (!wrapped && !hasControls) {
    return false;
  }
  const auto shifts = WrappingAnchorShifts(flat, excluded);
  const auto finite = wrapped ? FiniteWrappingGroups(runs, flat, params, shifts, lineHeightPx)
                              : std::vector<bool>(shifts.size(), true);
  for (const auto& item : flat) {
    if (finite[item.anchorGroup]) {
      PlaceWrappedGlyph(runs[item.run].glyphs[item.glyph], item, params,
                        wrapped ? shifts[item.anchorGroup] : 0.0, lineHeightPx);
    }
  }
  if (wrapped) {
    DiscardNonfiniteWrappingGroups(runs, flat, finite);
  }
  return wrapped;
}

}  // namespace text_engine_detail

using namespace text_engine_detail;  // NOLINT(google-build-using-namespace)

namespace {

std::optional<Box2d> BitmapGlyphExtent(
    const TextGlyph& glyph, const std::optional<TextBackend::BitmapGlyphMetrics>& bitmap) {
  if (!bitmap || bitmap->width <= 0 || bitmap->height <= 0) {
    return std::nullopt;
  }
  const Transform2d localFromBitmap = computeLocalFromBitmapGlyph(glyph, *bitmap);
  return localFromBitmap.transformBox(Box2d::FromXYWH(0, 0, bitmap->width, bitmap->height));
}

struct ApiCharacterIndexMappings {
  std::vector<size_t> byteToFirstUnit;
  std::vector<size_t> firstUnitByUnit;
};

/// Associate UTF-16 units with the first unit of their shaped typographic character.
ApiCharacterIndexMappings BuildApiCharacterIndexMappings(std::string_view text,
                                                         std::span<const TextGlyph> glyphs) {
  std::vector<uint8_t> shapedClusterStarts(text.size(), 0);
  for (const auto& glyph : glyphs) {
    if (glyph.cluster < text.size()) {
      shapedClusterStarts[glyph.cluster] = 1;
    }
  }
  ApiCharacterIndexMappings result;
  result.byteToFirstUnit.resize(text.size());
  result.firstUnitByUnit.reserve(text.size());
  size_t firstUnit = 0;
  size_t position = 0;
  while (position < text.size()) {
    const size_t start = position;
    const uint32_t codepoint = decodeUtf8(text, position);
    const bool startsCluster =
        std::find(shapedClusterStarts.begin() + start, shapedClusterStarts.begin() + position, 1) !=
        shapedClusterStarts.begin() + position;
    if (startsCluster && !isNonSpacing(codepoint)) {
      firstUnit = result.firstUnitByUnit.size();
    }
    for (size_t byte = start; byte < position; ++byte) {
      result.byteToFirstUnit[byte] = firstUnit;
    }
    const size_t units = codepoint >= 0x10000 ? 2 : 1;
    for (size_t unit = 0; unit < units; ++unit) {
      result.firstUnitByUnit.push_back(firstUnit);
    }
  }
  return result;
}

void ExpandApiCharacterGeometryAliases(components::ComputedTextGeometryComponent& cache,
                                       size_t baseIndex,
                                       const ApiCharacterIndexMappings& mappings) {
  for (size_t unit = 0; unit < mappings.firstUnitByUnit.size(); ++unit) {
    const size_t firstUnit = mappings.firstUnitByUnit[unit];
    if (unit != firstUnit) {
      cache.characters[baseIndex + unit] = cache.characters[baseIndex + firstUnit];
      // UTF-16 aliases share the glyph cell, but only its first unit contributes advance.
      cache.characters[baseIndex + unit].advance = 0.0;
    }
  }
}

void addBox(Box2d& accum, bool& initialized, const Box2d& box) {
  if (box.isEmpty()) {
    return;
  }

  if (!initialized) {
    accum = box;
    initialized = true;
  } else {
    accum.addBox(box);
  }
}

void AddTextObjectBounds(components::ComputedTextGeometryComponent& cache, Entity sourceEntity,
                         const Box2d& bounds) {
  if (bounds.isEmpty()) {
    return;
  }
  auto [stored, inserted] = cache.objectBoundsByEntity.try_emplace(sourceEntity, bounds);
  if (!inserted) {
    stored->second.addBox(bounds);
  }
}

void BuildTextObjectBoundsIndex(Registry& registry, Entity textRootEntity,
                                components::ComputedTextGeometryComponent& cache) {
  if (cache.objectBoundsByEntity.empty()) {
    return;
  }

  std::vector<Entity> subtreeEntities{textRootEntity};
  for (size_t index = 0; index < subtreeEntities.size(); ++index) {
    const auto* tree = registry.try_get<donner::components::TreeComponent>(subtreeEntities[index]);
    if (!tree) {
      continue;
    }
    for (Entity child = tree->firstChild(); child != entt::null;
         child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
      subtreeEntities.push_back(child);
    }
  }

  // Reverse parent-first order lets each subtree reach its parent before that parent propagates.
  for (auto it = subtreeEntities.rbegin(); it != subtreeEntities.rend(); ++it) {
    if (*it == textRootEntity) {
      continue;
    }
    const auto bounds = cache.objectBoundsByEntity.find(*it);
    if (bounds == cache.objectBoundsByEntity.end()) {
      continue;
    }
    const Box2d childBounds = bounds->second;
    const Entity parent = registry.get<donner::components::TreeComponent>(*it).parent();
    AddTextObjectBounds(cache, parent, childBounds);
  }
}

std::vector<const components::ComputedTextGeometryComponent::CharacterGeometry*> filteredCharacters(
    Registry& registry, EntityHandle handle,
    const components::ComputedTextGeometryComponent& cache) {
  std::vector<const components::ComputedTextGeometryComponent::CharacterGeometry*> result;
  for (const auto& character : cache.characters) {
    if (character.rendered && isDescendantOf(registry, character.sourceEntity, handle.entity())) {
      result.push_back(&character);
    }
  }
  return result;
}

template <typename Geometry>
bool FontDependenciesChanged(Geometry& geometry, uint64_t revision,
                             std::span<const FontFaceDependency> resolved) {
  if (geometry.fontResourceRevision == revision) {
    return false;
  }
  geometry.fontResourceRevision = revision;
  for (const auto& dependency : geometry.fontDependencies) {
    const auto current = std::find_if(resolved.begin(), resolved.end(), [&](const auto& face) {
      return face.family == dependency.family && face.request == dependency.request;
    });
    if (current == resolved.end() ||
        (current->state == FontFaceLoadState::Loaded &&
         (dependency.state != FontFaceLoadState::Loaded ||
          dependency.availability.contentId != current->availability.contentId ||
          dependency.availability.contentGeneration != current->availability.contentGeneration))) {
      return true;
    }
  }
  return false;
}

}  // namespace

TextEngine::TextEngine(FontManager& fontManager, Registry& registry)
    : fontManager_(fontManager), registry_(registry) {
#ifdef DONNER_TEXT_FULL
  backend_ = std::make_unique<TextBackendFull>(fontManager_, registry_);
#else
  backend_ = std::make_unique<TextBackendSimple>(fontManager_, registry_);
#endif
}

TextEngine::TextEngine(FontManager& fontManager, Registry& registry,
                       std::unique_ptr<TextBackend> backend)
    : fontManager_(fontManager), registry_(registry), backend_(std::move(backend)) {}

TextEngine::~TextEngine() = default;

bool TextEngine::needsFontResourceRefresh() const {
  return fontManager_.fontResourceRevision() != observedFontResourceRevision_ ||
         fontManager_.needsResourceRefresh();
}

std::vector<Entity> TextEngine::refreshFontResources() {
  fontManager_.refreshPendingFonts();
  const uint64_t revision = fontManager_.fontResourceRevision();
  const auto resolved = fontManager_.faceDependencies();

  std::vector<Entity> changedRoots;
  for (auto view = registry_.view<components::ComputedTextGeometryComponent>();
       const Entity root : view) {
    if (FontDependenciesChanged(view.get<components::ComputedTextGeometryComponent>(root), revision,
                                resolved)) {
      changedRoots.push_back(root);
    }
  }
  for (const Entity root : changedRoots) {
    components::InvalidateTextLayout(EntityHandle(registry_, root));
  }
  for (auto view = registry_.view<components::FontMetricDependenciesComponent>();
       const Entity entity : view) {
    if (!FontDependenciesChanged(view.get<components::FontMetricDependenciesComponent>(entity),
                                 revision, resolved)) {
      continue;
    }
    registry_.remove<components::ComputedPathComponent>(entity);
    registry_.get_or_emplace<components::DirtyFlagsComponent>(entity).mark(
        components::DirtyFlagsComponent::Shape | components::DirtyFlagsComponent::LayoutCascade |
        components::DirtyFlagsComponent::RenderInstance);
    changedRoots.push_back(entity);
  }
  for (auto view = registry_.view<components::ComputedClipPathsComponent>();
       const Entity entity : view) {
    if (!FontDependenciesChanged(view.get<components::ComputedClipPathsComponent>(entity), revision,
                                 resolved)) {
      continue;
    }
    registry_.get_or_emplace<components::DirtyFlagsComponent>(entity).mark(
        components::DirtyFlagsComponent::TextGeometry | components::DirtyFlagsComponent::Paint |
        components::DirtyFlagsComponent::RenderInstance);
    changedRoots.push_back(entity);
  }
  for (auto view = registry_.view<components::FontPaintDependenciesComponent>();
       const Entity entity : view) {
    if (!FontDependenciesChanged(view.get<components::FontPaintDependenciesComponent>(entity),
                                 revision, resolved)) {
      continue;
    }
    registry_.get_or_emplace<components::DirtyFlagsComponent>(entity).mark(
        components::DirtyFlagsComponent::TextGeometry | components::DirtyFlagsComponent::Filter |
        components::DirtyFlagsComponent::RenderInstance);
    changedRoots.push_back(entity);
  }
  observedFontResourceRevision_ = revision;
  return changedRoots;
}

void TextEngine::prepareForElement(EntityHandle handle, ParseWarningSink& outWarnings) {
  UTILS_RELEASE_ASSERT(handle.registry() == &registry_);
  const Entity textRootEntity = findTextRootEntity(handle);
  if (textRootEntity == entt::null) {
    return;
  }

  auto* resourceManager = registry_.ctx().find<components::ResourceManagerContext>();
  if (!resourceManager) {
    return;
  }

  if (resourceManager->fontFaces().empty()) {
    for (auto view = registry_.view<components::StylesheetComponent>(); auto entity : view) {
      resourceManager->synchronizeStylesheetFontFaces(
          entity, view.get<components::StylesheetComponent>(entity).stylesheet.fontFaces());
    }
  }
  resourceManager->loadResources(outWarnings);

  auto& fontManager = registry_.ctx().contains<FontManager>()
                          ? registry_.ctx().get<FontManager>()
                          : registry_.ctx().emplace<FontManager>(registry_);
  if (!registry_.ctx().contains<TextEngine>()) {
    registry_.ctx().emplace<TextEngine>(fontManager, registry_);
  }
  addFontFaces(resourceManager->fontFaces());

  components::StyleSystem styleSystem;
  const EntityHandle textRootHandle(registry_, textRootEntity);
  styleSystem.computeStyle(textRootHandle, outWarnings);
  donner::components::ForAllChildrenRecursive(
      textRootHandle, [&](EntityHandle child) { styleSystem.computeStyle(child, outWarnings); });

  components::TextSystem().instantiateComputedComponent(textRootHandle, outWarnings);
}

void TextEngine::resolvePerSpanLayoutStyles(EntityHandle textRootHandle,
                                            components::ComputedTextComponent& text,
                                            const TextStyleContext* styleContext) const {
  const auto* textComp = registry_.try_get<components::TextComponent>(textRootHandle.entity());
  const EntityHandle styleRoot(registry_,
                               styleContext ? styleContext->rootEntity : textRootHandle.entity());
  const auto* style = styleRoot.try_get<components::ComputedStyleComponent>();
  if (!textComp || !style || !style->properties) {
    return;
  }

  const TextLayoutParams params = buildTextLayoutParams(registry_, styleRoot, *style, *textComp);
  ResolvePerSpanLayoutStyles(registry_, text, params.viewBox, params.fontMetrics, styleContext);
}

void TextEngine::addFontFace(const css::FontFace& face) {
  fontManager_.addFontFace(face);
  ++registeredFontFaceCount_;
}

void TextEngine::addFontFaces(std::span<const css::FontFace> faces) {
  if (faces.size() <= registeredFontFaceCount_) {
    return;
  }

  for (size_t i = registeredFontFaceCount_; i < faces.size(); ++i) {
    fontManager_.addFontFace(faces[i]);
  }
  registeredFontFaceCount_ = faces.size();
}

namespace {

/**
 * @brief The glyphs of \p run whose painted geometry belongs in the text geometry cache.
 *
 * `visibility: hidden` and `visibility: collapse` suppress painting only. The span is still laid
 * out and keeps its positioned glyphs so it contributes to the element's object bounding box, per
 * the SVG object-bounding-box definition, but it produces no ink geometry. Logical character
 * metrics are recorded separately. Returns an empty span for such a run.
 *
 * @param span The span that produced \p run.
 * @param run The positioned layout run.
 * @return The glyphs to record as painted, which is empty when the span is not painted.
 */
std::span<const TextGlyph> PaintedSpanGlyphs(
    const components::ComputedTextComponent::TextSpan& span, const TextRun& run) {
  return span.visibility == Visibility::Visible ? std::span<const TextGlyph>(run.glyphs)
                                                : std::span<const TextGlyph>();
}

/// Painting visibility and used size gate ink, not positioned logical character records.
bool HasPaintedTextGlyphs(const components::ComputedTextComponent::TextSpan& span,
                          const TextRun& run) {
  return run.usedFontSizePx > 0.0f && !PaintedSpanGlyphs(span, run).empty();
}

/// Resolves a span's inherited font family and face attributes.
FontHandle ResolveSpanFace(FontManager& fontManager,
                           const components::ComputedTextComponent::TextSpan& span,
                           const SmallVector<RcString, 1>& families, FontHandle fallback) {
  const SmallVector<RcString, 1>& spanFamilies =
      span.fontFamilies.empty() ? families : span.fontFamilies;
  FontHandle spanFont = FindFirstAvailableFont(fontManager, spanFamilies);
  if (!spanFont) {
    spanFont = fallback;
  }
  if (span.fontWeight != 400 || span.fontStyle != FontStyle::Normal ||
      span.fontStretch != FontStretch::Normal) {
    const FontHandle candidate = FindFirstAvailableFont(fontManager, spanFamilies, span.fontWeight,
                                                        static_cast<int>(span.fontStyle),
                                                        static_cast<int>(span.fontStretch));
    if (candidate) {
      spanFont = candidate;
    }
  }

  return spanFont;
}

/// Applies the requested x-height ratio after the final face has been selected.
float AdjustFontSize(const TextBackend& backend, FontHandle font, float sizePx,
                     const std::optional<double>& fontSizeAdjust) {
  sizePx = CheckedFontSizePx(sizePx);
  if (sizePx == 0.0f || !fontSizeAdjust) {
    return sizePx;
  }
  if (!std::isfinite(*fontSizeAdjust) || *fontSizeAdjust <= 0.0) {
    return 0.0f;
  }
  const FontVMetrics metrics = backend.fontVMetrics(font);
  if (metrics.xHeight <= 0 || metrics.unitsPerEm <= 0) {
    return sizePx;
  }
  const double aspect = static_cast<double>(metrics.xHeight) / metrics.unitsPerEm;
  return CheckedFontSizePx(static_cast<double>(sizePx) * *fontSizeAdjust / aspect);
}

/// Apply synthesis policy to the actual face without mutating its native feature state.
FontVariant EffectiveSmallCapsVariant(const TextBackend& backend, FontHandle font,
                                      FontVariant variant, FontSynthesisSmallCaps synthesis) {
  return variant == FontVariant::SmallCaps && synthesis == FontSynthesisSmallCaps::None &&
                 !backend.hasSmallCapsFeature(font)
             ? FontVariant::Normal
             : variant;
}

/// Preserve same-family boundary shaping across size/face changes, using the preceding face's kern.
/// \p currentFont and \p previousFont are the faces that draw the glyphs on each side of the
/// boundary, which may be fallback faces rather than the spans' primary faces.
bool CompatibleKerningRuns(const FontManager& fontManager, FontHandle currentFont,
                           float currentSizePx, FontVariant currentVariant, FontHandle previousFont,
                           float previousSizePx, FontVariant previousVariant, bool currentKerning,
                           bool previousKerning) {
  return currentKerning && previousKerning && currentVariant == previousVariant &&
         currentSizePx > 0.0f && previousSizePx > 0.0f &&
         fontManager.fontsShareFamily(currentFont, previousFont);
}

/// Only adjacent flat runs or runs owned by the same textPath share a pair predecessor.
bool ContinuesKerningScope(const std::optional<Entity>& previousPath,
                           const components::ComputedTextComponent::TextSpan& current) {
  if (!current.pathSpline) {
    return !previousPath;
  }
  return previousPath && *previousPath != entt::null &&
         *previousPath == current.textPathSourceEntity;
}

/// An ignored absolute y cannot start a new horizontal path kerning chunk.
bool StartsNewKerningChunk(const components::ComputedTextComponent::TextSpan& span, bool vertical) {
  if (span.pathSpline && !vertical) {
    return !span.xList.empty() && span.xList.front().has_value();
  }
  return span.startsNewChunk;
}

/// Discard predecessor evidence that belongs to a different layout domain.
uint32_t KerningPredecessorForScope(const std::optional<Entity>& previousPath,
                                    const components::ComputedTextComponent::TextSpan& current,
                                    uint32_t codepoint) {
  return ContinuesKerningScope(previousPath, current) ? codepoint : 0;
}

/// Distinguish a flat span from an owned or unowned path span.
std::optional<Entity> SpanKerningPathSource(
    const components::ComputedTextComponent::TextSpan& span) {
  return span.pathSpline ? std::optional(span.textPathSourceEntity) : std::nullopt;
}

/// Keep source clusters in the positioning pipeline without invoking a font backend at size zero.
TextBackend::ShapedRun ShapeAddressableZeroSizeText(std::string_view text, size_t start,
                                                    size_t length) {
  TextBackend::ShapedRun result;
  const size_t end = start + length;
  for (size_t offset = start; offset < end;) {
    TextBackend::ShapedGlyph glyph;
    glyph.cluster = static_cast<uint32_t>(offset);
    result.glyphs.push_back(glyph);
    decodeUtf8(text, offset);
  }
  return result;
}

/// Joiners and selectors that fonts need not map; they never make a cluster fall back.
bool IsClusterJoiner(uint32_t cp) {
  return cp == 0x200C || cp == 0x200D || cp == 0x034F || (cp >= 0xFE00 && cp <= 0xFE0F) ||
         (cp >= 0xE0100 && cp <= 0xE01EF);
}

/// Returns the end of the cluster starting at \p start: its base codepoint, the nonspacing
/// codepoints that follow it, and each codepoint a zero-width joiner attaches.
size_t ClusterEnd(std::string_view text, size_t start, size_t end) {
  size_t position = start;
  uint32_t previous = decodeUtf8(text, position);
  while (position < end) {
    size_t next = position;
    const uint32_t codepoint = decodeUtf8(text, next);
    if (!isNonSpacing(codepoint) && previous != 0x200D) {
      break;
    }
    previous = codepoint;
    position = next;
  }
  return position;
}

/**
 * Memoizes and bounds the codepoint coverage queries that cluster fallback makes for one text
 * element. Each query shapes one codepoint, so a document with many faces and many distinct
 * unmapped codepoints could otherwise demand faces times codepoints shaping calls. Each span grants
 * a fixed number of queries per byte of its text; once the grant is spent, unknown pairs report no
 * coverage and their clusters stay `.notdef`. At most
 * \ref TextEngine::kMaximumFallbackCoverageEntries answers are retained, so memory does not grow
 * with text length times face count.
 */
class FallbackCoverage {
public:
  /// Adds the query allowance for a span of \p textBytes UTF-8 bytes, which bounds its
  /// codepoint count.
  void grant(size_t textBytes) { add(textBytes * kQueriesPerByte); }

  /// Raises the element's total grant to at least \p queries, so a short span can still try each
  /// of its fallback candidates once.
  void ensureGrantAtLeast(size_t queries) {
    if (granted_ < queries) {
      add(queries - granted_);
    }
  }

  /// Whether the element's query grant is spent, after which no new coverage can be learned.
  bool exhausted() const { return remaining_ == 0; }

  /// Number of retained coverage answers.
  std::size_t entries() const { return cache_.size(); }

  /// Whether \p font maps \p codepoint, from the cache or one budgeted backend query.
  bool covers(const TextBackend& backend, FontHandle font, float sizePx, uint32_t codepoint) {
    const uint64_t key =
        (static_cast<uint64_t>(static_cast<uint32_t>(font.entity())) << 32) | codepoint;
    if (const auto cached = cache_.find(key); cached != cache_.end()) {
      return cached->second;
    }
    if (remaining_ == 0) {
      return false;
    }
    --remaining_;
    const bool covered = fontSupportsCodepoint(backend, font, sizePx, codepoint);
    // Answers past the bound are recomputed when asked again, still charged to the grant, so
    // memory stays fixed while results stay the same.
    if (cache_.size() < TextEngine::kMaximumFallbackCoverageEntries) {
      cache_.emplace(key, covered);
    }
    return covered;
  }

private:
  /// Enough to try every face of a typical fallback chain for each codepoint.
  static constexpr size_t kQueriesPerByte = 32;

  void add(size_t queries) {
    granted_ += queries;
    remaining_ += queries;
  }

  std::unordered_map<uint64_t, bool> cache_;
  size_t granted_ = 0;
  size_t remaining_ = 0;
};

/// Where a span's clusters may fall back, in priority order after the span's own face: its
/// remaining font families, then every registered face, matched at the span's weight and style.
struct ClusterFallback {
  FontManager& fontManager;
  FallbackCoverage& coverage;
  std::span<const RcString> families;
  int weight = 400;
  int style = 0;
  int stretch = 5;
};

/// Shapes one span's chunks with its primary face, except clusters that face cannot render, which
/// take the first fallback face that can, as CSS per-character font fallback does.
class ChunkShaper {
public:
  ChunkShaper(const TextBackend& backend, const TextRun& run, std::string_view text, bool vertical,
              TextOrientation orientation, FontVariant variant, FontSynthesisSmallCaps synthesis,
              bool kerning, const ClusterFallback& fallback)
      : backend_(backend),
        run_(run),
        text_(text),
        vertical_(vertical),
        orientation_(orientation),
        variant_(variant),
        synthesis_(synthesis),
        kerning_(kerning),
        fallback_(fallback) {}

  /// Shapes \p chunk, recording each glyph's fallback face in \p faces, which stays empty when the
  /// primary face draws every glyph.
  TextBackend::ShapedRun shape(const ChunkRange& chunk, std::vector<FontHandle>& faces) const {
    if (run_.usedFontSizePx == 0.0f) {
      return ShapeAddressableZeroSizeText(text_, chunk.byteStart, chunk.byteEnd - chunk.byteStart);
    }
    TextBackend::ShapedRun primary = shapeWith(run_.font, chunk);
    PrimaryClusters clusters = primaryClusters(primary, chunk);
    if (!clusters.anyNotdef) {
      return primary;
    }
    std::vector<Item> items = itemize(chunk, clusters);
    if (items.size() == 1 && items.front().face == run_.font) {
      return primary;
    }
    // A chunk the backend laid out right to left keeps its items in visual order.
    if (primary.glyphs.size() > 1 &&
        primary.glyphs.front().cluster > primary.glyphs.back().cluster) {
      std::reverse(items.begin(), items.end());
    }
    return shapeItems(items, chunk, faces);
  }

private:
  /// A run of adjacent clusters drawn with one face.
  struct Item {
    ChunkRange range;
    FontHandle face;
  };

  /// Per-byte flags from the primary shaping of one chunk.
  struct PrimaryClusters {
    std::vector<bool> notdef;        ///< A `.notdef` glyph starts its cluster at this byte.
    std::vector<bool> clusterStart;  ///< The backend starts a cluster at this byte.
    bool anyNotdef = false;
  };

  /// A shaper merges a syllable or a base with its marks into one cluster and reports every glyph
  /// of it at the cluster's first byte, so fallback never splits a backend cluster.
  static PrimaryClusters primaryClusters(const TextBackend::ShapedRun& primary,
                                         const ChunkRange& chunk) {
    PrimaryClusters result{std::vector<bool>(chunk.byteEnd - chunk.byteStart, false),
                           std::vector<bool>(chunk.byteEnd - chunk.byteStart, false)};
    for (const TextBackend::ShapedGlyph& glyph : primary.glyphs) {
      if (glyph.cluster < chunk.byteStart || glyph.cluster >= chunk.byteEnd) {
        continue;
      }
      result.clusterStart[glyph.cluster - chunk.byteStart] = true;
      if (glyph.glyphIndex == 0) {
        result.notdef[glyph.cluster - chunk.byteStart] = true;
        result.anyNotdef = true;
      }
    }
    return result;
  }

  /// Splits \p chunk into runs of clusters that share a face, in logical order.
  std::vector<Item> itemize(const ChunkRange& chunk, const PrimaryClusters& clusters) const {
    std::vector<Item> items;
    for (size_t start = chunk.byteStart; start < chunk.byteEnd;) {
      size_t end = ClusterEnd(text_, start, chunk.byteEnd);
      while (end < chunk.byteEnd && !clusters.clusterStart[end - chunk.byteStart]) {
        end = ClusterEnd(text_, end, chunk.byteEnd);
      }
      const auto first =
          clusters.notdef.begin() + static_cast<std::ptrdiff_t>(start - chunk.byteStart);
      const auto last =
          clusters.notdef.begin() + static_cast<std::ptrdiff_t>(end - chunk.byteStart);
      const FontHandle face =
          std::find(first, last, true) != last ? clusterFace(start, end) : run_.font;
      if (!items.empty() && items.back().face == face) {
        items.back().range.byteEnd = end;
      } else {
        items.push_back({{start, end}, face});
      }
      start = end;
    }
    return items;
  }

  /// Shapes each item with its face as one part of \p chunk, recording fallback faces per glyph
  /// in \p faces. The chunk supplies each item's shaping context, so joining continues across
  /// faces and script-neutral items take the chunk's direction.
  TextBackend::ShapedRun shapeItems(const std::vector<Item>& items, const ChunkRange& chunk,
                                    std::vector<FontHandle>& faces) const {
    const TextBackend::ShapingContext context{chunk.byteStart, chunk.byteEnd - chunk.byteStart};
    TextBackend::ShapedRun result;
    for (const Item& item : items) {
      const TextBackend::ShapedRun shaped = backend_.shapeRunInContext(
          item.face, run_.usedFontSizePx, text_, context, item.range.byteStart,
          item.range.byteEnd - item.range.byteStart, vertical_, orientation_,
          EffectiveSmallCapsVariant(backend_, item.face, variant_, synthesis_), kerning_);
      result.glyphs.insert(result.glyphs.end(), shaped.glyphs.begin(), shaped.glyphs.end());
      faces.resize(result.glyphs.size(), item.face == run_.font ? FontHandle() : item.face);
    }
    return result;
  }

  TextBackend::ShapedRun shapeWith(FontHandle font, const ChunkRange& chunk) const {
    const size_t length = chunk.byteEnd - chunk.byteStart;
    const FontVariant variant = EffectiveSmallCapsVariant(backend_, font, variant_, synthesis_);
    return kerning_ ? backend_.shapeRun(font, run_.usedFontSizePx, text_, chunk.byteStart, length,
                                        vertical_, orientation_, variant, false)
                    : backend_.shapeRunNoKerning(font, run_.usedFontSizePx, text_, chunk.byteStart,
                                                 length, vertical_, orientation_, variant, false);
  }

  /// The face for a cluster whose primary glyphs include `.notdef`. Repeated clusters reuse the
  /// first answer, bounding the coverage queries per span by its distinct missing clusters.
  FontHandle clusterFace(size_t start, size_t end) const {
    const auto [entry, inserted] = clusterFaces_.try_emplace(text_.substr(start, end - start));
    if (inserted) {
      // A backend may report `.notdef` for a joiner the face need not map. Resolving the
      // candidates first applies the element's minimum grant before the exhaustion check.
      entry->second =
          covers(run_.font, start, end) || candidates().empty() || fallback_.coverage.exhausted()
              ? run_.font
              : findFace(start, end);
    }
    return entry->second;
  }

  /// Whether \p font maps every codepoint of the cluster at [start, end) except joiners.
  bool covers(FontHandle font, size_t start, size_t end) const {
    for (size_t position = start; position < end;) {
      const uint32_t codepoint = decodeUtf8(text_, position);
      if (!IsClusterJoiner(codepoint) &&
          !fallback_.coverage.covers(backend_, font, run_.usedFontSizePx, codepoint)) {
        return false;
      }
    }
    return true;
  }

  /// The first fallback face covering the cluster at [start, end), or the primary face when none
  /// does, which leaves the cluster's glyphs `.notdef`.
  FontHandle findFace(size_t start, size_t end) const {
    for (const FontHandle face : candidates()) {
      if (covers(face, start, end)) {
        return face;
      }
    }
    return run_.font;
  }

  /// The distinct fallback faces in priority order, resolved once per span.
  const std::vector<FontHandle>& candidates() const {
    if (candidatesResolved_) {
      return candidates_;
    }
    candidatesResolved_ = true;
    FontManager& fontManager = fallback_.fontManager;
    const auto add = [&](std::string_view family) {
      const FontHandle face =
          fontManager.findFont(family, fallback_.weight, fallback_.style, fallback_.stretch);
      if (face && face != run_.font &&
          std::find(candidates_.begin(), candidates_.end(), face) == candidates_.end()) {
        candidates_.push_back(face);
      }
    };
    for (const RcString& family : fallback_.families) {
      if (fontManager.hasFamily(family)) {
        add(family);
      }
    }
    const size_t faceCount = fontManager.numFaces();
    for (size_t index = 0; index < faceCount; ++index) {
      add(fontManager.faceFamilyName(index));
    }
    // The primary face's check plus one query per candidate.
    fallback_.coverage.ensureGrantAtLeast(candidates_.size() + 1);
    return candidates_;
  }

  const TextBackend& backend_;
  const TextRun& run_;
  std::string_view text_;
  bool vertical_;
  TextOrientation orientation_;
  FontVariant variant_;
  FontSynthesisSmallCaps synthesis_;
  bool kerning_;
  const ClusterFallback& fallback_;
  /// Resolved faces keyed by cluster text, which views the span text.
  mutable std::unordered_map<std::string_view, FontHandle> clusterFaces_;
  mutable std::vector<FontHandle> candidates_;
  mutable bool candidatesResolved_ = false;
};

/// A shaped chunk retains backend source clusters and separate SVG positioning groups.
struct PreparedSpanChunk {
  TextBackend::ShapedRun shaped;
  std::vector<uint32_t> positioningClusters;
  /// Per-glyph fallback faces, null where the run's face draws the glyph; empty when it draws all.
  std::vector<FontHandle> faces;

  /// The fallback face of glyph \p index, or null when the run's face draws it.
  FontHandle face(size_t index) const { return faces.empty() ? FontHandle() : faces[index]; }

  /// The face that draws glyph \p index, given the run's face \p primary.
  FontHandle drawingFace(size_t index, FontHandle primary) const {
    const FontHandle fallback = face(index);
    return fallback ? fallback : primary;
  }
};

/// Keep forced lines in source order while each line shapes into its own visual order.
std::vector<ChunkRange> SplitShapingRangesAtLineBreaks(std::string_view text,
                                                       const std::vector<ChunkRange>& ranges) {
  std::vector<ChunkRange> result;
  result.reserve(ranges.size());
  for (const auto& range : ranges) {
    const auto chunk = text.substr(range.byteStart, range.byteEnd - range.byteStart);
    size_t start = 0;
    for (size_t position = chunk.find('\n'); position != std::string_view::npos;
         position = chunk.find('\n', position + 1)) {
      if (position != start) {
        result.push_back({range.byteStart + start, range.byteStart + position});
      }
      result.push_back({range.byteStart + position, range.byteStart + position + 1});
      start = position + 1;
    }
    if (start != chunk.size()) {
      result.push_back({range.byteStart + start, range.byteEnd});
    }
  }
  return result;
}

std::vector<ChunkRange> FindSpanShapingRanges(
    const components::ComputedTextComponent::TextSpan& span, std::string_view text,
    const SmallVector<std::optional<Lengthd>, 1>& xList,
    const SmallVector<std::optional<Lengthd>, 1>& yList) {
  auto ranges = findChunkRanges(text, xList, yList);
  if (!span.pathSpline && PreservesSegmentBreaks(span.whiteSpace) &&
      text.find('\n') != std::string_view::npos) {
    return SplitShapingRangesAtLineBreaks(text, ranges);
  }
  return ranges;
}

/// Record control roles before shaping copies or replacements change their byte representation.
std::vector<bool> FindForcedShapingSeparators(
    const components::ComputedTextComponent::TextSpan& span, std::string_view text,
    const std::vector<ChunkRange>& ranges) {
  if (span.pathSpline || !PreservesSegmentBreaks(span.whiteSpace) ||
      text.find('\n') == std::string_view::npos) {
    return {};
  }
  std::vector<bool> result;
  result.reserve(ranges.size());
  for (const auto& range : ranges) {
    result.push_back(range.byteStart < text.size() && text[range.byteStart] == '\n');
  }
  return result;
}

/// Reuses one shaping result per existing chunk and records its positioning group starts.
struct PreparedSpanShaping {
  std::vector<PreparedSpanChunk> chunks;
  std::vector<bool> groupStarts;
};

PreparedSpanShaping PrepareSpanShaping(const ChunkShaper& shaper, std::string_view text,
                                       const std::vector<ChunkRange>& ranges,
                                       const ByteIndexMappings& mappings) {
  PreparedSpanShaping result;
  result.chunks.reserve(ranges.size());
  size_t codepointCount = 0;
  for (size_t pos = 0; pos < text.size(); ++codepointCount) {
    decodeUtf8(text, pos);
  }
  result.groupStarts.resize(codepointCount, false);
  for (const auto& range : ranges) {
    PreparedSpanChunk prepared;
    prepared.shaped = shaper.shape(range, prepared.faces);
    prepared.positioningClusters.reserve(prepared.shaped.glyphs.size());
    for (size_t index = 0; index < prepared.shaped.glyphs.size(); ++index) {
      const auto& glyph = prepared.shaped.glyphs[index];
      uint32_t positioningCluster = glyph.cluster;
      // The simple backend reports marks separately. Preserve a zero-advance mark
      // with its preceding group without merging a separately shaped spacing glyph.
      if (index != 0 && glyph.cluster < text.size() && glyph.xAdvance == 0.0) {
        size_t position = glyph.cluster;
        if (isNonSpacing(decodeUtf8(text, position))) {
          positioningCluster = prepared.positioningClusters.back();
        }
      }
      prepared.positioningClusters.push_back(positioningCluster);
      if (positioningCluster < mappings.byteToCharIdx.size()) {
        result.groupStarts[mappings.byteToCharIdx[positioningCluster]] = true;
      }
    }
    result.chunks.push_back(std::move(prepared));
  }
  return result;
}

/// Word spacing advances only after every glyph attached to the space group.
bool IsWordSpacingGroupEnd(const PreparedSpanChunk& chunk, size_t index, uint32_t codepoint) {
  return codepoint == 0x20 &&
         (index + 1 == chunk.positioningClusters.size() ||
          chunk.positioningClusters[index] != chunk.positioningClusters[index + 1]);
}

/// Applies internal displacements to the following group, retaining end-of-span carry.
SmallVector<std::optional<Lengthd>, 1> ResolveGroupDisplacements(
    const SmallVector<std::optional<Lengthd>, 1>& values, const std::vector<bool>& groupStarts,
    Lengthd::Extent extent, const TextLayoutParams& params, double& pending) {
  SmallVector<std::optional<Lengthd>, 1> result;
  result.resize(groupStarts.size());
  for (size_t index = 0; index < groupStarts.size(); ++index) {
    if (index < values.size() && values[index]) {
      pending += values[index]->toPixels(params.viewBox, params.fontMetrics, extent);
    }
    if (groupStarts[index]) {
      if (pending != 0.0) {
        result[index] = Lengthd(pending);
      }
      pending = 0.0;
    }
  }
  return result;
}

/// Returns a group's authored position only on its first glyph.
std::optional<Lengthd> PositionForGroup(const SmallVector<std::optional<Lengthd>, 1>& values,
                                        unsigned int index, bool firstGlyph) {
  return firstGlyph && index < values.size() ? values[index] : std::nullopt;
}

/// A valid computed zero retains logical characters without shaping at size zero.
bool HasAddressableSpanText(std::string_view text, float size, double specifiedSize,
                            const std::optional<double>& adjustment) {
  return !text.empty() &&
         (HasRenderableSpanText(text, size) || specifiedSize == 0.0 || adjustment == 0.0);
}

/// A zero used size needs no font scale query.
float UsedSpanEmScale(const TextBackend& backend, FontHandle font, float size) {
  return size > 0.0f ? backend.scaleForEmToPixels(font, size) : 0.0f;
}

/// Nonempty unshaped text interrupts kerning; an empty span keeps the previous pair.
void UpdateUnshapedSpanPredecessor(std::string_view text, const TextRun& run, bool kerningEnabled,
                                   uint32_t& previousCodepoint, FontHandle& previousFont,
                                   float& previousSizePx, bool& previousKerning) {
  if (text.empty()) {
    return;
  }
  previousCodepoint = 0;
  previousFont = run.font;
  previousSizePx = run.usedFontSizePx;
  previousKerning = kerningEnabled;
}

}  // namespace

ResolvedTextFont TextEngine::resolveUsedFont(EntityHandle styleOwner, const Box2d& viewBox,
                                             const FontMetrics& fontMetrics) const {
  UTILS_RELEASE_ASSERT(styleOwner.registry() == &registry_);
  const auto* style = registry_.try_get<components::ComputedStyleComponent>(styleOwner.entity());
  if (!style || !style->properties) {
    return {};
  }
  const auto& properties = *style->properties;
  components::ComputedTextComponent::TextSpan span;
  span.fontFamilies = properties.fontFamily.get().value();
  span.fontWeight = properties.fontWeight.get().value();
  span.fontStyle = properties.fontStyle.get().value();
  span.fontStretch = static_cast<FontStretch>(properties.fontStretch.get().value());
  FontHandle font =
      ResolveSpanFace(fontManager_, span, span.fontFamilies, fontManager_.fallbackFont());
  font = selectBackendSafeFont(*backend_, fontManager_, font);
  const float size = CheckedFontSizePx(
      properties.fontSize.get().value().toPixels(viewBox, fontMetrics, Lengthd::Extent::Mixed));
  return {font, AdjustFontSize(*backend_, font, size, properties.fontSizeAdjust.get().value())};
}

namespace {

bool HasInitialTextPosition(const SmallVector<std::optional<Lengthd>, 1>& positions) {
  return !positions.empty() && positions.front().has_value();
}

class TextLayoutInvocation {
public:
  TextLayoutInvocation(Registry& registry, FontManager& fontManager, TextBackend& backend,
                       const components::ComputedTextComponent& text,
                       const TextLayoutParams& params, bool* resourceRejected = nullptr,
                       std::size_t* fallbackCoverageEntries = nullptr)
      : registry_(registry),
        fontManager_(fontManager),
        backend_(backend),
        text_(text),
        params_(params),
        resourceRejected_(resourceRejected),
        fallbackCoverageEntries_(fallbackCoverageEntries) {}

  std::vector<TextRun> run() {
    resolveBaseFont();
    for (const auto& span : text_.spans) {
      layoutSpan(span);
    }
    finishTextPath();
    applyInlineWrap();
    applyFinalAdjustments();
    ApplyPostTextPathTranslations(runs_, text_, postPathBoundaries_);
    if (fallbackCoverageEntries_) {
      *fallbackCoverageEntries_ = fallbackCoverage_.entries();
    }
    return std::move(runs_);
  }

private:
  using TextSpan = components::ComputedTextComponent::TextSpan;

  struct SpanState {
    SpanState() = default;
    SpanState(const SpanState&) = delete;
    SpanState& operator=(const SpanState&) = delete;
    SpanState(SpanState&&) = delete;
    SpanState& operator=(SpanState&&) = delete;

    const TextSpan* span = nullptr;
    TextRun run;
    /// Aliases source text or owned shaping buffers for this immovable stack frame.
    std::string_view text;
    std::string controlShapingText;
    std::optional<text_transform::FullWidthText> fullWidthText;
    FontHandle font;
    double specifiedFontSizePx = 0.0;
    float fontSizePx = 0.0f;
    float scale = 0.0f;
    std::optional<double> fontSizeAdjust;
    bool vertical = false;
    TextOrientation orientation = TextOrientation::Mixed;
    bool fontKerning = true;
    double defaultY = 0.0;
    double verticalBaselineOffset = 0.0;
    double penX = 0.0;
    double penY = 0.0;
    double initialInlineDisplacement = 0.0;
    double runPenStartX = 0.0;
    double runPenStartY = 0.0;
    SmallVector<std::optional<Lengthd>, 1> xList;
    SmallVector<std::optional<Lengthd>, 1> yList;
    SmallVector<std::optional<Lengthd>, 1> dxList;
    SmallVector<std::optional<Lengthd>, 1> dyList;
    std::vector<ChunkRange> chunkRanges;
    std::vector<bool> forcedSeparators;
    ByteIndexMappings indexMappings;
    PreparedSpanShaping shaping;
    uint32_t lastCodepoint = 0;
    FontHandle lastFont;  ///< Face of the last laid-out glyph.
    double pathBoundaryKern = 0.0;
    uint32_t previousChunkCodepoint = 0;
    FontHandle previousChunkFont;
    float previousChunkFontSizePx = 0.0f;
    bool previousChunkKerning = true;
  };

  struct KernAdjustment {
    double value = 0.0;
    bool applied = false;
  };

  void resolveBaseFont() {
    baseFont_ = FindFirstAvailableFont(fontManager_, params_.fontFamilies);
    if (!baseFont_) {
      baseFont_ = fontManager_.fallbackFont();
    }
    baseFontSizePx_ = CheckedFontSizePx(
        params_.fontSize.toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::Mixed));
  }

  bool startsDifferentTextPath(const TextSpan& span) const {
    return firstPathRun_ && !span.hidden &&
           (!span.pathSpline || span.textPathSourceEntity == entt::null ||
            span.textPathSourceEntity != previousTextPathSource_);
  }

  void layoutSpan(const TextSpan& span) {
    if (startsDifferentTextPath(span)) {
      finishTextPath();
    }
    if (span.hidden || span.textPathFailed) {
      commitHiddenSpan();
      return;
    }

    SpanState state;
    prepareSpan(span, state);
    if (!HasAddressableSpanText(state.text, state.fontSizePx, state.specifiedFontSizePx,
                                state.fontSizeAdjust)) {
      commitUnshapedSpan(state);
      return;
    }

    if (!prepareShaping(state)) {
      commitUnshapedSpan(state);
      return;
    }
    layoutChunks(state);
    commitShapedSpan(state);
  }

  void commitHiddenSpan() {
    runExtents_.push_back({0.0, 0.0, 0.0, 0.0});
    runs_.emplace_back();
  }

  void prepareSpan(const TextSpan& span, SpanState& state) {
    previousSpanCodepoint_ =
        KerningPredecessorForScope(previousSpanPathSource_, span, previousSpanCodepoint_);
    state.span = &span;
    state.text = std::string_view(span.text.data() + span.start, span.end - span.start);
    prepareSpanFont(state);
    prepareSpanPen(state);
    state.fontKerning = span.fontKerning.value_or(params_.fontKerning) != FontKerning::None;
  }

  void prepareSpanFont(SpanState& state) {
    const TextSpan& span = *state.span;
    state.font = ResolveSpanFace(fontManager_, span, params_.fontFamilies, baseFont_);
    state.specifiedFontSizePx = ResolvedSpanFontSizePx(span, params_);
    state.fontSizePx = CheckedFontSizePx(state.specifiedFontSizePx);
    state.font = selectBackendSafeFont(backend_, fontManager_, state.font);
    state.fontSizeAdjust = span.fontSizeAdjust.value_or(params_.fontSizeAdjust);
    state.fontSizePx = AdjustFontSize(backend_, state.font, state.fontSizePx, state.fontSizeAdjust);
    state.run.font = state.font;
    state.run.usedFontSizePx = state.fontSizePx;
    state.scale = UsedSpanEmScale(backend_, state.font, state.fontSizePx);
  }

  double effectiveBaselineShift(const SpanState& state) const {
    if (!isVertical(params_.writingMode) &&
        state.span->alignmentBaseline != DominantBaseline::Auto) {
      return computeBaselineShift(state.span->alignmentBaseline, backend_.fontVMetrics(state.font),
                                  state.scale);
    }
    return 0.0;
  }

  /// Distance from the alphabetic to the central baseline, which CSS Inline 3 Appendix A.2 places
  /// halfway between ideographic-over and -under. Without those metrics this uses the unnormalized
  /// ascent/descent midpoint, the A.2 CJK fallback that Chromium and resvg also use.
  /// @param metrics Vertical metrics of the face drawing the glyph.
  /// @param scale Em-to-pixel scale of that face.
  double verticalCentralOriginOffset(const FontVMetrics& metrics, float scale) const {
    return computeBaselineShift(DominantBaseline::Central, metrics, scale);
  }

  double resolveVerticalBaselineOffset(const SpanState& state, double baselineShift) const {
    if (!state.vertical || state.span->pathSpline) {
      return 0.0;
    }
    const FontVMetrics metrics = backend_.fontVMetrics(state.font);
    const DominantBaseline baseline = state.span->alignmentBaseline;
    const bool usesCentralOrigin =
        baseline == DominantBaseline::Auto || baseline == DominantBaseline::Central ||
        baseline == DominantBaseline::UseScript || baseline == DominantBaseline::ResetSize ||
        baseline == DominantBaseline::NoChange;
    const double alignmentShift = usesCentralOrigin
                                      ? 0.0
                                      : verticalCentralOriginOffset(metrics, state.scale) -
                                            computeBaselineShift(baseline, metrics, state.scale);
    // Baseline alignment is transversal to the inline axis. In both vertical writing modes,
    // line-over is right and line-under is left, so a positive horizontal baseline distance
    // maps to negative X. The alignment offset above is already expressed in physical X;
    // positive baseline-shift then raises the aligned glyph toward line-over.
    return alignmentShift + baselineShift;
  }

  void prepareSpanPen(SpanState& state) {
    const TextSpan& span = *state.span;
    state.vertical = isVertical(params_.writingMode);
    state.orientation =
        !state.vertical || span.pathSpline ? TextOrientation::Mixed : span.textOrientation;
    state.penX = haveCurrentPosition_ ? currentPenX_ : 0.0;
    const double baselineShift =
        computeSpanBaselineShiftPx(backend_, span, state.font, state.scale, params_);
    state.verticalBaselineOffset = resolveVerticalBaselineOffset(state, baselineShift);
    state.defaultY =
        effectiveBaselineShift(state) - (state.vertical && !span.pathSpline ? 0.0 : baselineShift);
    state.penY =
        haveCurrentPosition_ ? currentPenY_ - previousDefaultY_ + state.defaultY : state.defaultY;
    state.xList = span.xList;
    state.yList = span.yList;
    if (span.pathSpline && !state.vertical) {
      state.yList.clear();
    }
    state.dxList = span.dxList;
    state.dyList = span.dyList;
    if (span.startsNewChunk || !haveCurrentPosition_) {
      const bool absoluteX = HasInitialTextPosition(state.xList);
      const bool absoluteY = HasInitialTextPosition(state.yList);
      state.initialInlineDisplacement = spanStartInlineDisplacement(state);
      consumeSpanStartPositions(state);
      chunkBoundaries_.push_back({runs_.size(), 0, span.textAnchor, absoluteX, absoluteY});
    }
  }

  double spanStartInlineDisplacement(const SpanState& state) const {
    const auto& values = state.vertical ? state.dyList : state.dxList;
    return !values.empty() && values[0]
               ? values[0]->toPixels(params_.viewBox, params_.fontMetrics,
                                     state.vertical ? Lengthd::Extent::Y : Lengthd::Extent::X)
               : 0.0;
  }

  void consumeSpanStartPositions(SpanState& state) const {
    if (!state.xList.empty() && state.xList[0]) {
      state.penX =
          state.xList[0]->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::X);
      state.xList[0].reset();
    }
    if (!state.dxList.empty() && state.dxList[0]) {
      state.penX +=
          state.dxList[0]->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::X);
      state.dxList[0].reset();
    }
    if (!state.yList.empty() && state.yList[0]) {
      state.penY =
          state.yList[0]->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::Y) +
          state.defaultY;
      state.yList[0].reset();
    }
    if (!state.dyList.empty() && state.dyList[0]) {
      state.penY +=
          state.dyList[0]->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::Y);
      state.dyList[0].reset();
    }
  }

  void commitUnshapedSpan(SpanState& state) {
    currentPenX_ = state.penX;
    currentPenY_ = state.penY;
    previousDefaultY_ = state.defaultY;
    haveCurrentPosition_ = true;
    UpdateUnshapedSpanPredecessor(state.text, state.run, state.fontKerning, previousSpanCodepoint_,
                                  previousSpanFont_, previousSpanFontSizePx_, previousSpanKerning_);
    runExtents_.push_back({state.penX, state.penY, state.penX, state.penY});
    runs_.push_back(std::move(state.run));
  }

  bool prepareFullWidthShapingText(SpanState& state) {
    if (state.span->textTransform == TextTransform::FullWidth) {
      const bool preserveSpaces =
          state.span->preservesSpaces || PreservesSpaces(state.span->whiteSpace);
      if (text_transform::NeedsFullWidth(state.text, preserveSpaces)) {
        state.fullWidthText = text_transform::MakeFullWidth(state.text, preserveSpaces);
        if (!state.fullWidthText) {
          return false;
        }
        state.text = state.fullWidthText->text;
      }
    }
    return true;
  }

  bool prepareShaping(SpanState& state) {
    if (!prepareFullWidthShapingText(state)) {
      return false;
    }
    state.chunkRanges = FindSpanShapingRanges(*state.span, state.text, state.xList, state.yList);
    state.forcedSeparators =
        FindForcedShapingSeparators(*state.span, state.text, state.chunkRanges);
    if (state.text.find_first_of("\n\t") != std::string_view::npos) {
      // Shape controls as blank glyphs while keeping byte clusters for line layout and DOM APIs.
      state.controlShapingText = std::string(state.text);
      for (char& c : state.controlShapingText) {
        if (c == '\n' || c == '\t') {
          c = ' ';
        }
      }
      state.text = state.controlShapingText;
    }
    state.runPenStartX = state.penX;
    state.runPenStartY = state.penY;
    state.indexMappings = buildByteIndexMappings(state.text);
    const TextSpan& span = *state.span;
    fallbackCoverage_.grant(state.text.size());
    const ClusterFallback fallback{
        fontManager_,
        fallbackCoverage_,
        span.fontFamilies.empty() ? params_.fontFamilies : span.fontFamilies,
        span.fontWeight,
        static_cast<int>(span.fontStyle),
        static_cast<int>(span.fontStretch)};
    const ChunkShaper shaper(backend_, state.run, state.text, state.vertical, state.orientation,
                             span.fontVariant, span.fontSynthesisSmallCaps, state.fontKerning,
                             fallback);
    state.shaping = PrepareSpanShaping(shaper, state.text, state.chunkRanges, state.indexMappings);
    state.dxList = ResolveGroupDisplacements(
        state.span->pathSpline ? state.span->dxList : state.dxList, state.shaping.groupStarts,
        Lengthd::Extent::X, params_, pendingDx_);
    state.dyList = ResolveGroupDisplacements(
        state.span->pathSpline ? state.span->dyList : state.dyList, state.shaping.groupStarts,
        Lengthd::Extent::Y, params_, pendingDy_);
    state.previousChunkCodepoint = previousSpanCodepoint_;
    state.previousChunkFont = previousSpanFont_;
    state.previousChunkFontSizePx = previousSpanFontSizePx_;
    state.previousChunkKerning = previousSpanKerning_;
    return true;
  }

  void layoutChunks(SpanState& state) {
    for (size_t chunkIndex = 0; chunkIndex < state.chunkRanges.size(); ++chunkIndex) {
      layoutChunk(state, chunkIndex);
    }
  }

  void layoutChunk(SpanState& state, size_t chunkIndex) {
    const PreparedSpanChunk& prepared = state.shaping.chunks[chunkIndex];
    const auto yOverride = chunkYOverride(state, chunkIndex);
    const KernAdjustment crossKern = resolveCrossKern(state, chunkIndex);
    std::optional<unsigned int> previousPositionedGroup;
    for (size_t glyphIndex = 0; glyphIndex < prepared.shaped.glyphs.size(); ++glyphIndex) {
      layoutGlyph(state, chunkIndex, glyphIndex, yOverride, crossKern, previousPositionedGroup);
    }
    updateChunkPredecessor(state, prepared);
  }

  std::optional<double> chunkYOverride(const SpanState& state, size_t chunkIndex) const {
    const auto& prepared = state.shaping.chunks[chunkIndex];
    const auto& shaped = prepared.shaped;
    const bool isRtl = shaped.glyphs.size() > 1 &&
                       prepared.positioningClusters.front() > prepared.positioningClusters.back();
    if (!isRtl || chunkIndex == 0) {
      return std::nullopt;
    }
    const size_t byteStart = state.chunkRanges[chunkIndex].byteStart;
    const unsigned int firstChar = byteStart < state.indexMappings.byteToCharIdx.size()
                                       ? state.indexMappings.byteToCharIdx[byteStart]
                                       : 0;
    if (firstChar >= state.yList.size() || !state.yList[firstChar]) {
      return std::nullopt;
    }
    return state.yList[firstChar]->toPixels(params_.viewBox, params_.fontMetrics,
                                            Lengthd::Extent::Y) +
           state.defaultY;
  }

  bool isForcedSeparator(const SpanState& state, size_t chunkIndex) const {
    return chunkIndex < state.forcedSeparators.size() && state.forcedSeparators[chunkIndex];
  }

  KernAdjustment resolveCrossKern(const SpanState& state, size_t chunkIndex) const {
    if (isForcedSeparator(state, chunkIndex)) {
      return {};
    }
    if (chunkIndex > 0) {
      return resolveCrossChunkKern(state, chunkIndex);
    }
    return resolveCrossSpanKern(state);
  }

  KernAdjustment resolveCrossChunkKern(const SpanState& state, size_t chunkIndex) const {
    const auto& glyphs = state.shaping.chunks[chunkIndex].shaped.glyphs;
    if (state.previousChunkCodepoint == 0 || glyphs.empty()) {
      return {};
    }
    size_t firstByte = state.chunkRanges[chunkIndex].byteStart;
    const uint32_t firstCodepoint = decodeUtf8(state.text, firstByte);
    const double value =
        state.fontKerning && state.previousChunkKerning
            ? backend_.crossSpanKern(state.previousChunkFont, state.previousChunkFontSizePx,
                                     state.shaping.chunks[chunkIndex].drawingFace(0, state.font),
                                     state.fontSizePx, state.previousChunkCodepoint, firstCodepoint,
                                     state.vertical, state.orientation)
            : 0.0;
    return {value, true};
  }

  KernAdjustment resolveCrossSpanKern(const SpanState& state) const {
    if (StartsNewKerningChunk(*state.span, state.vertical) || previousSpanCodepoint_ == 0 ||
        state.shaping.chunks.front().shaped.glyphs.empty()) {
      return {};
    }
    size_t firstByte = state.chunkRanges.front().byteStart;
    const uint32_t firstCodepoint = decodeUtf8(state.text, firstByte);
    const FontHandle firstFace = state.shaping.chunks.front().drawingFace(0, state.font);
    const bool compatible =
        previousSpanOrientation_ == state.orientation &&
        CompatibleKerningRuns(
            fontManager_, firstFace, state.run.usedFontSizePx,
            EffectiveSmallCapsVariant(backend_, firstFace, state.span->fontVariant,
                                      state.span->fontSynthesisSmallCaps),
            previousSpanFont_, previousSpanFontSizePx_, previousSpanFontVariant_, state.fontKerning,
            previousSpanKerning_);
    const double value =
        compatible ? backend_.crossSpanKern(previousSpanFont_, previousSpanFontSizePx_, firstFace,
                                            state.fontSizePx, previousSpanCodepoint_,
                                            firstCodepoint, state.vertical, state.orientation)
                   : 0.0;
    return {value, true};
  }

  void layoutGlyph(SpanState& state, size_t chunkIndex, size_t glyphIndex,
                   const std::optional<double>& yOverride, const KernAdjustment& crossKern,
                   std::optional<unsigned int>& previousPositionedGroup) {
    const PreparedSpanChunk& prepared = state.shaping.chunks[chunkIndex];
    const uint32_t positioningCluster = prepared.positioningClusters[glyphIndex];
    size_t clusterPosition = positioningCluster;
    const uint32_t codepoint = decodeUtf8(state.text, clusterPosition);
    const unsigned int charIndex = positioningCluster < state.indexMappings.byteToCharIdx.size()
                                       ? state.indexMappings.byteToCharIdx[positioningCluster]
                                       : 0;
    const bool applyPosition = previousPositionedGroup != charIndex;
    previousPositionedGroup = charIndex;
    const auto groupX = PositionForGroup(state.xList, charIndex, applyPosition);
    const auto groupY = PositionForGroup(state.yList, charIndex, applyPosition);
    const auto groupDx = PositionForGroup(state.dxList, charIndex, applyPosition);
    const auto groupDy = PositionForGroup(state.dyList, charIndex, applyPosition);
    recordInlineDisplacement(state, chunkIndex, glyphIndex, groupDx, groupDy);
    if (state.vertical) {
      layoutVerticalGlyph(state, chunkIndex, glyphIndex, codepoint, charIndex, positioningCluster,
                          groupX, groupY, groupDx, groupDy, crossKern);
    } else {
      layoutHorizontalGlyph(state, chunkIndex, glyphIndex, codepoint, charIndex, positioningCluster,
                            groupX, groupY, groupDx, groupDy, yOverride, crossKern);
    }
    state.lastCodepoint = isForcedSeparator(state, chunkIndex) ? 0 : codepoint;
    state.lastFont = prepared.drawingFace(glyphIndex, state.font);
  }

  void recordInlineDisplacement(const SpanState& state, size_t chunkIndex, size_t glyphIndex,
                                const std::optional<Lengthd>& dx,
                                const std::optional<Lengthd>& dy) {
    const auto& value = state.vertical ? dy : dx;
    double distance =
        value ? value->toPixels(params_.viewBox, params_.fontMetrics,
                                state.vertical ? Lengthd::Extent::Y : Lengthd::Extent::X)
              : 0.0;
    if (chunkIndex == 0 && glyphIndex == 0) {
      distance += state.initialInlineDisplacement;
    }
    if (distance != 0.0) {
      inlineDisplacements_.push_back({runs_.size(), state.run.glyphs.size(), distance});
    }
  }

  void recordChunkBoundary(const SpanState& state, size_t chunkIndex, size_t glyphIndex,
                           bool hasAbsoluteX, bool hasAbsoluteY) {
    if ((hasAbsoluteX || hasAbsoluteY) && !(chunkIndex == 0 && glyphIndex == 0)) {
      chunkBoundaries_.push_back({runs_.size(), state.run.glyphs.size(), state.span->textAnchor,
                                  hasAbsoluteX, hasAbsoluteY});
    }
  }

  void applyVerticalPosition(SpanState& state, size_t glyphIndex,
                             const TextBackend::ShapedGlyph& shapedGlyph,
                             const std::optional<Lengthd>& groupX,
                             const std::optional<Lengthd>& groupY,
                             const std::optional<Lengthd>& groupDx,
                             const std::optional<Lengthd>& groupDy,
                             const KernAdjustment& crossKern) const {
    if (groupY) {
      state.penY = groupY->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::Y);
    }
    if (groupDy) {
      state.penY += groupDy->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::Y);
    }
    if (groupX) {
      state.penX = groupX->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::X);
    }
    if (groupDx) {
      state.penX += groupDx->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::X);
    }
    if (!groupX && !groupY) {
      if (glyphIndex == 0 && crossKern.applied) {
        state.penY += crossKern.value;
      } else if (glyphIndex != 0) {
        state.penY += shapedGlyph.yKern;
      }
    }
  }

  double rotationFor(const TextSpan& span, unsigned int charIndex, double baseRotation) const {
    if (charIndex < span.rotateList.size()) {
      return span.rotateList[charIndex] + baseRotation;
    }
    if (!span.rotateList.empty()) {
      return span.rotateList.back() + baseRotation;
    }
    return baseRotation;
  }

  void orientVerticalGlyph(const SpanState& state, const TextBackend::ShapedGlyph& shapedGlyph,
                           uint32_t codepoint, unsigned int charIndex, TextGlyph& glyph) const {
    const bool sideways = state.orientation == TextOrientation::Sideways ||
                          (state.orientation == TextOrientation::Mixed && codepoint < 0x2E80);
    if (sideways) {
      glyph.yAdvance = shapedGlyph.xAdvance > 0 ? shapedGlyph.xAdvance : shapedGlyph.yAdvance;
      glyph.xPosition -= shapedGlyph.yOffset;
      glyph.yPosition += shapedGlyph.xOffset;
      // A fallback glyph centers on its own face's central baseline.
      const FontHandle face = glyph.font ? glyph.font : state.font;
      const float scale =
          glyph.font ? UsedSpanEmScale(backend_, face, state.fontSizePx) : state.scale;
      glyph.xPosition -= verticalCentralOriginOffset(backend_.fontVMetrics(face), scale);
      glyph.rotateDegrees = rotationFor(*state.span, charIndex, 90.0);
      glyph.sideways = true;
      return;
    }

    glyph.yAdvance = shapedGlyph.yAdvance > 0
                         ? shapedGlyph.yAdvance
                         : (shapedGlyph.xAdvance == 0 ? 0.0 : state.fontSizePx);
    glyph.xPosition = state.penX + shapedGlyph.xOffset;
    glyph.yPosition = state.penY + shapedGlyph.yOffset;
    glyph.verticalOriginOffset = shapedGlyph.yOffset;
    glyph.rotateDegrees = rotationFor(*state.span, charIndex, 0.0);
  }

  void advanceVerticalPen(SpanState& state, const PreparedSpanChunk& prepared, size_t glyphIndex,
                          uint32_t codepoint, const TextGlyph& glyph) const {
    state.penY += glyph.yAdvance;
    if (!backend_.isCursive(codepoint)) {
      state.penY += state.span->letterSpacingPx;
    }
    if (IsWordSpacingGroupEnd(prepared, glyphIndex, codepoint)) {
      state.penY += state.span->wordSpacingPx;
    }
  }

  void layoutVerticalGlyph(SpanState& state, size_t chunkIndex, size_t glyphIndex,
                           uint32_t codepoint, unsigned int charIndex, uint32_t positioningCluster,
                           const std::optional<Lengthd>& groupX,
                           const std::optional<Lengthd>& groupY,
                           const std::optional<Lengthd>& groupDx,
                           const std::optional<Lengthd>& groupDy, const KernAdjustment& crossKern) {
    const PreparedSpanChunk& prepared = state.shaping.chunks[chunkIndex];
    const auto& shapedGlyph = prepared.shaped.glyphs[glyphIndex];
    recordChunkBoundary(state, chunkIndex, glyphIndex, groupX.has_value(), groupY.has_value());
    applyVerticalPosition(state, glyphIndex, shapedGlyph, groupX, groupY, groupDx, groupDy,
                          crossKern);
    TextGlyph glyph;
    glyph.glyphIndex = shapedGlyph.glyphIndex;
    glyph.xPosition = state.penX;
    glyph.yPosition = state.penY;
    glyph.xAdvance = 0;
    glyph.fontSizeScale = shapedGlyph.fontSizeScale;
    glyph.cluster = shapedGlyph.cluster;
    glyph.positioningCluster = positioningCluster;
    glyph.font = prepared.face(glyphIndex);
    orientVerticalGlyph(state, shapedGlyph, codepoint, charIndex, glyph);
    glyph.xPosition += state.verticalBaselineOffset;
    state.run.glyphs.push_back(glyph);
    advanceVerticalPen(state, prepared, glyphIndex, codepoint, glyph);
  }

  void applyHorizontalKerning(SpanState& state, size_t glyphIndex,
                              const TextBackend::ShapedGlyph& shapedGlyph, bool hasAbsoluteX,
                              bool hasAbsoluteY, const KernAdjustment& crossKern) const {
    if (hasAbsoluteX || hasAbsoluteY) {
      return;
    }
    if (glyphIndex == 0 && crossKern.applied) {
      state.penX += crossKern.value;
      state.pathBoundaryKern = crossKern.value;
    } else if (glyphIndex != 0) {
      state.penX += shapedGlyph.xKern;
    }
  }

  void applyHorizontalPosition(SpanState& state, const std::optional<Lengthd>& groupX,
                               const std::optional<Lengthd>& groupY,
                               const std::optional<Lengthd>& groupDx,
                               const std::optional<Lengthd>& groupDy,
                               const std::optional<double>& yOverride) const {
    if (groupX) {
      state.penX = groupX->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::X);
    }
    if (groupDx) {
      state.penX += groupDx->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::X);
    }
    if (yOverride) {
      state.penY = *yOverride;
    } else if (groupY) {
      state.penY = groupY->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::Y) +
                   state.defaultY;
    }
    if (groupDy) {
      state.penY += groupDy->toPixels(params_.viewBox, params_.fontMetrics, Lengthd::Extent::Y);
    }
  }

  TextGlyph makeHorizontalGlyph(const SpanState& state, const TextBackend::ShapedGlyph& shapedGlyph,
                                uint32_t positioningCluster, unsigned int charIndex) const {
    TextGlyph glyph;
    glyph.glyphIndex = shapedGlyph.glyphIndex;
    glyph.xPosition =
        state.span->pathSpline ? shapedGlyph.xOffset : state.penX + shapedGlyph.xOffset;
    glyph.yPosition =
        state.span->pathSpline ? shapedGlyph.yOffset : state.penY + shapedGlyph.yOffset;
    glyph.xAdvance = shapedGlyph.xAdvance;
    glyph.yAdvance = shapedGlyph.yAdvance;
    glyph.xKern = shapedGlyph.xKern;
    glyph.fontSizeScale = shapedGlyph.fontSizeScale;
    glyph.cluster = shapedGlyph.cluster;
    glyph.positioningCluster = positioningCluster;
    glyph.rotateDegrees = rotationFor(*state.span, charIndex, 0.0);
    return glyph;
  }

  void rotateDetachedMark(const SpanState& state, bool hasAbsoluteX, bool hasAbsoluteY,
                          TextGlyph& glyph) const {
    if (state.span->pathSpline || hasAbsoluteX || hasAbsoluteY || glyph.rotateDegrees == 0.0 ||
        state.run.glyphs.empty() || glyph.xAdvance != 0.0) {
      return;
    }
    const auto& base = state.run.glyphs.back();
    if (base.xAdvance == 0.0) {
      return;
    }
    const double angle = glyph.rotateDegrees * MathConstants<double>::kDegToRad;
    const double dx = glyph.xPosition - base.xPosition;
    const double dy = glyph.yPosition - base.yPosition;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    glyph.xPosition = base.xPosition + dx * cosine - dy * sine;
    glyph.yPosition = base.yPosition + dx * sine + dy * cosine;
  }

  void advanceHorizontalPen(SpanState& state, const PreparedSpanChunk& prepared, size_t glyphIndex,
                            uint32_t codepoint, const TextBackend::ShapedGlyph& shapedGlyph) const {
    state.penX += shapedGlyph.xAdvance;
    if (!backend_.isCursive(codepoint)) {
      state.penX += state.span->letterSpacingPx;
    }
    if (IsWordSpacingGroupEnd(prepared, glyphIndex, codepoint)) {
      state.penX += state.span->wordSpacingPx;
    }
  }

  void layoutHorizontalGlyph(SpanState& state, size_t chunkIndex, size_t glyphIndex,
                             uint32_t codepoint, unsigned int charIndex,
                             uint32_t positioningCluster, const std::optional<Lengthd>& groupX,
                             const std::optional<Lengthd>& groupY,
                             const std::optional<Lengthd>& groupDx,
                             const std::optional<Lengthd>& groupDy,
                             const std::optional<double>& yOverride,
                             const KernAdjustment& crossKern) {
    const PreparedSpanChunk& prepared = state.shaping.chunks[chunkIndex];
    const auto& shapedGlyph = prepared.shaped.glyphs[glyphIndex];
    const bool hasAbsoluteX = groupX.has_value();
    const bool hasAbsoluteY = groupY.has_value();
    recordChunkBoundary(state, chunkIndex, glyphIndex, hasAbsoluteX, hasAbsoluteY);
    applyHorizontalKerning(state, glyphIndex, shapedGlyph, hasAbsoluteX, hasAbsoluteY, crossKern);
    applyHorizontalPosition(state, groupX, groupY, groupDx, groupDy, yOverride);
    TextGlyph glyph = makeHorizontalGlyph(state, shapedGlyph, positioningCluster, charIndex);
    glyph.font = prepared.face(glyphIndex);
    rotateDetachedMark(state, hasAbsoluteX, hasAbsoluteY, glyph);
    state.run.glyphs.push_back(glyph);
    advanceHorizontalPen(state, prepared, glyphIndex, codepoint, shapedGlyph);
  }

  void updateChunkPredecessor(SpanState& state, const PreparedSpanChunk& prepared) const {
    if (prepared.shaped.glyphs.empty()) {
      return;
    }
    state.previousChunkCodepoint = state.lastCodepoint;
    state.previousChunkFont = prepared.drawingFace(prepared.shaped.glyphs.size() - 1, state.font);
    state.previousChunkFontSizePx = state.fontSizePx;
    state.previousChunkKerning = state.fontKerning;
  }

  void updateSpanPredecessor(const SpanState& state) {
    previousSpanCodepoint_ = state.lastCodepoint;
    previousSpanFont_ = state.lastFont ? state.lastFont : state.font;
    previousSpanFontSizePx_ = state.fontSizePx;
    previousSpanKerning_ = state.fontKerning;
    previousSpanFontVariant_ = EffectiveSmallCapsVariant(
        backend_, previousSpanFont_, state.span->fontVariant, state.span->fontSynthesisSmallCaps);
    previousSpanOrientation_ = state.orientation;
    previousSpanPathSource_ = SpanKerningPathSource(*state.span);
  }

  void updateCurrentPen(const SpanState& state) {
    currentPenX_ = state.penX;
    currentPenY_ = state.penY;
    previousDefaultY_ = state.defaultY;
    haveCurrentPosition_ = true;
  }

  void stageTextPathSpan(SpanState& state) {
    if (!firstPathRun_) {
      firstPathRun_ = runs_.size();
      previousTextPathSource_ = state.span->textPathSourceEntity;
    }
    const std::string_view logicalText =
        state.fullWidthText ? std::string_view(state.fullWidthText->text)
                            : std::string_view(state.span->text.data() + state.span->start,
                                               state.span->end - state.span->start);
    StageTextPathRun(state.run, runs_.size(), *state.span, logicalText, state.indexMappings,
                     params_, backend_, state.defaultY, state.dxList, state.dyList,
                     state.pathBoundaryKern, pathStaging_);
    state.run.onPath = true;
  }

  /// Restore source byte indices only after shaping and path staging have consumed transformed
  /// text.
  void restoreSourceClusters(SpanState& state) const {
    if (!state.fullWidthText) {
      return;
    }
    const auto sourceCluster = [&state](uint32_t byte) -> uint32_t {
      if (byte >= state.indexMappings.byteToCharIdx.size()) {
        return static_cast<uint32_t>(state.span->end - state.span->start);
      }
      return state.fullWidthText->sourceByteOffsets[state.indexMappings.byteToCharIdx[byte]];
    };
    for (auto& glyph : state.run.glyphs) {
      glyph.cluster = sourceCluster(glyph.cluster);
      if (glyph.positioningCluster) {
        glyph.positioningCluster = sourceCluster(*glyph.positioningCluster);
      }
    }
  }

  void commitShapedSpan(SpanState& state) {
    updateSpanPredecessor(state);
    updateCurrentPen(state);
    if (state.span->pathSpline) {
      stageTextPathSpan(state);
    }
    restoreSourceClusters(state);
    runExtents_.push_back({state.runPenStartX, state.runPenStartY, state.penX, state.penY});
    runs_.push_back(std::move(state.run));
  }

  bool isClosedTextPath(const Path::MeasuredPath& measuredPath, const TextSpan& span) const {
    const double length = measuredPath.pathLength();
    return measuredPath.valid() && std::isfinite(length) && length > 0.0 &&
           IsSingleClosedTextPath(*span.pathSpline);
  }

  bool canRetainTextPath(const std::optional<size_t>& measuredBytes, bool clustersFit,
                         size_t clusterBytes) const {
    return measuredBytes && clustersFit && *measuredBytes <= kMaximumDeferredTextPathBytes &&
           clusterBytes <= kMaximumDeferredTextPathBytes - *measuredBytes &&
           deferredTextPathBytes_ <= kMaximumDeferredTextPathBytes - *measuredBytes - clusterBytes;
  }

  void retainTextPath(Path::MeasuredPath measuredPath, bool closed, size_t retainedBytes,
                      std::shared_ptr<TextPathMeasurementReservation> measurementReservation) {
    deferredTextPathBytes_ += retainedBytes;
    deferredTextPaths_.push_back({*firstPathRun_, runs_.size(), std::move(measuredPath), closed,
                                  std::move(pathStaging_.clusters),
                                  std::move(measurementReservation)});
  }

  void discardTextPathRuns() {
    for (size_t runIndex = *firstPathRun_; runIndex < runs_.size(); ++runIndex) {
      runs_[runIndex].glyphs.clear();
    }
  }

  void resetTextPathState() {
    pathStaging_.clusters.clear();
    firstPathRun_.reset();
    previousTextPathSource_ = entt::null;
    pathStaging_.advance = 0.0;
    pathStaging_.dy = 0.0;
    previousSpanCodepoint_ = 0;
  }

  std::optional<std::pair<size_t, size_t>> textPathLocalCapacity() const {
    if (pathStaging_.clusters.capacity() >
        kMaximumDeferredTextPathBytes / sizeof(TextPathCluster)) {
      return std::nullopt;
    }
    const size_t clusterBytes = pathStaging_.clusters.capacity() * sizeof(TextPathCluster);
    if (deferredTextPathBytes_ > kMaximumDeferredTextPathBytes ||
        clusterBytes > kMaximumDeferredTextPathBytes - deferredTextPathBytes_) {
      return std::nullopt;
    }
    return std::pair(kMaximumDeferredTextPathBytes - deferredTextPathBytes_ - clusterBytes,
                     clusterBytes);
  }

  void finishTextPath() {
    if (!firstPathRun_) {
      return;
    }
    const TextSpan& firstSpan = text_.spans[*firstPathRun_];
    constexpr size_t kMappingOverhead = sizeof(StretchedTextPath) + 2u * sizeof(void*);
    const Path& sourcePath = *firstSpan.pathSpline;
    const auto localCapacity = textPathLocalCapacity();
    TextPathMeasurementAdmission admission = AdmitTextPathMeasurement(
        registry_, sourcePath, localCapacity ? localCapacity->first : 0u, kMappingOverhead);
    Path::MeasuredPath measuredPath =
        admission.reservation ? sourcePath.measure(admission.workUnits) : Path::MeasuredPath();
    std::optional<size_t> measuredBytes;
    const bool measuredMappingFits = FinalizeTextPathMeasurementAdmission(
        admission, measuredPath, kMappingOverhead, measuredBytes);
    const bool closed = isClosedTextPath(measuredPath, firstSpan);
    postPathBoundaries_.push_back(
        {runs_.size(), TextPathContinuation(measuredPath, closed, firstSpan.pathStartOffset)});
    const size_t clusterBytes = localCapacity ? localCapacity->second : 0u;
    if (measuredMappingFits && localCapacity &&
        canRetainTextPath(measuredBytes, true, clusterBytes)) {
      retainTextPath(std::move(measuredPath), closed, *measuredBytes + clusterBytes,
                     std::move(admission.reservation));
    } else {
      if (resourceRejected_) {
        *resourceRejected_ = true;
      }
      discardTextPathRuns();
    }
    resetTextPathState();
  }

  std::vector<RunPenExtent> flatGlyphExtents() const {
    std::vector<RunPenExtent> result;
    result.reserve(runs_.size());
    for (const auto& run : runs_) {
      if (run.glyphs.empty() || run.onPath) {
        result.push_back({});
        continue;
      }
      const auto& first = run.glyphs.front();
      const auto& last = run.glyphs.back();
      result.push_back({first.xPosition, first.yPosition, last.xPosition + last.xAdvance,
                        last.yPosition + last.yAdvance});
    }
    return result;
  }

  void updateControlOnlyExtents(const std::vector<RunPenExtent>& before) {
    const auto after = flatGlyphExtents();
    for (size_t i = 0; i < runExtents_.size(); ++i) {
      runExtents_[i].startX += after[i].startX - before[i].startX;
      runExtents_[i].startY += after[i].startY - before[i].startY;
      runExtents_[i].endX += after[i].endX - before[i].endX;
      runExtents_[i].endY += after[i].endY - before[i].endY;
    }
    if (!runExtents_.empty()) {
      currentPenX_ = runExtents_.back().endX;
      currentPenY_ = runExtents_.back().endY;
    }
  }

  void applyInlineWrap() {
    if (runs_.empty()) {
      return;
    }
    if (!(params_.inlineSizePx > 0.0) &&
        std::none_of(text_.spans.begin(), text_.spans.end(), [](const TextSpan& span) {
          return PreservesSegmentBreaks(span.whiteSpace) &&
                 std::string_view(span.text.data() + span.start, span.end - span.start)
                         .find_first_of("\n\t") != std::string_view::npos;
        })) {
      return;
    }
    const FontVMetrics metrics = backend_.fontVMetrics(baseFont_);
    const float pixelHeightScale = backend_.scaleForPixelHeight(baseFont_, baseFontSizePx_);
    double lineHeight = static_cast<double>(metrics.ascent - metrics.descent + metrics.lineGap) *
                        static_cast<double>(pixelHeightScale);
    if (!(lineHeight > 0.0)) {
      lineHeight = static_cast<double>(baseFontSizePx_) * 1.2;
    }
    const auto before = flatGlyphExtents();
    wrapped_ =
        applyInlineSizeWrap(runs_, text_, params_, params_.inlineSizePx, lineHeight, &backend_,
                            chunkBoundaries_, inlineDisplacements_, &anchorExclusions_);
    if (!wrapped_) {
      updateControlOnlyExtents(before);
    }
  }

  void placeDeferredTextPaths() {
    for (auto& path : deferredTextPaths_) {
      placeTextPath(text_, params_, path, runs_);
    }
  }

  void applyFinalAdjustments() {
    if (runs_.empty()) {
      return;
    }
    if (wrapped_) {
      placeDeferredTextPaths();
      return;
    }
    const bool vertical = isVertical(params_.writingMode);
    const bool ownerHandled = ApplyTextLengthOwnerPlan(registry_, runs_, text_, runExtents_,
                                                       params_, vertical, deferredTextPaths_);
    if (!ownerHandled) {
      applyTextLength(runs_, text_, runExtents_, params_, vertical, currentPenX_, currentPenY_);
    }
    placeDeferredTextPaths();
    applyTextAnchor(runs_, chunkBoundaries_, text_, vertical, anchorExclusions_);
  }

  Registry& registry_;
  FontManager& fontManager_;
  TextBackend& backend_;
  const components::ComputedTextComponent& text_;
  const TextLayoutParams& params_;
  FontHandle baseFont_;
  float baseFontSizePx_ = 0.0f;
  std::vector<TextRun> runs_;
  bool haveCurrentPosition_ = false;
  double pendingDx_ = 0.0;
  double pendingDy_ = 0.0;
  double currentPenX_ = 0.0;
  double currentPenY_ = 0.0;
  double previousDefaultY_ = 0.0;
  uint32_t previousSpanCodepoint_ = 0;
  FontHandle previousSpanFont_;
  float previousSpanFontSizePx_ = 0.0f;
  bool previousSpanKerning_ = true;
  FontVariant previousSpanFontVariant_ = FontVariant::Normal;
  TextOrientation previousSpanOrientation_ = TextOrientation::Mixed;
  std::optional<Entity> previousSpanPathSource_;
  Entity previousTextPathSource_ = entt::null;
  std::optional<size_t> firstPathRun_;
  TextPathStagingState pathStaging_;
  std::vector<DeferredTextPath> deferredTextPaths_;
  size_t deferredTextPathBytes_ = 0;
  std::vector<PostTextPathBoundary> postPathBoundaries_;
  std::vector<ChunkBoundary> chunkBoundaries_;
  std::vector<InlineDisplacement> inlineDisplacements_;
  AnchorExclusions anchorExclusions_;
  std::vector<RunPenExtent> runExtents_;
  bool wrapped_ = false;
  bool* resourceRejected_ = nullptr;
  std::size_t* fallbackCoverageEntries_ = nullptr;
  FallbackCoverage fallbackCoverage_;
};

}  // namespace

std::vector<TextRun> TextEngine::layout(const components::ComputedTextComponent& text,
                                        const TextLayoutParams& params) {
  bool resourceRejected = false;
  return layoutWithResourceStatus(text, params, resourceRejected);
}

std::vector<TextRun> TextEngine::layoutWithResourceStatus(
    const components::ComputedTextComponent& text, const TextLayoutParams& params,
    bool& resourceRejected) {
  ++layoutCallCount_;
  resourceRejected = false;
  return TextLayoutInvocation(registry_, fontManager_, *backend_, text, params, &resourceRejected,
                              &fallbackCoverageEntries_)
      .run();
}

FontVMetrics TextEngine::fontVMetrics(FontHandle font) const {
  return backend_->fontVMetrics(font);
}

float TextEngine::scaleForPixelHeight(FontHandle font, float pixelHeight) const {
  return backend_->scaleForPixelHeight(font, pixelHeight);
}

float TextEngine::scaleForEmToPixels(FontHandle font, float pixelHeight) const {
  return backend_->scaleForEmToPixels(font, pixelHeight);
}

std::optional<UnderlineMetrics> TextEngine::underlineMetrics(FontHandle font) const {
  return backend_->underlineMetrics(font);
}

std::optional<UnderlineMetrics> TextEngine::strikeoutMetrics(FontHandle font) const {
  return backend_->strikeoutMetrics(font);
}

std::optional<SubSuperMetrics> TextEngine::subSuperMetrics(FontHandle font) const {
  return backend_->subSuperMetrics(font);
}

Path TextEngine::glyphOutline(FontHandle font, int glyphIndex, float scale) const {
  return backend_->glyphOutline(font, glyphIndex, scale);
}

bool TextEngine::isBitmapOnly(FontHandle font) const {
  return backend_->isBitmapOnly(font);
}

std::optional<TextBackend::BitmapGlyph> TextEngine::bitmapGlyph(FontHandle font, int glyphIndex,
                                                                float scale) const {
  return backend_->bitmapGlyph(font, glyphIndex, scale);
}

std::optional<TextBackend::BitmapGlyphMetrics> TextEngine::bitmapGlyphMetrics(FontHandle font,
                                                                              int glyphIndex,
                                                                              float scale) const {
  return backend_->bitmapGlyphMetrics(font, glyphIndex, scale);
}

std::optional<TextBackend::BitmapGlyph> TextEngine::bitmapGlyph(
    FontHandle font, int glyphIndex, float scale, const TextBackend::BitmapGlyphMetrics& admitted,
    std::size_t maximumRgbaBytes) const {
  return backend_->bitmapGlyph(font, glyphIndex, scale, admitted, maximumRgbaBytes);
}

std::optional<double> TextEngine::measureChUnitInEm(std::span<const RcString> fontFamilies,
                                                    Entity geometryOwner) {
  std::vector<FontFaceDependency> dependencies;
  const auto measured = [&]() -> std::optional<double> {
    const auto capture = fontManager_.captureDependencies(dependencies);
    FontHandle font = FindFirstAvailableFont(fontManager_, fontFamilies);
    if (!font) {
      font = fontManager_.fallbackFont();
    }

    TextBackendSimple measurementBackend(fontManager_, registry_);
    font = selectBackendSafeFont(measurementBackend, fontManager_, font);
    const auto shaped =
        measurementBackend.shapeRun(font, 1.0f, "0", 0, 1, false, FontVariant::Normal, false);
    if (shaped.glyphs.empty()) {
      return std::nullopt;
    }
    return shaped.glyphs.front().xAdvance;
  }();
  if (geometryOwner != entt::null) {
    if (dependencies.empty()) {
      registry_.remove<components::FontMetricDependenciesComponent>(geometryOwner);
    } else {
      registry_.emplace_or_replace<components::FontMetricDependenciesComponent>(
          geometryOwner, std::move(dependencies), fontManager_.fontResourceRevision());
    }
  }
  return measured;
}

components::GeometryPreparationResourceBudget& TextGeometryBudget(Registry& registry) {
  auto& context = registry.ctx();
  if (!context.contains<components::GeometryPreparationResourceBudget>()) {
    std::shared_ptr<components::DocumentResourceFamilyBudget> family;
    if (const auto* familyContext = context.find<components::DocumentResourceFamilyContext>()) {
      family = familyContext->budget;
    }
    context.emplace<components::GeometryPreparationResourceBudget>(std::move(family));
  }
  return context.get<components::GeometryPreparationResourceBudget>();
}

bool ReserveStretchedCacheOutline(Registry& registry, FontManager& fontManager, Entity owner,
                                  FontHandle font, int glyphIndex, std::size_t& retainedBytes) {
  constexpr std::size_t kExpansion = kMaximumTextPathWarpExpansion;
  const auto complexity = fontManager.glyphOutlineComplexity(font, glyphIndex);
  if (!complexity || complexity->maximumVertices > Path::kMaximumGeometryQueryWork / kExpansion ||
      complexity->work > Path::kMaximumGeometryQueryWork / kExpansion) {
    return false;
  }
  constexpr std::size_t kPerVertexBytes = sizeof(Vector2d) + sizeof(Path::Command);
  const std::size_t expandedVertices = complexity->maximumVertices * kExpansion;
  if (expandedVertices >
      (components::GeometryPreparationResourceBudget::kMaximumBytes - retainedBytes) /
          kPerVertexBytes) {
    return false;
  }
  retainedBytes += expandedVertices * kPerVertexBytes;
  return TextGeometryBudget(registry).reserve(owner, retainedBytes);
}

std::optional<Box2d> CachedTextRunCellBounds(const TextRun& run, double emTop, double emBottom) {
  Box2d result;
  bool initialized = false;
  for (const TextGlyph& glyph : run.glyphs) {
    Path cell;
    if (run.stretchedTextPath && glyph.textPathOrigin) {
      cell = PlaceStretchedTextPathCell(run, glyph, emTop, emBottom);
    } else {
      const double geometricScaleY = glyph.stretchScaleY;
      cell = PathBuilder()
                 .addRect(Box2d::FromXYWH(0.0, -emTop * geometricScaleY, glyph.xAdvance,
                                          (emTop + emBottom) * geometricScaleY))
                 .build();
      cell = transformPath(cell, Transform2d::Translate(glyph.xPosition, glyph.yPosition));
    }
    if (!cell.empty()) {
      addBox(result, initialized, cell.bounds());
    }
  }
  return initialized ? std::optional(result) : std::nullopt;
}

Path PlaceCachedGlyphOutline(Path outline, const TextRun& run, const TextGlyph& glyph) {
  if (glyph.stretchScaleX != 1.0f || glyph.stretchScaleY != 1.0f) {
    outline = transformPath(outline, Transform2d::Scale(glyph.stretchScaleX, glyph.stretchScaleY));
  }
  if (run.stretchedTextPath && glyph.textPathOrigin) {
    return PlaceStretchedTextPathOutline(std::move(outline), run, glyph);
  }
  Transform2d textElementFromGlyphLocal = Transform2d::Translate(glyph.xPosition, glyph.yPosition);
  if (glyph.rotateDegrees != 0.0) {
    textElementFromGlyphLocal =
        Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
        textElementFromGlyphLocal;
  }
  return transformPath(outline, textElementFromGlyphLocal);
}

void PopulateCachedGlyph(TextEngine& engine, Registry& registry, FontManager& fontManager,
                         Entity textRootEntity, const TextRun& run,
                         const components::ComputedTextComponent::TextSpan& span,
                         const TextGlyph& glyph, float runFontSizePx, bool hasPaintedGlyphs,
                         components::ComputedTextGeometryComponent::CharacterGeometry& charGeom,
                         components::ComputedTextGeometryComponent& cache, bool& hasInkBounds,
                         std::size_t& stretchedCacheReservation) {
  if (!charGeom.rendered) {
    const auto warpedStart =
        run.stretchedTextPath ? MapTextPathPoint(*run.stretchedTextPath, glyph, Vector2d(0.0, 0.0))
                              : std::nullopt;
    charGeom.startPosition = warpedStart.value_or(Vector2d(glyph.xPosition, glyph.yPosition));
    charGeom.rotation = glyph.rotateDegrees;
    charGeom.rendered = true;
  }
  const auto warpedEnd = run.stretchedTextPath
                             ? MapTextPathPoint(*run.stretchedTextPath, glyph,
                                                Vector2d(glyph.xAdvance, glyph.yAdvance))
                             : std::nullopt;
  charGeom.endPosition = warpedEnd.value_or(
      Vector2d(glyph.xPosition + glyph.xAdvance, glyph.yPosition + glyph.yAdvance));
  charGeom.advance += std::hypot(glyph.xAdvance, glyph.yAdvance);
  if (!hasPaintedGlyphs) {
    return;
  }

  const FontHandle font = GlyphFont(run, glyph);
  const float emScale = font ? engine.scaleForEmToPixels(font, runFontSizePx) : 0.0f;
  const bool outlineAdmitted =
      !run.stretchedTextPath ||
      ReserveStretchedCacheOutline(registry, fontManager, textRootEntity, font, glyph.glyphIndex,
                                   stretchedCacheReservation);
  Path glyphPath = outlineAdmitted
                       ? engine.glyphOutline(font, glyph.glyphIndex, emScale * glyph.fontSizeScale)
                       : Path();
  if (!glyphPath.empty()) {
    Path transformed = PlaceCachedGlyphOutline(std::move(glyphPath), run, glyph);
    if (transformed.empty()) {
      return;
    }
    const Box2d extent = transformed.bounds();
    cache.glyphs.push_back({span.sourceEntity, std::move(transformed), extent});
    addBox(cache.inkBounds, hasInkBounds, extent);
    addBox(charGeom.extent, charGeom.hasExtent, extent);
  } else if (const auto extent = BitmapGlyphExtent(
                 glyph, engine.bitmapGlyphMetrics(font, glyph.glyphIndex, emScale))) {
    addBox(cache.inkBounds, hasInkBounds, *extent);
    addBox(charGeom.extent, charGeom.hasExtent, *extent);
  }
}

void PopulateCachedTextRun(TextEngine& engine, Registry& registry, FontManager& fontManager,
                           Entity textRootEntity, const TextRun& run,
                           const components::ComputedTextComponent::TextSpan& span,
                           components::ComputedTextGeometryComponent& cache, bool& hasInkBounds,
                           bool& hasEmBoxBounds, std::size_t& objectBoundingBoxSpanVisits,
                           std::size_t& stretchedCacheReservation) {
  const std::string_view spanText(span.text.data() + span.start, span.end - span.start);
  if (spanText.empty()) {
    return;
  }
  const float runFontSizePx = run.usedFontSizePx;
  const float runScale = run.font ? engine.scaleForPixelHeight(run.font, runFontSizePx) : 0.0f;
  double emTop = static_cast<double>(runFontSizePx);
  double emBottom = 0.0;
  if (run.font && runScale > 0.0f) {
    const FontVMetrics metrics = engine.fontVMetrics(run.font);
    emTop = static_cast<double>(metrics.ascent) * runScale;
    emBottom = -static_cast<double>(metrics.descent) * runScale;
  }
  const ApiCharacterIndexMappings apiMappings =
      BuildApiCharacterIndexMappings(spanText, run.glyphs);
  const size_t localCharCount = apiMappings.firstUnitByUnit.size();
  const size_t charBaseIndex = cache.characters.size();
  cache.characters.resize(charBaseIndex + localCharCount);
  for (size_t i = 0; i < localCharCount; ++i) {
    cache.characters[charBaseIndex + i].sourceEntity = span.sourceEntity;
  }
  if (const auto runEmBounds = CachedTextRunCellBounds(run, emTop, emBottom)) {
    addBox(cache.emBoxBounds, hasEmBoxBounds, *runEmBounds);
    ++objectBoundingBoxSpanVisits;
    AddTextObjectBounds(cache, span.sourceEntity, *runEmBounds);
  }
  const bool hasPaintedGlyphs = HasPaintedTextGlyphs(span, run);
  for (const auto& glyph : run.glyphs) {
    const size_t localCharIndex = glyph.cluster < apiMappings.byteToFirstUnit.size()
                                      ? apiMappings.byteToFirstUnit[glyph.cluster]
                                      : 0;
    if (localCharIndex < localCharCount) {
      PopulateCachedGlyph(engine, registry, fontManager, textRootEntity, run, span, glyph,
                          runFontSizePx, hasPaintedGlyphs,
                          cache.characters[charBaseIndex + localCharIndex], cache, hasInkBounds,
                          stretchedCacheReservation);
    }
  }
  ExpandApiCharacterGeometryAliases(cache, charBaseIndex, apiMappings);
}

namespace {

class TextReferenceWork {
public:
  explicit TextReferenceWork(std::size_t& remaining) : remaining_(remaining) {}

  bool reserve(std::size_t count) {
    if (failed_ || count > remaining_) {
      failed_ = true;
      remaining_ = 0;
      return false;
    }
    remaining_ -= count;
    return true;
  }

  bool reserveProduct(std::size_t count, std::size_t factor) {
    if (factor != 0 && count > remaining_ / factor) {
      return reject();
    }
    return reserve(count * factor);
  }

  bool reject() {
    failed_ = true;
    remaining_ = 0;
    return false;
  }

  bool reservePath(const Path& path) {
    return reserve(path.commands().size()) && reserve(path.points().size());
  }

  bool failed() const { return failed_; }

private:
  std::size_t& remaining_;
  bool failed_ = false;
};

bool ReserveTextReferenceSpan(const components::ComputedTextComponent::TextSpan& span,
                              TextReferenceWork& work) {
  if (span.start > span.end || span.end > span.text.size()) {
    return work.reject();
  }
  const std::array counts{
      span.end - span.start, span.fontFamilies.size(), span.ancestorBaselineShifts.size(),
      span.xList.size(),     span.yList.size(),        span.dxList.size(),
      span.dyList.size(),    span.rotateList.size()};
  for (const std::size_t count : counts) {
    if (!work.reserve(count)) {
      return false;
    }
  }
  return !span.pathSpline || work.reservePath(*span.pathSpline);
}

bool ReserveTextReferenceInput(const components::ComputedTextComponent& text,
                               TextReferenceWork& work) {
  if (!work.reserve(text.spans.size()) || !work.reserve(text.ownerSpanRanges.size()) ||
      !work.reserve(text.textLengthOwnerFontSizes.size())) {
    return false;
  }
  for (const auto& span : text.spans) {
    if (!ReserveTextReferenceSpan(span, work)) {
      return false;
    }
  }
  return true;
}

bool ReferenceSpanIsInSubtree(Registry& registry, Entity source, Entity subtree,
                              TextReferenceWork& work) {
  while (source != entt::null) {
    if (!work.reserve(1)) {
      return false;
    }
    if (source == subtree) {
      return true;
    }
    const auto* tree = registry.try_get<donner::components::TreeComponent>(source);
    source = tree ? tree->parent() : entt::null;
  }
  return false;
}

Transform2d TextRootFromReferenceGlyph(const TextGlyph& glyph) {
  return Transform2d::Rotate(glyph.rotateDegrees * MathConstants<double>::kPi / 180.0) *
         Transform2d::Translate(glyph.xPosition, glyph.yPosition);
}

bool AddReferenceGlyphCell(TextEngine::ReferenceGeometry& result, const TextRun& run,
                           const TextGlyph& glyph, double emTop, double emBottom,
                           TextReferenceWork& work) {
  Box2d cell;
  if (run.stretchedTextPath && glyph.textPathOrigin) {
    constexpr std::size_t kCellCommandsAndPoints = 5u + 4u;
    if (!work.reserveProduct(kCellCommandsAndPoints, kMaximumTextPathWarpExpansion) ||
        !work.reserve(1u)) {
      return false;
    }
    Path cellPath = PlaceStretchedTextPathCell(run, glyph, emTop, emBottom);
    if (cellPath.empty()) {
      return work.reject();
    }
    cell = cellPath.bounds();
  } else {
    const double geometricScaleY = glyph.stretchScaleY;
    const Box2d localCell(Vector2d(std::min(0.0, glyph.xAdvance), -emTop * geometricScaleY),
                          Vector2d(std::max(0.0, glyph.xAdvance), emBottom * geometricScaleY));
    cell = TextRootFromReferenceGlyph(glyph).transformBox(localCell);
  }
  if (!std::isfinite(cell.topLeft.x) || !std::isfinite(cell.topLeft.y) ||
      !std::isfinite(cell.bottomRight.x) || !std::isfinite(cell.bottomRight.y)) {
    return work.reject();
  }
  result.objectBounds = result.objectBounds ? Box2d::Union(*result.objectBounds, cell) : cell;
  return true;
}

bool AdmitReferenceOutline(FontManager& manager, FontHandle font, int glyphIndex, bool stretched,
                           TextReferenceWork& work) {
  auto complexity = manager.glyphOutlineComplexity(font, glyphIndex);
  if (!complexity) {
    if (!manager.isTrustedFont(font)) {
      return work.reject();
    }
    // Unknown trusted outlines use the global conservative ceiling, never a guessed glyph size.
    complexity =
        FontManager::GlyphOutlineComplexity{.maximumVertices = fonts::kMaximumExpandedGlyphVertices,
                                            .work = fonts::kMaximumGlyphOutlineWork};
  }
  constexpr std::size_t kDecodedOutlineFactor = 15u;
  constexpr std::size_t kWarpedCommandAndPointFactor = 2u * 3u * kMaximumTextPathWarpExpansion;
  const std::size_t expansion =
      stretched ? kDecodedOutlineFactor + kWarpedCommandAndPointFactor : kDecodedOutlineFactor;
  return work.reserve(complexity->work) &&
         work.reserveProduct(complexity->maximumVertices, expansion);
}

bool AppendReferenceOutline(const TextEngine& engine, FontManager& manager, const TextRun& run,
                            const TextGlyph& glyph, Entity source,
                            TextEngine::ReferenceGeometry& result, TextReferenceWork& work) {
  const FontHandle font = GlyphFont(run, glyph);
  if (engine.isBitmapOnly(font)) {
    return true;
  }
  if (!AdmitReferenceOutline(manager, font, glyph.glyphIndex,
                             static_cast<bool>(run.stretchedTextPath), work)) {
    return false;
  }
  const float scale = engine.scaleForEmToPixels(font, run.usedFontSizePx) * glyph.fontSizeScale;
  Path raw = engine.glyphOutline(font, glyph.glyphIndex, scale);
  if (raw.empty()) {
    return true;
  }
  if (!work.reservePath(raw) || !work.reserve(1)) {
    return false;
  }
  Path placed = PlaceCachedGlyphOutline(std::move(raw), run, glyph);
  if (placed.empty()) {
    return work.reject();
  }
  const Box2d bounds = placed.bounds();
  if (!IsFiniteTextPosition(bounds.topLeft) || !IsFiniteTextPosition(bounds.bottomRight)) {
    return work.reject();
  }
  result.outlines.push_back({std::move(placed), source});
  return true;
}

bool AppendTextReferenceRun(const TextEngine& engine, FontManager& manager, const TextRun& run,
                            Entity source, bool includeOutlines,
                            TextEngine::ReferenceGeometry& result, TextReferenceWork& work) {
  if (!work.reserve(run.glyphs.size())) {
    return false;
  }
  const float scale = run.font ? engine.scaleForPixelHeight(run.font, run.usedFontSizePx) : 0.0f;
  const FontVMetrics metrics = run.font ? engine.fontVMetrics(run.font) : FontVMetrics();
  const double emTop = scale > 0.0f ? metrics.ascent * scale : run.usedFontSizePx;
  const double emBottom = scale > 0.0f ? -metrics.descent * scale : 0.0;
  for (const auto& glyph : run.glyphs) {
    if (!AddReferenceGlyphCell(result, run, glyph, emTop, emBottom, work) ||
        (includeOutlines &&
         !AppendReferenceOutline(engine, manager, run, glyph, source, result, work))) {
      return false;
    }
  }
  return true;
}

bool AppendTextReferenceRuns(const TextEngine& engine, FontManager& manager, Registry& registry,
                             const components::ComputedTextComponent& text,
                             const std::vector<TextRun>& runs, Entity subtree, bool includeOutlines,
                             TextEngine::ReferenceGeometry& result, TextReferenceWork& work) {
  if (!work.reserve(runs.size())) {
    return false;
  }
  for (std::size_t index = 0; index < runs.size() && index < text.spans.size(); ++index) {
    const Entity source = text.spans[index].sourceEntity;
    if (ReferenceSpanIsInSubtree(registry, source, subtree, work) &&
        !AppendTextReferenceRun(engine, manager, runs[index], source, includeOutlines, result,
                                work)) {
      return false;
    }
    if (work.failed()) {
      return false;
    }
  }
  return true;
}

bool ReserveTextStyleAncestors(Registry& registry, Entity styleEntity,
                               const TextStyleContext& styleContext, TextReferenceWork& work) {
  std::size_t ancestors = 0;
  while (styleEntity != styleContext.rootEntity) {
    const auto* tree = registry.try_get<donner::components::TreeComponent>(styleEntity);
    if (!tree || ++ancestors > styleContext.entities.size() || !work.reserve(1u)) {
      return false;
    }
    styleEntity = tree->parent();
    if (styleEntity == entt::null) {
      return false;
    }
  }
  return true;
}

bool ReserveTextStyleContextExpansion(Registry& registry,
                                      const components::ComputedTextComponent& computed,
                                      const TextStyleContext* styleContext,
                                      TextReferenceWork& work) {
  if (!styleContext) {
    return true;
  }
  for (const auto& span : computed.spans) {
    if (span.sourceEntity == entt::null) {
      continue;
    }
    Entity styleEntity = styleContext->styleEntityFor(span.sourceEntity);
    const auto* spanStyle = registry.try_get<components::ComputedStyleComponent>(styleEntity);
    if (!spanStyle || !spanStyle->properties ||
        !work.reserve(spanStyle->properties->fontFamily.get().value().size())) {
      return false;
    }
    if (!ReserveTextStyleAncestors(registry, styleEntity, *styleContext, work)) {
      return false;
    }
  }
  return true;
}

bool RemapReferenceOutlineSources(const TextStyleContext* styleContext,
                                  TextEngine::ReferenceGeometry& result, TextReferenceWork& work) {
  if (!styleContext) {
    return true;
  }
  for (auto& glyph : result.outlines) {
    glyph.sourceEntity = styleContext->styleEntityFor(glyph.sourceEntity);
    if (glyph.sourceEntity == entt::null) {
      return work.reject();
    }
  }
  return true;
}

bool AdmitTextReferenceInput(const components::ComputedTextComponent& computed,
                             const components::ComputedStyleComponent& style,
                             TextReferenceWork& work) {
  if (!ReserveTextReferenceInput(computed, work) ||
      !work.reserve(style.properties->fontFamily.get().value().size())) {
    return false;
  }
  if (isVertical(style.properties->writingMode.get().value())) {
    // The horizontal glyph-cell contract cannot stand in for upright/sideways vertical cells.
    return work.reject();
  }
  return true;
}

}  // namespace

std::optional<TextEngine::ReferenceGeometry> TextEngine::referenceGeometry(
    EntityHandle handle, bool includeOutlines, std::size_t& remainingWork,
    const TextStyleContext* styleContext) const {
  const ScopedTextPathMeasurementWorkEpoch textPathWorkEpoch(registry_);
  TextReferenceWork work(remainingWork);
  const Entity root = findTextRootEntity(handle);
  const auto* computed = registry_.try_get<components::ComputedTextComponent>(root);
  const auto* text = registry_.try_get<components::TextComponent>(root);
  const Entity styleRootEntity = styleContext ? styleContext->rootEntity : root;
  const auto* style = registry_.try_get<components::ComputedStyleComponent>(styleRootEntity);
  if (!computed || !text || !style || !style->properties) {
    return ReferenceGeometry();
  }
  if (!AdmitTextReferenceInput(*computed, *style, work)) {
    return std::nullopt;
  }
  if (!ReserveTextStyleContextExpansion(registry_, *computed, styleContext, work)) {
    return std::nullopt;
  }
  ReferenceGeometry result;
  {
    const auto capture = fontManager_.captureDependencies(result.fontDependencies);
    auto styled = *computed;
    const TextLayoutParams params =
        buildTextLayoutParams(registry_, EntityHandle(registry_, styleRootEntity), *style, *text);
    ResolvePerSpanLayoutStyles(registry_, styled, params.viewBox, params.fontMetrics, styleContext);
    bool layoutResourceRejected = false;
    const auto runs = const_cast<TextEngine*>(this)->layoutWithResourceStatus(
        styled, params, layoutResourceRejected);
    if (layoutResourceRejected) {
      work.reject();
      return std::nullopt;
    }
    if (!AppendTextReferenceRuns(*this, fontManager_, registry_, styled, runs, handle.entity(),
                                 includeOutlines, result, work)) {
      return std::nullopt;
    }
    if (!RemapReferenceOutlineSources(styleContext, result, work)) {
      return std::nullopt;
    }
  }
  return result;
}

const components::ComputedTextGeometryComponent& TextEngine::ensureComputedTextGeometryComponent(
    EntityHandle handle) const {
  const Entity textRootEntity = findTextRootEntity(handle);
  UTILS_RELEASE_ASSERT_MSG(textRootEntity != entt::null, "Text content element has no text root");
  const ScopedTextPathMeasurementWorkEpoch textPathWorkEpoch(registry_);

  // Return cached component if it already exists.
  if (const auto* existing =
          registry_.try_get<components::ComputedTextGeometryComponent>(textRootEntity)) {
    const bool workCanRetry =
        !TextPathResourceFamily(registry_)->securityStats().textPathMeasurementWorkRejected;
    if (!existing->retryAfterTextPathWorkRejection || !workCanRetry) {
      fontManager_.recordDependencies(existing->fontDependencies);
      return *existing;
    }
    registry_.remove<components::ComputedTextGeometryComponent>(textRootEntity);
  }

  auto emptyAndReturn = [&]() -> const components::ComputedTextGeometryComponent& {
    return registry_.emplace_or_replace<components::ComputedTextGeometryComponent>(textRootEntity);
  };

  const auto* computedText = registry_.try_get<components::ComputedTextComponent>(textRootEntity);
  const auto* textComp = registry_.try_get<components::TextComponent>(textRootEntity);
  const auto* style = registry_.try_get<components::ComputedStyleComponent>(textRootEntity);
  if (!computedText || !textComp || !style || !style->properties) {
    return emptyAndReturn();
  }

  components::ComputedTextComponent styledText = *computedText;
  const EntityHandle rootHandle(registry_, textRootEntity);
  const TextLayoutParams params = buildTextLayoutParams(registry_, rootHandle, *style, *textComp);
  ResolvePerSpanLayoutStyles(registry_, styledText, params.viewBox, params.fontMetrics);

  components::ComputedTextGeometryComponent cache;
  std::vector<TextRun> runs;
  {
    const auto capture = fontManager_.captureDependencies(cache.fontDependencies);
    runs = const_cast<TextEngine*>(this)->layout(styledText, params);
  }
  cache.fontResourceRevision = fontManager_.fontResourceRevision();
  bool hasInkBounds = false;
  bool hasEmBoxBounds = false;
  std::size_t stretchedCacheReservation = 0u;
  for (size_t runIndex = 0; runIndex < runs.size() && runIndex < styledText.spans.size();
       ++runIndex) {
    PopulateCachedTextRun(const_cast<TextEngine&>(*this), registry_, fontManager_, textRootEntity,
                          runs[runIndex], styledText.spans[runIndex], cache, hasInkBounds,
                          hasEmBoxBounds, objectBoundingBoxSpanVisits_, stretchedCacheReservation);
  }

  BuildTextObjectBoundsIndex(registry_, textRootEntity, cache);

  cache.runs = runs;
  cache.retryAfterTextPathWorkRejection =
      TextPathResourceFamily(registry_)->securityStats().textPathMeasurementWorkRejected;

  return registry_.emplace_or_replace<components::ComputedTextGeometryComponent>(textRootEntity,
                                                                                 std::move(cache));
}

std::vector<Path> TextEngine::computedGlyphPaths(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  std::vector<Path> result;
  for (const auto& glyph : cache.glyphs) {
    if (isDescendantOf(registry_, glyph.sourceEntity, handle.entity())) {
      result.push_back(glyph.path);
    }
  }
  return result;
}

std::vector<TextEngine::GlyphOutline> TextEngine::computedGlyphOutlines(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  std::vector<GlyphOutline> result;
  for (const auto& glyph : cache.glyphs) {
    if (isDescendantOf(registry_, glyph.sourceEntity, handle.entity())) {
      result.push_back(GlyphOutline{glyph.path, glyph.sourceEntity});
    }
  }
  return result;
}

Box2d TextEngine::computedInkBounds(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  Box2d result;
  bool initialized = false;
  for (const auto& character : cache.characters) {
    if (character.hasExtent && isDescendantOf(registry_, character.sourceEntity, handle.entity())) {
      addBox(result, initialized, character.extent);
    }
  }
  return result;
}

Entity TextEngine::textRootEntity(EntityHandle handle) const {
  return findTextRootEntity(handle);
}

Box2d TextEngine::computedObjectBoundingBox(EntityHandle handle) const {
  const Entity rootEntity = findTextRootEntity(handle);
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  if (handle.entity() == rootEntity) {
    return cache.emBoxBounds;
  }

  // The index stores the union of each span's glyph cells and all contributing descendants.
  const auto bounds = cache.objectBoundsByEntity.find(handle.entity());
  return bounds != cache.objectBoundsByEntity.end() ? bounds->second : Box2d();
}

long TextEngine::getNumberOfChars(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  return static_cast<long>(filteredCharacters(registry_, handle, cache).size());
}

double TextEngine::getComputedTextLength(EntityHandle handle) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  double total = 0.0;
  for (const auto* character : filteredCharacters(registry_, handle, cache)) {
    total += character->advance;
  }
  return total;
}

double TextEngine::getSubStringLength(EntityHandle handle, std::size_t charnum,
                                      std::size_t nchars) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  double total = 0.0;
  for (size_t i = charnum; i < characters.size() && i < charnum + nchars; ++i) {
    total += characters[i]->advance;
  }
  return total;
}

Vector2d TextEngine::getStartPositionOfChar(EntityHandle handle, std::size_t charnum) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  if (charnum >= characters.size()) {
    return Vector2d();
  }
  return characters[charnum]->startPosition;
}

Vector2d TextEngine::getEndPositionOfChar(EntityHandle handle, std::size_t charnum) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  if (charnum >= characters.size()) {
    return Vector2d();
  }
  return characters[charnum]->endPosition;
}

Box2d TextEngine::getExtentOfChar(EntityHandle handle, std::size_t charnum) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  if (charnum >= characters.size()) {
    return Box2d();
  }
  return characters[charnum]->extent;
}

double TextEngine::getRotationOfChar(EntityHandle handle, std::size_t charnum) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  if (charnum >= characters.size()) {
    return 0.0;
  }
  return characters[charnum]->rotation;
}

long TextEngine::getCharNumAtPosition(EntityHandle handle, const Vector2d& point) const {
  const auto& cache = ensureComputedTextGeometryComponent(handle);
  const auto characters = filteredCharacters(registry_, handle, cache);
  for (size_t i = 0; i < characters.size(); ++i) {
    if (characters[i]->hasExtent && characters[i]->extent.contains(point)) {
      return static_cast<long>(i);
    }
  }
  return -1;
}

}  // namespace donner::svg
