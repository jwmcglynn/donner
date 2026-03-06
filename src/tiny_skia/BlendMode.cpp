#include "tiny_skia/BlendMode.h"

namespace tiny_skia {

bool shouldPreScaleCoverage(BlendMode blendMode) {
  switch (blendMode) {
    case BlendMode::Destination:
    case BlendMode::DestinationOver:
    case BlendMode::DestinationOut:
    case BlendMode::SourceAtop:
    case BlendMode::SourceOver:
    case BlendMode::Xor:
    case BlendMode::Plus:
      return true;
    default:
      return false;
  }
}

std::optional<pipeline::Stage> toStage(BlendMode blendMode) {
  switch (blendMode) {
    case BlendMode::Clear:
      return pipeline::Stage::Clear;
    case BlendMode::Source:
      return std::nullopt;
    case BlendMode::Destination:
      return pipeline::Stage::MoveDestinationToSource;
    case BlendMode::SourceOver:
      return pipeline::Stage::SourceOver;
    case BlendMode::DestinationOver:
      return pipeline::Stage::DestinationOver;
    case BlendMode::SourceIn:
      return pipeline::Stage::SourceIn;
    case BlendMode::DestinationIn:
      return pipeline::Stage::DestinationIn;
    case BlendMode::SourceOut:
      return pipeline::Stage::SourceOut;
    case BlendMode::DestinationOut:
      return pipeline::Stage::DestinationOut;
    case BlendMode::SourceAtop:
      return pipeline::Stage::SourceAtop;
    case BlendMode::DestinationAtop:
      return pipeline::Stage::DestinationAtop;
    case BlendMode::Xor:
      return pipeline::Stage::Xor;
    case BlendMode::Plus:
      return pipeline::Stage::Plus;
    case BlendMode::Modulate:
      return pipeline::Stage::Modulate;
    case BlendMode::Screen:
      return pipeline::Stage::Screen;
    case BlendMode::Overlay:
      return pipeline::Stage::Overlay;
    case BlendMode::Darken:
      return pipeline::Stage::Darken;
    case BlendMode::Lighten:
      return pipeline::Stage::Lighten;
    case BlendMode::ColorDodge:
      return pipeline::Stage::ColorDodge;
    case BlendMode::ColorBurn:
      return pipeline::Stage::ColorBurn;
    case BlendMode::HardLight:
      return pipeline::Stage::HardLight;
    case BlendMode::SoftLight:
      return pipeline::Stage::SoftLight;
    case BlendMode::Difference:
      return pipeline::Stage::Difference;
    case BlendMode::Exclusion:
      return pipeline::Stage::Exclusion;
    case BlendMode::Multiply:
      return pipeline::Stage::Multiply;
    case BlendMode::Hue:
      return pipeline::Stage::Hue;
    case BlendMode::Saturation:
      return pipeline::Stage::Saturation;
    case BlendMode::Color:
      return pipeline::Stage::Color;
    case BlendMode::Luminosity:
      return pipeline::Stage::Luminosity;
    default:
      return std::nullopt;
  }
}

}  // namespace tiny_skia
