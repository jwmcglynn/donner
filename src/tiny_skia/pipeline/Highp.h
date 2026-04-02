#pragma once

#include <array>
#include <cstddef>

#include "tiny_skia/pipeline/Pipeline.h"

namespace tiny_skia::pipeline::highp {

struct Pipeline;

using StageFn = void (*)(Pipeline&);

constexpr std::size_t kStageWidth = 8;

void justReturn(Pipeline&);

void start(const std::array<StageFn, tiny_skia::pipeline::kMaxStages>& functions,
           const std::array<StageFn, tiny_skia::pipeline::kMaxStages>& tailFunctions,
           const ScreenIntRect& rect, const AAMaskCtx& aaMaskCtx, const MaskCtx& maskCtx,
           Context& ctx, const PixmapView& pixmapSrc, MutableSubPixmapView* pixmapDst);

bool fnPtrEq(StageFn a, StageFn b);
const void* fnPtr(StageFn fn);

extern const std::array<StageFn, kStagesCount> STAGES;

}  // namespace tiny_skia::pipeline::highp
