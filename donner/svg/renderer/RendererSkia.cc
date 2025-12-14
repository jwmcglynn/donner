#include "donner/svg/renderer/RendererSkia.h"

#include <algorithm>  // For std::sort
#include <limits>     // For std::numeric_limits

// Skia
#include "include/core/SkColorFilter.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPathMeasure.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkStream.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkLumaColorFilter.h"
#include "include/pathops/SkPathOps.h"
#include "modules/skshaper/include/SkShaper.h"

#ifdef SK_SHAPER_HARFBUZZ_AVAILABLE
#include "modules/skshaper/include/SkShaper_harfbuzz.h"
#include "modules/skunicode/include/SkUnicode.h"
#endif

#ifdef SK_SHAPER_CORETEXT_AVAILABLE
#include "modules/skshaper/include/SkShaper_coretext.h"
#endif

#ifdef DONNER_USE_CORETEXT
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(DONNER_USE_FREETYPE)
#include "include/ports/SkFontMgr_empty.h"
#elif defined(DONNER_USE_FREETYPE_WITH_FONTCONFIG)
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#else
#error \
    "Neither DONNER_USE_CORETEXT, DONNER_USE_FREETYPE, nor DONNER_USE_FREETYPE_WITH_FONTCONFIG is defined"
#endif
// Donner
#include "donner/base/fonts/WoffFont.h"
#include "donner/base/xml/components/TreeComponent.h"  // ForAllChildren
#include "donner/svg/SVGMarkerElement.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/IdComponent.h"  // For verbose logging.
#include "donner/svg/components/PathLengthComponent.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/MarkerComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererUtils.h"
#include "donner/svg/renderer/TypefaceResolver.h"
#include "donner/svg/renderer/common/RenderingInstanceView.h"

// Embedded resources

namespace donner::svg {

namespace {

const Boxd kUnitPathBounds(Vector2d::Zero(), Vector2d(1, 1));

SkPoint toSkia(Vector2d value) {
  return SkPoint::Make(NarrowToFloat(value.x), NarrowToFloat(value.y));
}

SkMatrix toSkiaMatrix(const Transformd& transform) {
  return SkMatrix::MakeAll(NarrowToFloat(transform.data[0]),  // scaleX
                           NarrowToFloat(transform.data[2]),  // skewX
                           NarrowToFloat(transform.data[4]),  // transX
                           NarrowToFloat(transform.data[1]),  // skewY
                           NarrowToFloat(transform.data[3]),  // scaleY
                           NarrowToFloat(transform.data[5]),  // transY
                           0, 0, 1);
}

SkFontStyle::Slant ToSkFontSlant(FontStyle style) {
  switch (style) {
    case FontStyle::Normal: return SkFontStyle::Slant::kUpright_Slant;
    case FontStyle::Italic: return SkFontStyle::Slant::kItalic_Slant;
    case FontStyle::Oblique: return SkFontStyle::Slant::kOblique_Slant;
  }

  UTILS_UNREACHABLE();
}

SkFontStyle::Width ToSkFontWidth(FontStretch stretch) {
  switch (stretch) {
    case FontStretch::UltraCondensed: return SkFontStyle::kUltraCondensed_Width;
    case FontStretch::ExtraCondensed: return SkFontStyle::kExtraCondensed_Width;
    case FontStretch::Condensed: return SkFontStyle::kCondensed_Width;
    case FontStretch::SemiCondensed: return SkFontStyle::kSemiCondensed_Width;
    case FontStretch::Normal: return SkFontStyle::kNormal_Width;
    case FontStretch::SemiExpanded: return SkFontStyle::kSemiExpanded_Width;
    case FontStretch::Expanded: return SkFontStyle::kExpanded_Width;
    case FontStretch::ExtraExpanded: return SkFontStyle::kExtraExpanded_Width;
    case FontStretch::UltraExpanded: return SkFontStyle::kUltraExpanded_Width;
  }

  UTILS_UNREACHABLE();
}

int ToSkFontWeight(const FontWeight& weight) {
  const int resolved = weight.kind == FontWeight::Kind::Number ? weight.value
                       : weight.kind == FontWeight::Kind::Bold ? 700
                                                               : 400;
  return std::clamp(resolved, 1, 1000);
}

SkFontStyle ToSkFontStyle(const components::ComputedTextStyleComponent& style) {
  return SkFontStyle(ToSkFontWeight(style.fontWeight), ToSkFontWidth(style.fontStretch),
                     ToSkFontSlant(style.fontStyle));
}

SkM44 toSkia(const Transformd& transform) {
  return SkM44{float(transform.data[0]),
               float(transform.data[2]),
               0.0f,
               float(transform.data[4]),
               float(transform.data[1]),
               float(transform.data[3]),
               0.0f,
               float(transform.data[5]),
               0.0f,
               0.0f,
               1.0f,
               0.0f,
               0.0f,
               0.0f,
               0.0f,
               1.0f};
}

SkRect toSkia(const Boxd& box) {
  return SkRect::MakeLTRB(
      static_cast<SkScalar>(box.topLeft.x), static_cast<SkScalar>(box.topLeft.y),
      static_cast<SkScalar>(box.bottomRight.x), static_cast<SkScalar>(box.bottomRight.y));
}

SkColor toSkia(const css::RGBA rgba) {
  return SkColorSetARGB(rgba.a, rgba.r, rgba.g, rgba.b);
}

SkPaint::Cap toSkia(StrokeLinecap lineCap) {
  switch (lineCap) {
    case StrokeLinecap::Butt: return SkPaint::Cap::kButt_Cap;
    case StrokeLinecap::Round: return SkPaint::Cap::kRound_Cap;
    case StrokeLinecap::Square: return SkPaint::Cap::kSquare_Cap;
  }

  UTILS_UNREACHABLE();
}

SkPaint::Join toSkia(StrokeLinejoin lineJoin) {
  // TODO(jwmcglynn): Implement MiterClip and Arcs. For now, fallback to Miter, which is the default
  // linejoin, since the feature is not implemented.
  switch (lineJoin) {
    case StrokeLinejoin::Miter: return SkPaint::Join::kMiter_Join;
    case StrokeLinejoin::MiterClip: return SkPaint::Join::kMiter_Join;
    case StrokeLinejoin::Round: return SkPaint::Join::kRound_Join;
    case StrokeLinejoin::Bevel: return SkPaint::Join::kBevel_Join;
    case StrokeLinejoin::Arcs: return SkPaint::Join::kMiter_Join;
  }

  UTILS_UNREACHABLE();
}

SkPath toSkia(const PathSpline& spline) {
  SkPath path;

  const std::vector<Vector2d>& points = spline.points();
  for (const PathSpline::Command& command : spline.commands()) {
    switch (command.type) {
      case PathSpline::CommandType::MoveTo: {
        auto pt = points[command.pointIndex];
        path.moveTo(static_cast<SkScalar>(pt.x), static_cast<SkScalar>(pt.y));
        break;
      }
      case PathSpline::CommandType::CurveTo: {
        auto c0 = points[command.pointIndex];
        auto c1 = points[command.pointIndex + 1];
        auto end = points[command.pointIndex + 2];
        path.cubicTo(static_cast<SkScalar>(c0.x), static_cast<SkScalar>(c0.y),
                     static_cast<SkScalar>(c1.x), static_cast<SkScalar>(c1.y),
                     static_cast<SkScalar>(end.x), static_cast<SkScalar>(end.y));
        break;
      }
      case PathSpline::CommandType::LineTo: {
        auto pt = points[command.pointIndex];
        path.lineTo(static_cast<SkScalar>(pt.x), static_cast<SkScalar>(pt.y));
        break;
      }
      case PathSpline::CommandType::ClosePath: {
        path.close();
        break;
      }
    }
  }

  return path;
}

SkTileMode toSkia(GradientSpreadMethod spreadMethod) {
  switch (spreadMethod) {
    case GradientSpreadMethod::Pad: return SkTileMode::kClamp;
    case GradientSpreadMethod::Reflect: return SkTileMode::kMirror;
    case GradientSpreadMethod::Repeat: return SkTileMode::kRepeat;
  }

  UTILS_UNREACHABLE();
}

#if defined(__GNUC__) || defined(__clang__)
#define DONNER_PACKED __attribute__((packed))
#else
#define DONNER_PACKED
#endif

// SFNT header structure, see https://learn.microsoft.com/en-us/typography/opentype/spec/otff
struct DONNER_PACKED SfntHeader {
  uint32_t sfntVersion;
  uint16_t numTables;
  uint16_t searchRange;
  uint16_t entrySelector;
  uint16_t rangeShift;
};

// SFNT table record, see https://learn.microsoft.com/en-us/typography/opentype/spec/otff
struct DONNER_PACKED SfntTableRecord {
  uint32_t tag;
  uint32_t checksum;
  uint32_t origOffset;
  uint32_t origLength;
};

static inline uint16_t ByteSwap16(uint16_t x) {
  return (x >> 8) | (x << 8);
}

static inline uint32_t ByteSwap32(uint32_t x) {
  return ((x & 0xFF000000) >> 24) | ((x & 0x00FF0000) >> 8) | ((x & 0x0000FF00) << 8) |
         ((x & 0x000000FF) << 24);
}

// Helper function to calculate floor log2 of a number
static inline uint32_t FloorLog2(uint32_t x) {
  if (x == 0) return 0;
  uint32_t result = 0;
  while (x >>= 1) result++;
  return result;
}

/// Aligns \p n to the next 4‑byte boundary.
static constexpr size_t Align4(size_t n) {
  return (n + 3u) & ~size_t{3};
}

static sk_sp<SkData> CreateInMemoryFont(const donner::fonts::WoffFont& font) {
  // The sfnt spec (§5.2) requires table records to be sorted by tag.  Skipping
  // this leads to FreeType rejecting some fonts (notably those with an `OTTO`
  // flavor).  We also have to keep each table 4‑byte aligned.
  std::vector<const donner::fonts::WoffTable*> sortedTables;
  sortedTables.reserve(font.tables.size());
  for (const auto& t : font.tables) {
    sortedTables.push_back(&t);
  }
  std::sort(sortedTables.begin(), sortedTables.end(),
            [](const donner::fonts::WoffTable* a, const donner::fonts::WoffTable* b) {
              return a->tag < b->tag;
            });

  const size_t numTables = sortedTables.size();
  const size_t headerSize = sizeof(SfntHeader) + numTables * sizeof(SfntTableRecord);

  // Include padding so every table starts on a 4‑byte boundary.
  size_t totalSize = headerSize;
  for (const auto* table : sortedTables) {
    totalSize += Align4(table->data.size());
  }

  auto fontData = SkData::MakeUninitialized(totalSize);
  uint8_t* data = static_cast<uint8_t*>(fontData->writable_data());
  uint8_t* const base = data;

  // Write the SFNT header
  SfntHeader header;
  header.sfntVersion = ByteSwap32(font.flavor);

  header.numTables = ByteSwap16(static_cast<uint16_t>(numTables));

  // searchRange, entrySelector and rangeShift are calculated from numTables.
  const uint16_t maxPowerOf2 = (numTables > 0) ? (1u << FloorLog2(numTables)) : 0;
  header.searchRange = ByteSwap16(static_cast<uint16_t>(maxPowerOf2 * 16));
  header.entrySelector = (numTables > 0) ? ByteSwap16(FloorLog2(numTables)) : 0;
  header.rangeShift =
      (numTables > 0) ? ByteSwap16(static_cast<uint16_t>(numTables * 16 - maxPowerOf2 * 16)) : 0;

  memcpy(data, &header, sizeof(header));
  data += sizeof(header);

  // Write table directory
  const uint32_t headerStartOffset = static_cast<uint32_t>(headerSize);
  uint32_t payloadOffset = headerStartOffset;

  for (const auto* table : sortedTables) {
    SfntTableRecord record;
    record.tag = ByteSwap32(table->tag);
    record.checksum = 0;  // Freetype doesn't check
    record.origOffset = ByteSwap32(payloadOffset);
    record.origLength = ByteSwap32(static_cast<uint32_t>(table->data.size()));

    memcpy(data, &record, sizeof(record));
    data += sizeof(record);

    payloadOffset += static_cast<uint32_t>(Align4(table->data.size()));
  }

  // Write table data
  for (const auto* table : sortedTables) {
    const auto& bytes = table->data;
    memcpy(data, bytes.data(), bytes.size());
    data += bytes.size();

    const size_t pad = Align4(bytes.size()) - bytes.size();
    if (pad) {
      memset(data, 0, pad);
      data += pad;
    }
  }

  assert(static_cast<size_t>(data - base) == totalSize);
  return fontData;
}

}  // namespace

/// Implementation class for \ref RendererSkia
class RendererSkia::Impl {
public:
  Impl(RendererSkia& renderer, const RenderingInstanceView& view)
      : renderer_(renderer), view_(view) {}

  void initialize(Registry& registry) {
    // Load typeface by family
    if (!renderer_.fontMgr_) {
#ifdef DONNER_USE_CORETEXT
      renderer_.fontMgr_ = SkFontMgr_New_CoreText(nullptr);
#elif defined(DONNER_USE_FREETYPE)
      renderer_.fontMgr_ = SkFontMgr_New_Custom_Empty();
#elif defined(DONNER_USE_FREETYPE_WITH_FONTCONFIG)
      renderer_.fontMgr_ = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
    }

    // If we have custom fonts, load them into a font manager.
    auto& resourceManager = registry.ctx().get<components::ResourceManagerContext>();

    const std::vector<donner::svg::components::FontResource>& loadedFonts =
        resourceManager.loadedFonts();
    for (const auto& font : loadedFonts) {
      auto fontData = CreateInMemoryFont(font.font);
      if (auto typeface = renderer_.fontMgr_->makeFromData(std::move(fontData))) {
        if (font.font.familyName.has_value()) {
          auto& familyTypefaces = renderer_.typefaces_[font.font.familyName.value()];
          familyTypefaces.push_back(std::move(typeface));
        } else {
          // We don't have a family name, so the font will not be usable. Ignore it.
        }
      } else {
        std::cerr << "Failed to load font face from data for family: "
                  << (font.font.familyName.has_value() ? font.font.familyName.value() : "unknown")
                  << "\n";
      }
    }

    AddEmbeddedFonts(renderer_.typefaces_, *renderer_.fontMgr_);

    if (!renderer_.fallbackTypeface_ && renderer_.fallbackFontFamily_) {
      renderer_.fallbackTypeface_ = renderer_.fontMgr_->matchFamilyStyle(
          renderer_.fallbackFontFamily_->c_str(), SkFontStyle());
    }

    if (!renderer_.fallbackTypeface_) {
      renderer_.fallbackTypeface_ = CreateEmbeddedFallbackTypeface(*renderer_.fontMgr_);
    }
  }

  void drawUntil(Registry& registry, Entity endEntity) {
    bool foundEndEntity = false;

    while (!view_.done() && !foundEndEntity) {
      // When we find the end we do one more iteration of the loop and then exit.
      foundEndEntity = view_.currentEntity() == endEntity;

      const components::RenderingInstanceComponent& instance = view_.get();
      const Entity entity = view_.currentEntity();
      view_.advance();

      const Transformd entityFromCanvas = layerBaseTransform_ * instance.entityFromWorldTransform;

      if (renderer_.verbose_) {
        std::cout << "Rendering "
                  << registry.get<components::ElementTypeComponent>(instance.dataEntity).type()
                  << " ";

        if (const auto* idComponent =
                registry.try_get<components::IdComponent>(instance.dataEntity)) {
          std::cout << "id=" << idComponent->id() << " ";
        }

        std::cout << instance.dataEntity;
        if (instance.isShadow(registry)) {
          std::cout << " (shadow " << instance.styleHandle(registry).entity() << ")";
        }

        std::cout << " transform=" << entityFromCanvas << "\n";

        std::cout << "\n";
      }

      if (instance.clipRect) {
        renderer_.currentCanvas_->save();
        if (renderer_.verbose_) {
          std::cout << "Clipping to " << instance.clipRect.value() << "\n";
        }
        renderer_.currentCanvas_->clipRect(toSkia(instance.clipRect.value()));
      }

      renderer_.currentCanvas_->setMatrix(toSkia(entityFromCanvas));

      const components::ComputedStyleComponent& styleComponent =
          instance.styleHandle(registry).get<components::ComputedStyleComponent>();
      const auto& properties = styleComponent.properties.value();

      if (instance.isolatedLayer) {
        // Create a new layer if opacity is less than 1.
        if (properties.opacity.getRequired() < 1.0) {
          SkPaint opacityPaint;
          opacityPaint.setAlphaf(NarrowToFloat(properties.opacity.getRequired()));

          // const SkRect layerBounds = toSkia(shapeWorldBounds.value_or(Boxd()));
          renderer_.currentCanvas_->saveLayer(nullptr, &opacityPaint);
        }

        if (instance.resolvedFilter) {
          SkPaint filterPaint;
          filterPaint.setAntiAlias(renderer_.antialias_);
          createFilterPaint(filterPaint, registry, instance.resolvedFilter.value());

          // const SkRect layerBounds = toSkia(shapeWorldBounds.value_or(Boxd()));
          renderer_.currentCanvas_->saveLayer(nullptr, &filterPaint);
        }

        if (instance.clipPath) {
          const components::ResolvedClipPath& ref = instance.clipPath.value();

          Transformd userSpaceFromClipPathContent;
          if (ref.units == ClipPathUnits::ObjectBoundingBox) {
            if (auto maybeBounds =
                    components::ShapeSystem().getShapeBounds(instance.dataHandle(registry))) {
              const Boxd bounds = maybeBounds.value();
              userSpaceFromClipPathContent =
                  Transformd::Scale(bounds.size()) * Transformd::Translate(bounds.topLeft);
            }
          }

          renderer_.currentCanvas_->save();
          const SkMatrix skUserSpaceFromClipPathContent =
              toSkiaMatrix(userSpaceFromClipPathContent);

          SkPath fullPath;
          SmallVector<SkPath, 5> layeredPaths;

          // Iterate over children and add any paths to the clip.
          const auto& clipPaths =
              instance.styleHandle(registry).get<components::ComputedClipPathsComponent>();

          int currentLayer = 0;
          for (const components::ComputedClipPathsComponent::ClipPath& clipPath :
               clipPaths.clipPaths) {
            SkPath path = toSkia(clipPath.path);
            path.transform(toSkiaMatrix(clipPath.entityFromParent) *
                           skUserSpaceFromClipPathContent);

            path.setFillType(clipPath.clipRule == ClipRule::NonZero ? SkPathFillType::kWinding
                                                                    : SkPathFillType::kEvenOdd);

            if (clipPath.layer > currentLayer) {
              layeredPaths.push_back(path);
              currentLayer = clipPath.layer;
              continue;
            } else if (clipPath.layer < currentLayer) {
              // Need to apply the last layer.
              assert(!layeredPaths.empty());

              SkPath layerPath = layeredPaths[layeredPaths.size() - 1];
              layeredPaths.pop_back();

              // Intersect the layer with the current path.
              layerPath.transform(toSkiaMatrix(clipPath.entityFromParent) *
                                  skUserSpaceFromClipPathContent);
              Op(layerPath, path, kIntersect_SkPathOp, &path);

              currentLayer = clipPath.layer;

              if (currentLayer != 0) {
                // Add this back to layeredPaths.
                layeredPaths.push_back(path);
                continue;
              }
            }

            SkPath& targetPath =
                layeredPaths.empty() ? fullPath : layeredPaths[layeredPaths.size() - 1];
            Op(targetPath, path, kUnion_SkPathOp, &targetPath);
          }

          renderer_.currentCanvas_->clipPath(fullPath, SkClipOp::kIntersect, true);
        }

        if (instance.mask) {
          const components::ResolvedMask& ref = instance.mask.value();

          SkPaint maskFilter;
          // TODO: SRGB colorspace conversion
          // Use Luma color filter for the mask, which converts the mask to alpha.
          maskFilter.setColorFilter(SkLumaColorFilter::Make());

          // Save the current layer with the mask filter
          renderer_.currentCanvas_->saveLayer(nullptr, &maskFilter);

          // Render the mask content
          instantiateMask(ref.reference.handle, instance, instance.dataHandle(registry), ref);

          // Content layer
          // Dst is the mask, Src is the content.
          // kSrcIn multiplies the mask alpha: r = s * da
          SkPaint maskPaint;
          maskPaint.setBlendMode(SkBlendMode::kSrcIn);
          renderer_.currentCanvas_->saveLayer(nullptr, &maskPaint);

          // Restore the matrix after starting the layer
          renderer_.currentCanvas_->setMatrix(toSkia(entityFromCanvas));
        }
      }

      if (instance.visible) {
        if (const auto* path =
                instance.dataHandle(registry).try_get<components::ComputedPathComponent>()) {
          drawPath(
              instance.dataHandle(registry), instance, *path, styleComponent.properties.value(),
              components::LayoutSystem().getViewBox(instance.dataHandle(registry)), FontMetrics());
        } else if (const auto* image =
                       instance.dataHandle(registry).try_get<components::LoadedImageComponent>()) {
          drawImage(instance.dataHandle(registry), instance, *image);
        } else if (const auto* text =
                       instance.dataHandle(registry).try_get<components::ComputedTextComponent>()) {
          // Draw text spans
          drawText(
              instance.dataHandle(registry), instance, *text, styleComponent.properties.value(),
              components::LayoutSystem().getViewBox(instance.dataHandle(registry)), FontMetrics());
        }
      }

      if (instance.subtreeInfo) {
        subtreeMarkers_.push_back(instance.subtreeInfo.value());
      }

      while (!subtreeMarkers_.empty() && subtreeMarkers_.back().lastRenderedEntity == entity) {
        const components::SubtreeInfo subtreeInfo = subtreeMarkers_.back();
        subtreeMarkers_.pop_back();

        // SkCanvas also has restoreToCount, but it just calls restore in a loop.
        for (int i = 0; i < subtreeInfo.restorePopDepth; ++i) {
          renderer_.currentCanvas_->restore();
        }
      }
    }

    renderer_.currentCanvas_->restoreToCount(1);
  }

  void skipUntil(Registry& registry, Entity endEntity) {
    bool foundEndEntity = false;

    while (!view_.done() && !foundEndEntity) {
      // When we find the end we do one more iteration of the loop and then exit.
      foundEndEntity = view_.currentEntity() == endEntity;

      view_.advance();
    }
  }

  void drawRange(Registry& registry, Entity startEntity, Entity endEntity) {
    bool foundStartEntity = false;

    while (!view_.done() && !foundStartEntity) {
      if (view_.currentEntity() == startEntity) {
        break;
      } else {
        view_.advance();
      }
    }

    drawUntil(registry, endEntity);
  }

  void drawPath(EntityHandle dataHandle, const components::RenderingInstanceComponent& instance,
                const components::ComputedPathComponent& path, const PropertyRegistry& style,
                const Boxd& viewBox, const FontMetrics& fontMetrics) {
    if (HasPaint(instance.resolvedFill)) {
      drawPathFill(dataHandle, path, instance.resolvedFill, style, viewBox);
    }

    if (HasPaint(instance.resolvedStroke)) {
      drawPathStroke(dataHandle, path, instance.resolvedStroke, style, viewBox, fontMetrics);
    }

    drawMarkers(dataHandle, instance, path, viewBox, fontMetrics);
  }

  std::optional<SkPaint> createFallbackPaint(const components::PaintResolvedReference& ref,
                                             css::RGBA currentColor, float opacity) {
    if (ref.fallback) {
      SkPaint paint;
      paint.setAntiAlias(renderer_.antialias_);
      paint.setColor(toSkia(ref.fallback.value().resolve(currentColor, opacity)));
      return paint;
    }

    return std::nullopt;
  }

  inline Lengthd toPercent(Lengthd value, bool numbersArePercent) {
    if (!numbersArePercent) {
      return value;
    }

    if (value.unit == Lengthd::Unit::None) {
      value.value *= 100.0;
      value.unit = Lengthd::Unit::Percent;
    }

    assert(value.unit == Lengthd::Unit::Percent);
    return value;
  }

  inline SkScalar resolveGradientCoord(Lengthd value, const Boxd& viewBox, bool numbersArePercent) {
    // Not plumbing FontMetrics here, since only percentage values are accepted.
    return NarrowToFloat(toPercent(value, numbersArePercent).toPixels(viewBox, FontMetrics()));
  }

  Vector2d resolveGradientCoords(Lengthd x, Lengthd y, const Boxd& viewBox,
                                 bool numbersArePercent) {
    return Vector2d(
        toPercent(x, numbersArePercent).toPixels(viewBox, FontMetrics(), Lengthd::Extent::X),
        toPercent(y, numbersArePercent).toPixels(viewBox, FontMetrics(), Lengthd::Extent::Y));
  }

  static bool CircleContainsPoint(Vector2d center, double radius, Vector2d point) {
    return (point - center).lengthSquared() <= radius * radius;
  }

  Transformd ResolveTransform(
      const components::ComputedLocalTransformComponent* maybeTransformComponent,
      const Boxd& viewBox, const FontMetrics& fontMetrics) {
    if (maybeTransformComponent) {
      const Vector2d origin = maybeTransformComponent->transformOrigin;
      const Transformd entityFromParent =
          maybeTransformComponent->rawCssTransform.compute(viewBox, fontMetrics);
      return Transformd::Translate(origin) * entityFromParent * Transformd::Translate(-origin);
    } else {
      return Transformd();
    }
  }

  std::optional<SkPaint> instantiateGradient(
      EntityHandle target, const components::ComputedGradientComponent& computedGradient,
      const components::PaintResolvedReference& ref, const Boxd& pathBounds, const Boxd& viewBox,
      css::RGBA currentColor, float opacity) {
    // Apply gradientUnits and gradientTransform.
    const bool objectBoundingBox =
        computedGradient.gradientUnits == GradientUnits::ObjectBoundingBox;

    const auto* maybeTransformComponent =
        target.try_get<components::ComputedLocalTransformComponent>();

    bool numbersArePercent = false;
    Transformd gradientFromGradientUnits;

    if (objectBoundingBox) {
      // From https://www.w3.org/TR/SVG2/coords.html#ObjectBoundingBoxUnits:
      //
      // > Keyword objectBoundingBox should not be used when the geometry of the applicable
      // > element has no width or no height, such as the case of a horizontal or vertical line,
      // > even when the line has actual thickness when viewed due to having a non-zero stroke
      // > width since stroke width is ignored for bounding box calculations. When the geometry
      // > of the applicable element has no width or height and objectBoundingBox is specified,
      // > then the given effect (e.g., a gradient or a filter) will be ignored.
      //
      if (NearZero(pathBounds.width()) || NearZero(pathBounds.height())) {
        return createFallbackPaint(ref, currentColor, opacity);
      }

      gradientFromGradientUnits =
          ResolveTransform(maybeTransformComponent, kUnitPathBounds, FontMetrics());

      // Apply scaling and translation from unit box to path bounds
      const Transformd objectBoundingBoxFromUnitBox =
          Transformd::Scale(pathBounds.size()) * Transformd::Translate(pathBounds.topLeft);

      // Combine the transforms
      gradientFromGradientUnits = gradientFromGradientUnits * objectBoundingBoxFromUnitBox;

      // TODO(jwmcglynn): Can numbersArePercent be represented by the transform instead?
      numbersArePercent = true;
    } else {
      gradientFromGradientUnits = ResolveTransform(maybeTransformComponent, viewBox, FontMetrics());
    }

    const Boxd& bounds = objectBoundingBox ? kUnitPathBounds : viewBox;

    std::vector<SkScalar> pos;
    std::vector<SkColor> color;
    for (const GradientStop& stop : computedGradient.stops) {
      pos.push_back(stop.offset);
      color.push_back(toSkia(stop.color.resolve(currentColor, stop.opacity * opacity)));
    }

    assert(pos.size() == color.size());

    // From https://www.w3.org/TR/SVG2/pservers.html#StopNotes:
    //
    // > It is necessary that at least two stops defined to have a gradient effect. If no stops
    // > are defined, then painting shall occur as if 'none' were specified as the paint style.
    // > If one stop is defined, then paint with the solid color fill using the color defined
    // > for that gradient stop.
    //
    if (pos.empty() || pos.size() > std::numeric_limits<int>::max()) {
      return createFallbackPaint(ref, currentColor, opacity);
    }

    const int numStops = static_cast<int>(pos.size());
    if (numStops == 1) {
      SkPaint paint;
      paint.setAntiAlias(renderer_.antialias_);
      paint.setColor(color[0]);
      return paint;
    }

    // Transform applied to the gradient coordinates, and for radial gradients the focal point and
    // radius.
    const SkMatrix skGradientFromGradientUnits = toSkiaMatrix(gradientFromGradientUnits);

    if (const auto* linearGradient =
            target.try_get<components::ComputedLinearGradientComponent>()) {
      const SkPoint points[] = {toSkia(resolveGradientCoords(linearGradient->x1, linearGradient->y1,
                                                             bounds, numbersArePercent)),
                                toSkia(resolveGradientCoords(linearGradient->x2, linearGradient->y2,
                                                             bounds, numbersArePercent))};

      SkPaint paint;
      paint.setAntiAlias(renderer_.antialias_);
      paint.setShader(SkGradientShader::MakeLinear(
          static_cast<const SkPoint*>(points), color.data(), pos.data(), numStops,
          toSkia(computedGradient.spreadMethod), 0, &skGradientFromGradientUnits));
      return paint;
    } else {
      const auto& radialGradient = target.get<components::ComputedRadialGradientComponent>();
      const Vector2d center =
          resolveGradientCoords(radialGradient.cx, radialGradient.cy, bounds, numbersArePercent);
      const SkScalar radius = resolveGradientCoord(radialGradient.r, bounds, numbersArePercent);

      Vector2d focalCenter = resolveGradientCoords(radialGradient.fx.value_or(radialGradient.cx),
                                                   radialGradient.fy.value_or(radialGradient.cy),
                                                   bounds, numbersArePercent);
      const SkScalar focalRadius =
          resolveGradientCoord(radialGradient.fr, bounds, numbersArePercent);

      if (NearZero(radius)) {
        SkPaint paint;
        paint.setAntiAlias(renderer_.antialias_);
        paint.setColor(color.back());
        return paint;
      }

      // NOTE: In SVG1, if the focal point lies outside of the circle, the focal point set to
      // the intersection of the circle and the focal point.
      //
      // This changes in SVG2, where a cone is created,
      // https://www.w3.org/TR/SVG2/pservers.html#RadialGradientNotes:
      //
      // > If the start circle defined by 'fx', 'fy' and 'fr' lies outside the end circle
      // > defined by 'cx', 'cy', and 'r', effectively a cone is created, touched by the two
      // > circles. Areas outside the cone stay untouched by the gradient (transparent black).
      //
      // Skia will automatically create the cone, but we need to handle the degenerate case:
      //
      // > If the start [focal] circle fully overlaps with the end circle, no gradient is drawn.
      // > The area stays untouched (transparent black).
      //
      const double distanceBetweenCenters = (center - focalCenter).length();
      if (distanceBetweenCenters + radius <= focalRadius) {
        return std::nullopt;
      }

      SkPaint paint;
      paint.setAntiAlias(renderer_.antialias_);
      if (NearZero(focalRadius) && focalCenter == center) {
        paint.setShader(SkGradientShader::MakeRadial(
            toSkia(center), radius, color.data(), pos.data(), numStops,
            toSkia(computedGradient.spreadMethod), 0, &skGradientFromGradientUnits));
      } else {
        paint.setShader(SkGradientShader::MakeTwoPointConical(
            toSkia(focalCenter), focalRadius, toSkia(center), radius, color.data(), pos.data(),
            numStops, toSkia(computedGradient.spreadMethod), 0, &skGradientFromGradientUnits));
      }
      return paint;
    }
  }

  PreserveAspectRatio GetPreserveAspectRatio(EntityHandle handle) {
    if (const auto* preserveAspectRatioComponent =
            handle.try_get<components::PreserveAspectRatioComponent>()) {
      return preserveAspectRatioComponent->preserveAspectRatio;
    }

    return PreserveAspectRatio::None();
  }

  /**
   * Renders the mask contents to the current layer. The caller should call saveLayer before this
   * call.
   *
   * @param dataHandle The handle to the pattern data.
   * @param instance The rendering instance component for the currently rendered entity (same entity
   * as \p target).
   * @param target The target entity to which the pattern is applied.
   * @param ref The reference to the mask.
   */
  void instantiateMask(EntityHandle dataHandle,
                       const components::RenderingInstanceComponent& instance, EntityHandle target,
                       const components::ResolvedMask& ref) {
    if (!ref.subtreeInfo) {
      // Subtree did not instantiate, indicating that recursion was detected.
      return;
    }

    Registry& registry = *dataHandle.registry();

    auto layerBaseRestore = overrideLayerBaseTransform(instance.entityFromWorldTransform);

    if (renderer_.verbose_) {
      std::cout << "Start mask contents\n";
    }

    // Get maskUnits and maskContentUnits
    const components::MaskComponent& maskComponent =
        ref.reference.handle.get<components::MaskComponent>();

    // Get x, y, width, height with default values
    const Lengthd x = maskComponent.x.value_or(Lengthd(-10.0, Lengthd::Unit::Percent));
    const Lengthd y = maskComponent.y.value_or(Lengthd(-10.0, Lengthd::Unit::Percent));
    const Lengthd width = maskComponent.width.value_or(Lengthd(120.0, Lengthd::Unit::Percent));
    const Lengthd height = maskComponent.height.value_or(Lengthd(120.0, Lengthd::Unit::Percent));

    const Boxd shapeLocalBounds = components::ShapeSystem().getShapeBounds(target).value_or(Boxd());

    // Compute the reference bounds based on maskUnits
    Boxd maskUnitsBounds;

    if (maskComponent.maskUnits == MaskUnits::ObjectBoundingBox) {
      maskUnitsBounds = shapeLocalBounds;
    } else {
      // maskUnits == UserSpaceOnUse
      // Use the viewport as bounds
      maskUnitsBounds = components::LayoutSystem().getViewBox(instance.dataHandle(registry));
    }

    if (!maskComponent.useAutoBounds()) {
      // Resolve x, y, width, height
      const double x_px = x.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::X);
      const double y_px = y.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::Y);
      const double width_px = width.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::X);
      const double height_px = height.toPixels(maskUnitsBounds, FontMetrics(), Lengthd::Extent::Y);

      // Create maskBounds
      const Boxd maskBounds = Boxd::FromXYWH(x_px, y_px, width_px, height_px);

      // Apply clipRect with maskBounds
      renderer_.currentCanvas_->clipRect(toSkia(maskBounds), SkClipOp::kIntersect, true);
    }

    // Adjust layerBaseTransform_ according to maskContentUnits
    if (maskComponent.maskContentUnits == MaskContentUnits::ObjectBoundingBox) {
      // Compute the transform from mask content coordinate system to user space
      const Transformd userSpaceFromMaskContent = Transformd::Scale(shapeLocalBounds.size()) *
                                                  Transformd::Translate(shapeLocalBounds.topLeft);

      // Update the layer base transform
      layerBaseTransform_ = userSpaceFromMaskContent * layerBaseTransform_;
    } else {
      // maskContentUnits == UserSpaceOnUse
      // No adjustment needed
    }

    // Render the mask content
    assert(ref.subtreeInfo);
    if (!shapeLocalBounds.isEmpty()) {
      drawUntil(registry, ref.subtreeInfo->lastRenderedEntity);
    } else {
      // Skip child elements.
      skipUntil(registry, ref.subtreeInfo->lastRenderedEntity);
    }

    if (renderer_.verbose_) {
      std::cout << "End mask contents\n";
    }
  }

  /**
   * Instantiates a pattern paint. See \ref PatternUnits, \ref PatternContentUnits for details on
   * their behavior.
   *
   * @param branchType Determined by whether this is the fill or stroke.
   * @param dataHandle The handle to the pattern data.
   * @param target The target entity to which the pattern is applied.
   * @param computedPattern The resolved pattern component.
   * @param ref The reference to the pattern.
   * @param pathBounds The bounds of the path to which the pattern is applied.
   * @param viewBox The viewBox of the the target entity.
   * @param currentColor Current context color inherited from the parent.
   * @param opacity Current opacity inherited from the parent.
   * @return SkPaint instance containing the pattern.
   */
  std::optional<SkPaint> instantiatePattern(
      components::ShadowBranchType branchType, EntityHandle dataHandle, EntityHandle target,
      const components::ComputedPatternComponent& computedPattern,
      const components::PaintResolvedReference& ref, const Boxd& pathBounds, const Boxd& viewBox,
      css::RGBA currentColor, float opacity) {
    if (!ref.subtreeInfo) {
      // Subtree did not instantiate, indicating that recursion was detected.
      return std::nullopt;
    }

    Registry& registry = *dataHandle.registry();
    const bool objectBoundingBox = computedPattern.patternUnits == PatternUnits::ObjectBoundingBox;
    const bool patternContentObjectBoundingBox =
        computedPattern.patternContentUnits == PatternContentUnits::ObjectBoundingBox;

    const auto* maybeTransformComponent =
        target.try_get<components::ComputedLocalTransformComponent>();

    Transformd patternContentFromPatternTile;
    Transformd patternTileFromTarget;
    Boxd rect = computedPattern.tileRect;

    if (NearZero(computedPattern.tileRect.width()) || NearZero(computedPattern.tileRect.height())) {
      return createFallbackPaint(ref, currentColor, opacity);
    }

    if (objectBoundingBox) {
      // From https://www.w3.org/TR/SVG2/coords.html#ObjectBoundingBoxUnits:
      //
      // > Keyword objectBoundingBox should not be used when the geometry of the applicable
      // > element has no width or no height, such as the case of a horizontal or vertical line,
      // > even when the line has actual thickness when viewed due to having a non-zero stroke
      // > width since stroke width is ignored for bounding box calculations. When the geometry
      // > of the applicable element has no width or height and objectBoundingBox is specified,
      // > then the given effect (e.g., a gradient or a filter) will be ignored.
      //
      if (NearZero(pathBounds.width()) || NearZero(pathBounds.height())) {
        // Skip rendering the pattern contents
        assert(ref.subtreeInfo);
        skipUntil(registry, ref.subtreeInfo->lastRenderedEntity);

        return createFallbackPaint(ref, currentColor, opacity);
      }

      const Vector2d rectSize = rect.size();

      rect.topLeft = rect.topLeft * pathBounds.size() + pathBounds.topLeft;
      rect.bottomRight = rectSize * pathBounds.size() + rect.topLeft;
    }

    if (computedPattern.viewBox) {
      patternContentFromPatternTile =
          computedPattern.preserveAspectRatio.elementContentFromViewBoxTransform(
              rect.toOrigin(), computedPattern.viewBox);
    } else if (patternContentObjectBoundingBox) {
      patternContentFromPatternTile = Transformd::Scale(pathBounds.size());
    }

    patternTileFromTarget = Transformd::Translate(rect.topLeft) *
                            ResolveTransform(maybeTransformComponent, viewBox, FontMetrics());

    const SkRect skTileRect = toSkia(rect.toOrigin());

    SkCanvas* const savedCanvas = renderer_.currentCanvas_;
    const Transformd savedLayerBaseTransform = layerBaseTransform_;

    if (renderer_.verbose_) {
      std::cout << "Start pattern contents\n";
    }

    SkPictureRecorder recorder;
    renderer_.currentCanvas_ = recorder.beginRecording(skTileRect);
    layerBaseTransform_ = patternContentFromPatternTile;

    // Render the subtree into the offscreen SkPictureRecorder.
    assert(ref.subtreeInfo);
    drawUntil(registry, ref.subtreeInfo->lastRenderedEntity);

    if (renderer_.verbose_) {
      std::cout << "End pattern contents\n";
    }

    renderer_.currentCanvas_ = savedCanvas;
    layerBaseTransform_ = savedLayerBaseTransform;

    // Transform to apply to the pattern contents.
    const SkMatrix skPatternContentFromPatternTile = toSkiaMatrix(patternTileFromTarget);

    SkPaint skPaint;
    skPaint.setAntiAlias(renderer_.antialias_);
    skPaint.setShader(recorder.finishRecordingAsPicture()->makeShader(
        SkTileMode::kRepeat, SkTileMode::kRepeat, SkFilterMode::kLinear,
        &skPatternContentFromPatternTile, &skTileRect));
    return skPaint;
  }

  std::optional<SkPaint> instantiatePaintReference(components::ShadowBranchType branchType,
                                                   EntityHandle dataHandle,
                                                   const components::PaintResolvedReference& ref,
                                                   const Boxd& pathBounds, const Boxd& viewBox,
                                                   css::RGBA currentColor, float opacity) {
    const EntityHandle target = ref.reference.handle;

    if (const auto* computedGradient = target.try_get<components::ComputedGradientComponent>()) {
      return instantiateGradient(target, *computedGradient, ref, pathBounds, viewBox, currentColor,
                                 opacity);
    } else if (const auto* computedPattern =
                   target.try_get<components::ComputedPatternComponent>()) {
      return instantiatePattern(branchType, dataHandle, target, *computedPattern, ref, pathBounds,
                                viewBox, currentColor, opacity);
    }

    UTILS_UNREACHABLE();  // The computed tree should invalidate any references that don't point
                          // to a valid point server, see IsValidPaintServer.
  }

  void drawPathFillWithSkPaint(const components::ComputedPathComponent& path, SkPaint& skPaint,
                               const PropertyRegistry& style) {
    SkPath skPath = toSkia(path.spline);
    if (style.fillRule.get() == FillRule::EvenOdd) {
      skPath.setFillType(SkPathFillType::kEvenOdd);
    }

    skPaint.setAntiAlias(renderer_.antialias_);
    skPaint.setStyle(SkPaint::Style::kFill_Style);
    renderer_.currentCanvas_->drawPath(skPath, skPaint);
  }

  void drawPathFill(EntityHandle dataHandle, const components::ComputedPathComponent& path,
                    const components::ResolvedPaintServer& paint, const PropertyRegistry& style,
                    const Boxd& viewBox) {
    const float fillOpacity = NarrowToFloat(style.fillOpacity.get().value());

    if (renderer_.verbose_) {
      std::cout << "Drawing path bounds " << path.spline.bounds() << "\n";
    }

    if (const auto* solid = std::get_if<PaintServer::Solid>(&paint)) {
      SkPaint skPaint;
      skPaint.setAntiAlias(renderer_.antialias_);
      skPaint.setColor(toSkia(solid->color.resolve(style.color.getRequired().rgba(), fillOpacity)));

      drawPathFillWithSkPaint(path, skPaint, style);
    } else if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint)) {
      std::optional<SkPaint> skPaint = instantiatePaintReference(
          components::ShadowBranchType::OffscreenFill, dataHandle, *ref, path.spline.bounds(),
          viewBox, style.color.getRequired().rgba(), fillOpacity);
      if (skPaint) {
        drawPathFillWithSkPaint(path, skPaint.value(), style);
      }
    }
  }

  struct StrokeConfig {
    double strokeWidth;
    double miterLimit;
  };

  void drawPathStrokeWithSkPaint(EntityHandle dataHandle,
                                 const components::ComputedPathComponent& path,
                                 const StrokeConfig& config, SkPaint& skPaint,
                                 const PropertyRegistry& style, const Boxd& viewBox,
                                 const FontMetrics& fontMetrics) {
    const SkPath skPath = toSkia(path.spline);

    if (style.strokeDasharray.hasValue()) {
      double dashUnitsScale = 1.0;
      if (const auto* pathLength = dataHandle.try_get<components::PathLengthComponent>();
          pathLength && !NearZero(pathLength->value)) {
        // If the user specifies a path length, we need to scale between the user's length
        // and computed length.
        const double skiaLength = SkPathMeasure(skPath, false).getLength();
        dashUnitsScale = skiaLength / pathLength->value;
      }

      // Use getRequiredRef to avoid copying the vector on access.
      const StrokeDasharray& dashes = style.strokeDasharray.getRequiredRef();

      // We need to repeat if there are an odd number of values, Skia requires an even number
      // of dash lengths.
      const int numRepeats = (dashes.size() & 1) ? 2 : 1;

      std::vector<SkScalar> skiaDashes;
      skiaDashes.reserve(dashes.size() * numRepeats);

      for (int i = 0; i < numRepeats; ++i) {
        for (const Lengthd& dash : dashes) {
          skiaDashes.push_back(
              static_cast<float>(dash.toPixels(viewBox, fontMetrics) * dashUnitsScale));
        }
      }

      skPaint.setPathEffect(SkDashPathEffect::Make(
          skiaDashes.data(), static_cast<int>(skiaDashes.size()),
          static_cast<SkScalar>(
              style.strokeDashoffset.get().value().toPixels(viewBox, fontMetrics) *
              dashUnitsScale)));
    }

    skPaint.setAntiAlias(renderer_.antialias_);
    skPaint.setStyle(SkPaint::Style::kStroke_Style);

    skPaint.setStrokeWidth(static_cast<SkScalar>(config.strokeWidth));
    skPaint.setStrokeCap(toSkia(style.strokeLinecap.get().value()));
    skPaint.setStrokeJoin(toSkia(style.strokeLinejoin.get().value()));
    skPaint.setStrokeMiter(static_cast<SkScalar>(config.miterLimit));

    renderer_.currentCanvas_->drawPath(skPath, skPaint);
  }

  void drawPathStroke(EntityHandle dataHandle, const components::ComputedPathComponent& path,
                      const components::ResolvedPaintServer& paint, const PropertyRegistry& style,
                      const Boxd& viewBox, const FontMetrics& fontMetrics) {
    const StrokeConfig config = {
        .strokeWidth = style.strokeWidth.get().value().toPixels(viewBox, fontMetrics),
        .miterLimit = style.strokeMiterlimit.get().value()};
    const double strokeOpacity = style.strokeOpacity.get().value();

    if (config.strokeWidth <= 0.0) {
      return;
    }

    if (const auto* solid = std::get_if<PaintServer::Solid>(&paint)) {
      SkPaint skPaint;
      skPaint.setAntiAlias(renderer_.antialias_);
      skPaint.setColor(toSkia(solid->color.resolve(style.color.getRequired().rgba(),
                                                   static_cast<float>(strokeOpacity))));

      drawPathStrokeWithSkPaint(dataHandle, path, config, skPaint, style, viewBox, fontMetrics);
    } else if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint)) {
      std::optional<SkPaint> skPaint = instantiatePaintReference(
          components::ShadowBranchType::OffscreenStroke, dataHandle, *ref,
          path.spline.strokeMiterBounds(config.strokeWidth, config.miterLimit), viewBox,
          style.color.getRequired().rgba(), NarrowToFloat((strokeOpacity)));
      if (skPaint) {
        drawPathStrokeWithSkPaint(dataHandle, path, config, skPaint.value(), style, viewBox,
                                  fontMetrics);
      }
    }
  }

  void drawImage(EntityHandle dataHandle, const components::RenderingInstanceComponent& instance,
                 const components::LoadedImageComponent& image) {
    if (!image.image) {
      return;
    }

    SkBitmap bitmap;
    bitmap.allocPixels(SkImageInfo::MakeN32Premul(image.image->width, image.image->height));
    memcpy(bitmap.getPixels(), image.image->data.data(), image.image->data.size());

    bitmap.setImmutable();
    sk_sp<SkImage> skImage = SkImages::RasterFromBitmap(bitmap);

    SkPaint paint;
    paint.setAntiAlias(renderer_.antialias_);
    paint.setStroke(true);
    paint.setColor(toSkia(css::RGBA(255, 255, 255, 255)));

    const auto& sizedElement = dataHandle.get<components::ComputedSizedElementComponent>();

    const PreserveAspectRatio preserveAspectRatio =
        dataHandle.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;

    const Boxd intrinsicSize = Boxd::WithSize(Vector2d(image.image->width, image.image->height));

    const Transformd imageFromLocal =
        preserveAspectRatio.elementContentFromViewBoxTransform(sizedElement.bounds, intrinsicSize);

    renderer_.currentCanvas_->save();
    renderer_.currentCanvas_->clipRect(toSkia(sizedElement.bounds));
    renderer_.currentCanvas_->concat(toSkia(imageFromLocal));
    renderer_.currentCanvas_->drawImage(skImage, 0, 0, SkSamplingOptions(SkFilterMode::kLinear),
                                        &paint);
    renderer_.currentCanvas_->restore();
  }

  // Draws text content using computed spans and typography resolved per span
  void drawText(EntityHandle dataHandle, const components::RenderingInstanceComponent& instance,
                const components::ComputedTextComponent& text, const PropertyRegistry& style,
                const Boxd& viewBox, const FontMetrics& fontMetrics) {
    SkPaint skPaint;

    if (!HasPaint(instance.resolvedFill)) {
      return;
    }

    if (const auto* solid = std::get_if<PaintServer::Solid>(&instance.resolvedFill)) {
      const float fillOpacity = NarrowToFloat(style.fillOpacity.get().value());

      skPaint.setAntiAlias(renderer_.antialias_);
      skPaint.setColor(toSkia(solid->color.resolve(style.color.getRequired().rgba(), fillOpacity)));
    } else {
      return;
    }

    // Draw each text span
    for (const auto& span : text.spans) {
      const auto& spanStyle = span.style;

      // Determine font size in pixels
      const Lengthd sizeLen = spanStyle.fontSize;
      const SkScalar fontSizePx =
          static_cast<SkScalar>(sizeLen.toPixels(viewBox, fontMetrics, Lengthd::Extent::Mixed));

      // Load typeface by family and style
      const SkFontStyle skFontStyle = ToSkFontStyle(spanStyle);
      const sk_sp<SkTypeface> typeface =
          ResolveTypeface(spanStyle.fontFamily, skFontStyle, renderer_.typefaces_,
                          *renderer_.fontMgr_, renderer_.fallbackTypeface_);

      SkFont font(typeface, fontSizePx);
      if (renderer_.antialias_) {
        font.setEdging(SkFont::Edging::kAntiAlias);
        font.setSubpixel(true);
      } else {
        font.setEdging(SkFont::Edging::kAlias);
        font.setSubpixel(false);
      }

      // Check if we have per-glyph positioning (multiple x/y values)
      const bool hasPerGlyphPositioning = span.x.size() > 1 || span.y.size() > 1;

      // Compute base positions (used when no per-glyph positioning)
      const auto computeX = [&](size_t index = 0) -> SkScalar {
        const Lengthd xVal = index < span.x.size() ? span.x[index] : (span.x.empty() ? Lengthd() : span.x[0]);
        const Lengthd dxVal = index < span.dx.size() ? span.dx[index] : (span.dx.empty() ? Lengthd() : span.dx[0]);
        return static_cast<SkScalar>(xVal.toPixels(viewBox, fontMetrics, Lengthd::Extent::X) +
                                     dxVal.toPixels(viewBox, fontMetrics, Lengthd::Extent::X));
      };

      const auto computeY = [&](size_t index = 0) -> SkScalar {
        const Lengthd yVal = index < span.y.size() ? span.y[index] : (span.y.empty() ? Lengthd() : span.y[0]);
        const Lengthd dyVal = index < span.dy.size() ? span.dy[index] : (span.dy.empty() ? Lengthd() : span.dy[0]);
        return static_cast<SkScalar>(yVal.toPixels(viewBox, fontMetrics, Lengthd::Extent::Y) +
                                     dyVal.toPixels(viewBox, fontMetrics, Lengthd::Extent::Y));
      };

      const SkScalar x = computeX(0);
      const SkScalar y = computeY(0);

      // Apply rotation if specified
      bool rotated = false;
      if (span.rotateDegrees != 0.0) {
        const SkScalar angle = static_cast<SkScalar>(span.rotateDegrees);
        renderer_.currentCanvas_->save();
        renderer_.currentCanvas_->translate(x, y);
        renderer_.currentCanvas_->rotate(angle);
        renderer_.currentCanvas_->translate(-x, -y);
        rotated = true;
      }

      // Shape text using SkShaper for proper kerning, ligatures, and complex text layout
      // When antialiasing is disabled (e.g., for tests), use simple text rendering for consistency
      const std::string_view textStr = span.text;
      std::unique_ptr<SkShaper> shaper;

      if (renderer_.antialias_) {
#if defined(SK_SHAPER_CORETEXT_AVAILABLE)
        // Use CoreText shaper on macOS (no glib dependency needed)
        shaper = SkShapers::CT::CoreText();
#elif defined(SK_SHAPER_HARFBUZZ_AVAILABLE)
        // Use HarfBuzz shaper for proper kerning on Linux
        sk_sp<SkUnicode> unicode = nullptr;  // No Unicode support needed for basic Latin text
        shaper = SkShapers::HB::ShaperDrivenWrapper(unicode, renderer_.fontMgr_);
#endif
      }

      if (shaper && hasPerGlyphPositioning) {
        // Per-glyph positioning: render each character at its specified position
        // This implements SVG's multiple x/y values feature
        const SkScalar baselineOffset = -font.getSpacing() * 0.78f;

        size_t charIndex = 0;
        size_t byteIndex = 0;
        while (byteIndex < textStr.size()) {
          // Find the next UTF-8 character
          size_t charBytes = 1;
          unsigned char firstByte = static_cast<unsigned char>(textStr[byteIndex]);
          if ((firstByte & 0x80) == 0) {
            charBytes = 1;  // ASCII
          } else if ((firstByte & 0xE0) == 0xC0) {
            charBytes = 2;
          } else if ((firstByte & 0xF0) == 0xE0) {
            charBytes = 3;
          } else if ((firstByte & 0xF8) == 0xF0) {
            charBytes = 4;
          }

          // Get the character substring
          std::string_view charStr = textStr.substr(byteIndex, charBytes);

          // Compute position for this character
          const SkScalar charX = computeX(charIndex);
          const SkScalar charY = computeY(charIndex);

          // Shape and draw this single character
          auto fontIter = std::make_unique<SkShaper::TrivialFontRunIterator>(font, charStr.size());
          auto bidiIter = std::make_unique<SkShaper::TrivialBiDiRunIterator>(0, charStr.size());
          auto scriptIter = std::make_unique<SkShaper::TrivialScriptRunIterator>(
              SkSetFourByteTag('L', 'a', 't', 'n'), charStr.size());
          auto langIter = std::make_unique<SkShaper::TrivialLanguageRunIterator>("en", charStr.size());

          SkTextBlobBuilderRunHandler runHandler(charStr.data(), {0, baselineOffset});
          shaper->shape(charStr.data(), charStr.size(), *fontIter, *bidiIter, *scriptIter,
                        *langIter, nullptr, 0, std::numeric_limits<SkScalar>::max(), &runHandler);
          sk_sp<SkTextBlob> blob = runHandler.makeBlob();

          if (blob) {
            renderer_.currentCanvas_->drawTextBlob(blob, charX, charY, skPaint);
          }

          byteIndex += charBytes;
          charIndex++;
        }
      } else if (shaper) {
        // Normal text flow: shape and render the entire span at once
        // Create simple run iterators for basic text shaping
        auto fontIter = std::make_unique<SkShaper::TrivialFontRunIterator>(font, textStr.size());
        auto bidiIter =
            std::make_unique<SkShaper::TrivialBiDiRunIterator>(0 /* LTR */, textStr.size());
        auto scriptIter = std::make_unique<SkShaper::TrivialScriptRunIterator>(
            SkSetFourByteTag('L', 'a', 't', 'n'), textStr.size());
        auto langIter =
            std::make_unique<SkShaper::TrivialLanguageRunIterator>("en", textStr.size());

        // SkShaper positions glyphs differently than drawSimpleText - we need to adjust
        // the baseline position. The shaper outputs glyph positions with baseline at y=0,
        // but we need to shift them up so the baseline matches what drawSimpleText would produce.
        // Use font spacing (ascent + descent) as approximation for baseline adjustment
        // TODO(jwm): Debug why this baseline shift is required
        const SkScalar baselineOffset =
            -font.getSpacing() * 0.78f;  // Shift up by approximate ascent

        // Use SkTextBlobBuilderRunHandler to shape text into a SkTextBlob
        // Position glyphs with baseline adjustment
        SkTextBlobBuilderRunHandler runHandler(textStr.data(), {0, baselineOffset});
        shaper->shape(textStr.data(), textStr.size(), *fontIter, *bidiIter, *scriptIter, *langIter,
                      nullptr /* features */, 0 /* featuresSize */,
                      std::numeric_limits<SkScalar>::max() /* width - no wrapping */, &runHandler);
        sk_sp<SkTextBlob> blob = runHandler.makeBlob();

        if (blob) {
          // Draw the blob at the text position (baseline at x, y)
          renderer_.currentCanvas_->drawTextBlob(blob, x, y, skPaint);
        }
      } else {
        // Fallback to simple text if shaper is not available
        renderer_.currentCanvas_->drawSimpleText(textStr.data(), textStr.size(),
                                                 SkTextEncoding::kUTF8, x, y, font, skPaint);
      }

      if (rotated) {
        renderer_.currentCanvas_->restore();
      }
    }
  }

  void createFilterChain(SkPaint& filterPaint, const std::vector<FilterEffect>& effectList) {
    for (const FilterEffect& effect : effectList) {
      std::visit(entt::overloaded{//
                                  [&](const FilterEffect::None&) {},
                                  [&](const FilterEffect::Blur& blur) {
                                    // TODO(jwmcglynn): Convert these Length units
                                    filterPaint.setImageFilter(SkImageFilters::Blur(
                                        static_cast<float>(blur.stdDeviationX.value),
                                        static_cast<float>(blur.stdDeviationY.value), nullptr));
                                  },
                                  [&](const FilterEffect::ElementReference& ref) {
                                    assert(false && "Element references must already be resolved");
                                  }},
                 effect.value);
    }
  }

  void createFilterPaint(SkPaint& filterPaint, Registry& registry,
                         const components::ResolvedFilterEffect& filter) {
    if (const auto* effects = std::get_if<std::vector<FilterEffect>>(&filter)) {
      createFilterChain(filterPaint, *effects);
    } else if (const auto* reference = std::get_if<ResolvedReference>(&filter)) {
      if (const auto* computedFilter =
              registry.try_get<components::ComputedFilterComponent>(*reference)) {
        createFilterChain(filterPaint, computedFilter->effectChain);
      }
    }
  }

  void drawMarkers(EntityHandle dataHandle, const components::RenderingInstanceComponent& instance,
                   const components::ComputedPathComponent& path, const Boxd& viewBox,
                   const FontMetrics& fontMetrics) {
    const auto& commands = path.spline.commands();

    if (commands.size() < 2) {
      return;
    }

    bool hasMarkerStart = instance.markerStart.has_value();
    bool hasMarkerMid = instance.markerMid.has_value();
    bool hasMarkerEnd = instance.markerEnd.has_value();

    if (hasMarkerStart || hasMarkerMid || hasMarkerEnd) {
      const RenderingInstanceView::SavedState viewSnapshot = view_.save();

      const std::vector<PathSpline::Vertex> vertices = path.spline.vertices();

      for (size_t i = 0; i < vertices.size(); ++i) {
        const PathSpline::Vertex& vertex = vertices[i];

        if (i == 0) {
          if (hasMarkerStart) {
            drawMarker(dataHandle, instance, instance.markerStart.value(), vertex.point,
                       vertex.orientation, MarkerOrient::MarkerType::Start, viewBox, fontMetrics);
          }
        } else if (i == vertices.size() - 1) {
          if (hasMarkerEnd) {
            drawMarker(dataHandle, instance, instance.markerEnd.value(), vertex.point,
                       vertex.orientation, MarkerOrient::MarkerType::Default, viewBox, fontMetrics);
          }
        } else if (hasMarkerMid) {
          drawMarker(dataHandle, instance, instance.markerMid.value(), vertex.point,
                     vertex.orientation, MarkerOrient::MarkerType::Default, viewBox, fontMetrics);
        }

        view_.restore(viewSnapshot);
      }

      // Skipping the rendered marker definitions to avoid duplication
      if (hasMarkerEnd) {
        skipUntil(*dataHandle.registry(),
                  instance.markerEnd.value().subtreeInfo->lastRenderedEntity);
      } else if (hasMarkerMid) {
        skipUntil(*dataHandle.registry(),
                  instance.markerMid.value().subtreeInfo->lastRenderedEntity);
      } else if (hasMarkerStart) {
        skipUntil(*dataHandle.registry(),
                  instance.markerStart.value().subtreeInfo->lastRenderedEntity);
      }
    }
  }

  void drawMarker(EntityHandle dataHandle, const components::RenderingInstanceComponent& instance,
                  const components::ResolvedMarker& marker, const Vector2d& vertexPosition,
                  const Vector2d& direction, MarkerOrient::MarkerType markerOrientType,
                  const Boxd& viewBox, const FontMetrics& fontMetrics) {
    Registry& registry = *dataHandle.registry();

    const EntityHandle markerHandle = marker.reference.handle;

    if (!markerHandle.valid()) {
      return;
    }

    // Get the marker component
    const auto& markerComponent = markerHandle.get<components::MarkerComponent>();

    if (markerComponent.markerWidth <= 0.0 || markerComponent.markerHeight <= 0.0) {
      return;
    }

    const Boxd markerSize =
        Boxd::FromXYWH(0, 0, markerComponent.markerWidth, markerComponent.markerHeight);

    // Get the marker's viewBox and preserveAspectRatio
    components::LayoutSystem layoutSystem;

    const std::optional<Boxd> markerViewBox =
        layoutSystem.overridesViewBox(markerHandle)
            ? std::optional<Boxd>(layoutSystem.getViewBox(markerHandle))
            : std::nullopt;
    const PreserveAspectRatio preserveAspectRatio =
        markerHandle.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;

    // Compute the rotation angle according to the orient attribute
    const double angleRadians =
        markerComponent.orient.computeAngleRadians(direction, markerOrientType);

    // Compute scale according to markerUnits
    double markerScale = 1.0;
    if (markerComponent.markerUnits == MarkerUnits::StrokeWidth) {
      // Scale by stroke width
      const components::ComputedStyleComponent& styleComponent =
          instance.styleHandle(registry).get<components::ComputedStyleComponent>();
      const double strokeWidth = styleComponent.properties->strokeWidth.getRequired().value;
      markerScale = strokeWidth;
    }

    const Transformd markerUnitsFromViewBox =
        preserveAspectRatio.elementContentFromViewBoxTransform(markerSize, markerViewBox);

    const Transformd markerOffsetFromVertex =
        Transformd::Translate(-markerComponent.refX * markerUnitsFromViewBox.data[0],
                              -markerComponent.refY * markerUnitsFromViewBox.data[3]);

    const Transformd vertexFromEntity = Transformd::Scale(markerScale) *
                                        Transformd::Rotate(angleRadians) *
                                        Transformd::Translate(vertexPosition);

    const Transformd vertexFromWorld =
        vertexFromEntity * layerBaseTransform_ * instance.entityFromWorldTransform;

    const Transformd markerUserSpaceFromWorld =
        Transformd::Scale(markerUnitsFromViewBox.data[0], markerUnitsFromViewBox.data[3]) *
        markerOffsetFromVertex * vertexFromWorld;

    // Now, render the marker's content with the computed transform
    auto layerBaseRestore = overrideLayerBaseTransform(markerUserSpaceFromWorld);

    renderer_.currentCanvas_->save();
    renderer_.currentCanvas_->resetMatrix();

    const auto& computedStyle = markerHandle.get<components::ComputedStyleComponent>();
    const Overflow overflow = computedStyle.properties->overflow.getRequired();
    if (overflow != Overflow::Visible && overflow != Overflow::Auto) {
      renderer_.currentCanvas_->clipRect(
          toSkia(markerUserSpaceFromWorld.transformBox(markerViewBox.value_or(markerSize))));
    }

    // Render the marker's content
    if (marker.subtreeInfo) {
      // Draw the marker's subtree
      drawRange(registry, marker.subtreeInfo->firstRenderedEntity,
                marker.subtreeInfo->lastRenderedEntity);
    }

    renderer_.currentCanvas_->restore();
  }

private:
  struct LayerBaseRestore {
    LayerBaseRestore(RendererSkia::Impl& impl, const Transformd& savedTransform)
        : impl_(impl), savedTransform_(savedTransform) {}

    ~LayerBaseRestore() { impl_.layerBaseTransform_ = savedTransform_; }

  private:
    RendererSkia::Impl& impl_;
    Transformd savedTransform_;
  };

  LayerBaseRestore overrideLayerBaseTransform(const Transformd& newLayerBaseTransform) {
    const Transformd savedTransform = layerBaseTransform_;
    layerBaseTransform_ = newLayerBaseTransform;
    return LayerBaseRestore(*this, savedTransform);
  }

  RendererSkia& renderer_;
  RenderingInstanceView view_;

  std::vector<components::SubtreeInfo> subtreeMarkers_;
  Transformd layerBaseTransform_ = Transformd();
};

RendererSkia::RendererSkia(bool verbose) : verbose_(verbose) {}

RendererSkia::~RendererSkia() {}

RendererSkia::RendererSkia(RendererSkia&&) noexcept = default;
RendererSkia& RendererSkia::operator=(RendererSkia&&) noexcept = default;

void RendererSkia::draw(SVGDocument& document) {
  // TODO(jwmcglynn): Plumb outWarnings.
  std::vector<ParseError> warnings;
  RendererUtils::prepareDocumentForRendering(document, verbose_, verbose_ ? &warnings : nullptr);

  if (!warnings.empty()) {
    for (const ParseError& warning : warnings) {
      std::cerr << warning << '\n';
    }
  }

  const Vector2i renderingSize = document.canvasSize();

  bitmap_.allocPixels(
      SkImageInfo::MakeN32(renderingSize.x, renderingSize.y, SkAlphaType::kUnpremul_SkAlphaType));
  SkCanvas canvas(bitmap_);
  rootCanvas_ = &canvas;
  currentCanvas_ = &canvas;

  draw(document.registry());

  rootCanvas_ = currentCanvas_ = nullptr;
}

std::string RendererSkia::drawIntoAscii(SVGDocument& document) {
  // TODO(jwmcglynn): Plumb outWarnings.
  RendererUtils::prepareDocumentForRendering(document, verbose_, nullptr);

  const Vector2i renderingSize = document.canvasSize();

  assert(renderingSize.x <= 64 && renderingSize.y <= 64 &&
         "Rendering size must be less than or equal to 64x64");

  bitmap_.allocPixels(SkImageInfo::Make(renderingSize.x, renderingSize.y, kGray_8_SkColorType,
                                        kOpaque_SkAlphaType));
  SkCanvas canvas(bitmap_);
  rootCanvas_ = &canvas;
  currentCanvas_ = &canvas;

  draw(document.registry());

  rootCanvas_ = currentCanvas_ = nullptr;

  std::string asciiArt;
  asciiArt.reserve(renderingSize.x * renderingSize.y +
                   renderingSize.y);  // Reserve space including newlines

  static const std::array<char, 10> grayscaleTable = {'.', ',', ':', '-', '=',
                                                      '+', '*', '#', '%', '@'};

  for (int y = 0; y < renderingSize.y; ++y) {
    for (int x = 0; x < renderingSize.x; ++x) {
      const uint8_t pixel = *bitmap_.getAddr8(x, y);
      size_t index = pixel / static_cast<size_t>(256 / grayscaleTable.size());
      if (index >= grayscaleTable.size()) {
        index = grayscaleTable.size() - 1;
      }
      asciiArt += grayscaleTable.at(index);
    }

    asciiArt += '\n';
  }

  bitmap_.reset();

  return asciiArt;
}

sk_sp<SkPicture> RendererSkia::drawIntoSkPicture(SVGDocument& document) {
  Registry& registry = document.registry();

  // TODO(jwmcglynn): Plumb outWarnings.
  RendererUtils::prepareDocumentForRendering(document, verbose_);

  const Vector2i renderingSize = components::LayoutSystem().calculateCanvasScaledDocumentSize(
      registry, components::LayoutSystem::InvalidSizeBehavior::ReturnDefault);

  SkPictureRecorder recorder;
  rootCanvas_ = recorder.beginRecording(toSkia(Boxd::WithSize(renderingSize)));
  currentCanvas_ = rootCanvas_;

  draw(registry);

  rootCanvas_ = currentCanvas_ = nullptr;

  return recorder.finishRecordingAsPicture();
}

bool RendererSkia::save(const char* filename) {
  assert(bitmap_.colorType() == kRGBA_8888_SkColorType);
  return RendererImageIO::writeRgbaPixelsToPngFile(filename, pixelData(), bitmap_.width(),
                                                   bitmap_.height());
}

std::span<const uint8_t> RendererSkia::pixelData() const {
  return std::span<const uint8_t>(static_cast<const uint8_t*>(bitmap_.getPixels()),
                                  bitmap_.computeByteSize());
}

void RendererSkia::draw(Registry& registry) {
  Impl impl(*this, RenderingInstanceView{registry});
  impl.initialize(registry);
  impl.drawUntil(registry, entt::null);
}

}  // namespace donner::svg
