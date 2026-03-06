#include "tiny_skia/Mask.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace tiny_skia {

std::optional<Mask> Mask::fromSize(std::uint32_t width, std::uint32_t height) {
  const auto size = IntSize::fromWH(width, height);
  if (!size.has_value()) {
    return std::nullopt;
  }

  const auto dataLen = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  return Mask(std::vector<std::uint8_t>(dataLen, 0), size.value());
}

std::optional<Mask> Mask::fromVec(std::vector<std::uint8_t> data, IntSize size) {
  const auto dataLen =
      static_cast<std::size_t>(size.width()) * static_cast<std::size_t>(size.height());
  if (data.size() != dataLen) {
    return std::nullopt;
  }

  return Mask(std::move(data), size);
}

Mask Mask::fromPixmap(const PixmapView& pixmap, MaskType maskType) {
  const auto dataLen =
      static_cast<std::size_t>(pixmap.width()) * static_cast<std::size_t>(pixmap.height());
  auto data = std::vector<std::uint8_t>(dataLen, 0);

  const auto pixels = pixmap.pixels();
  switch (maskType) {
    case MaskType::Alpha:
      for (std::size_t i = 0; i < pixels.size(); ++i) {
        data[i] = pixels[i].alpha();
      }
      break;
    case MaskType::Luminance:
      for (std::size_t i = 0; i < pixels.size(); ++i) {
        float r = static_cast<float>(pixels[i].red()) / 255.0f;
        float g = static_cast<float>(pixels[i].green()) / 255.0f;
        float b = static_cast<float>(pixels[i].blue()) / 255.0f;
        const float a = static_cast<float>(pixels[i].alpha()) / 255.0f;

        if (pixels[i].alpha() != 0) {
          r /= a;
          g /= a;
          b /= a;
        }

        const auto luma = r * 0.2126f + g * 0.7152f + b * 0.0722f;
        const auto masked = std::clamp((luma * a) * 255.0f, 0.0f, 255.0f);
        data[i] = static_cast<std::uint8_t>(std::ceil(masked));
      }
      break;
  }

  return Mask(std::move(data), pixmap.size());
}

SubMaskView Mask::submask() const {
  return SubMaskView{
      .size = size_,
      .realWidth = width(),
      .data = data_.data(),
  };
}

std::optional<SubMaskView> Mask::submask(IntRect rect) const {
  const auto self = IntRect::fromXYWH(0, 0, width(), height());
  if (!self.has_value()) {
    return std::nullopt;
  }

  const auto intersection = self->intersect(rect);
  if (!intersection.has_value()) {
    return std::nullopt;
  }

  const auto rowBytes = static_cast<std::size_t>(width());
  const auto offset = static_cast<std::size_t>(intersection->top()) * rowBytes +
                      static_cast<std::size_t>(intersection->left());
  const auto subSize = IntSize::fromWH(intersection->width(), intersection->height());
  if (!subSize.has_value()) {
    return std::nullopt;
  }

  return SubMaskView{
      .size = subSize.value(),
      .realWidth = width(),
      .data = data_.data() + offset,
  };
}

MutableSubMaskView Mask::subpixmap() {
  return MutableSubMaskView{
      .size = size_,
      .realWidth = width(),
      .data = data_.data(),
  };
}

std::optional<MutableSubMaskView> Mask::subpixmap(IntRect rect) {
  const auto self = IntRect::fromXYWH(0, 0, width(), height());
  if (!self.has_value()) {
    return std::nullopt;
  }

  const auto intersection = self->intersect(rect);
  if (!intersection.has_value()) {
    return std::nullopt;
  }

  const auto rowBytes = static_cast<std::size_t>(width());
  const auto offset = static_cast<std::size_t>(intersection->top()) * rowBytes +
                      static_cast<std::size_t>(intersection->left());
  const auto subSize = IntSize::fromWH(intersection->width(), intersection->height());
  if (!subSize.has_value()) {
    return std::nullopt;
  }

  return MutableSubMaskView{
      .size = subSize.value(),
      .realWidth = width(),
      .data = data_.data() + offset,
  };
}

std::vector<std::uint8_t> Mask::release() {
  size_ = IntSize{};
  return std::move(data_);
}

void Mask::invert() {
  for (auto& a : data_) {
    a = static_cast<std::uint8_t>(255 - a);
  }
}

void Mask::clear() { std::fill(data_.begin(), data_.end(), static_cast<std::uint8_t>(0)); }

}  // namespace tiny_skia
