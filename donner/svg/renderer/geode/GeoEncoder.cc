#include "donner/svg/renderer/geode/GeoEncoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "donner/svg/renderer/geode/GeodeBufferPool.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeImagePipeline.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"
#include "donner/svg/renderer/geode/GeodePipeline.h"
#include "donner/svg/renderer/geode/GeodeResidentPathComponent.h"
#include "donner/svg/renderer/geode/GeodeTextureEncoder.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::geode {

namespace {

/// Round size up to a multiple of 4 (WebGPU requires buffer sizes to be
/// multiples of 4 for COPY_SRC/COPY_DST operations).
constexpr uint64_t roundUp4(uint64_t size) {
  return (size + 3u) & ~uint64_t{3u};
}

/// Round `value` up to a multiple of `alignment` (a power of two).
constexpr uint64_t alignUp(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

// WebGPU binding offset alignment requirements for the per-draw arenas
// (design doc 0030 Milestone 1).
//
// Vertex buffer: `setVertexBuffer(slot, buffer, offset, size)` requires
// `offset` be a multiple of 4 (WebGPU spec §23.8).
constexpr uint64_t kVertexOffsetAlignment = 4u;
// Storage buffer bind-group offset: defaults to 256 across wgpu-native
// backends (the spec-mandated floor for portability). Querying the
// device's `minStorageBufferOffsetAlignment` would let us tighten this
// to 16 or 32 on modern adapters, but 256 is safe everywhere and the
// wasted-tail memory is negligible at typical path sizes.
constexpr uint64_t kStorageOffsetAlignment = 256u;
// Uniform buffer bind-group offset: same 256-byte default across
// wgpu-native backends (`minUniformBufferOffsetAlignment`).
constexpr uint64_t kUniformOffsetAlignment = 256u;

template <typename Handle>
void DestroyResourceBacking(ScopedWgpuHandle<Handle>& handle) {
  if (handle) {
    handle.get().destroy();
  }
}

/// Layout of the per-draw uniform buffer (must match shaders/slug_fill.wgsl).
///
/// WGSL struct layout requires the total size to be a multiple of the largest
/// member alignment. With two mat4x4 members (16-byte alignment) the struct
/// rounds to 176 bytes. We pad explicitly to keep this stable across
/// compilers / WGSL backends.
///
/// Field order must stay in lock-step with the WGSL `Uniforms` struct.
struct alignas(16) Uniforms {
  float mvp[16];              //   0 ..  64
  float patternFromPath[16];  //  64 .. 128
  float viewport[2];          // 128 .. 136
  float tileSize[2];          // 136 .. 144
  float color[4];             // 144 .. 160
  uint32_t fillRule;          // 160 .. 164
  uint32_t paintMode;         // 164 .. 168
  float patternOpacity;       // 168 .. 172
  uint32_t hasClipPolygon;    // 172 .. 176 - 0 = no clip, 1 = clipPolygon active
  // Phase 3b path-clip mask flag. When nonzero, the shader samples the
  // clip mask texture at binding 5 (linear-filtered RGBA8Unorm) and folds
  // its averaged coverage into the fragment colour. A 1x1 dummy
  // texture is always bound so the `textureSample` is always legal.
  uint32_t hasClipMask;  // 176 .. 180
  uint32_t _clipPad0;    // 180 .. 184 - std140 alignment for next vec4 array
  uint32_t _clipPad1;    // 184 .. 188
  uint32_t _clipPad2;    // 188 .. 192
  // Band-grid parameters (0041 §8.1). Two vec4-aligned rows: the fragment
  // shader maps a path-space sample to its horizontal band via
  // floor((y - yBase)/hStride) and its vertical band via
  // floor((x - xBase)/vStride). Field order must match the WGSL `Uniforms`
  // grid block in slug_fill.wgsl.
  float gridYBase;          // 192 .. 196
  float gridHStride;        // 196 .. 200
  uint32_t gridHBandCount;  // 200 .. 204
  float gridXBase;          // 204 .. 208
  float gridVStride;        // 208 .. 212
  uint32_t gridVBandCount;  // 212 .. 216
  uint32_t _gridPad0;       // 216 .. 220
  uint32_t _gridPad1;       // 220 .. 224
  // Phase 3a polygon clipping: a 4-vertex convex clip polygon expressed
  // as 4 edge half-planes, one per side, in VIEWPORT-PIXEL space. Each
  // edge is `(a, b, c)` such that `a*x + b*y + c >= 0` marks the inside
  // half-plane (the normal `(a, b)` points into the clipped region).
  // The fragment shader discards fragments outside these half-planes.
  // Used by `RendererGeode::pushClip` for
  // transformed rectangular viewports (`<symbol>` / `<use>` /
  // `<svg>` viewports with a non-axis-aligned transform) where the
  // true clip shape is a parallelogram that WebGPU's rectangular
  // scissor rect cannot express. Stored as `vec4f[4]` (vec4 = xyz + pad)
  // so the struct stays mat4x4-aligned and the WGSL side reads
  // `array<vec4f, 4>` directly.
  float clipPolygonPlanes[16];  // 224 .. 288 (4 edges × vec4)
};
static_assert(sizeof(Uniforms) == 288, "Uniforms struct layout mismatch");

/// Build a column-major 4x4 matrix from an affine `Transform2d` and write it
/// into the first 16 floats of the output array. Used for the `mvp` and
/// `patternFromPath` uniform fields - both expect mat4x4 layout even though
/// the 2D content only needs a 2x3 affine.
void affineToMat4(const Transform2d& t, float* out16) {
  const double a = t.data[0];
  const double b = t.data[1];
  const double c = t.data[2];
  const double d = t.data[3];
  const double e = t.data[4];
  const double f = t.data[5];
  // col0
  out16[0] = static_cast<float>(a);
  out16[1] = static_cast<float>(b);
  out16[2] = 0.0f;
  out16[3] = 0.0f;
  // col1
  out16[4] = static_cast<float>(c);
  out16[5] = static_cast<float>(d);
  out16[6] = 0.0f;
  out16[7] = 0.0f;
  // col2
  out16[8] = 0.0f;
  out16[9] = 0.0f;
  out16[10] = 1.0f;
  out16[11] = 0.0f;
  // col3
  out16[12] = static_cast<float>(e);
  out16[13] = static_cast<float>(f);
  out16[14] = 0.0f;
  out16[15] = 1.0f;
}

/// Must match `kMaxStops` in `shaders/slug_gradient.wgsl`.
constexpr uint32_t kMaxGradientStops = 16u;

/// Layout of the gradient per-draw uniform buffer. Must match
/// `GradientUniforms` in `shaders/slug_gradient.wgsl`.
///
/// Layout (offsets / sizes):
///   mvp                  64 bytes  [  0 ..  64]
///   viewport             8         [ 64 ..  72]
///   fillRule             4         [ 72 ..  76]
///   spreadMode           4         [ 76 ..  80]
///   row0                 16        [ 80 ..  96]
///   row1                 16        [ 96 .. 112]
///   startGrad            8         [112 .. 120]  (linear)
///   endGrad              8         [120 .. 128]  (linear)
///   radialCenter         8         [128 .. 136]  (radial)
///   radialFocal          8         [136 .. 144]  (radial)
///   radialRadius         4         [144 .. 148]  (radial)
///   radialFocalRadius    4         [148 .. 152]  (radial)
///   gradientKind         4         [152 .. 156]
///   stopCount            4         [156 .. 160]
///   stopColors           16 * 16   [160 .. 416]
///   stopOffsets           4 * 16   [416 .. 480]
///
/// Total: 480 bytes, a multiple of 16.
struct alignas(16) GradientUniforms {
  float mvp[16];             // 0   .. 64
  float viewport[2];         // 64  .. 72
  uint32_t fillRule;         // 72  .. 76
  uint32_t spreadMode;       // 76  .. 80
  float row0[4];             // 80  .. 96
  float row1[4];             // 96  .. 112
  float startGrad[2];        // 112 .. 120
  float endGrad[2];          // 120 .. 128
  float radialCenter[2];     // 128 .. 136
  float radialFocal[2];      // 136 .. 144
  float radialRadius;        // 144 .. 148
  float radialFocalRadius;   // 148 .. 152
  uint32_t gradientKind;     // 152 .. 156
  uint32_t stopCount;        // 156 .. 160
  float stopColors[16 * 4];  // 160 .. 416
  float stopOffsets[4 * 4];  // 416 .. 480
  // Phase 3a convex clip polygon + Phase 3b path-clip mask flag.
  // Layout mirrors `slug_gradient.wgsl` - `hasClipPolygon` +
  // `hasClipMask` + 2 pad u32 to reach vec4 alignment, then the 4
  // half-plane rows.
  uint32_t hasClipPolygon;  // 480 .. 484
  uint32_t hasClipMask;     // 484 .. 488
  uint32_t _clipPad1;       // 488 .. 492
  uint32_t _clipPad2;       // 492 .. 496
  // Band-grid parameters (0041 §8.1), matching the WGSL `GradientUniforms`.
  float gridYBase;              // 496 .. 500
  float gridHStride;            // 500 .. 504
  uint32_t gridHBandCount;      // 504 .. 508
  float gridXBase;              // 508 .. 512
  float gridVStride;            // 512 .. 516
  uint32_t gridVBandCount;      // 516 .. 520
  uint32_t _gridPad0;           // 520 .. 524
  uint32_t _gridPad1;           // 524 .. 528
  float clipPolygonPlanes[16];  // 528 .. 592
};
static_assert(sizeof(GradientUniforms) == 592, "GradientUniforms struct layout mismatch");

/// Gradient kind values shared with `shaders/slug_gradient.wgsl`.
constexpr uint32_t kGradientKindLinear = 0u;
constexpr uint32_t kGradientKindRadial = 1u;

}  // namespace

struct GeoEncoder::Impl {
  GeodeDevice* device;
  const GeodePipeline* pipeline;
  const GeodeGradientPipeline* gradientPipeline;
  const GeodeImagePipeline* imagePipeline;

  /// Per-encoder growable GPU buffer used as a bump-allocation arena
  /// for per-draw data (vertex / band / curve). Design doc 0030
  /// Milestone 1 - replaces the per-fill `dev.createBuffer` pattern
  /// with a single long-lived buffer that each draw writes into at the
  /// current `offset`, then advances.
  ///
  /// Alignment: callers pass the required alignment for the binding.
  /// Vertex buffers need 4-byte offset alignment (WebGPU spec §23.8);
  /// storage buffers need `minStorageBufferOffsetAlignment` which
  /// defaults to 256 on the wgpu-native backends Geode supports.
  ///
  /// Lifetime: when the arena grows, the previous buffer is moved into
  /// `retired` and kept alive for the encoder's lifetime. Commands
  /// already recorded into the command encoder that reference the
  /// old buffer remain valid because the `wgpu::Buffer` RAII wrapper
  /// holds the handle until the `Arena` itself is destroyed (at
  /// encoder destruction, after `finish()` has submitted all work).
  struct Arena {
    ScopedWgpuHandle<wgpu::Buffer> buffer;
    uint64_t capacity = 0;
    uint64_t offset = 0;  // Next unused byte within `buffer`.
    std::vector<ScopedWgpuHandle<wgpu::Buffer>> retired;
    wgpu::BufferUsage usage = wgpu::BufferUsage::CopyDst;
    const char* label = "GeodeArena";
  };
  Arena vertexArena;
  Arena bandArena;
  Arena curveArena;
  Arena uniformArena;
  /// Analytic dual-ray fill (0041 §8): vertical band/curve SSBOs and the dense
  /// H/V band-grid lookup tables. Bound at bindings 8-11 of the fill pipeline.
  Arena vBandArena;
  Arena vCurveArena;
  Arena hGridArena;
  Arena vGridArena;
  /// M6-B step 3: per-instance affine transforms for `fillPathInstanced`.
  /// Packed as two vec4f rows per instance (32 bytes), matching the WGSL
  /// `InstanceTransform` struct. Alignment uses `kStorageOffsetAlignment`
  /// since the binding is storage read-only.
  Arena instanceTransformArena;

  /// Optional cross-frame buffer pool (owned by `RendererGeode::Impl`).
  /// When set, arena growth prefers recycled buffers and arena teardown
  /// returns the fully-grown buffers instead of destroying them. See
  /// `GeodeBufferPool` and design doc 0030 M1.
  GeodeBufferPool* bufferPool = nullptr;

  void destroyArenaBackings(Arena& arena) {
    // With a pool installed, the current (largest, fully-grown) buffer
    // is recycled for the next frame's encoders instead of destroyed.
    // Retired buffers are the outgrown smaller generations - steady
    // state has none - so they are destroyed as before. Safe because
    // encoder destruction only happens after the commands referencing
    // these buffers were submitted (same invariant the eager destroy
    // below already relies on).
    if (bufferPool != nullptr && arena.buffer) {
      bufferPool->release(std::move(arena.buffer), arena.usage, arena.label, arena.capacity);
    }
    DestroyResourceBacking(arena.buffer);
    arena.buffer.reset();
    for (ScopedWgpuHandle<wgpu::Buffer>& buffer : arena.retired) {
      DestroyResourceBacking(buffer);
      buffer.reset();
    }
    arena.retired.clear();
  }

  void destroyOwnedResourceBackings() {
    pass.reset();
    maskPass.reset();

    destroyArenaBackings(vertexArena);
    destroyArenaBackings(bandArena);
    destroyArenaBackings(curveArena);
    destroyArenaBackings(uniformArena);
    destroyArenaBackings(vBandArena);
    destroyArenaBackings(vCurveArena);
    destroyArenaBackings(hGridArena);
    destroyArenaBackings(vGridArena);
    destroyArenaBackings(instanceTransformArena);
    transientResources.destroyBackings();
  }

  // When true, this encoder owns its `commandEncoder` and `finish()`
  // calls `commandEncoder.finish()` + `queue().submit()`. When false
  // (design doc 0030 Milestone 3), the encoder is borrowed from the
  // caller (`RendererGeode`) and `finish()` only ends any open pass.
  bool ownsCommandEncoder = true;

  /// Allocate `size` bytes in `arena` at an offset aligned to
  /// `alignment`, growing if necessary. Writes `size` bytes of `data`
  /// into the buffer via `queue.writeBuffer`. Returns the
  /// (buffer, offset) pair the draw should bind.
  struct Allocation {
    const wgpu::Buffer* buffer;
    uint64_t offset;
    uint64_t size;
  };
  Allocation allocInArena(Arena& arena, const void* data, uint64_t size, uint64_t alignment) {
    uint64_t alignedOffset = (arena.offset + alignment - 1) & ~(alignment - 1);
    if (alignedOffset + size > arena.capacity) {
      // Retire the current buffer (if any) so already-recorded commands
      // can still reference it through encoder submission time, then
      // allocate a new, larger buffer.
      if (arena.buffer) {
        arena.retired.push_back(std::move(arena.buffer));
      }
      constexpr uint64_t kMinGrow = uint64_t{64} * 1024;
      uint64_t newCap = std::max(arena.capacity * 2u, kMinGrow);
      while (newCap < size) newCap *= 2;
      // Prefer a pooled buffer recycled from a previous frame's encoder
      // (design doc 0030 M1: steady-state bufferCreates -> 0). Falls
      // back to a fresh allocation when the pool has no fit.
      if (bufferPool != nullptr) {
        uint64_t pooledCapacity = 0;
        ScopedWgpuHandle<wgpu::Buffer> pooled =
            bufferPool->acquire(arena.usage, arena.label, newCap, &pooledCapacity);
        if (pooled) {
          arena.buffer = std::move(pooled);
          arena.capacity = pooledCapacity;
        }
      }
      if (!arena.buffer) {
        wgpu::BufferDescriptor desc = {};
        desc.label = wgpuLabel(arena.label);
        desc.size = newCap;
        desc.usage = arena.usage;
        arena.buffer.reset(device->device().createBuffer(desc));
        device->countBuffer();
        arena.capacity = newCap;
      }
      arena.offset = 0;
      alignedOffset = 0;
    }
    device->queue().writeBuffer(arena.buffer.get(), alignedOffset, data, size);
    device->countBufferWrite(size);
    arena.offset = alignedOffset + size;
    return {&arena.buffer.get(), alignedOffset, size};
  }

  /// Allocate a read-only storage binding for `byteCount` bytes of `data`,
  /// rounding the size up to a multiple of 4. When `byteCount == 0` (a
  /// degenerate axis with no vertical band data, or an empty grid) a single
  /// zeroed 4-byte slot is bound instead, since WebGPU rejects zero-sized
  /// storage bindings. The shader gates on the band-count uniform so it never
  /// dereferences the dummy slot.
  Allocation allocStorageOrDummy(Arena& arena, const void* data, uint64_t byteCount) {
    if (byteCount == 0) {
      static const uint32_t kZero = 0u;
      return allocInArena(arena, &kZero, sizeof(kZero), kStorageOffsetAlignment);
    }
    return allocInArena(arena, data, roundUp4(byteCount), kStorageOffsetAlignment);
  }

  // ---- GPU residence (design doc 0030 wave 2) ------------------------
  // Defined out-of-line below `FillDrawArgs` (they reference it). See the
  // `GeodeResidentSlot` header and `submitResidentFillDraw` for the flow.

  /// Populate a solid-fill `Uniforms` block from `args` + `encoded`. The
  /// single source of truth shared by the arena `submitFillDraw` and the
  /// resident `submitResidentFillDraw`, so a path that flips between the
  /// two paths (clip toggling) produces byte-identical uniforms.
  void populateFillUniform(Uniforms& u, const EncodedPath& encoded, const FillDrawArgs& args);

  /// (Re)upload `encoded` into `slot`'s persistent combined buffer and
  /// reset its cached bind group. Bumps `bufferCreates` + one
  /// `bufferWrite` (geometry only; the uniform is written separately).
  void uploadResidentGeometry(GeodeResidentSlot& slot, const EncodedPath& encoded);

  /// Build + cache the twelve-entry fill bind group for `slot` (all
  /// bindings reference `slot.buffer` sub-ranges + device dummies).
  void buildResidentBindGroup(GeodeResidentSlot& slot);

  /// Resident solid-fill draw: ensure geometry residence, rewrite the
  /// uniform only if it changed, reuse the cached bind group, and record
  /// the draw. Steady-state (unchanged) frame: zero writes, zero bind
  /// group creates.
  void submitResidentFillDraw(GeodeResidentSlot& slot, const EncodedPath& encoded,
                              const FillDrawArgs& args);

  /// Cheap size fingerprint of an encode, used alongside the
  /// `EncodedPath*` address to defend a resident slot against a missed
  /// invalidation (e.g. the in-place stroke-slot rebuild).
  static uint64_t residentFingerprint(const EncodedPath& e) {
    uint64_t h = e.quadVertices.size();
    h = h * 1000003u + e.bands.size();
    h = h * 1000003u + e.curves.size();
    h = h * 1000003u + e.vBands.size();
    h = h * 1000003u + e.vCurves.size();
    h = h * 1000003u + e.hBandGrid.size();
    h = h * 1000003u + e.vBandGrid.size();
    return h;
  }

  /// Upload the analytic dual-ray buffers + grid params for a gradient draw,
  /// build the 9-binding bind group, and record the single-quad draw. Shared
  /// by the linear and radial gradient paths (0041 §8). `u` is filled in by the
  /// caller except for the grid params, which this writes from `encoded`.
  void submitGradientDraw(GradientUniforms& u, const EncodedPath& encoded) {
    u.gridYBase = encoded.yBase;
    u.gridHStride = encoded.hStride;
    u.gridHBandCount = encoded.hBandCount;
    u.gridXBase = encoded.xBase;
    u.gridVStride = encoded.vStride;
    u.gridVBandCount = encoded.vBandCount;

    const wgpu::Device& dev = device->device();

    const uint64_t vbSize = roundUp4(encoded.quadVertices.size() * sizeof(EncodedPath::Vertex));
    const auto vbAlloc =
        allocInArena(vertexArena, encoded.quadVertices.data(), vbSize, kVertexOffsetAlignment);

    const auto bandsAlloc = allocStorageOrDummy(bandArena, encoded.bands.data(),
                                                encoded.bands.size() * sizeof(EncodedPath::Band));
    const auto curvesAlloc = allocStorageOrDummy(curveArena, encoded.curves.data(),
                                                 encoded.curves.size() * 6u * sizeof(float));
    const auto vBandsAlloc = allocStorageOrDummy(vBandArena, encoded.vBands.data(),
                                                 encoded.vBands.size() * sizeof(EncodedPath::Band));
    const auto vCurvesAlloc = allocStorageOrDummy(vCurveArena, encoded.vCurves.data(),
                                                  encoded.vCurves.size() * 6u * sizeof(float));
    const auto hGridAlloc = allocStorageOrDummy(hGridArena, encoded.hBandGrid.data(),
                                                encoded.hBandGrid.size() * sizeof(uint32_t));
    const auto vGridAlloc = allocStorageOrDummy(vGridArena, encoded.vBandGrid.data(),
                                                encoded.vBandGrid.size() * sizeof(uint32_t));

    const auto uniAlloc =
        allocInArena(uniformArena, &u, sizeof(GradientUniforms), kUniformOffsetAlignment);

    wgpu::BindGroupEntry entries[9] = {};
    entries[0].binding = 0;
    entries[0].buffer = *uniAlloc.buffer;
    entries[0].offset = uniAlloc.offset;
    entries[0].size = uniAlloc.size;
    entries[1].binding = 1;
    entries[1].buffer = *bandsAlloc.buffer;
    entries[1].offset = bandsAlloc.offset;
    entries[1].size = bandsAlloc.size;
    entries[2].binding = 2;
    entries[2].buffer = *curvesAlloc.buffer;
    entries[2].offset = curvesAlloc.offset;
    entries[2].size = curvesAlloc.size;
    entries[3].binding = 3;
    entries[3].textureView = currentClipMaskView();
    entries[4].binding = 4;
    entries[4].sampler = device->dummyClipMaskSampler();
    entries[5].binding = 5;
    entries[5].buffer = *vBandsAlloc.buffer;
    entries[5].offset = vBandsAlloc.offset;
    entries[5].size = vBandsAlloc.size;
    entries[6].binding = 6;
    entries[6].buffer = *vCurvesAlloc.buffer;
    entries[6].offset = vCurvesAlloc.offset;
    entries[6].size = vCurvesAlloc.size;
    entries[7].binding = 7;
    entries[7].buffer = *hGridAlloc.buffer;
    entries[7].offset = hGridAlloc.offset;
    entries[7].size = hGridAlloc.size;
    entries[8].binding = 8;
    entries[8].buffer = *vGridAlloc.buffer;
    entries[8].offset = vGridAlloc.offset;
    entries[8].size = vGridAlloc.size;

    wgpu::BindGroupDescriptor bgDesc = {};
    bgDesc.label = wgpuLabel("GeodeGradientBindGroup");
    bgDesc.layout = gradientPipeline->bindGroupLayout();
    bgDesc.entryCount = 9;
    bgDesc.entries = entries;
    wgpu::BindGroup bindGroup = transientResources.retain(dev.createBindGroup(bgDesc));
    device->countBindGroup();

    pass.get().setVertexBuffer(0, *vbAlloc.buffer, vbAlloc.offset, vbAlloc.size);
    pass.get().setBindGroup(0, bindGroup, 0, nullptr);
    pass.get().draw(static_cast<uint32_t>(encoded.quadVertices.size()), 1, 0, 0);
    device->countDraw();
  }
  // Direct-render texture. External code may sample or copy from it.
  wgpu::Texture target;
  ScopedWgpuHandle<wgpu::TextureView> targetView;
  ScopedWgpuHandle<wgpu::CommandEncoder> ownedCommandEncoder;
  wgpu::CommandEncoder commandEncoder;
  uint32_t targetWidth;
  uint32_t targetHeight;
  // Dummy texture / sampler resources are now owned by `GeodeDevice`
  // and shared across every GeoEncoder - see `GeodeDevice::dummyPatternTexture()`
  // and the M4.2 notes in design doc 0030. Access them via
  // `device->dummyPatternTextureView()`, etc. The bind-group layout
  // always includes the pattern + clip-mask slots so the pipeline can
  // be shared between solid/pattern/gradient/masked draws; the device
  // dummies fill the unused slots.

  // Currently-bound clip mask state (Phase 3b). When
  // `activeClipMaskView` is non-null, `hasClipMask == 1` in the
  // uniforms and draws sample `activeClipMaskView` through the
  // clip-mask binding. When null, the device's shared dummy is bound.
  //
  // `activeClipMaskTexture` keeps the VIEW's parent texture alive for
  // as long as the encoder holds the view. Without this keepalive,
  // when the clip-stack entry that originated the view is destroyed
  // (in `RendererGeode::popClip` - `clipStack.pop_back()` happens
  // BEFORE the follow-up `updateEncoderScissor`), the underlying
  // Vulkan `VkImage` / `VkImageView` are freed while our C++ handle
  // still points at them. A subsequent `createBindGroup` then passes
  // a stale `VkImageView` handle to the Vulkan driver, tripping
  // lvp's `vk_object_base_assert_valid` on debug Mesa and corrupting
  // the heap on release Mesa - see issue #551.
  wgpu::Texture activeClipMaskTexture;
  wgpu::TextureView activeClipMaskView;

  // Non-owning pointer to the device's shared `GeodeMaskPipeline`.
  // Built on demand via `GeodeDevice::maskPipeline()` the first time
  // `beginMaskPass` fires on any encoder in the process; encoders that
  // never open a mask pass pay nothing. Sharing matters because
  // wgpu-native's internal pipeline cache never drains - constructing
  // `GeodeMaskPipeline` per-encoder used to leak alongside the main
  // render pipelines (issue #575).
  GeodeMaskPipeline* maskPipelineOwned = nullptr;

  // While a mask pass is open (`maskPassOpen == true`), the main
  // render pass is closed - draw calls that hit the mask pipeline go
  // through `maskPass`. `beginMaskPass` saves the current `transform`
  // so main-pass draw code picks back up exactly where it left off
  // when the mask pass ends.
  bool maskPassOpen = false;
  ScopedWgpuHandle<wgpu::RenderPassEncoder> maskPass;
  // Transform active when the mask pass was opened, so mask draws use
  // the same device-pixel space as the parent content. The mask pass
  // always renders into the mask texture the caller passed in, which
  // is the same size as the main target.
  Transform2d maskPassSavedTransform = Transform2d();

  // Pending draws are recorded into a render pass that's lazily opened.
  // The first clear/fill triggers `beginPass()`; finish() ends it.
  bool passOpen = false;
  ScopedWgpuHandle<wgpu::RenderPassEncoder> pass;

  // Per-encoder resources created while recording draw calls. They are
  // released together when the encoder is destroyed, after `finish()` has
  // ended the open pass and submitted the command buffer in owning mode.
  ScopedWgpuResourceArena transientResources;

  // Default load op = clear-to-transparent until clear() is called explicitly.
  wgpu::Color clearColor = {0.0, 0.0, 0.0, 0.0};
  bool hasExplicitClear = false;
  // When true, the next render pass uses LoadOp::Load so previously
  // submitted content is preserved. Set via `setLoadPreserve()`.
  bool loadPreserve = false;

  // Current transform - applied to MVP for the next draw.
  Transform2d transform = Transform2d();  // Identity.

  // Current scissor rectangle in target-pixel coords. An empty/unset
  // scissor means "no clipping" (full target extent). Applied to each
  // render pass as it opens via `SetScissorRect` - subsequent pushClip /
  // popClip updates also re-apply during the currently-active pass.
  //
  // `scissorActive == false` means the scissor has never been set OR was
  // popped back to the default, and draws should rasterize into the full
  // target. `scissorActive == true` means the current scissor is
  // `scissorX,scissorY,scissorW,scissorH` (all in target-pixel units).
  bool scissorActive = false;
  uint32_t scissorX = 0;
  uint32_t scissorY = 0;
  uint32_t scissorW = 0;
  uint32_t scissorH = 0;

  /// Phase 3a polygon clipping state. When `clipPolygonActive` is true,
  /// the 4 planes in `clipPolygonPlanes` describe the inside half-plane
  /// of each edge of a convex 4-vertex clip polygon in VIEWPORT-PIXEL
  /// space. Each plane is `(a, b, c)` such that a fragment at
  /// `@builtin(position).xy` is inside when `a*x + b*y + c >= 0`. The
  /// fragment shader discards fragments outside these half-planes.
  ///
  /// Set via `setClipPolygon` from `RendererGeode::pushClip` when the
  /// current clip is a rectangular viewport with a non-axis-aligned
  /// ancestor transform (where WebGPU's scissor rect can only describe
  /// the AABB of the transformed rect, not the true parallelogram).
  /// Cleared via `clearClipPolygon` when popClip restores a clip with
  /// no active polygon.
  bool clipPolygonActive = false;
  float clipPolygonPlanes[16] = {0};  // 4 edges × vec4 (xyz + pad)

  /// Populate the `hasClipPolygon` + `clipPolygonPlanes` fields on an
  /// outgoing `Uniforms` / `GradientUniforms` struct. Keeps the
  /// encoding of the clip state centralised so every draw helper that
  /// writes a uniform picks up the same snapshot.
  void writeClipPolygonUniforms(uint32_t& outFlag, float (&outPlanes)[16]) const {
    outFlag = clipPolygonActive ? 1u : 0u;
    for (size_t i = 0; i < 16; ++i) {
      outPlanes[i] = clipPolygonPlanes[i];
    }
  }

  /// Apply the current scissor to the open render pass. No-op if the
  /// pass isn't open yet - `ensurePassOpen` will call this on first
  /// open. Safe to call whenever `scissorActive` / `scissor*` changes.
  void applyScissorIfPassOpen() {
    if (!passOpen) {
      return;
    }
    if (scissorActive) {
      // Clamp to the target so out-of-bounds scissor rects don't trigger
      // WebGPU validation errors. Required when the clip rect is outside
      // the current viewport (e.g., a nested SVG positioned at the edge).
      uint32_t x = std::min(scissorX, targetWidth);
      uint32_t y = std::min(scissorY, targetHeight);
      uint32_t maxW = targetWidth - x;
      uint32_t maxH = targetHeight - y;
      uint32_t w = std::min(scissorW, maxW);
      uint32_t h = std::min(scissorH, maxH);
      pass.get().setScissorRect(x, y, w, h);
    } else {
      pass.get().setScissorRect(0, 0, targetWidth, targetHeight);
    }
  }

  /// Return the texture view that should be bound to the clip-mask
  /// slot for the next draw - the active mask if set, or the device's
  /// shared identity-mask dummy otherwise (see
  /// `GeodeDevice::dummyClipMaskTextureView()`).
  const wgpu::TextureView& currentClipMaskView() {
    if (activeClipMaskView) {
      return activeClipMaskView;
    }
    return device->dummyClipMaskTextureView();
  }

  /// Open the render pass on demand.
  void ensurePassOpen() {
    if (passOpen) {
      return;
    }
    wgpu::RenderPassColorAttachment color = {};

    color.view = targetView.get();
    color.loadOp = loadPreserve ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    color.clearValue = clearColor;
    // Dawn (browser WebGPU) rejects depthSlice=0 on non-3D views with
    // "Depth slice was provided but the color attachment's view is not
    // 3D". wgpu-native is lenient. Set the UNDEFINED sentinel for
    // cross-backend compatibility.
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

    wgpu::RenderPassDescriptor desc = {};
    desc.colorAttachmentCount = 1;
    desc.colorAttachments = &color;
    desc.label = wgpuLabel("GeoEncoderPass");
    pass.reset(commandEncoder.beginRenderPass(desc));
    // Pipelines are set per-draw - `fillPath` / `fillPathLinearGradient` /
    // `drawImage` each rebind their own pipeline before issuing a draw call.
    // Re-binding only happens when the bound pipeline differs from the next
    // one (see bindSolidPipeline / bindGradientPipeline / bindImagePipeline).
    currentPipeline = BoundPipeline::kNone;
    currentPipelineIsGradient = false;
    passOpen = true;
    // Install any deferred scissor that the renderer requested before the
    // pass was open (e.g., a `pushClip` made before the first draw of the
    // encoder). Also ensures a fresh pass re-applies the scissor if a
    // previous pass was finished and a new one opened.
    applyScissorIfPassOpen();
  }

  /// Track which pipeline is currently bound so we can emit `SetPipeline`
  /// only when it actually changes.
  enum class BoundPipeline { kNone, kSolid, kGradient, kImage };
  BoundPipeline currentPipeline = BoundPipeline::kNone;
  // Kept for backward compat with the gradient binding flag - see
  // bindGradientPipeline below.
  bool currentPipelineIsGradient = false;
  void bindSolidPipeline() {
    if (currentPipeline != BoundPipeline::kSolid) {
      pass.get().setPipeline(pipeline->pipeline());
      device->countPipelineSwitch();
      currentPipeline = BoundPipeline::kSolid;
      currentPipelineIsGradient = false;
    }
  }
  void bindGradientPipeline() {
    if (currentPipeline != BoundPipeline::kGradient) {
      pass.get().setPipeline(gradientPipeline->pipeline());
      device->countPipelineSwitch();
      currentPipeline = BoundPipeline::kGradient;
      currentPipelineIsGradient = true;
    }
  }
  void bindImagePipeline(const wgpu::RenderPipeline& imageRenderPipeline) {
    if (currentPipeline != BoundPipeline::kImage) {
      pass.get().setPipeline(imageRenderPipeline);
      device->countPipelineSwitch();
      currentPipeline = BoundPipeline::kImage;
      currentPipelineIsGradient = false;
    }
  }

  /// Build the MVP matrix from the current transform.
  /// Maps target-pixel coordinates to clip space (-1..+1 with Y down).
  void buildMvp(float* out16) const {
    // First: convert path-space → pixel space via the transform.
    // Then: pixel space → clip space.
    //   x_clip =  2 * x_pixel / width  - 1
    //   y_clip = -2 * y_pixel / height + 1   (Y flip for top-left origin)

    const double sx = 2.0 / static_cast<double>(targetWidth);
    const double sy = -2.0 / static_cast<double>(targetHeight);

    // Composed: clip = scale(sx, sy) * translate(-1, +1) * transform
    // For affine 2D transforms in row-major mat4:
    //   [ a c 0 e ]   where transform.data = [a b c d e f] (column-major 2x3)
    //   [ b d 0 f ]
    //   [ 0 0 1 0 ]
    //   [ 0 0 0 1 ]
    const double a = transform.data[0];
    const double b = transform.data[1];
    const double c = transform.data[2];
    const double d = transform.data[3];
    const double e = transform.data[4];
    const double f = transform.data[5];

    // Compose with the clip-space matrix.
    // Final mat4 (column-major for WGSL):
    //   col0 = (sx*a, sy*b, 0, 0)
    //   col1 = (sx*c, sy*d, 0, 0)
    //   col2 = (0, 0, 1, 0)
    //   col3 = (sx*e - 1, sy*f + 1, 0, 1)
    out16[0] = static_cast<float>(sx * a);
    out16[1] = static_cast<float>(sy * b);
    out16[2] = 0.0f;
    out16[3] = 0.0f;

    out16[4] = static_cast<float>(sx * c);
    out16[5] = static_cast<float>(sy * d);
    out16[6] = 0.0f;
    out16[7] = 0.0f;

    out16[8] = 0.0f;
    out16[9] = 0.0f;
    out16[10] = 1.0f;
    out16[11] = 0.0f;

    out16[12] = static_cast<float>(sx * e - 1.0);
    out16[13] = static_cast<float>(sy * f + 1.0);
    out16[14] = 0.0f;
    out16[15] = 1.0f;
  }
};

// Shared initialisation used by both constructors of GeoEncoder. Fills
// in everything on Impl except `commandEncoder` (which differs between
// the owning and shared-CommandEncoder flavours).
void GeoEncoder::initImpl(GeoEncoder::Impl& impl, GeodeDevice& device,
                          const GeodePipeline& fillPipeline,
                          const GeodeGradientPipeline& gradientPipeline,
                          const GeodeImagePipeline& imagePipeline, const wgpu::Texture& target) {
  impl.device = &device;
  impl.pipeline = &fillPipeline;
  impl.gradientPipeline = &gradientPipeline;
  impl.imagePipeline = &imagePipeline;
  impl.target = target;
  impl.targetView.reset(target.createView());
  impl.targetWidth = target.getWidth();
  impl.targetHeight = target.getHeight();
}

// Post-init: configure per-draw arenas. Runs once `commandEncoder`
// has been installed. Called by both constructors.
void GeoEncoder::finalizeImpl(GeoEncoder::Impl& impl) {
  // Dummy pattern / clip-mask textures + samplers are now owned by
  // `GeodeDevice` (see design doc 0030 §M4.2) and shared across
  // encoders - no per-encoder create needed.

  // Configure per-draw arenas (bump-allocated GPU buffers, design
  // doc 0030 Milestone 1). They stay empty here; the first draw
  // triggers lazy growth to 64 KiB and doubles from there.
  impl.vertexArena.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
  impl.vertexArena.label = "GeodeVertexArena";
  impl.bandArena.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  impl.bandArena.label = "GeodeBandArena";
  impl.curveArena.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  impl.curveArena.label = "GeodeCurveArena";
  impl.uniformArena.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  impl.uniformArena.label = "GeodeUniformArena";
  impl.instanceTransformArena.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  impl.instanceTransformArena.label = "GeodeInstanceTransformArena";
  impl.vBandArena.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  impl.vBandArena.label = "GeodeVBandArena";
  impl.vCurveArena.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  impl.vCurveArena.label = "GeodeVCurveArena";
  impl.hGridArena.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  impl.hGridArena.label = "GeodeHGridArena";
  impl.vGridArena.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  impl.vGridArena.label = "GeodeVGridArena";
}

GeoEncoder::GeoEncoder(GeodeDevice& device, const GeodePipeline& fillPipeline,
                       const GeodeGradientPipeline& gradientPipeline,
                       const GeodeImagePipeline& imagePipeline, const wgpu::Texture& target)
    : impl_(std::make_unique<Impl>()) {
  initImpl(*impl_, device, fillPipeline, gradientPipeline, imagePipeline, target);

  wgpu::CommandEncoderDescriptor desc = {};
  desc.label = wgpuLabel("GeoEncoder");
  impl_->ownedCommandEncoder.reset(device.device().createCommandEncoder(desc));
  impl_->commandEncoder = impl_->ownedCommandEncoder.get();
  impl_->ownsCommandEncoder = true;

  finalizeImpl(*impl_);
}

GeoEncoder::GeoEncoder(GeodeDevice& device, const GeodePipeline& fillPipeline,
                       const GeodeGradientPipeline& gradientPipeline,
                       const GeodeImagePipeline& imagePipeline, const wgpu::Texture& target,
                       wgpu::CommandEncoder sharedCommandEncoder)
    : impl_(std::make_unique<Impl>()) {
  initImpl(*impl_, device, fillPipeline, gradientPipeline, imagePipeline, target);
  impl_->commandEncoder = std::move(sharedCommandEncoder);
  impl_->ownsCommandEncoder = false;
  finalizeImpl(*impl_);
}

GeoEncoder::~GeoEncoder() {
  if (impl_) {
    impl_->destroyOwnedResourceBackings();
  }
}
GeoEncoder::GeoEncoder(GeoEncoder&&) noexcept = default;
GeoEncoder& GeoEncoder::operator=(GeoEncoder&&) noexcept = default;

void GeoEncoder::clear(const css::RGBA& color) {
  // If the pass is already open, the clear has effectively been baked in;
  // recording a "clear" mid-pass is unsupported in this MVP. Document and
  // ignore - the right approach for mid-pass clears is to draw a fullscreen
  // quad, which we can add later if needed.
  if (impl_->passOpen) {
    return;
  }
  // Premultiply the clear color by alpha to match the pipeline's
  // premultiplied blend state. `fillPath` premultiplies its paint color the
  // same way, and the read-back in `RendererGeode::takeSnapshot` assumes the
  // texture contents are premultiplied throughout. Clearing with a straight-
  // alpha value would break that invariant for any semi-transparent clear.
  const double alpha = color.a / 255.0;
  impl_->clearColor.r = (color.r / 255.0) * alpha;
  impl_->clearColor.g = (color.g / 255.0) * alpha;
  impl_->clearColor.b = (color.b / 255.0) * alpha;
  impl_->clearColor.a = alpha;
  impl_->hasExplicitClear = true;
}

void GeoEncoder::setTransform(const Transform2d& transform) {
  impl_->transform = transform;
}

void GeoEncoder::setScissorRect(int32_t x, int32_t y, int32_t w, int32_t h) {
  // Clamp negative / overflowing values at the u32 boundary so WebGPU's
  // strict validation never sees an out-of-range value. A scissor with
  // zero area is valid (clips everything).
  const int32_t clampedX = std::max(0, x);
  const int32_t clampedY = std::max(0, y);
  const int32_t clampedW = std::max(0, w - (clampedX - x));
  const int32_t clampedH = std::max(0, h - (clampedY - y));
  impl_->scissorActive = true;
  impl_->scissorX = static_cast<uint32_t>(clampedX);
  impl_->scissorY = static_cast<uint32_t>(clampedY);
  impl_->scissorW = static_cast<uint32_t>(clampedW);
  impl_->scissorH = static_cast<uint32_t>(clampedH);
  impl_->applyScissorIfPassOpen();
}

void GeoEncoder::clearScissorRect() {
  impl_->scissorActive = false;
  impl_->applyScissorIfPassOpen();
}

void GeoEncoder::setClipPolygon(const Vector2d corners[4]) {
  // Compute the inward-facing half-plane for each of the 4 edges of the
  // convex polygon. Edge i runs from `corners[i]` to `corners[(i+1)%4]`,
  // with direction `d = corners[(i+1)%4] - corners[i]`. The inward
  // normal is `n = (-d.y, d.x)` when the winding is counter-clockwise
  // in screen space (which in SVG's y-down coord system is "clockwise
  // when viewed on-screen"). We DETECT the winding by computing the
  // signed area of the polygon; if the area is negative we flip the
  // normals so the half-plane equations always point *inside*.
  //
  // Each plane is stored as (a, b, c, pad) where `a*x + b*y + c >= 0`
  // marks the inside half-plane. `c = -(a*corners[i].x + b*corners[i].y)`
  // offsets the plane to pass through the edge start.

  // Signed area (Shoelace formula / 2). Positive → CCW in standard math
  // (y-up) but in SVG (y-down) that maps to a CW visual winding.
  double signedArea = 0.0;
  for (size_t i = 0; i < 4; ++i) {
    const Vector2d& p0 = corners[i];
    const Vector2d& p1 = corners[(i + 1) % 4];
    signedArea += (p0.x * p1.y) - (p1.x * p0.y);
  }
  const double windingSign = signedArea >= 0.0 ? 1.0 : -1.0;

  for (size_t i = 0; i < 4; ++i) {
    const Vector2d& p0 = corners[i];
    const Vector2d& p1 = corners[(i + 1) % 4];
    const double dx = p1.x - p0.x;
    const double dy = p1.y - p0.y;
    // Inward normal: rotate (dx, dy) by +90° (= (-dy, dx)) and flip if
    // the overall polygon winding is negative.
    double nx = -dy * windingSign;
    double ny = dx * windingSign;
    // Normalise so the half-plane value is in viewport-pixel units
    // (makes the per-sample test resolution-independent).
    const double len = std::sqrt(nx * nx + ny * ny);
    if (len > 1e-12) {
      nx /= len;
      ny /= len;
    }
    const double c = -(nx * p0.x + ny * p0.y);
    impl_->clipPolygonPlanes[i * 4 + 0] = static_cast<float>(nx);
    impl_->clipPolygonPlanes[i * 4 + 1] = static_cast<float>(ny);
    impl_->clipPolygonPlanes[i * 4 + 2] = static_cast<float>(c);
    impl_->clipPolygonPlanes[i * 4 + 3] = 0.0f;
  }
  impl_->clipPolygonActive = true;
}

void GeoEncoder::clearClipPolygon() {
  impl_->clipPolygonActive = false;
  for (size_t i = 0; i < 16; ++i) {
    impl_->clipPolygonPlanes[i] = 0.0f;
  }
}

// ============================================================================
// Phase 3b: clip mask pass
// ============================================================================

void GeoEncoder::beginMaskPass(const wgpu::Texture& mask) {
  if (!mask) {
    return;
  }

  // Close the current main render pass so the new mask pass can open
  // against the mask texture. A subsequent main draw will lazily
  // reopen the main pass with `LoadOp::Load` via `setLoadPreserve()`.
  // Only flip `loadPreserve` when the main pass was *actually* open:
  // if no main draw has landed yet the initial clear still needs to
  // run on the next open.
  const bool mainPassWasOpen = impl_->passOpen;
  if (mainPassWasOpen) {
    impl_->pass.get().end();
    impl_->pass.reset();
    impl_->passOpen = false;
    impl_->loadPreserve = true;
  }

  // Fetch the device's shared mask pipeline - lazily built on first
  // `maskPipeline()` call.
  if (!impl_->maskPipelineOwned) {
    impl_->maskPipelineOwned = &impl_->device->maskPipeline();
  }

  impl_->maskPassSavedTransform = impl_->transform;

  wgpu::TextureView maskView = impl_->transientResources.retain(mask.createView());

  wgpu::RenderPassColorAttachment color = {};
  color.view = maskView;
  color.loadOp = wgpu::LoadOp::Clear;
  color.storeOp = wgpu::StoreOp::Store;
  color.clearValue = {0.0, 0.0, 0.0, 0.0};
  // See ensurePassOpen above - Dawn requires UNDEFINED on non-3D views.
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

  wgpu::RenderPassDescriptor desc = {};
  desc.colorAttachmentCount = 1;
  desc.colorAttachments = &color;
  desc.label = wgpuLabel("GeoEncoderMaskPass");
  impl_->maskPass.reset(impl_->commandEncoder.beginRenderPass(desc));
  impl_->maskPass.get().setPipeline(impl_->maskPipelineOwned->pipeline());
  impl_->device->countPipelineSwitch();
  // Full-target scissor so clip-path fills aren't clipped by any
  // outer scissor still cached in the encoder state.
  impl_->maskPass.get().setScissorRect(0, 0, impl_->targetWidth, impl_->targetHeight);
  impl_->maskPassOpen = true;
}

void GeoEncoder::fillPathIntoMask(const Path& path, FillRule rule,
                                  const EncodedPath* precomputedEncoded) {
  if (!impl_->maskPassOpen) {
    return;
  }
  // The mask pipeline now samples a nested clip mask via bindings
  // Dummy resources are pre-created in the encoder constructor so
  // the bind group is always complete, even if `beginMaskPass` fires
  // before any main draw.
  EncodedPath ownedEncoded;
  const EncodedPath* encodedPtr = precomputedEncoded;
  if (!encodedPtr) {
    impl_->device->countPathEncode();
    ownedEncoded = GeodePathEncoder::encode(path, rule);
    encodedPtr = &ownedEncoded;
  }
  const EncodedPath& encoded = *encodedPtr;
  if (encoded.empty()) {
    return;
  }

  const wgpu::Device& dev = impl_->device->device();

  // Analytic dual-ray buffers (0041 §8): single quad + H/V bands/curves/grids.
  const uint64_t vbSize = roundUp4(encoded.quadVertices.size() * sizeof(EncodedPath::Vertex));
  const auto vbAlloc = impl_->allocInArena(impl_->vertexArena, encoded.quadVertices.data(), vbSize,
                                           kVertexOffsetAlignment);

  const auto bandsAlloc = impl_->allocStorageOrDummy(
      impl_->bandArena, encoded.bands.data(), encoded.bands.size() * sizeof(EncodedPath::Band));
  const auto curvesAlloc = impl_->allocStorageOrDummy(impl_->curveArena, encoded.curves.data(),
                                                      encoded.curves.size() * 6u * sizeof(float));
  const auto vBandsAlloc = impl_->allocStorageOrDummy(
      impl_->vBandArena, encoded.vBands.data(), encoded.vBands.size() * sizeof(EncodedPath::Band));
  const auto vCurvesAlloc = impl_->allocStorageOrDummy(impl_->vCurveArena, encoded.vCurves.data(),
                                                       encoded.vCurves.size() * 6u * sizeof(float));
  const auto hGridAlloc = impl_->allocStorageOrDummy(impl_->hGridArena, encoded.hBandGrid.data(),
                                                     encoded.hBandGrid.size() * sizeof(uint32_t));
  const auto vGridAlloc = impl_->allocStorageOrDummy(impl_->vGridArena, encoded.vBandGrid.data(),
                                                     encoded.vBandGrid.size() * sizeof(uint32_t));

  // Mask uniforms - mvp, viewport, fillRule, hasClipMask, grid params. The
  // `hasClipMask` field gates whether the fragment shader intersects with the
  // nested clip mask at binding 3 (nested `<clipPath>` references).
  struct alignas(16) MaskUniforms {
    float mvp[16];            //  0 ..  64
    float viewport[2];        // 64 ..  72
    uint32_t fillRule;        // 72 ..  76
    uint32_t hasClipMask;     // 76 ..  80
    float gridYBase;          // 80 ..  84
    float gridHStride;        // 84 ..  88
    uint32_t gridHBandCount;  // 88 ..  92
    float gridXBase;          // 92 ..  96
    float gridVStride;        // 96 .. 100
    uint32_t gridVBandCount;  // 100 .. 104
    uint32_t _gridPad0;       // 104 .. 108
    uint32_t _gridPad1;       // 108 .. 112
  };
  static_assert(sizeof(MaskUniforms) == 112, "MaskUniforms layout mismatch");

  MaskUniforms u = {};
  impl_->buildMvp(u.mvp);
  u.viewport[0] = static_cast<float>(impl_->targetWidth);
  u.viewport[1] = static_cast<float>(impl_->targetHeight);
  u.fillRule = (rule == FillRule::EvenOdd) ? 1u : 0u;
  u.hasClipMask = impl_->activeClipMaskView ? 1u : 0u;
  u.gridYBase = encoded.yBase;
  u.gridHStride = encoded.hStride;
  u.gridHBandCount = encoded.hBandCount;
  u.gridXBase = encoded.xBase;
  u.gridVStride = encoded.vStride;
  u.gridVBandCount = encoded.vBandCount;

  const auto uniAlloc =
      impl_->allocInArena(impl_->uniformArena, &u, sizeof(MaskUniforms), kUniformOffsetAlignment);

  wgpu::BindGroupEntry entries[9] = {};
  entries[0].binding = 0;
  entries[0].buffer = *uniAlloc.buffer;
  entries[0].offset = uniAlloc.offset;
  entries[0].size = uniAlloc.size;
  entries[1].binding = 1;
  entries[1].buffer = *bandsAlloc.buffer;
  entries[1].offset = bandsAlloc.offset;
  entries[1].size = bandsAlloc.size;
  entries[2].binding = 2;
  entries[2].buffer = *curvesAlloc.buffer;
  entries[2].offset = curvesAlloc.offset;
  entries[2].size = curvesAlloc.size;
  entries[3].binding = 3;
  entries[3].textureView = impl_->currentClipMaskView();
  entries[4].binding = 4;
  entries[4].sampler = impl_->device->dummyClipMaskSampler();
  entries[5].binding = 5;
  entries[5].buffer = *vBandsAlloc.buffer;
  entries[5].offset = vBandsAlloc.offset;
  entries[5].size = vBandsAlloc.size;
  entries[6].binding = 6;
  entries[6].buffer = *vCurvesAlloc.buffer;
  entries[6].offset = vCurvesAlloc.offset;
  entries[6].size = vCurvesAlloc.size;
  entries[7].binding = 7;
  entries[7].buffer = *hGridAlloc.buffer;
  entries[7].offset = hGridAlloc.offset;
  entries[7].size = hGridAlloc.size;
  entries[8].binding = 8;
  entries[8].buffer = *vGridAlloc.buffer;
  entries[8].offset = vGridAlloc.offset;
  entries[8].size = vGridAlloc.size;

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = wgpuLabel("GeodeMaskBindGroup");
  bgDesc.layout = impl_->maskPipelineOwned->bindGroupLayout();
  bgDesc.entryCount = 9;
  bgDesc.entries = entries;
  wgpu::BindGroup bindGroup = impl_->transientResources.retain(dev.createBindGroup(bgDesc));
  impl_->device->countBindGroup();

  impl_->maskPass.get().setVertexBuffer(0, *vbAlloc.buffer, vbAlloc.offset, vbAlloc.size);
  impl_->maskPass.get().setBindGroup(0, bindGroup, 0, nullptr);
  impl_->maskPass.get().draw(static_cast<uint32_t>(encoded.quadVertices.size()), 1, 0, 0);
  impl_->device->countDraw();
}

void GeoEncoder::endMaskPass() {
  if (!impl_->maskPassOpen) {
    return;
  }
  impl_->maskPass.get().end();
  impl_->maskPass.reset();
  impl_->maskPassOpen = false;
  // Rebind pipeline tracker - the main pass will need to re-select a
  // pipeline on its next draw.
  impl_->currentPipeline = Impl::BoundPipeline::kNone;
  impl_->currentPipelineIsGradient = false;
  // Restore encoder transform in case the caller trampled it with a
  // mask-local transform.
  impl_->transform = impl_->maskPassSavedTransform;
}

void GeoEncoder::setClipMask(const wgpu::TextureView& maskView) {
  impl_->activeClipMaskView = maskView;
  // activeClipMaskTexture keepalive is left empty by this overload; the
  // caller must guarantee the parent stays alive. The 2-arg overload
  // is preferred for any path that doesn't own the texture's lifetime
  // at the call site (e.g. `RendererGeode::updateEncoderScissor`
  // pulling the view off a clip-stack entry that may be destroyed
  // before the next draw).
  impl_->activeClipMaskTexture = wgpu::Texture{};
}

void GeoEncoder::setClipMask(const wgpu::Texture& maskTexture, const wgpu::TextureView& maskView) {
  impl_->activeClipMaskTexture = maskTexture;
  impl_->activeClipMaskView = maskView;
}

void GeoEncoder::clearClipMask() {
  impl_->activeClipMaskView = wgpu::TextureView{};
  impl_->activeClipMaskTexture = wgpu::Texture{};
}

void GeoEncoder::setBufferPool(GeodeBufferPool* pool) {
  impl_->bufferPool = pool;
}

void GeoEncoder::setLoadPreserve() {
  // No-op if a pass is already open - loadOp is a pass-construction
  // parameter and can't be changed mid-pass. The RendererGeode caller
  // always invokes this right after constructing a fresh encoder, so the
  // pass isn't open yet in practice.
  if (impl_->passOpen) {
    return;
  }
  impl_->loadPreserve = true;
}

/// Parameters for the shared `submitFillDraw` helper. Solid-color and
/// pattern-tile fills share the same encode → upload → draw pipeline; only
/// the uniform contents and bound texture differ.
struct GeoEncoder::FillDrawArgs {
  const Path* path;
  FillRule rule;

  /// Optional precomputed encode. When non-null, `submitFillDraw`
  /// uses this directly and skips `GeodePathEncoder::encode` +
  /// `countPathEncode`. Used by the M2 `GeodePathCacheComponent`
  /// cache-hit path (design doc 0030). When null, the encode runs
  /// inline (legacy behavior).
  const EncodedPath* precomputedEncoded = nullptr;

  // Paint mode selector. 0 = solid, 1 = pattern.
  uint32_t paintMode;

  // Solid-fill-only fields (ignored when paintMode == 1).
  float solidColor[4];  // Premultiplied.

  // Pattern-fill-only fields (ignored when paintMode == 0).
  wgpu::TextureView patternView;
  wgpu::Sampler patternSampler;
  Transform2d patternFromPath;
  Vector2d tileSize;
  float patternOpacity;

  /// M6 Bullet 2: optional per-instance transform buffer. When null,
  /// `submitFillDraw` binds `GeodeDevice::identityInstanceTransformBuffer`
  /// (single-instance draws). When set, the caller has uploaded N
  /// copies of the 2-vec4f `InstanceTransform` struct at
  /// `instanceTransformsOffset`, and `submitFillDraw` issues a draw
  /// with `instanceCount == N`.
  const wgpu::Buffer* instanceTransformsBuffer = nullptr;
  uint64_t instanceTransformsOffset = 0;
  uint64_t instanceTransformsSize = 0;
  uint32_t instanceCount = 1;
};

void GeoEncoder::fillPath(const Path& path, const css::RGBA& color, FillRule rule,
                          const EncodedPath* precomputedEncoded) {
  FillDrawArgs args = {};
  args.path = &path;
  args.rule = rule;
  args.precomputedEncoded = precomputedEncoded;
  args.paintMode = 0u;
  const float alpha = color.a / 255.0f;
  args.solidColor[0] = (color.r / 255.0f) * alpha;
  args.solidColor[1] = (color.g / 255.0f) * alpha;
  args.solidColor[2] = (color.b / 255.0f) * alpha;
  args.solidColor[3] = alpha;
  args.patternOpacity = 1.0f;

  // Solid-mode binds the pre-created dummy texture + sampler so the
  // bind group layout (which always includes pattern bindings) is
  // complete. Both are built once in the encoder constructor.
  args.patternView = impl_->device->dummyPatternTextureView();
  args.patternSampler = impl_->device->dummyPatternSampler();
  args.tileSize = Vector2d(1.0, 1.0);
  args.patternFromPath = Transform2d();

  submitFillDraw(args);
}

// ============================================================================
// GPU residence (design doc 0030 wave 2)
// ============================================================================

void GeoEncoder::Impl::populateFillUniform(Uniforms& u, const EncodedPath& encoded,
                                           const FillDrawArgs& args) {
  buildMvp(u.mvp);
  affineToMat4(args.patternFromPath, u.patternFromPath);
  u.viewport[0] = static_cast<float>(targetWidth);
  u.viewport[1] = static_cast<float>(targetHeight);
  u.tileSize[0] = static_cast<float>(args.tileSize.x);
  u.tileSize[1] = static_cast<float>(args.tileSize.y);
  u.color[0] = args.solidColor[0];
  u.color[1] = args.solidColor[1];
  u.color[2] = args.solidColor[2];
  u.color[3] = args.solidColor[3];
  u.fillRule = (args.rule == FillRule::EvenOdd) ? 1u : 0u;
  u.paintMode = args.paintMode;
  u.patternOpacity = args.patternOpacity;
  writeClipPolygonUniforms(u.hasClipPolygon, u.clipPolygonPlanes);
  u.hasClipMask = activeClipMaskView ? 1u : 0u;
  u.gridYBase = encoded.yBase;
  u.gridHStride = encoded.hStride;
  u.gridHBandCount = encoded.hBandCount;
  u.gridXBase = encoded.xBase;
  u.gridVStride = encoded.vStride;
  u.gridVBandCount = encoded.vBandCount;
}

void GeoEncoder::Impl::uploadResidentGeometry(GeodeResidentSlot& slot, const EncodedPath& encoded) {
  // Drop any previous residence (first upload, a re-upload after the stroke
  // slot rebuilt in place, or a re-upload because a DIFFERENT device now
  // renders this document). Frees the old buffer and settles the live-bytes
  // gauge before we account the new allocation. When the existing buffer
  // belongs to a different device we release our ref WITHOUT calling
  // `destroy()`: that device may already be gone, and destroy() would route
  // into it (see `GeodeResidentSlot::owningDeviceId`).
  const bool ownedByCurrentDevice = slot.owningDeviceId == device->deviceId();
  slot.reset(/*destroyBuffer=*/ownedByCurrentDevice);

  const uint64_t vBytes = encoded.quadVertices.size() * sizeof(EncodedPath::Vertex);
  const uint64_t bandsBytes = encoded.bands.size() * sizeof(EncodedPath::Band);
  const uint64_t curvesBytes = encoded.curves.size() * 6u * sizeof(float);
  const uint64_t vBandsBytes = encoded.vBands.size() * sizeof(EncodedPath::Band);
  const uint64_t vCurvesBytes = encoded.vCurves.size() * 6u * sizeof(float);
  const uint64_t hGridBytes = encoded.hBandGrid.size() * sizeof(uint32_t);
  const uint64_t vGridBytes = encoded.vBandGrid.size() * sizeof(uint32_t);

  // Lay out the combined buffer. Each storage region's offset satisfies
  // `kStorageOffsetAlignment`; the vertex region needs 4-byte alignment;
  // the uniform region needs `kUniformOffsetAlignment`. Empty storage
  // regions reserve a 4-byte zero-filled dummy slot (the shader's
  // band-count gate never dereferences them), mirroring the arena
  // `allocStorageOrDummy` dummy.
  uint64_t cursor = 0;
  auto place = [&](GeodeResidentSlot::Region& r, uint64_t bytes, uint64_t alignment,
                   bool storageDummy) {
    cursor = alignUp(cursor, alignment);
    r.offset = cursor;
    r.size = (storageDummy && bytes == 0) ? uint64_t{4} : roundUp4(bytes);
    cursor += r.size;
  };
  place(slot.vertex, vBytes, kVertexOffsetAlignment, /*storageDummy=*/false);
  place(slot.bands, bandsBytes, kStorageOffsetAlignment, /*storageDummy=*/true);
  place(slot.curves, curvesBytes, kStorageOffsetAlignment, /*storageDummy=*/true);
  place(slot.vBands, vBandsBytes, kStorageOffsetAlignment, /*storageDummy=*/true);
  place(slot.vCurves, vCurvesBytes, kStorageOffsetAlignment, /*storageDummy=*/true);
  place(slot.hGrid, hGridBytes, kStorageOffsetAlignment, /*storageDummy=*/true);
  place(slot.vGrid, vGridBytes, kStorageOffsetAlignment, /*storageDummy=*/true);
  place(slot.uniform, sizeof(Uniforms), kUniformOffsetAlignment, /*storageDummy=*/false);

  const uint64_t totalSize = cursor;
  const uint64_t geometrySize = slot.uniform.offset;  // everything before the uniform.

  wgpu::BufferDescriptor desc = {};
  desc.label = wgpuLabel("GeodeResidentPath");
  desc.size = totalSize;
  desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::Storage |
               wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  slot.buffer.reset(device->device().createBuffer(desc));
  device->countBuffer();

  // Assemble the geometry portion in one zero-filled staging blob so
  // padding + dummy regions read as zero, then upload it in a single
  // `writeBuffer`. The uniform region is written separately by the draw
  // path (so a camera/color-only change rewrites just 288 bytes).
  std::vector<uint8_t> staging(geometrySize, 0u);
  auto blit = [&](const GeodeResidentSlot::Region& r, const void* data, uint64_t bytes) {
    if (bytes > 0) {
      std::memcpy(staging.data() + r.offset, data, bytes);
    }
  };
  blit(slot.vertex, encoded.quadVertices.data(), vBytes);
  blit(slot.bands, encoded.bands.data(), bandsBytes);
  blit(slot.curves, encoded.curves.data(), curvesBytes);
  blit(slot.vBands, encoded.vBands.data(), vBandsBytes);
  blit(slot.vCurves, encoded.vCurves.data(), vCurvesBytes);
  blit(slot.hGrid, encoded.hBandGrid.data(), hGridBytes);
  blit(slot.vGrid, encoded.vBandGrid.data(), vGridBytes);

  device->queue().writeBuffer(slot.buffer.get(), 0, staging.data(), geometrySize);
  device->countBufferWrite(geometrySize);

  slot.vertexCount = static_cast<uint32_t>(encoded.quadVertices.size());
  slot.resident = true;
  slot.lastUniform.clear();  // Force the first uniform write below.

  slot.liveBytesGauge = device->residentBytesGauge();
  slot.accountedBytes = static_cast<int64_t>(totalSize);
  slot.liveBytesGauge->fetch_add(slot.accountedBytes, std::memory_order_relaxed);

  // Stamp the current device's identity so a later render by a different
  // device re-uploads instead of binding this device's buffer / bind group
  // into the other device's render pass (WebGPU rejects that).
  slot.owningDeviceId = device->deviceId();
}

void GeoEncoder::Impl::buildResidentBindGroup(GeodeResidentSlot& slot) {
  const wgpu::Device& dev = device->device();
  const wgpu::Buffer& buf = slot.buffer.get();

  wgpu::BindGroupEntry entries[12] = {};
  auto bufEntry = [&](int i, uint32_t binding, const GeodeResidentSlot::Region& r) {
    entries[i].binding = binding;
    entries[i].buffer = buf;
    entries[i].offset = r.offset;
    entries[i].size = r.size;
  };
  bufEntry(0, 0, slot.uniform);
  bufEntry(1, 1, slot.bands);
  bufEntry(2, 2, slot.curves);
  entries[3].binding = 3;
  entries[3].textureView = device->dummyPatternTextureView();
  entries[4].binding = 4;
  entries[4].sampler = device->dummyPatternSampler();
  // Residence is only taken with no active clip, so the clip-mask slot is
  // always the device's identity-coverage dummy - bind it directly so the
  // cached bind group is deterministic.
  entries[5].binding = 5;
  entries[5].textureView = device->dummyClipMaskTextureView();
  entries[6].binding = 6;
  entries[6].sampler = device->dummyClipMaskSampler();
  entries[7].binding = 7;
  entries[7].buffer = device->identityInstanceTransformBuffer();
  entries[7].offset = 0;
  entries[7].size = 32u;
  bufEntry(8, 8, slot.vBands);
  bufEntry(9, 9, slot.vCurves);
  bufEntry(10, 10, slot.hGrid);
  bufEntry(11, 11, slot.vGrid);

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = wgpuLabel("GeodeResidentBindGroup");
  bgDesc.layout = pipeline->bindGroupLayout();
  bgDesc.entryCount = 12;
  bgDesc.entries = entries;
  slot.bindGroup.reset(dev.createBindGroup(bgDesc));
  device->countBindGroup();
}

void GeoEncoder::Impl::submitResidentFillDraw(GeodeResidentSlot& slot, const EncodedPath& encoded,
                                              const FillDrawArgs& args) {
  ensurePassOpen();
  bindSolidPipeline();

  // Ensure the geometry is resident and current AND owned by THIS device.
  // Component removal is the primary invalidation; the pointer + fingerprint
  // guard catches the in-place stroke-slot rebuild (which replaces the encode
  // contents without removing the component); the device-id guard catches a
  // second device rendering a document whose residence was filled by a
  // now-different device (WebGPU rejects cross-device buffers / bind groups).
  const uint64_t fingerprint = residentFingerprint(encoded);
  const bool needUpload = !slot.resident || !slot.buffer || slot.encodedKey != &encoded ||
                          slot.encodedFingerprint != fingerprint ||
                          slot.owningDeviceId != device->deviceId();
  if (needUpload) {
    uploadResidentGeometry(slot, encoded);
    slot.encodedKey = &encoded;
    slot.encodedFingerprint = fingerprint;
  }

  // Rewrite the uniform only when it actually changed. A static
  // re-render (same viewport, same paint) produces byte-identical
  // uniforms, so this write is skipped entirely and the frame emits zero
  // buffer writes.
  Uniforms u = {};
  populateFillUniform(u, encoded, args);
  const auto* uBytes = reinterpret_cast<const uint8_t*>(&u);
  if (slot.lastUniform.size() != sizeof(Uniforms) ||
      std::memcmp(slot.lastUniform.data(), uBytes, sizeof(Uniforms)) != 0) {
    device->queue().writeBuffer(slot.buffer.get(), slot.uniform.offset, &u, sizeof(Uniforms));
    device->countBufferWrite(sizeof(Uniforms));
    slot.lastUniform.assign(uBytes, uBytes + sizeof(Uniforms));
  }

  if (!slot.bindGroup) {
    buildResidentBindGroup(slot);
  }

  pass.get().setVertexBuffer(0, slot.buffer.get(), slot.vertex.offset, slot.vertex.size);
  pass.get().setBindGroup(0, slot.bindGroup.get(), 0, nullptr);
  pass.get().draw(slot.vertexCount, 1, 0, 0);
  device->countDraw();
}

void GeoEncoder::fillPathResident(GeodeResidentSlot& slot, const EncodedPath& encoded,
                                  const css::RGBA& color, FillRule rule, uint64_t frameId) {
  if (encoded.empty()) {
    return;
  }

  // Build the same solid-fill args `fillPath` would, so both paths share
  // `populateFillUniform` and the fallback stays bit-exact.
  FillDrawArgs args = {};
  args.path = nullptr;
  args.rule = rule;
  args.precomputedEncoded = &encoded;
  args.paintMode = 0u;
  const float alpha = color.a / 255.0f;
  args.solidColor[0] = (color.r / 255.0f) * alpha;
  args.solidColor[1] = (color.g / 255.0f) * alpha;
  args.solidColor[2] = (color.b / 255.0f) * alpha;
  args.solidColor[3] = alpha;
  args.patternOpacity = 1.0f;
  args.patternView = impl_->device->dummyPatternTextureView();
  args.patternSampler = impl_->device->dummyPatternSampler();
  args.tileSize = Vector2d(1.0, 1.0);
  args.patternFromPath = Transform2d();

  // Residence is only safe (bind group stable, uniform clip flags zero)
  // when no clip mask / clip polygon / mask pass is active. Otherwise fall
  // back to the wave-1 arena path so clipped / masked draws stay exact.
  if (impl_->activeClipMaskView || impl_->clipPolygonActive || impl_->maskPassOpen) {
    submitFillDraw(args);
    return;
  }
  // A slot's single uniform buffer can only carry one draw's uniform per
  // frame. If this slot was already drawn resident this frame (markers,
  // non-adjacent repeated `<use>` at distinct transforms), route the
  // repeat through the arena path so it gets its own uniform + bind group
  // instead of clobbering the first draw's transform. The frame counter is
  // per-renderer, so this same-frame gate only applies when the slot is
  // already resident ON THIS device; a matching frame index carried over
  // from a DIFFERENT device that previously rendered this document is not a
  // same-frame repeat and must re-upload (submitResidentFillDraw re-uploads
  // on the device-id mismatch) rather than fall back.
  if (slot.owningDeviceId == impl_->device->deviceId() && slot.lastResidentFrame == frameId) {
    submitFillDraw(args);
    return;
  }
  slot.lastResidentFrame = frameId;
  impl_->submitResidentFillDraw(slot, encoded, args);
}

void GeoEncoder::fillPathInstanced(const EncodedPath& encoded, const css::RGBA& color,
                                   FillRule rule, std::span<const float> instanceTransforms) {
  if (encoded.empty() || instanceTransforms.empty()) {
    return;
  }
  // Caller packs 8 floats per instance - two vec4f rows of a row-major
  // affine. Malformed inputs drop silently rather than tripping a
  // validation error mid-frame.
  const size_t totalFloats = instanceTransforms.size();
  const uint32_t instanceCount = static_cast<uint32_t>(totalFloats / 8u);
  if (instanceCount == 0u || totalFloats != static_cast<size_t>(instanceCount) * 8u) {
    return;
  }

  // Upload packed transforms into the dedicated instance arena.
  const uint64_t itSize = roundUp4(totalFloats * sizeof(float));
  const auto itAlloc = impl_->allocInArena(impl_->instanceTransformArena, instanceTransforms.data(),
                                           itSize, kStorageOffsetAlignment);

  FillDrawArgs args = {};
  // `args.path` stays null - with a precomputed encode the submit path
  // never re-encodes, so the raw Path isn't needed. Gradient/pattern
  // variants aren't batchable in this PR; pure solid only.
  args.path = nullptr;
  args.rule = rule;
  args.precomputedEncoded = &encoded;
  args.paintMode = 0u;
  const float alpha = color.a / 255.0f;
  args.solidColor[0] = (color.r / 255.0f) * alpha;
  args.solidColor[1] = (color.g / 255.0f) * alpha;
  args.solidColor[2] = (color.b / 255.0f) * alpha;
  args.solidColor[3] = alpha;
  args.patternOpacity = 1.0f;
  args.patternView = impl_->device->dummyPatternTextureView();
  args.patternSampler = impl_->device->dummyPatternSampler();
  args.tileSize = Vector2d(1.0, 1.0);
  args.patternFromPath = Transform2d();

  args.instanceTransformsBuffer = itAlloc.buffer;
  args.instanceTransformsOffset = itAlloc.offset;
  args.instanceTransformsSize = itAlloc.size;
  args.instanceCount = instanceCount;

  submitFillDraw(args);
}

void GeoEncoder::fillPathPattern(const Path& path, FillRule rule, const PatternPaint& paint,
                                 const EncodedPath* precomputedEncoded) {
  if (!paint.tile || paint.tileSize.x <= 0.0 || paint.tileSize.y <= 0.0) {
    return;
  }

  // Build a sampler for the tile. We use linear filtering with Repeat
  // wrap mode; the shader also performs explicit modulo-style wrapping
  // via `fract()` so texture sampling never steps outside [0,1] UVs, but
  // Repeat is still the right conceptual wrap mode for any implicit
  // derivative / mip work WebGPU might do on the sampler.
  wgpu::SamplerDescriptor sd{wgpu::Default};
  sd.label = wgpuLabel("GeoEncoderPatternSampler");
  sd.addressModeU = wgpu::AddressMode::Repeat;
  sd.addressModeV = wgpu::AddressMode::Repeat;
  sd.minFilter = wgpu::FilterMode::Linear;
  sd.magFilter = wgpu::FilterMode::Linear;
  sd.maxAnisotropy = 1;
  wgpu::Sampler sampler =
      impl_->transientResources.retain(impl_->device->device().createSampler(sd));

  FillDrawArgs args = {};
  args.path = &path;
  args.rule = rule;
  args.precomputedEncoded = precomputedEncoded;
  args.paintMode = 1u;
  args.patternView = impl_->transientResources.retain(paint.tile.createView());
  args.patternSampler = sampler;
  args.patternFromPath = paint.patternFromPath;
  args.tileSize = paint.tileSize;
  args.patternOpacity = static_cast<float>(paint.opacity);

  submitFillDraw(args);
}

void GeoEncoder::submitFillDraw(const FillDrawArgs& args) {
  // Dummy resources are pre-created in the encoder constructor; no
  // per-draw ensure call is needed.
  impl_->ensurePassOpen();
  // `bindSolidPipeline` is the single source of truth for the current
  // pipeline - it issues `setPipeline` only when the tracker reports a
  // change (from image / gradient / None). Calling `setPipeline`
  // unconditionally here used to defeat the tracker on every solid
  // draw; see design doc 0030, Tier 4.
  impl_->bindSolidPipeline();

  // 1. CPU encode the path into Slug band data - unless the caller
  // already supplied a precomputed encode via the M2 cache
  // (`GeodePathCacheComponent`). Cache hits skip both the encode
  // work and the `pathEncodes` counter bump.
  EncodedPath ownedEncoded;
  const EncodedPath* encodedPtr = args.precomputedEncoded;
  if (!encodedPtr) {
    impl_->device->countPathEncode();
    ownedEncoded = GeodePathEncoder::encode(*args.path, args.rule);
    encodedPtr = &ownedEncoded;
  }
  const EncodedPath& encoded = *encodedPtr;
  if (encoded.empty()) {
    return;  // Nothing to draw.
  }

  const wgpu::Device& dev = impl_->device->device();

  // 2. Allocate and upload GPU buffers via the per-encoder arenas. The
  // analytic dual-ray fill (0041 §8) draws the SINGLE bounding quad
  // (`quadVertices`) and binds two band/curve SSBO sets (horizontal +
  // vertical) plus the dense H/V band grids.
  const uint64_t vbSize = roundUp4(encoded.quadVertices.size() * sizeof(EncodedPath::Vertex));
  const auto vbAlloc = impl_->allocInArena(impl_->vertexArena, encoded.quadVertices.data(), vbSize,
                                           /*alignment=*/kVertexOffsetAlignment);

  const auto bandsAlloc = impl_->allocStorageOrDummy(
      impl_->bandArena, encoded.bands.data(), encoded.bands.size() * sizeof(EncodedPath::Band));
  const auto curvesAlloc = impl_->allocStorageOrDummy(impl_->curveArena, encoded.curves.data(),
                                                      encoded.curves.size() * 6u * sizeof(float));
  const auto vBandsAlloc = impl_->allocStorageOrDummy(
      impl_->vBandArena, encoded.vBands.data(), encoded.vBands.size() * sizeof(EncodedPath::Band));
  const auto vCurvesAlloc = impl_->allocStorageOrDummy(impl_->vCurveArena, encoded.vCurves.data(),
                                                       encoded.vCurves.size() * 6u * sizeof(float));
  const auto hGridAlloc = impl_->allocStorageOrDummy(impl_->hGridArena, encoded.hBandGrid.data(),
                                                     encoded.hBandGrid.size() * sizeof(uint32_t));
  const auto vGridAlloc = impl_->allocStorageOrDummy(impl_->vGridArena, encoded.vBandGrid.data(),
                                                     encoded.vBandGrid.size() * sizeof(uint32_t));

  // Uniform buffer - still per-draw today; Milestone 1.f lifts it into
  // a ring buffer with dynamic offsets. `populateFillUniform` is shared
  // with the resident fill path so both produce byte-identical uniforms.
  Uniforms u = {};
  impl_->populateFillUniform(u, encoded, args);

  const auto uniAlloc =
      impl_->allocInArena(impl_->uniformArena, &u, sizeof(Uniforms), kUniformOffsetAlignment);

  // 3. Bind group - twelve entries: uniforms, H bands SSBO, H curves SSBO,
  // pattern texture, pattern sampler, clip-mask texture, clip-mask sampler,
  // per-instance transforms SSBO, V bands SSBO, V curves SSBO, H band grid,
  // V band grid.
  wgpu::BindGroupEntry entries[12] = {};
  entries[0].binding = 0;
  entries[0].buffer = *uniAlloc.buffer;
  entries[0].offset = uniAlloc.offset;
  entries[0].size = uniAlloc.size;
  entries[1].binding = 1;
  entries[1].buffer = *bandsAlloc.buffer;
  entries[1].offset = bandsAlloc.offset;
  entries[1].size = bandsAlloc.size;
  entries[2].binding = 2;
  entries[2].buffer = *curvesAlloc.buffer;
  entries[2].offset = curvesAlloc.offset;
  entries[2].size = curvesAlloc.size;
  entries[3].binding = 3;
  entries[3].textureView = args.patternView;
  entries[4].binding = 4;
  entries[4].sampler = args.patternSampler;
  entries[5].binding = 5;
  entries[5].textureView = impl_->currentClipMaskView();
  entries[6].binding = 6;
  entries[6].sampler = impl_->device->dummyClipMaskSampler();
  entries[7].binding = 7;
  if (args.instanceTransformsBuffer != nullptr) {
    entries[7].buffer = *args.instanceTransformsBuffer;
    entries[7].offset = args.instanceTransformsOffset;
    entries[7].size = args.instanceTransformsSize;
  } else {
    // One-element identity buffer owned by GeodeDevice. Layout is two
    // vec4f rows = 32 bytes; kept in sync with the WGSL
    // `InstanceTransform` struct in `shaders/slug_fill.wgsl`.
    entries[7].buffer = impl_->device->identityInstanceTransformBuffer();
    entries[7].offset = 0;
    entries[7].size = 32u;
  }
  entries[8].binding = 8;
  entries[8].buffer = *vBandsAlloc.buffer;
  entries[8].offset = vBandsAlloc.offset;
  entries[8].size = vBandsAlloc.size;
  entries[9].binding = 9;
  entries[9].buffer = *vCurvesAlloc.buffer;
  entries[9].offset = vCurvesAlloc.offset;
  entries[9].size = vCurvesAlloc.size;
  entries[10].binding = 10;
  entries[10].buffer = *hGridAlloc.buffer;
  entries[10].offset = hGridAlloc.offset;
  entries[10].size = hGridAlloc.size;
  entries[11].binding = 11;
  entries[11].buffer = *vGridAlloc.buffer;
  entries[11].offset = vGridAlloc.offset;
  entries[11].size = vGridAlloc.size;

  wgpu::BindGroupDescriptor bgDesc = {};
  bgDesc.label = wgpuLabel("GeodeBindGroup");
  bgDesc.layout = impl_->pipeline->bindGroupLayout();
  bgDesc.entryCount = 12;
  bgDesc.entries = entries;
  wgpu::BindGroup bindGroup = impl_->transientResources.retain(dev.createBindGroup(bgDesc));
  impl_->device->countBindGroup();

  // 4. Record the draw call - one quad (6 vertices) per path.
  impl_->pass.get().setVertexBuffer(0, *vbAlloc.buffer, vbAlloc.offset, vbAlloc.size);
  impl_->pass.get().setBindGroup(0, bindGroup, 0, nullptr);
  impl_->pass.get().draw(static_cast<uint32_t>(encoded.quadVertices.size()), args.instanceCount, 0,
                         0);
  impl_->device->countDraw();
}

namespace {

/// Populate the non-kind-specific fields of a `GradientUniforms` struct from
/// the shared parameters (transform, spread mode, stops) passed to any
/// gradient-fill call. The caller fills in `gradientKind` and any
/// kind-specific fields (start/end for linear, center/focal/radii for
/// radial) afterwards.
template <typename StopT>
void populateSharedGradientUniforms(GradientUniforms& u, const Transform2d& gradientFromPath,
                                    uint32_t spreadMode, std::span<const StopT> stops,
                                    FillRule rule) {
  u.fillRule = (rule == FillRule::EvenOdd) ? 1u : 0u;
  u.spreadMode = spreadMode;

  // Pack `gradientFromPath` as two row-vectors:
  //   row0 = (a, c, e, 0) → gx = a*px + c*py + e
  //   row1 = (b, d, f, 0) → gy = b*px + d*py + f
  // (Transform2d::data is column-major 2x3: [a, b, c, d, e, f].)
  u.row0[0] = static_cast<float>(gradientFromPath.data[0]);
  u.row0[1] = static_cast<float>(gradientFromPath.data[2]);
  u.row0[2] = static_cast<float>(gradientFromPath.data[4]);
  u.row0[3] = 0.0f;
  u.row1[0] = static_cast<float>(gradientFromPath.data[1]);
  u.row1[1] = static_cast<float>(gradientFromPath.data[3]);
  u.row1[2] = static_cast<float>(gradientFromPath.data[5]);
  u.row1[3] = 0.0f;

  // Upload stops in STRAIGHT (unpremultiplied) alpha. The shader's
  // `sample_stops` linearly interpolates between these values and then the
  // fragment stage premultiplies at output before the premultiplied-alpha
  // blend pipeline composites onto the framebuffer. This matches tiny-skia /
  // Skia gradient behavior - e.g. `a-stop-opacity-001` transitions from
  // white to black@0.2 and expects straight-space interpolation, not
  // premultiplied-space mix. Clamp the stop count to the shader's hard cap;
  // overflow was warned about at the caller.
  const uint32_t stopCount =
      std::min<uint32_t>(kMaxGradientStops, static_cast<uint32_t>(stops.size()));
  u.stopCount = stopCount;
  for (uint32_t i = 0; i < stopCount; ++i) {
    const StopT& s = stops[i];
    u.stopColors[i * 4 + 0] = s.rgba[0];
    u.stopColors[i * 4 + 1] = s.rgba[1];
    u.stopColors[i * 4 + 2] = s.rgba[2];
    u.stopColors[i * 4 + 3] = s.rgba[3];
    // Packed 4-per-vec4: stop i lives in stopOffsets[i/4].(x|y|z|w).
    u.stopOffsets[i] = s.offset;
  }
}

}  // namespace

void GeoEncoder::fillPathLinearGradient(const Path& path, const LinearGradientParams& params,
                                        FillRule rule, const EncodedPath* precomputedEncoded) {
  if (params.stops.empty()) {
    return;
  }

  // Gradient bind group includes a clip-mask binding (Phase 3b); the
  // dummy that stands in when no clip is active is pre-created in the
  // encoder constructor.
  impl_->ensurePassOpen();
  impl_->bindGradientPipeline();

  // 1. CPU encode the path into Slug band data (same as fillPath) -
  // unless the M2 cache already has a precomputed encode.
  EncodedPath ownedEncoded;
  const EncodedPath* encodedPtr = precomputedEncoded;
  if (!encodedPtr) {
    impl_->device->countPathEncode();
    ownedEncoded = GeodePathEncoder::encode(path, rule);
    encodedPtr = &ownedEncoded;
  }
  const EncodedPath& encoded = *encodedPtr;
  if (encoded.empty()) {
    return;
  }

  // 3. Build gradient uniforms.
  GradientUniforms u = {};
  impl_->buildMvp(u.mvp);
  u.viewport[0] = static_cast<float>(impl_->targetWidth);
  u.viewport[1] = static_cast<float>(impl_->targetHeight);
  populateSharedGradientUniforms<LinearGradientParams::Stop>(u, params.gradientFromPath,
                                                             params.spreadMode, params.stops, rule);
  u.gradientKind = kGradientKindLinear;
  impl_->writeClipPolygonUniforms(u.hasClipPolygon, u.clipPolygonPlanes);
  u.hasClipMask = impl_->activeClipMaskView ? 1u : 0u;

  u.startGrad[0] = static_cast<float>(params.startGrad.x);
  u.startGrad[1] = static_cast<float>(params.startGrad.y);
  u.endGrad[0] = static_cast<float>(params.endGrad.x);
  u.endGrad[1] = static_cast<float>(params.endGrad.y);

  impl_->submitGradientDraw(u, encoded);
}

void GeoEncoder::fillPathRadialGradient(const Path& path, const RadialGradientParams& params,
                                        FillRule rule, const EncodedPath* precomputedEncoded) {
  if (params.stops.empty()) {
    return;
  }
  // Degenerate radius: nothing to draw meaningfully - match tiny-skia's
  // early return. A radius of zero collapses the quadratic, and the caller
  // should have dropped the draw anyway.
  if (params.radius <= 0.0) {
    return;
  }

  // Dummy texture for the clip-mask slot is pre-created in the encoder
  // constructor (see fillPathLinearGradient note).
  impl_->ensurePassOpen();
  impl_->bindGradientPipeline();

  EncodedPath ownedEncoded;
  const EncodedPath* encodedPtr = precomputedEncoded;
  if (!encodedPtr) {
    impl_->device->countPathEncode();
    ownedEncoded = GeodePathEncoder::encode(path, rule);
    encodedPtr = &ownedEncoded;
  }
  const EncodedPath& encoded = *encodedPtr;
  if (encoded.empty()) {
    return;
  }

  GradientUniforms u = {};
  impl_->buildMvp(u.mvp);
  u.viewport[0] = static_cast<float>(impl_->targetWidth);
  u.viewport[1] = static_cast<float>(impl_->targetHeight);
  populateSharedGradientUniforms<RadialGradientParams::Stop>(u, params.gradientFromPath,
                                                             params.spreadMode, params.stops, rule);
  u.gradientKind = kGradientKindRadial;
  impl_->writeClipPolygonUniforms(u.hasClipPolygon, u.clipPolygonPlanes);
  u.hasClipMask = impl_->activeClipMaskView ? 1u : 0u;

  u.radialCenter[0] = static_cast<float>(params.center.x);
  u.radialCenter[1] = static_cast<float>(params.center.y);
  u.radialFocal[0] = static_cast<float>(params.focalCenter.x);
  u.radialFocal[1] = static_cast<float>(params.focalCenter.y);
  u.radialRadius = static_cast<float>(params.radius);
  u.radialFocalRadius = static_cast<float>(params.focalRadius);

  impl_->submitGradientDraw(u, encoded);
}

void GeoEncoder::blitFullTarget(const wgpu::Texture& src, double opacity) {
  if (!src) {
    return;
  }
  impl_->ensurePassOpen();
  impl_->bindImagePipeline(impl_->imagePipeline->pipeline());

  // Identity MVP: map target-pixel coords (0..W, 0..H) directly to clip
  // space (-1..+1 with Y flipped). This is the same math
  // `Impl::buildMvp` does with `transform == identity`.
  const double sx = 2.0 / static_cast<double>(impl_->targetWidth);
  const double sy = -2.0 / static_cast<double>(impl_->targetHeight);
  float mvp[16] = {0};
  mvp[0] = static_cast<float>(sx);
  mvp[5] = static_cast<float>(sy);
  mvp[10] = 1.0f;
  mvp[12] = -1.0f;
  mvp[13] = 1.0f;
  mvp[15] = 1.0f;

  GeodeTextureEncoder::QuadParams qp;
  qp.destRect = Box2d(Vector2d(0.0, 0.0), Vector2d(static_cast<double>(impl_->targetWidth),
                                                   static_cast<double>(impl_->targetHeight)));
  qp.srcRect = Box2d({0.0, 0.0}, {1.0, 1.0});
  qp.opacity = opacity;
  qp.filter = GeodeTextureEncoder::Filter::Linear;
  // Layer textures are offscreen render targets produced by the Geode
  // premultiplied source-over pipeline, so their storage is already in
  // premultiplied-alpha form. The shader needs to skip its default
  // straight→premult conversion, otherwise nested `pushIsolatedLayer`
  // calls double-darken the RGB channel (e.g.
  // structure/use/opacity-inheritance where a use opacity=0.5 wraps a
  // rect opacity=0.5 and should composite to 0.25).
  qp.sourceIsPremultiplied = true;
  qp.clipMaskView = impl_->activeClipMaskView;

  GeodeTextureEncoder::drawTexturedQuad(*impl_->device, *impl_->imagePipeline, impl_->pass.get(),
                                        src, mvp, impl_->targetWidth, impl_->targetHeight, qp,
                                        impl_->transientResources);
}

void GeoEncoder::blitFullTargetMasked(const wgpu::Texture& content, const wgpu::Texture& mask,
                                      const std::optional<Box2d>& maskBounds) {
  if (!content || !mask) {
    return;
  }
  impl_->ensurePassOpen();
  impl_->bindImagePipeline(impl_->imagePipeline->pipeline());

  // Identity MVP for target-pixel → clip space, same as blitFullTarget.
  const double sx = 2.0 / static_cast<double>(impl_->targetWidth);
  const double sy = -2.0 / static_cast<double>(impl_->targetHeight);
  float mvp[16] = {0};
  mvp[0] = static_cast<float>(sx);
  mvp[5] = static_cast<float>(sy);
  mvp[10] = 1.0f;
  mvp[12] = -1.0f;
  mvp[13] = 1.0f;
  mvp[15] = 1.0f;

  GeodeTextureEncoder::QuadParams qp;
  qp.destRect = Box2d(Vector2d(0.0, 0.0), Vector2d(static_cast<double>(impl_->targetWidth),
                                                   static_cast<double>(impl_->targetHeight)));
  qp.srcRect = Box2d({0.0, 0.0}, {1.0, 1.0});
  qp.opacity = 1.0;
  qp.filter = GeodeTextureEncoder::Filter::Linear;
  // Both content and mask are offscreen render targets produced by
  // Geode's premultiplied source-over pipeline, so they're already
  // in premultiplied alpha.
  qp.sourceIsPremultiplied = true;
  qp.maskTexture = mask;
  qp.clipMaskView = impl_->activeClipMaskView;
  if (maskBounds.has_value()) {
    qp.applyMaskBounds = true;
    qp.maskBounds = *maskBounds;
  }

  GeodeTextureEncoder::drawTexturedQuad(*impl_->device, *impl_->imagePipeline, impl_->pass.get(),
                                        content, mvp, impl_->targetWidth, impl_->targetHeight, qp,
                                        impl_->transientResources);
}

void GeoEncoder::blitFullTargetBlended(const wgpu::Texture& layer, const wgpu::Texture& dstSnapshot,
                                       uint32_t blendMode, double opacity) {
  if (!layer || !dstSnapshot || blendMode == 0u) {
    return;
  }
  impl_->ensurePassOpen();
  impl_->bindImagePipeline(impl_->imagePipeline->pipeline());

  // Identity MVP for target-pixel → clip space, same as blitFullTarget.
  const double sx = 2.0 / static_cast<double>(impl_->targetWidth);
  const double sy = -2.0 / static_cast<double>(impl_->targetHeight);
  float mvp[16] = {0};
  mvp[0] = static_cast<float>(sx);
  mvp[5] = static_cast<float>(sy);
  mvp[10] = 1.0f;
  mvp[12] = -1.0f;
  mvp[13] = 1.0f;
  mvp[15] = 1.0f;

  GeodeTextureEncoder::QuadParams qp;
  qp.destRect = Box2d(Vector2d(0.0, 0.0), Vector2d(static_cast<double>(impl_->targetWidth),
                                                   static_cast<double>(impl_->targetHeight)));
  qp.srcRect = Box2d({0.0, 0.0}, {1.0, 1.0});
  // Per W3C Compositing 1, group opacity scales the SOURCE colour
  // BEFORE the blend formula: `Cs_effective = Cs * groupOpacity` and
  // `αs_effective = αs * groupOpacity`. The shader's leading
  // multiply `color = sampled * uniforms.opacity` does exactly this
  // to the premultiplied layer texel before calling
  // `composite_with_blend`, so forwarding `opacity` here is enough.
  qp.opacity = opacity;
  qp.filter = GeodeTextureEncoder::Filter::Linear;
  // Both layer and dst_snapshot are offscreen render targets already
  // stored in premultiplied alpha.
  qp.sourceIsPremultiplied = true;
  qp.blendMode = blendMode;
  qp.dstSnapshotTexture = dstSnapshot;
  qp.clipMaskView = impl_->activeClipMaskView;

  GeodeTextureEncoder::drawTexturedQuad(*impl_->device, *impl_->imagePipeline, impl_->pass.get(),
                                        layer, mvp, impl_->targetWidth, impl_->targetHeight, qp,
                                        impl_->transientResources);
}

void GeoEncoder::drawImage(const svg::ImageResource& image, const Box2d& destRect, double opacity,
                           bool pixelated) {
  if (image.data.empty() || image.width <= 0 || image.height <= 0) {
    return;
  }
  if (destRect.isEmpty()) {
    return;
  }
  // Size cap: refuse pathological images. 16384 × 16384 × 4 bytes = 1 GiB,
  // which is already past any sensible WebGPU device limit. The texture
  // creation itself enforces tighter limits on the device side, but a sanity
  // check here turns "invalid texture descriptor → uncaptured device error"
  // into a clean no-op for the renderer.
  constexpr int kMaxImageDim = 16384;
  if (image.width > kMaxImageDim || image.height > kMaxImageDim) {
    return;
  }
  const size_t expectedBytes =
      static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4u;
  if (image.data.size() < expectedBytes) {
    return;
  }

  impl_->ensurePassOpen();
  impl_->bindImagePipeline(impl_->imagePipeline->pipeline());

  // Upload the image to a sampled texture.
  wgpu::Texture texture = impl_->transientResources.retain(GeodeTextureEncoder::uploadRgba8Texture(
      *impl_->device, image.data.data(), static_cast<uint32_t>(image.width),
      static_cast<uint32_t>(image.height)));
  if (!texture) {
    return;
  }

  // Build the same MVP the Slug-fill path uses, so the image's local-space
  // destination rectangle lands in the correct spot after the current
  // transform.
  float mvp[16];
  impl_->buildMvp(mvp);

  GeodeTextureEncoder::QuadParams qp;
  qp.destRect = destRect;
  qp.srcRect = Box2d({0.0, 0.0}, {1.0, 1.0});
  qp.opacity = opacity;
  qp.filter =
      pixelated ? GeodeTextureEncoder::Filter::Nearest : GeodeTextureEncoder::Filter::Linear;
  qp.clipMaskView = impl_->activeClipMaskView;

  GeodeTextureEncoder::drawTexturedQuad(*impl_->device, *impl_->imagePipeline, impl_->pass.get(),
                                        texture, mvp, impl_->targetWidth, impl_->targetHeight, qp,
                                        impl_->transientResources);
}

void GeoEncoder::drawTexture(const wgpu::Texture& texture, const Box2d& destRect, double opacity,
                             bool pixelated) {
  if (!texture || destRect.isEmpty() || opacity <= 0.0) {
    return;
  }

  impl_->ensurePassOpen();
  impl_->bindImagePipeline(impl_->imagePipeline->pipeline());

  float mvp[16];
  impl_->buildMvp(mvp);

  GeodeTextureEncoder::QuadParams qp;
  qp.destRect = destRect;
  qp.srcRect = Box2d({0.0, 0.0}, {1.0, 1.0});
  qp.opacity = opacity;
  qp.filter =
      pixelated ? GeodeTextureEncoder::Filter::Nearest : GeodeTextureEncoder::Filter::Linear;
  // Renderer texture snapshots come from Geode render targets, which store
  // premultiplied RGBA. Sampling them as straight-alpha would multiply RGB by
  // alpha a second time and visibly darken translucent cached layers.
  qp.sourceIsPremultiplied = true;
  qp.clipMaskView = impl_->activeClipMaskView;

  GeodeTextureEncoder::drawTexturedQuad(*impl_->device, *impl_->imagePipeline, impl_->pass.get(),
                                        texture, mvp, impl_->targetWidth, impl_->targetHeight, qp,
                                        impl_->transientResources);
}

void GeoEncoder::finish() {
  if (impl_->passOpen) {
    impl_->pass.get().end();
    impl_->pass.reset();
    impl_->passOpen = false;
  } else if (impl_->hasExplicitClear) {
    // No draws but a clear was requested - open and immediately close a pass
    // so the clear actually happens.
    impl_->ensurePassOpen();
    impl_->pass.get().end();
    impl_->pass.reset();
    impl_->passOpen = false;
  }

  // In shared-CommandEncoder mode (design doc 0030 Milestone 3), the
  // caller owns the CommandEncoder and is responsible for finishing +
  // submitting it. We only ended the open pass above.
  if (!impl_->ownsCommandEncoder) {
    return;
  }

  {
    ScopedWgpuHandle<wgpu::CommandBuffer> cmdBuf(impl_->commandEncoder.finish());
    impl_->device->queue().submit(1, &cmdBuf.get());
    impl_->device->countSubmit();
  }
  impl_->ownedCommandEncoder.reset();
  impl_->commandEncoder = wgpu::CommandEncoder();
}

}  // namespace donner::geode
