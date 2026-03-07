// Disable FMA contraction to match Rust's highp pipeline, which uses software
// SIMD wrappers (f32x8) that prevent LLVM from fusing multiply-add.
#ifdef __clang__
#pragma clang fp contract(off)
#endif

#include "tiny_skia/pipeline/Highp.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/Math.h"
#include "tiny_skia/Pixmap.h"

namespace tiny_skia::pipeline::highp {

struct Pipeline {
  std::array<float, kStageWidth> r{};
  std::array<float, kStageWidth> g{};
  std::array<float, kStageWidth> b{};
  std::array<float, kStageWidth> a{};
  std::array<float, kStageWidth> dr{};
  std::array<float, kStageWidth> dg{};
  std::array<float, kStageWidth> db{};
  std::array<float, kStageWidth> da{};

  const std::array<StageFn, tiny_skia::pipeline::kMaxStages>* functions = nullptr;
  std::size_t index = 0;
  std::size_t dx = 0;
  std::size_t dy = 0;
  std::size_t tail = 0;
  const ScreenIntRect* rect = nullptr;
  const AAMaskCtx* aaMaskCtx = nullptr;
  const MaskCtx* maskCtx = nullptr;
  Context* ctx = nullptr;
  const PixmapView* pixmapSrc = nullptr;
  MutableSubPixmapView* pixmapDst = nullptr;

  Pipeline(const std::array<StageFn, tiny_skia::pipeline::kMaxStages>& fun,
           const std::array<StageFn, tiny_skia::pipeline::kMaxStages>&,
           const ScreenIntRect& rectArg, const AAMaskCtx& aaMaskCtxArg,
           const MaskCtx& maskCtxArg, Context& ctxArg, const PixmapView& pixmapSrcArg,
           MutableSubPixmapView* pixmapDstArg)
      : functions(&fun),
        rect(&rectArg),
        aaMaskCtx(&aaMaskCtxArg),
        maskCtx(&maskCtxArg),
        ctx(&ctxArg),
        pixmapSrc(&pixmapSrcArg),
        pixmapDst(pixmapDstArg) {}

  void nextStage() {
    const auto next = (*functions)[index];
    ++index;
    next(*this);
  }
};

bool fnPtrEq(StageFn a, StageFn b) { return a == b; }

const void* fnPtr(StageFn fn) { return reinterpret_cast<const void*>(fn); }

namespace {

void moveSourceToDestination(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.dr[i] = pipeline.r[i];
    pipeline.dg[i] = pipeline.g[i];
    pipeline.db[i] = pipeline.b[i];
    pipeline.da[i] = pipeline.a[i];
  }
  pipeline.nextStage();
}

void moveDestinationToSource(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = pipeline.dr[i];
    pipeline.g[i] = pipeline.dg[i];
    pipeline.b[i] = pipeline.db[i];
    pipeline.a[i] = pipeline.da[i];
  }
  pipeline.nextStage();
}

void clamp0(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = std::max(0.0f, pipeline.r[i]);
    pipeline.g[i] = std::max(0.0f, pipeline.g[i]);
    pipeline.b[i] = std::max(0.0f, pipeline.b[i]);
    pipeline.a[i] = std::max(0.0f, pipeline.a[i]);
  }
  pipeline.nextStage();
}

void clampA(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = std::min(1.0f, pipeline.r[i]);
    pipeline.g[i] = std::min(1.0f, pipeline.g[i]);
    pipeline.b[i] = std::min(1.0f, pipeline.b[i]);
    pipeline.a[i] = std::min(1.0f, pipeline.a[i]);
  }
  pipeline.nextStage();
}

void premultiply(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] *= pipeline.a[i];
    pipeline.g[i] *= pipeline.a[i];
    pipeline.b[i] *= pipeline.a[i];
  }
  pipeline.nextStage();
}

void unpremultiply(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float a = pipeline.a[i];
    if (a > 0.0f) {
      const float invA = 1.0f / a;
      pipeline.r[i] *= invA;
      pipeline.g[i] *= invA;
      pipeline.b[i] *= invA;
    }
  }
  pipeline.nextStage();
}

void premultiplyDestination(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.dr[i] *= pipeline.da[i];
    pipeline.dg[i] *= pipeline.da[i];
    pipeline.db[i] *= pipeline.da[i];
  }
  pipeline.nextStage();
}

void uniformColor(Pipeline& pipeline) {
  const auto& u = pipeline.ctx->uniformColor;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = u.r;
    pipeline.g[i] = u.g;
    pipeline.b[i] = u.b;
    pipeline.a[i] = u.a;
  }
  pipeline.nextStage();
}

void seedShader(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.dr[i] = 0.0f;
    pipeline.dg[i] = 0.0f;
    pipeline.db[i] = 0.0f;
    pipeline.da[i] = 0.0f;
    pipeline.r[i] = static_cast<float>(pipeline.dx) + static_cast<float>(i) + 0.5f;
    pipeline.g[i] = static_cast<float>(pipeline.dy) + 0.5f;
    pipeline.b[i] = 1.0f;
    pipeline.a[i] = 0.0f;
  }
  pipeline.nextStage();
}

void scaleU8(Pipeline& pipeline) {
  const auto data = pipeline.aaMaskCtx->copyAtXY(pipeline.dx, pipeline.dy, pipeline.tail);
  const std::array<float, kStageWidth> c{
      static_cast<float>(data[0]) / 255.0f,
      static_cast<float>(data[1]) / 255.0f,
      0.0f,
      0.0f,
      0.0f,
      0.0f,
      0.0f,
      0.0f,
  };

  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] *= c[i];
    pipeline.g[i] *= c[i];
    pipeline.b[i] *= c[i];
    pipeline.a[i] *= c[i];
  }
  pipeline.nextStage();
}

void lerpU8(Pipeline& pipeline) {
  const auto data = pipeline.aaMaskCtx->copyAtXY(pipeline.dx, pipeline.dy, pipeline.tail);
  const std::array<float, kStageWidth> c{
      static_cast<float>(data[0]) / 255.0f,
      static_cast<float>(data[1]) / 255.0f,
      0.0f,
      0.0f,
      0.0f,
      0.0f,
      0.0f,
      0.0f,
  };

  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = pipeline.dr[i] + (pipeline.r[i] - pipeline.dr[i]) * c[i];
    pipeline.g[i] = pipeline.dg[i] + (pipeline.g[i] - pipeline.dg[i]) * c[i];
    pipeline.b[i] = pipeline.db[i] + (pipeline.b[i] - pipeline.db[i]) * c[i];
    pipeline.a[i] = pipeline.da[i] + (pipeline.a[i] - pipeline.da[i]) * c[i];
  }
  pipeline.nextStage();
}

void scale1Float(Pipeline& pipeline) {
  const auto c = pipeline.ctx->currentCoverage;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] *= c;
    pipeline.g[i] *= c;
    pipeline.b[i] *= c;
    pipeline.a[i] *= c;
  }
  pipeline.nextStage();
}

void lerp1Float(Pipeline& pipeline) {
  const auto c = pipeline.ctx->currentCoverage;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = pipeline.dr[i] + (pipeline.r[i] - pipeline.dr[i]) * c;
    pipeline.g[i] = pipeline.dg[i] + (pipeline.g[i] - pipeline.dg[i]) * c;
    pipeline.b[i] = pipeline.db[i] + (pipeline.b[i] - pipeline.db[i]) * c;
    pipeline.a[i] = pipeline.da[i] + (pipeline.a[i] - pipeline.da[i]) * c;
  }
  pipeline.nextStage();
}

void clear(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = 0.0f;
    pipeline.g[i] = 0.0f;
    pipeline.b[i] = 0.0f;
    pipeline.a[i] = 0.0f;
  }
  pipeline.nextStage();
}

void sourceAtop(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto s = pipeline.r[i];
    const auto d = pipeline.dr[i];
    const auto sa = pipeline.a[i];
    const auto da = pipeline.da[i];
    pipeline.r[i] = s * da + d * (1.0f - sa);
    pipeline.g[i] = pipeline.g[i] * da + pipeline.dg[i] * (1.0f - sa);
    pipeline.b[i] = pipeline.b[i] * da + pipeline.db[i] * (1.0f - sa);
    pipeline.a[i] = pipeline.a[i] * da + pipeline.da[i] * (1.0f - pipeline.a[i]);
  }
  pipeline.nextStage();
}

void sourceIn(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] *= pipeline.da[i];
    pipeline.g[i] *= pipeline.da[i];
    pipeline.b[i] *= pipeline.da[i];
    pipeline.a[i] *= pipeline.da[i];
  }
  pipeline.nextStage();
}

void sourceOut(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto daInv = 1.0f - pipeline.da[i];
    pipeline.r[i] *= daInv;
    pipeline.g[i] *= daInv;
    pipeline.b[i] *= daInv;
    pipeline.a[i] *= daInv;
  }
  pipeline.nextStage();
}

void destinationAtop(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto s = pipeline.r[i];
    const auto d = pipeline.dr[i];
    const auto sa = pipeline.a[i];
    const auto da = pipeline.da[i];
    pipeline.r[i] = d * sa + s * (1.0f - da);
    pipeline.g[i] = pipeline.g[i] * sa + pipeline.dg[i] * (1.0f - da);
    pipeline.b[i] = pipeline.b[i] * sa + pipeline.db[i] * (1.0f - da);
    pipeline.a[i] = pipeline.da[i] * pipeline.a[i] + pipeline.a[i] * (1.0f - pipeline.da[i]);
  }
  pipeline.nextStage();
}

void destinationIn(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto sa = pipeline.a[i];
    pipeline.r[i] = pipeline.dr[i] * sa;
    pipeline.g[i] = pipeline.dg[i] * sa;
    pipeline.b[i] = pipeline.db[i] * sa;
    pipeline.a[i] = pipeline.da[i] * sa;
  }
  pipeline.nextStage();
}

void destinationOut(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto saInv = 1.0f - pipeline.a[i];
    pipeline.r[i] = pipeline.dr[i] * saInv;
    pipeline.g[i] = pipeline.dg[i] * saInv;
    pipeline.b[i] = pipeline.db[i] * saInv;
    pipeline.a[i] = pipeline.da[i] * saInv;
  }
  pipeline.nextStage();
}

void sourceOver(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto saInv = 1.0f - pipeline.a[i];
    pipeline.r[i] = pipeline.dr[i] * saInv + pipeline.r[i];
    pipeline.g[i] = pipeline.dg[i] * saInv + pipeline.g[i];
    pipeline.b[i] = pipeline.db[i] * saInv + pipeline.b[i];
    pipeline.a[i] = pipeline.da[i] * saInv + pipeline.a[i];
  }
  pipeline.nextStage();
}

void destinationOver(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto daInv = 1.0f - pipeline.da[i];
    pipeline.r[i] = pipeline.r[i] * daInv + pipeline.dr[i];
    pipeline.g[i] = pipeline.g[i] * daInv + pipeline.dg[i];
    pipeline.b[i] = pipeline.b[i] * daInv + pipeline.db[i];
    pipeline.a[i] = pipeline.da[i] + pipeline.a[i] * daInv;
  }
  pipeline.nextStage();
}

void modulate(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] *= pipeline.dr[i];
    pipeline.g[i] *= pipeline.dg[i];
    pipeline.b[i] *= pipeline.db[i];
    pipeline.a[i] *= pipeline.da[i];
  }
  pipeline.nextStage();
}

void multiply(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto invDa = 1.0f - pipeline.da[i];
    const auto invSa = 1.0f - pipeline.a[i];
    pipeline.r[i] =
        pipeline.r[i] * invDa + pipeline.dr[i] * invSa + pipeline.r[i] * pipeline.dr[i];
    pipeline.g[i] =
        pipeline.g[i] * invDa + pipeline.dg[i] * invSa + pipeline.g[i] * pipeline.dg[i];
    pipeline.b[i] =
        pipeline.b[i] * invDa + pipeline.db[i] * invSa + pipeline.b[i] * pipeline.db[i];
    pipeline.a[i] =
        pipeline.a[i] * invDa + pipeline.da[i] * invSa + pipeline.a[i] * pipeline.da[i];
  }
  pipeline.nextStage();
}

void plus(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = std::min(1.0f, pipeline.r[i] + pipeline.dr[i]);
    pipeline.g[i] = std::min(1.0f, pipeline.g[i] + pipeline.dg[i]);
    pipeline.b[i] = std::min(1.0f, pipeline.b[i] + pipeline.db[i]);
    pipeline.a[i] = std::min(1.0f, pipeline.a[i] + pipeline.da[i]);
  }
  pipeline.nextStage();
}

void screen(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = pipeline.r[i] + pipeline.dr[i] - pipeline.r[i] * pipeline.dr[i];
    pipeline.g[i] = pipeline.g[i] + pipeline.dg[i] - pipeline.g[i] * pipeline.dg[i];
    pipeline.b[i] = pipeline.b[i] + pipeline.db[i] - pipeline.b[i] * pipeline.db[i];
    pipeline.a[i] = pipeline.a[i] + pipeline.da[i] - pipeline.a[i] * pipeline.da[i];
  }
  pipeline.nextStage();
}

void xOr(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto invDa = 1.0f - pipeline.da[i];
    const auto invSa = 1.0f - pipeline.a[i];
    pipeline.r[i] = pipeline.r[i] * invDa + pipeline.dr[i] * invSa;
    pipeline.g[i] = pipeline.g[i] * invDa + pipeline.dg[i] * invSa;
    pipeline.b[i] = pipeline.b[i] * invDa + pipeline.db[i] * invSa;
    pipeline.a[i] = pipeline.a[i] * invDa + pipeline.da[i] * invSa;
  }
  pipeline.nextStage();
}

void colorBurn(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto s = pipeline.r[i];
    const auto d = pipeline.dr[i];
    const auto sa = pipeline.a[i];
    const auto da = pipeline.da[i];
    const auto invDa = 1.0f - da;
    const auto invSa = 1.0f - sa;
    if (d == da) {
      pipeline.r[i] = d + s * invDa;
    } else if (s == 0.0f) {
      pipeline.r[i] = d * invSa;
    } else {
      pipeline.r[i] = sa * (da - std::min(da, (da - d) * sa / s)) + s * invDa + d * invSa;
    }
    const auto sg = pipeline.g[i];
    const auto dg = pipeline.dg[i];
    if (dg == da) {
      pipeline.g[i] = dg + sg * invDa;
    } else if (sg == 0.0f) {
      pipeline.g[i] = dg * invSa;
    } else {
      pipeline.g[i] = sa * (da - std::min(da, (da - dg) * sa / sg)) + sg * invDa + dg * invSa;
    }
    const auto sb = pipeline.b[i];
    const auto db = pipeline.db[i];
    if (db == da) {
      pipeline.b[i] = db + sb * invDa;
    } else if (sb == 0.0f) {
      pipeline.b[i] = db * invSa;
    } else {
      pipeline.b[i] = sa * (da - std::min(da, (da - db) * sa / sb)) + sb * invDa + db * invSa;
    }
    pipeline.a[i] = sa + da - sa * da;
  }
  pipeline.nextStage();
}

void colorDodge(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto s = pipeline.r[i];
    const auto d = pipeline.dr[i];
    const auto sa = pipeline.a[i];
    const auto da = pipeline.da[i];
    const auto invDa = 1.0f - da;
    const auto invSa = 1.0f - sa;
    if (d == 0.0f) {
      pipeline.r[i] = s * invDa;
    } else if (s == sa) {
      pipeline.r[i] = s + d * invSa;
    } else {
      pipeline.r[i] = sa * std::min(da, (d * sa) / (sa - s)) + s * invDa + d * invSa;
    }
    const auto sg = pipeline.g[i];
    const auto dg = pipeline.dg[i];
    if (dg == 0.0f) {
      pipeline.g[i] = sg * invDa;
    } else if (sg == sa) {
      pipeline.g[i] = sg + dg * invSa;
    } else {
      pipeline.g[i] = sa * std::min(da, (dg * sa) / (sa - sg)) + sg * invDa + dg * invSa;
    }
    const auto sb = pipeline.b[i];
    const auto db = pipeline.db[i];
    if (db == 0.0f) {
      pipeline.b[i] = sb * invDa;
    } else if (sb == sa) {
      pipeline.b[i] = sb + db * invSa;
    } else {
      pipeline.b[i] = sa * std::min(da, (db * sa) / (sa - sb)) + sb * invDa + db * invSa;
    }
    pipeline.a[i] = sa + da - sa * da;
  }
  pipeline.nextStage();
}

constexpr float kInv255 = 1.0f / 255.0f;

void load8888(const std::uint8_t* data, std::size_t stride, std::size_t dx, std::size_t dy,
               std::size_t count, Pipeline& p, std::array<float, kStageWidth>& or_,
               std::array<float, kStageWidth>& og, std::array<float, kStageWidth>& ob,
               std::array<float, kStageWidth>& oa) {
  const auto offset = (dy * stride + dx) * 4;
  for (std::size_t i = 0; i < count; ++i) {
    const auto base = offset + i * 4;
    or_[i] = static_cast<float>(data[base + 0]) * kInv255;
    og[i] = static_cast<float>(data[base + 1]) * kInv255;
    ob[i] = static_cast<float>(data[base + 2]) * kInv255;
    oa[i] = static_cast<float>(data[base + 3]) * kInv255;
  }
  for (std::size_t i = count; i < kStageWidth; ++i) {
    or_[i] = 0.0f;
    og[i] = 0.0f;
    ob[i] = 0.0f;
    oa[i] = 0.0f;
  }
  (void)p;
}

// Clamp to [0,1], multiply by 255, round-to-nearest-even.
// Uses std::nearbyintf which follows the current rounding mode (FE_TONEAREST by default),
// matching ARM NEON vcvtnq_s32_f32 used by Rust's roundInt().
inline std::uint8_t unnorm(float v) {
  float clamped = std::max(0.0f, std::min(1.0f, v));
  return static_cast<std::uint8_t>(std::nearbyintf(clamped * 255.0f));
}

void store8888(std::uint8_t* data, std::size_t stride, std::size_t dx, std::size_t dy,
                std::size_t count, const std::array<float, kStageWidth>& r,
                const std::array<float, kStageWidth>& g, const std::array<float, kStageWidth>& b,
                const std::array<float, kStageWidth>& a) {
  const auto offset = (dy * stride + dx) * 4;
  for (std::size_t i = 0; i < count; ++i) {
    const auto base = offset + i * 4;
    data[base + 0] = unnorm(r[i]);
    data[base + 1] = unnorm(g[i]);
    data[base + 2] = unnorm(b[i]);
    data[base + 3] = unnorm(a[i]);
  }
}

void loadDst(Pipeline& pipeline) {
  if (pipeline.pixmapDst == nullptr) {
    pipeline.nextStage();
    return;
  }
  load8888(pipeline.pixmapDst->data, pipeline.pixmapDst->realWidth, pipeline.dx, pipeline.dy,
            pipeline.tail, pipeline, pipeline.dr, pipeline.dg, pipeline.db, pipeline.da);
  pipeline.nextStage();
}

void store(Pipeline& pipeline) {
  if (pipeline.pixmapDst == nullptr) {
    pipeline.nextStage();
    return;
  }
  store8888(pipeline.pixmapDst->data, pipeline.pixmapDst->realWidth, pipeline.dx, pipeline.dy,
             pipeline.tail, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
  pipeline.nextStage();
}

// U8 (mask) stages are unreachable in highp; all mask/A8 pixmaps use lowp.
void loadDstU8(Pipeline& pipeline) { pipeline.nextStage(); }
void storeU8(Pipeline& pipeline) { pipeline.nextStage(); }
void loadMaskU8(Pipeline& pipeline) { pipeline.nextStage(); }

void maskU8(Pipeline& pipeline) {
  if (pipeline.maskCtx == nullptr || pipeline.maskCtx->data == nullptr) {
    pipeline.nextStage();
    return;
  }
  const auto offset = pipeline.maskCtx->byteOffset(pipeline.dx, pipeline.dy);
  std::array<float, kStageWidth> c{};
  for (std::size_t i = 0; i < pipeline.tail; ++i) {
    c[i] = static_cast<float>(pipeline.maskCtx->data[offset + i]) * kInv255;
  }
  bool allZero = true;
  for (std::size_t i = 0; i < pipeline.tail; ++i) {
    if (c[i] != 0.0f) {
      allZero = false;
      break;
    }
  }
  if (allZero) {
    return;  // Early return, skip remaining stages.
  }
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] *= c[i];
    pipeline.g[i] *= c[i];
    pipeline.b[i] *= c[i];
    pipeline.a[i] *= c[i];
  }
  pipeline.nextStage();
}

void sourceOverRgba(Pipeline& pipeline) {
  if (pipeline.pixmapDst == nullptr) {
    pipeline.nextStage();
    return;
  }
  load8888(pipeline.pixmapDst->data, pipeline.pixmapDst->realWidth, pipeline.dx, pipeline.dy,
            pipeline.tail, pipeline, pipeline.dr, pipeline.dg, pipeline.db, pipeline.da);
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto invA = 1.0f - pipeline.a[i];
    pipeline.r[i] = pipeline.dr[i] * invA + pipeline.r[i];
    pipeline.g[i] = pipeline.dg[i] * invA + pipeline.g[i];
    pipeline.b[i] = pipeline.db[i] * invA + pipeline.b[i];
    pipeline.a[i] = pipeline.da[i] * invA + pipeline.a[i];
  }
  store8888(pipeline.pixmapDst->data, pipeline.pixmapDst->realWidth, pipeline.dx, pipeline.dy,
             pipeline.tail, pipeline.r, pipeline.g, pipeline.b, pipeline.a);
  pipeline.nextStage();
}

// ---------------------------------------------------------------------------
// Helper functions for pipeline stages
// ---------------------------------------------------------------------------

/// Subtract one ULP (unit in the last place) from a float.
/// Converts exclusive upper bound to inclusive: e.g. width 100 -> 99.999...
inline float ulpSub(float v) {
  auto bits = std::bit_cast<std::uint32_t>(v);
  bits -= 1;
  return std::bit_cast<float>(bits);
}

/// Clamp a float to [0, 1].
inline float normalize(float v) { return std::max(0.0f, std::min(1.0f, v)); }

/// Fused multiply-add: f * m + a.
inline float mad(float f, float m, float a) { return f * m + a; }

/// Repeat tiling for coordinates in [0, limit) range.
inline float exclusiveRepeat(float v, float limit, float invLimit) {
  return v - std::floor(v * invLimit) * limit;
}

/// Reflect tiling for coordinates in [0, limit) range.
inline float exclusiveReflect(float v, float limit, float invLimit) {
  return std::abs((v - limit) - (limit + limit) * std::floor((v - limit) * (invLimit * 0.5f)) -
                  limit);
}

/// Apply tile mode to a coordinate.
inline float tile(float v, SpreadMode mode, float limit, float invLimit) {
  switch (mode) {
    case SpreadMode::Pad:
      return v;
    case SpreadMode::Repeat:
      return exclusiveRepeat(v, limit, invLimit);
    case SpreadMode::Reflect:
      return exclusiveReflect(v, limit, invLimit);
  }
  return v;
}

/// Compute pixel indices from clamped x,y coordinates.
inline std::uint32_t gatherIxScalar(const PixmapView& pixmap, float x, float y) {
  const float w = ulpSub(static_cast<float>(pixmap.width()));
  const float h = ulpSub(static_cast<float>(pixmap.height()));
  x = std::max(0.0f, std::min(w, x));
  y = std::max(0.0f, std::min(h, y));
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(y)) * pixmap.width() +
         static_cast<std::uint32_t>(static_cast<std::int32_t>(x));
}

/// Load a gathered pixel into float channels.
inline void loadGatheredPixel(const PixmapView& pixmap, std::uint32_t ix, float& r, float& g,
                                float& b, float& a) {
  const auto pixels = pixmap.pixels();
  const auto idx = std::min(static_cast<std::size_t>(ix), pixels.size() - 1);
  const auto& px = pixels[idx];
  r = static_cast<float>(px.red()) * kInv255;
  g = static_cast<float>(px.green()) * kInv255;
  b = static_cast<float>(px.blue()) * kInv255;
  a = static_cast<float>(px.alpha()) * kInv255;
}

/// Sample a single pixel with tiling applied.
inline void sample(const PixmapView& pixmap, const SamplerCtx& ctx, float x, float y, float& r,
                   float& g, float& b, float& a) {
  x = tile(x, ctx.spreadMode, static_cast<float>(pixmap.width()), ctx.invWidth);
  y = tile(y, ctx.spreadMode, static_cast<float>(pixmap.height()), ctx.invHeight);
  const auto ix = gatherIxScalar(pixmap, x, y);
  loadGatheredPixel(pixmap, ix, r, g, b, a);
}

/// Bicubic near weight: 1/18 + 9/18*t + 27/18*t^2 - 21/18*t^3
inline float bicubicNear(float t) {
  return mad(t, mad(t, mad(-21.0f / 18.0f, t, 27.0f / 18.0f), 9.0f / 18.0f), 1.0f / 18.0f);
}

/// Bicubic far weight: t^2 * (7/18*t - 6/18)
inline float bicubicFar(float t) { return (t * t) * mad(7.0f / 18.0f, t, -6.0f / 18.0f); }

/// HSL saturation: max(r,g,b) - min(r,g,b)
inline float sat(float r, float g, float b) { return std::max({r, g, b}) - std::min({r, g, b}); }

/// HSL luminosity: 0.30*r + 0.59*g + 0.11*b
inline float lum(float r, float g, float b) { return r * 0.30f + g * 0.59f + b * 0.11f; }

/// Set saturation of (r,g,b) to target saturation s.
inline void setSat(float& r, float& g, float& b, float s) {
  const float mn = std::min({r, g, b});
  const float mx = std::max({r, g, b});
  const float satv = mx - mn;
  auto scale = [&](float c) -> float { return satv == 0.0f ? 0.0f : (c - mn) * s / satv; };
  r = scale(r);
  g = scale(g);
  b = scale(b);
}

/// Set luminosity of (r,g,b) to target luminosity l.
inline void setLum(float& r, float& g, float& b, float l) {
  const float diff = l - lum(r, g, b);
  r += diff;
  g += diff;
  b += diff;
}

/// Clip color to valid premultiplied range.
inline void clipColor(float& r, float& g, float& b, float a) {
  const float mn = std::min({r, g, b});
  const float mx = std::max({r, g, b});
  const float l = lum(r, g, b);
  auto clip = [&](float c) -> float {
    if (mn < 0.0f) {
      c = l + (c - l) * l / (l - mn);
    }
    if (mx > a) {
      c = l + (c - l) * (a - l) / (mx - l);
    }
    return std::max(0.0f, c);
  };
  r = clip(r);
  g = clip(g);
  b = clip(b);
}

/// sRGB expand: linear -> sRGB
inline float srgbExpandScalar(float x) {
  if (x <= 0.04045f) {
    return x / 12.92f;
  }
  return approxPowf((x + 0.055f) / 1.055f, 2.4f);
}

/// sRGB compress: sRGB -> linear
inline float srgbCompressScalar(float x) {
  if (x <= 0.0031308f) {
    return x * 12.92f;
  }
  return approxPowf(x, 0.416666666f) * 1.055f - 0.055f;
}

/// Bitwise AND a float with a uint32 mask (reinterpret cast).
inline float floatAndMask(float v, std::uint32_t mask) {
  auto bits = std::bit_cast<std::uint32_t>(v);
  bits &= mask;
  return std::bit_cast<float>(bits);
}

// ---------------------------------------------------------------------------
// Pipeline stage implementations
// ---------------------------------------------------------------------------

void gather(Pipeline& pipeline) {
  if (pipeline.pixmapSrc == nullptr) {
    pipeline.nextStage();
    return;
  }
  const auto& pixmap = *pipeline.pixmapSrc;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const auto ix = gatherIxScalar(pixmap, pipeline.r[i], pipeline.g[i]);
    loadGatheredPixel(pixmap, ix, pipeline.r[i], pipeline.g[i], pipeline.b[i], pipeline.a[i]);
  }
  pipeline.nextStage();
}

void transform(Pipeline& pipeline) {
  const auto& ts = pipeline.ctx->transform;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float x = pipeline.r[i];
    const float y = pipeline.g[i];
    pipeline.r[i] = mad(x, ts.sx, mad(y, ts.kx, ts.tx));
    pipeline.g[i] = mad(x, ts.ky, mad(y, ts.sy, ts.ty));
  }
  pipeline.nextStage();
}

void reflect(Pipeline& pipeline) {
  const auto& lx = pipeline.ctx->limitX;
  const auto& ly = pipeline.ctx->limitY;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = exclusiveReflect(pipeline.r[i], lx.scale, lx.invScale);
    pipeline.g[i] = exclusiveReflect(pipeline.g[i], ly.scale, ly.invScale);
  }
  pipeline.nextStage();
}

void repeat(Pipeline& pipeline) {
  const auto& lx = pipeline.ctx->limitX;
  const auto& ly = pipeline.ctx->limitY;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = exclusiveRepeat(pipeline.r[i], lx.scale, lx.invScale);
    pipeline.g[i] = exclusiveRepeat(pipeline.g[i], ly.scale, ly.invScale);
  }
  pipeline.nextStage();
}

void bilinear(Pipeline& pipeline) {
  if (pipeline.pixmapSrc == nullptr) {
    pipeline.nextStage();
    return;
  }
  const auto& pixmap = *pipeline.pixmapSrc;
  const auto& ctx = pipeline.ctx->sampler;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float cx = pipeline.r[i];
    const float cy = pipeline.g[i];
    const float fx = (cx + 0.5f) - std::floor(cx + 0.5f);
    const float fy = (cy + 0.5f) - std::floor(cy + 0.5f);
    const float wx[2] = {1.0f - fx, fx};
    const float wy[2] = {1.0f - fy, fy};

    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    float y = cy - 0.5f;
    for (int j = 0; j < 2; ++j) {
      float x = cx - 0.5f;
      for (int k = 0; k < 2; ++k) {
        float sr, sg, sb, sa;
        sample(pixmap, ctx, x, y, sr, sg, sb, sa);
        const float w = wx[k] * wy[j];
        r = mad(w, sr, r);
        g = mad(w, sg, g);
        b = mad(w, sb, b);
        a = mad(w, sa, a);
        x += 1.0f;
      }
      y += 1.0f;
    }
    pipeline.r[i] = r;
    pipeline.g[i] = g;
    pipeline.b[i] = b;
    pipeline.a[i] = a;
  }
  pipeline.nextStage();
}

void bicubic(Pipeline& pipeline) {
  if (pipeline.pixmapSrc == nullptr) {
    pipeline.nextStage();
    return;
  }
  const auto& pixmap = *pipeline.pixmapSrc;
  const auto& ctx = pipeline.ctx->sampler;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float cx = pipeline.r[i];
    const float cy = pipeline.g[i];
    const float fx = (cx + 0.5f) - std::floor(cx + 0.5f);
    const float fy = (cy + 0.5f) - std::floor(cy + 0.5f);
    const float wx[4] = {bicubicFar(1.0f - fx), bicubicNear(1.0f - fx), bicubicNear(fx),
                         bicubicFar(fx)};
    const float wy[4] = {bicubicFar(1.0f - fy), bicubicNear(1.0f - fy), bicubicNear(fy),
                         bicubicFar(fy)};

    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    float y = cy - 1.5f;
    for (int j = 0; j < 4; ++j) {
      float x = cx - 1.5f;
      for (int k = 0; k < 4; ++k) {
        float sr, sg, sb, sa;
        sample(pixmap, ctx, x, y, sr, sg, sb, sa);
        const float w = wx[k] * wy[j];
        r = mad(w, sr, r);
        g = mad(w, sg, g);
        b = mad(w, sb, b);
        a = mad(w, sa, a);
        x += 1.0f;
      }
      y += 1.0f;
    }
    pipeline.r[i] = r;
    pipeline.g[i] = g;
    pipeline.b[i] = b;
    pipeline.a[i] = a;
  }
  pipeline.nextStage();
}

void padX1(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = normalize(pipeline.r[i]);
  }
  pipeline.nextStage();
}

void reflectX1(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float v = pipeline.r[i];
    pipeline.r[i] = normalize(std::abs((v - 1.0f) - 2.0f * std::floor((v - 1.0f) * 0.5f) - 1.0f));
  }
  pipeline.nextStage();
}

void repeatX1(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float v = pipeline.r[i];
    pipeline.r[i] = normalize(v - std::floor(v));
  }
  pipeline.nextStage();
}

void evenlySpaced2StopGradient(Pipeline& pipeline) {
  const auto& ctx = pipeline.ctx->evenlySpaced2StopGradient;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float t = pipeline.r[i];
    pipeline.r[i] = mad(t, ctx.factor.r, ctx.bias.r);
    pipeline.g[i] = mad(t, ctx.factor.g, ctx.bias.g);
    pipeline.b[i] = mad(t, ctx.factor.b, ctx.bias.b);
    pipeline.a[i] = mad(t, ctx.factor.a, ctx.bias.a);
  }
  pipeline.nextStage();
}

void gradient(Pipeline& pipeline) {
  const auto& ctx = pipeline.ctx->gradient;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float t = pipeline.r[i];
    // Find which stop index to use by counting how many t_values are <= t.
    // Loop starts at 1 because idx 0 is the color before the first stop.
    std::uint32_t idx = 0;
    for (std::size_t s = 1; s < ctx.len; ++s) {
      if (t >= ctx.tValues[s]) {
        idx += 1;
      }
    }
    // Lookup factor and bias for this stop and interpolate.
    const auto& factor = ctx.factors[idx];
    const auto& bias = ctx.biases[idx];
    pipeline.r[i] = mad(t, factor.r, bias.r);
    pipeline.g[i] = mad(t, factor.g, bias.g);
    pipeline.b[i] = mad(t, factor.b, bias.b);
    pipeline.a[i] = mad(t, factor.a, bias.a);
  }
  pipeline.nextStage();
}

void xyToUnitAngle(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float x = pipeline.r[i];
    const float y = pipeline.g[i];
    const float xAbs = std::abs(x);
    const float yAbs = std::abs(y);
    const float maxAbs = std::max(xAbs, yAbs);
    const float minAbs = std::min(xAbs, yAbs);
    const float slope = (maxAbs == 0.0f) ? 0.0f : minAbs / maxAbs;
    const float s = slope * slope;
    // 7th degree polynomial approximation of atan/(2*pi)
    float phi =
        slope *
        (0.15912117063999176025390625f +
         s * (-5.185396969318389892578125e-2f +
              s * (2.476101927459239959716796875e-2f + s * (-7.0547382347285747528076171875e-3f))));
    if (xAbs < yAbs) phi = 0.25f - phi;
    if (x < 0.0f) phi = 0.5f - phi;
    if (y < 0.0f) phi = 1.0f - phi;
    // NaN check: if phi != phi, set to 0.
    if (phi != phi) phi = 0.0f;
    pipeline.r[i] = phi;
  }
  pipeline.nextStage();
}

void xyToRadius(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float x = pipeline.r[i];
    const float y = pipeline.g[i];
    pipeline.r[i] = std::sqrt(x * x + y * y);
  }
  pipeline.nextStage();
}

// --- Separable blend modes ---

void darken(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float sa = pipeline.a[i];
    const float da = pipeline.da[i];
    pipeline.r[i] =
        pipeline.r[i] + pipeline.dr[i] - std::max(pipeline.r[i] * da, pipeline.dr[i] * sa);
    pipeline.g[i] =
        pipeline.g[i] + pipeline.dg[i] - std::max(pipeline.g[i] * da, pipeline.dg[i] * sa);
    pipeline.b[i] =
        pipeline.b[i] + pipeline.db[i] - std::max(pipeline.b[i] * da, pipeline.db[i] * sa);
    pipeline.a[i] = mad(da, 1.0f - sa, sa);
  }
  pipeline.nextStage();
}

void lighten(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float sa = pipeline.a[i];
    const float da = pipeline.da[i];
    pipeline.r[i] =
        pipeline.r[i] + pipeline.dr[i] - std::min(pipeline.r[i] * da, pipeline.dr[i] * sa);
    pipeline.g[i] =
        pipeline.g[i] + pipeline.dg[i] - std::min(pipeline.g[i] * da, pipeline.dg[i] * sa);
    pipeline.b[i] =
        pipeline.b[i] + pipeline.db[i] - std::min(pipeline.b[i] * da, pipeline.db[i] * sa);
    pipeline.a[i] = mad(da, 1.0f - sa, sa);
  }
  pipeline.nextStage();
}

void difference(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float sa = pipeline.a[i];
    const float da = pipeline.da[i];
    pipeline.r[i] =
        pipeline.r[i] + pipeline.dr[i] - 2.0f * std::min(pipeline.r[i] * da, pipeline.dr[i] * sa);
    pipeline.g[i] =
        pipeline.g[i] + pipeline.dg[i] - 2.0f * std::min(pipeline.g[i] * da, pipeline.dg[i] * sa);
    pipeline.b[i] =
        pipeline.b[i] + pipeline.db[i] - 2.0f * std::min(pipeline.b[i] * da, pipeline.db[i] * sa);
    pipeline.a[i] = mad(da, 1.0f - sa, sa);
  }
  pipeline.nextStage();
}

void exclusion(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = pipeline.r[i] + pipeline.dr[i] - 2.0f * pipeline.r[i] * pipeline.dr[i];
    pipeline.g[i] = pipeline.g[i] + pipeline.dg[i] - 2.0f * pipeline.g[i] * pipeline.dg[i];
    pipeline.b[i] = pipeline.b[i] + pipeline.db[i] - 2.0f * pipeline.b[i] * pipeline.db[i];
    pipeline.a[i] = mad(pipeline.da[i], 1.0f - pipeline.a[i], pipeline.a[i]);
  }
  pipeline.nextStage();
}

void hardLight(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float s = pipeline.r[i], d = pipeline.dr[i];
    const float sa = pipeline.a[i], da = pipeline.da[i];
    pipeline.r[i] = s * (1.0f - da) + d * (1.0f - sa) +
                    ((2.0f * s <= sa) ? 2.0f * s * d : sa * da - 2.0f * (da - d) * (sa - s));
    const float sg = pipeline.g[i], dg = pipeline.dg[i];
    pipeline.g[i] = sg * (1.0f - da) + dg * (1.0f - sa) +
                    ((2.0f * sg <= sa) ? 2.0f * sg * dg : sa * da - 2.0f * (da - dg) * (sa - sg));
    const float sb = pipeline.b[i], db = pipeline.db[i];
    pipeline.b[i] = sb * (1.0f - da) + db * (1.0f - sa) +
                    ((2.0f * sb <= sa) ? 2.0f * sb * db : sa * da - 2.0f * (da - db) * (sa - sb));
    pipeline.a[i] = mad(da, 1.0f - sa, sa);
  }
  pipeline.nextStage();
}

void overlay(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float s = pipeline.r[i], d = pipeline.dr[i];
    const float sa = pipeline.a[i], da = pipeline.da[i];
    pipeline.r[i] = s * (1.0f - da) + d * (1.0f - sa) +
                    ((2.0f * d <= da) ? 2.0f * s * d : sa * da - 2.0f * (da - d) * (sa - s));
    const float sg = pipeline.g[i], dg = pipeline.dg[i];
    pipeline.g[i] = sg * (1.0f - da) + dg * (1.0f - sa) +
                    ((2.0f * dg <= da) ? 2.0f * sg * dg : sa * da - 2.0f * (da - dg) * (sa - sg));
    const float sb = pipeline.b[i], db = pipeline.db[i];
    pipeline.b[i] = sb * (1.0f - da) + db * (1.0f - sa) +
                    ((2.0f * db <= da) ? 2.0f * sb * db : sa * da - 2.0f * (da - db) * (sa - sb));
    pipeline.a[i] = mad(da, 1.0f - sa, sa);
  }
  pipeline.nextStage();
}

void softLight(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float s = pipeline.r[i], d = pipeline.dr[i];
    const float sa = pipeline.a[i], da = pipeline.da[i];
    const float invSa = 1.0f - sa;

    auto softLightChannel = [](float sc, float dc, float sca, float dca) -> float {
      const float m = (dca > 0.0f) ? dc / dca : 0.0f;
      const float s2 = 2.0f * sc;
      const float m4 = 4.0f * m;
      const float darkSrc = dc * (sca + (s2 - sca) * (1.0f - m));
      const float darkDst = (m4 * m4 + m4) * (m - 1.0f) + 7.0f * m;
      const float liteDst = std::sqrt(m) - m;
      const float liteSrc =
          dc * sca + dca * (s2 - sca) * ((4.0f * dc <= dca) ? darkDst : liteDst);
      return sc * (1.0f - dca) + dc * (1.0f - sca) + ((s2 <= sca) ? darkSrc : liteSrc);
    };

    pipeline.r[i] = softLightChannel(s, d, sa, da);
    pipeline.g[i] = softLightChannel(pipeline.g[i], pipeline.dg[i], sa, da);
    pipeline.b[i] = softLightChannel(pipeline.b[i], pipeline.db[i], sa, da);
    pipeline.a[i] = mad(da, invSa, sa);
  }
  pipeline.nextStage();
}

// --- Non-separable blend modes ---

void hue(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float sr = pipeline.r[i], sg = pipeline.g[i], sb = pipeline.b[i];
    const float sa = pipeline.a[i];
    const float dr = pipeline.dr[i], dg = pipeline.dg[i], db = pipeline.db[i];
    const float da = pipeline.da[i];

    float rr = sr * sa, gg = sg * sa, bb = sb * sa;
    setSat(rr, gg, bb, sat(dr, dg, db) * sa);
    setLum(rr, gg, bb, lum(dr, dg, db) * sa);
    clipColor(rr, gg, bb, sa * da);

    pipeline.r[i] = sr * (1.0f - da) + dr * (1.0f - sa) + rr;
    pipeline.g[i] = sg * (1.0f - da) + dg * (1.0f - sa) + gg;
    pipeline.b[i] = sb * (1.0f - da) + db * (1.0f - sa) + bb;
    pipeline.a[i] = sa + da - sa * da;
  }
  pipeline.nextStage();
}

void saturation(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float sr = pipeline.r[i], sg = pipeline.g[i], sb = pipeline.b[i];
    const float sa = pipeline.a[i];
    const float dr = pipeline.dr[i], dg = pipeline.dg[i], db = pipeline.db[i];
    const float da = pipeline.da[i];

    float rr = dr * sa, gg = dg * sa, bb = db * sa;
    setSat(rr, gg, bb, sat(sr, sg, sb) * da);
    setLum(rr, gg, bb, lum(dr, dg, db) * sa);
    clipColor(rr, gg, bb, sa * da);

    pipeline.r[i] = sr * (1.0f - da) + dr * (1.0f - sa) + rr;
    pipeline.g[i] = sg * (1.0f - da) + dg * (1.0f - sa) + gg;
    pipeline.b[i] = sb * (1.0f - da) + db * (1.0f - sa) + bb;
    pipeline.a[i] = sa + da - sa * da;
  }
  pipeline.nextStage();
}

void color(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float sr = pipeline.r[i], sg = pipeline.g[i], sb = pipeline.b[i];
    const float sa = pipeline.a[i];
    const float dr = pipeline.dr[i], dg = pipeline.dg[i], db = pipeline.db[i];
    const float da = pipeline.da[i];

    float rr = sr * da, gg = sg * da, bb = sb * da;
    setLum(rr, gg, bb, lum(dr, dg, db) * sa);
    clipColor(rr, gg, bb, sa * da);

    pipeline.r[i] = sr * (1.0f - da) + dr * (1.0f - sa) + rr;
    pipeline.g[i] = sg * (1.0f - da) + dg * (1.0f - sa) + gg;
    pipeline.b[i] = sb * (1.0f - da) + db * (1.0f - sa) + bb;
    pipeline.a[i] = sa + da - sa * da;
  }
  pipeline.nextStage();
}

void luminosity(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float sr = pipeline.r[i], sg = pipeline.g[i], sb = pipeline.b[i];
    const float sa = pipeline.a[i];
    const float dr = pipeline.dr[i], dg = pipeline.dg[i], db = pipeline.db[i];
    const float da = pipeline.da[i];

    float rr = dr * sa, gg = dg * sa, bb = db * sa;
    setLum(rr, gg, bb, lum(sr, sg, sb) * da);
    clipColor(rr, gg, bb, sa * da);

    pipeline.r[i] = sr * (1.0f - da) + dr * (1.0f - sa) + rr;
    pipeline.g[i] = sg * (1.0f - da) + dg * (1.0f - sa) + gg;
    pipeline.b[i] = sb * (1.0f - da) + db * (1.0f - sa) + bb;
    pipeline.a[i] = sa + da - sa * da;
  }
  pipeline.nextStage();
}

// --- 2-point conical gradient stages ---

void xyTo2ptConicalFocalOnCircle(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float x = pipeline.r[i];
    const float y = pipeline.g[i];
    pipeline.r[i] = (x == 0.0f) ? 0.0f : x + y * y / x;
  }
  pipeline.nextStage();
}

void xyTo2ptConicalWellBehaved(Pipeline& pipeline) {
  const float p0 = pipeline.ctx->twoPointConicalGradient.p0;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float x = pipeline.r[i];
    const float y = pipeline.g[i];
    pipeline.r[i] = std::sqrt(x * x + y * y) - x * p0;
  }
  pipeline.nextStage();
}

void xyTo2ptConicalSmaller(Pipeline& pipeline) {
  const float p0 = pipeline.ctx->twoPointConicalGradient.p0;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float x = pipeline.r[i];
    const float y = pipeline.g[i];
    pipeline.r[i] = -std::sqrt(x * x - y * y) - x * p0;
  }
  pipeline.nextStage();
}

void xyTo2ptConicalGreater(Pipeline& pipeline) {
  const float p0 = pipeline.ctx->twoPointConicalGradient.p0;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float x = pipeline.r[i];
    const float y = pipeline.g[i];
    pipeline.r[i] = std::sqrt(x * x - y * y) - x * p0;
  }
  pipeline.nextStage();
}

void xyTo2ptConicalStrip(Pipeline& pipeline) {
  const float p0 = pipeline.ctx->twoPointConicalGradient.p0;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float x = pipeline.r[i];
    const float y = pipeline.g[i];
    pipeline.r[i] = x + std::sqrt(p0 - y * y);
  }
  pipeline.nextStage();
}

void mask2ptConicalNan(Pipeline& pipeline) {
  auto& ctx = pipeline.ctx->twoPointConicalGradient;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float t = pipeline.r[i];
    const bool isNan = (t != t);
    pipeline.r[i] = isNan ? 0.0f : t;
    ctx.mask[i] = isNan ? 0u : 0xFFFFFFFFu;
  }
  pipeline.nextStage();
}

void mask2ptConicalDegenerates(Pipeline& pipeline) {
  auto& ctx = pipeline.ctx->twoPointConicalGradient;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    const float t = pipeline.r[i];
    const bool isDegenerate = (t <= 0.0f) || (t != t);
    pipeline.r[i] = isDegenerate ? 0.0f : t;
    ctx.mask[i] = isDegenerate ? 0u : 0xFFFFFFFFu;
  }
  pipeline.nextStage();
}

void applyVectorMask(Pipeline& pipeline) {
  const auto& mask = pipeline.ctx->twoPointConicalGradient.mask;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = floatAndMask(pipeline.r[i], mask[i]);
    pipeline.g[i] = floatAndMask(pipeline.g[i], mask[i]);
    pipeline.b[i] = floatAndMask(pipeline.b[i], mask[i]);
    pipeline.a[i] = floatAndMask(pipeline.a[i], mask[i]);
  }
  pipeline.nextStage();
}

void alter2ptConicalCompensateFocal(Pipeline& pipeline) {
  const float p1 = pipeline.ctx->twoPointConicalGradient.p1;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] += p1;
  }
  pipeline.nextStage();
}

void alter2ptConicalUnswap(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = 1.0f - pipeline.r[i];
  }
  pipeline.nextStage();
}

void negateX(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = -pipeline.r[i];
  }
  pipeline.nextStage();
}

void applyConcentricScaleBias(Pipeline& pipeline) {
  const auto& ctx = pipeline.ctx->twoPointConicalGradient;
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = pipeline.r[i] * ctx.p0 + ctx.p1;
  }
  pipeline.nextStage();
}

// --- Gamma correction stages ---

void gammaExpand2(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] *= pipeline.r[i];
    pipeline.g[i] *= pipeline.g[i];
    pipeline.b[i] *= pipeline.b[i];
  }
  pipeline.nextStage();
}

void gammaExpandDst2(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.dr[i] *= pipeline.dr[i];
    pipeline.dg[i] *= pipeline.dg[i];
    pipeline.db[i] *= pipeline.db[i];
  }
  pipeline.nextStage();
}

void gammaCompress2(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = std::sqrt(pipeline.r[i]);
    pipeline.g[i] = std::sqrt(pipeline.g[i]);
    pipeline.b[i] = std::sqrt(pipeline.b[i]);
  }
  pipeline.nextStage();
}

void gammaExpand22(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = approxPowf(pipeline.r[i], 2.2f);
    pipeline.g[i] = approxPowf(pipeline.g[i], 2.2f);
    pipeline.b[i] = approxPowf(pipeline.b[i], 2.2f);
  }
  pipeline.nextStage();
}

void gammaExpandDst22(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.dr[i] = approxPowf(pipeline.dr[i], 2.2f);
    pipeline.dg[i] = approxPowf(pipeline.dg[i], 2.2f);
    pipeline.db[i] = approxPowf(pipeline.db[i], 2.2f);
  }
  pipeline.nextStage();
}

void gammaCompress22(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = approxPowf(pipeline.r[i], 0.45454545f);
    pipeline.g[i] = approxPowf(pipeline.g[i], 0.45454545f);
    pipeline.b[i] = approxPowf(pipeline.b[i], 0.45454545f);
  }
  pipeline.nextStage();
}

void gammaExpandSrgb(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = srgbExpandScalar(pipeline.r[i]);
    pipeline.g[i] = srgbExpandScalar(pipeline.g[i]);
    pipeline.b[i] = srgbExpandScalar(pipeline.b[i]);
  }
  pipeline.nextStage();
}

void gammaExpandDstSrgb(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.dr[i] = srgbExpandScalar(pipeline.dr[i]);
    pipeline.dg[i] = srgbExpandScalar(pipeline.dg[i]);
    pipeline.db[i] = srgbExpandScalar(pipeline.db[i]);
  }
  pipeline.nextStage();
}

void gammaCompressSrgb(Pipeline& pipeline) {
  for (std::size_t i = 0; i < kStageWidth; ++i) {
    pipeline.r[i] = srgbCompressScalar(pipeline.r[i]);
    pipeline.g[i] = srgbCompressScalar(pipeline.g[i]);
    pipeline.b[i] = srgbCompressScalar(pipeline.b[i]);
  }
  pipeline.nextStage();
}

}  // namespace

void justReturn(Pipeline& pipeline) { (void)pipeline; }

void start(const std::array<StageFn, tiny_skia::pipeline::kMaxStages>& functions,
           const std::array<StageFn, tiny_skia::pipeline::kMaxStages>& tailFunctions,
           const ScreenIntRect& rect, const AAMaskCtx& aaMaskCtx, const MaskCtx& maskCtx,
           Context& ctx, const PixmapView& pixmapSrc, MutableSubPixmapView* pixmapDst) {
  Pipeline p(functions, tailFunctions, rect, aaMaskCtx, maskCtx, ctx, pixmapSrc, pixmapDst);

  for (std::size_t y = rect.y(); y < rect.bottom(); ++y) {
    std::size_t x = rect.x();
    const std::size_t end = rect.right();

    p.functions = &functions;
    while (x + kStageWidth <= end) {
      p.index = 0;
      p.dx = x;
      p.dy = y;
      p.tail = kStageWidth;
      p.nextStage();
      x += kStageWidth;
    }

    if (x != end) {
      p.index = 0;
      p.functions = &tailFunctions;
      p.dx = x;
      p.dy = y;
      p.tail = end - x;
      p.nextStage();
    }
  }
}

const std::array<StageFn, kStagesCount> STAGES = {
    moveSourceToDestination,
    moveDestinationToSource,
    clamp0,
    clampA,
    premultiply,
    uniformColor,
    seedShader,
    loadDst,
    store,
    loadDstU8,
    storeU8,
    gather,
    loadMaskU8,
    maskU8,
    scaleU8,
    lerpU8,
    scale1Float,
    lerp1Float,
    destinationAtop,
    destinationIn,
    destinationOut,
    destinationOver,
    sourceAtop,
    sourceIn,
    sourceOut,
    sourceOver,
    clear,
    modulate,
    multiply,
    plus,
    screen,
    xOr,
    colorBurn,
    colorDodge,
    darken,
    difference,
    exclusion,
    hardLight,
    lighten,
    overlay,
    softLight,
    hue,
    saturation,
    color,
    luminosity,
    sourceOverRgba,
    transform,
    reflect,
    repeat,
    bilinear,
    bicubic,
    padX1,
    reflectX1,
    repeatX1,
    gradient,
    evenlySpaced2StopGradient,
    xyToUnitAngle,
    xyToRadius,
    xyTo2ptConicalFocalOnCircle,
    xyTo2ptConicalWellBehaved,
    xyTo2ptConicalSmaller,
    xyTo2ptConicalGreater,
    xyTo2ptConicalStrip,
    mask2ptConicalNan,
    mask2ptConicalDegenerates,
    applyVectorMask,
    alter2ptConicalCompensateFocal,
    alter2ptConicalUnswap,
    negateX,
    applyConcentricScaleBias,
    gammaExpand2,
    gammaExpandDst2,
    gammaCompress2,
    gammaExpand22,
    gammaExpandDst22,
    gammaCompress22,
    gammaExpandSrgb,
    gammaExpandDstSrgb,
    gammaCompressSrgb,
    unpremultiply,
    premultiplyDestination,
};

}  // namespace tiny_skia::pipeline::highp
