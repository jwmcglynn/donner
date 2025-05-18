#include "donner/svg/components/text/FontContext.h"

#include "include/core/SkFontMgr.h"

namespace donner::svg::components {

FontContext::FontContext(Registry& registry) : registry_(registry) {}

void FontContext::addFont(const RcString& family, sk_sp<SkData> data) {
  if (!data) {
    return;
  }
  sk_sp<SkTypeface> typeface = SkFontMgr::RefDefault()->makeFromData(std::move(data));
  if (typeface) {
    fonts_[family] = std::move(typeface);
  }
}

sk_sp<SkTypeface> FontContext::getTypeface(const RcString& family) const {
  auto it = fonts_.find(family);
  return it != fonts_.end() ? it->second : nullptr;
}

}  // namespace donner::svg::components
