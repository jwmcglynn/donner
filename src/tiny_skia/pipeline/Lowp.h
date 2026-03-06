#pragma once

#include <array>
#include <cstddef>

#include "tiny_skia/pipeline/Pipeline.h"

namespace tiny_skia::pipeline::lowp {

struct Pipeline;

using StageFn = void (*)(Pipeline&);

constexpr std::size_t kStageWidth = 16;

void justReturn(Pipeline&);

void start(const std::array<StageFn, tiny_skia::pipeline::kMaxStages>& functions,
           const std::array<StageFn, tiny_skia::pipeline::kMaxStages>& tailFunctions,
           const ScreenIntRect& rect, const AAMaskCtx& aaMaskCtx, const MaskCtx& maskCtx,
           Context& ctx, MutableSubPixmapView* pixmapDst);

bool fnPtrEq(StageFn a, StageFn b);
const void* fnPtr(StageFn fn);

extern const std::array<StageFn, kStagesCount> STAGES;
extern const std::array<StageFn, kStagesCount> STAGES_TAIL;

}  // namespace tiny_skia::pipeline::lowp
