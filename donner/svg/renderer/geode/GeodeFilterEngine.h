#pragma once
/// @file
/// GPU filter-graph executor for the Geode rendering backend.
///
/// Owns compute pipelines for filter primitives and executes a
/// \ref donner::svg::components::FilterGraph against a source-graphic
/// texture, returning the filtered output texture.
///
/// Implemented primitives: `feGaussianBlur`, `feOffset`, `feColorMatrix`,
/// `feFlood`, `feMerge`, `feComposite`, `feBlend`, `feMorphology`,
/// `feComponentTransfer`, `feConvolveMatrix`, `feTurbulence`,
/// `feDisplacementMap`, `feDiffuseLighting`, `feSpecularLighting`,
/// `feDropShadow`, `feImage`, `feTile`. The primitive visitor is exhaustive.

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string_view>
#include <webgpu/webgpu.hpp>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/shader/CompiledShader.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::svg::components {
class FilterExecutionBudget;
struct FilterGraph;
struct FilterNode;

namespace filter_primitive {
struct Offset;
struct ColorMatrix;
struct Flood;
struct Merge;
struct Composite;
struct Blend;
struct Morphology;
struct ComponentTransfer;
struct ConvolveMatrix;
struct Turbulence;
struct DisplacementMap;
struct DiffuseLighting;
struct SpecularLighting;
struct DropShadow;
struct Image;
struct Tile;
}  // namespace filter_primitive
}  // namespace donner::svg::components

namespace donner::geode {

class GeodeDevice;
struct FilterResourceArena;
/// Per-frame uniform scratch and pass bind-group cache state. Defined in
/// GeodeFilterEngine.cc; the engine owns one instance across frames.
struct FilterResourceCache;

/**
 * A compute pipeline built from a precompiled shader artifact, with the objects it is layered on.
 *
 * Every handle is null when any step of the build failed, which is what a dispatch checks before
 * recording: a pipeline that was never created must not be dispatched with. Binding slots come
 * from the artifact's reflected resource names, never from a host-side table.
 */
struct RuntimeComputeProgram {
  gpu::ShaderModule shaderModule;        //!< Module holding the compute entry point.
  gpu::BindGroupLayout bindGroupLayout;  //!< Layout of group 0.
  gpu::PipelineLayout pipelineLayout;    //!< Pipeline layout over that one group.
  gpu::ComputePipeline pipeline;         //!< The pipeline itself.
  gpu::WorkgroupSize workgroupSize;      //!< Dispatch dimensions declared by the entry point.
  std::array<uint32_t, 3> inputOutputParameterBindings = {0, 1, 2};
  //!< Reflected input texture, output texture, and parameter resource bindings.
  std::optional<uint32_t> transferTableBinding;  //!< Optional reflected lookup-table binding.
  std::array<uint32_t, 4> twoInputBindings = {0, 1, 2, 3};
  //!< Reflected source, backdrop, output and parameter bindings of a two-input program.
};

/**
 * Creates the compute program of one shader family from the projection \p runtime consumes.
 *
 * Both artifacts are required so that a caller cannot hand a device a projection it does not
 * consume: a native device selects \p nativeShader, every other device selects \p wgslShader,
 * and a build that links no native artifact passes null and fails closed at module creation.
 * The shader module descriptor and the group-zero binding layout are both derived from the
 * selected artifact, so the source a device compiles and the interface the host binds against
 * always come from the same compiled program. A family that does not expose exactly one
 * two-dimensional compute entry point yields a program with null handles, which a dispatch
 * refuses.
 *
 * @param runtime Device receiving the selected precompiled projection.
 * @param wgslShader Authored WGSL artifact of the family.
 * @param nativeShader Platform-native artifact of the family, or null when this build links none.
 * @param label Diagnostic program label.
 * @return The built program, or one with null handles when the build failed.
 */
RuntimeComputeProgram CreateReflectedProgram(gpu::Device& runtime,
                                             const gpu::shader::CompiledShaderView& wgslShader,
                                             const gpu::shader::CompiledShaderView* nativeShader,
                                             std::string_view label);

/**
 * Renderer-owned allocation boundary for filter textures.
 *
 * Filter work is recorded into the renderer's frame command encoder, so textures must remain
 * unavailable for reuse until that frame submits. Implementations provide exact-descriptor
 * pooling and defer releases to the frame boundary.
 */
class FilterTextureAllocator {
public:
  virtual ~FilterTextureAllocator() = default;

  /// Takes a filter intermediate matching \p desc from the renderer's pool, or an invalid
  /// texture when the renderer's budget refuses it.
  /// @param desc Descriptor the intermediate is allocated with.
  virtual gpu::Texture acquireFilterTexture(const gpu::TextureDescriptor& desc) = 0;

  /// Hands \p texture back for reuse once the frame it was recorded into has submitted.
  /// @param texture Texture to return; consumed.
  /// @param desc Descriptor \p texture was acquired with; must match, or the next acquire for
  ///   that descriptor misses its bucket.
  virtual void releaseFilterTextureAtFrameEnd(gpu::Texture texture,
                                              const gpu::TextureDescriptor& desc) = 0;

  /// Retains a texture whose commands may have been accepted but lack completion proof.
  ///
  /// The texture must not return to a reusable pool. The allocator keeps its backing alive until
  /// the owning device is torn down or another backend-specific completion proof exists.
  virtual void retainFailedFilterTexture(gpu::Texture texture,
                                         const gpu::TextureDescriptor& desc) = 0;
};

/**
 * A texture flowing between filter primitives: the runtime handle holding it and the descriptor
 * it was allocated with.
 *
 * Both pointers name storage the executing graph's arena owns for the whole execution, so a copy
 * of this value stays usable across further allocations. Two values name the same texture exactly
 * when they compare equal, because the arena gives every texture one handle: identity here is
 * what decides intermediate reuse and color-space caching.
 *
 * A default-constructed value is empty, which is how a refused allocation or a primitive that
 * could not record is reported to its caller.
 */
struct FilterTexture {
  const gpu::Texture* texture = nullptr;         //!< Arena-owned runtime handle.
  const gpu::TextureDescriptor* desc = nullptr;  //!< Descriptor \ref texture was allocated with.

  /// Returns true when this value names a texture.
  explicit operator bool() const { return texture != nullptr; }

  /// Equality operator. @param other Value to compare against.
  bool operator==(const FilterTexture& other) const = default;

  /// Width in texels. Requires a non-empty value.
  uint32_t width() const { return desc->size.width; }

  /// Height in texels. Requires a non-empty value.
  uint32_t height() const { return desc->size.height; }

  /// Texel format. Requires a non-empty value.
  gpu::TextureFormat format() const { return desc->format; }

  /// Allocated usage flags. Requires a non-empty value.
  gpu::TextureUsage usage() const { return desc->usage; }
};

/**
 * Outcome of running a filter graph, and the ownership that comes with it.
 *
 * The three kinds are distinct outcomes for the caller: a failure composites nothing, a declined
 * graph composites the caller's own unmodified source, and an output hands the caller a texture
 * it must release through the allocator it passed in.
 */
struct FilterExecutionResult {
  /// What the caller composites, and what it owns.
  enum class Kind : uint8_t {
    Failed,         //!< Nothing usable was produced; there is no result to composite.
    SourceGraphic,  //!< The graph was declined; the caller composites its own source unchanged.
    Output,         //!< \ref texture holds the result and the caller owns it.
  };

  Kind kind = Kind::Failed;     //!< Which outcome this is.
  gpu::Texture texture;         //!< Result texture; valid only for \ref Kind::Output.
  gpu::TextureDescriptor desc;  //!< Descriptor \ref texture must be released with.
};

/**
 * Streams \p kind for diagnostics.
 *
 * @param os Output stream. @param kind Value to print.
 */
inline std::ostream& operator<<(std::ostream& os, FilterExecutionResult::Kind kind) {
  switch (kind) {
    case FilterExecutionResult::Kind::Failed: return os << "Failed";
    case FilterExecutionResult::Kind::SourceGraphic: return os << "SourceGraphic";
    case FilterExecutionResult::Kind::Output: return os << "Output";
  }
  return os << "Kind(" << static_cast<int>(kind) << ")";
}

/// Exact-resolution tile layout with overlapping sampling halos.
struct FilterTilePlan {
  uint32_t width = 0;       //!< Full source/output width.
  uint32_t height = 0;      //!< Full source/output height.
  uint32_t tileWidth = 0;   //!< Fixed working width, including halos.
  uint32_t tileHeight = 0;  //!< Fixed working height, including halos.
  uint32_t coreWidth = 0;   //!< Non-overlapping output step.
  uint32_t coreHeight = 0;  //!< Non-overlapping output step.
  uint64_t tiles = 1;       //!< Number of complete graph executions.
  uint64_t pixels() const { return uint64_t{tileWidth} * tileHeight; }
  uint64_t workPixels() const { return pixels() * tiles; }
  uint64_t additionalTextureBytes() const {
    return tiles > 1 ? (uint64_t{width} * height + pixels()) * 4 : 0;
  }
};

/// GPU allocation bytes retained by an execution, excluding the caller's source/capture.
struct FilterExecutionMemory {
  uint64_t textures = 0;           //!< Texture descriptors, including dead reusable scratch.
  uint64_t persistentBuffers = 0;  //!< Parameter arenas and immutable transfer tables.

  uint64_t workUnits = 0;       //!< Work charged for the chosen execution plan.
  uint64_t tileExecutions = 0;  //!< Complete graph evaluations, including sampling halos.

  /// Total retained allocation bytes.
  uint64_t total() const { return textures + persistentBuffers; }
};

/**
 * GPU filter-graph executor.
 *
 * Given a `FilterGraph` and a source-graphic texture (the offscreen layer
 * snapshot captured between `pushFilterLayer` / `popFilterLayer`), executes
 * the graph's primitives on the GPU and returns the final output texture.
 *
 * Intermediate textures between primitives are allocated on demand and
 * keyed by `result` names.
 *
 * Currently supports:
 * - `feGaussianBlur` (two-pass separable Gaussian via compute shader)
 * - `feOffset` (pixel shift via compute shader)
 * - `feColorMatrix` (4x5 matrix transform via compute shader)
 * - `feFlood` (constant color fill via compute shader)
 * - `feMerge` (alpha-over composite of N inputs via compute shader)
 * - `feComposite` (Porter-Duff compositing of two inputs via compute shader)
 * - `feBlend` (W3C Compositing 1 blend modes via compute shader)
 * - `feMorphology` (erode / dilate via min / max rectangular kernel)
 * - `feComponentTransfer` (per-channel LUT transform via compute shader)
 * - `feConvolveMatrix` (NxM kernel convolution via compute shader)
 * - `feTurbulence` (Perlin noise / fractal noise via compute shader)
 * - `feDisplacementMap` (per-pixel channel-driven displacement via compute shader)
 * - `feDiffuseLighting` (Lambertian shading with distant/point/spot lights)
 * - `feSpecularLighting` (Phong shading with distant/point/spot lights)
 * - `feDropShadow` (blur alpha + offset + flood-tint + source-over)
 * - `feImage` (Mitchell-Netravali bicubic placement of external raster / in-document fragment)
 * - `feTile` (wraparound tiling of input subregion across filter region)
 *
 * Unsupported primitives pass the current buffer through unchanged.
 */
class GeodeFilterEngine {
public:
  /// @param device The Geode device (owns wgpu::Device + queue).
  /// @param verbose If true, emit one-shot warnings for unsupported primitives.
  explicit GeodeFilterEngine(GeodeDevice& device, bool verbose = false);

  ~GeodeFilterEngine();

  GeodeFilterEngine(const GeodeFilterEngine&) = delete;
  GeodeFilterEngine& operator=(const GeodeFilterEngine&) = delete;

  /**
   * Execute a filter graph against the source-graphic texture.
   *
   * The source texture must be sampled-capable. A \ref FilterExecutionResult::Kind::Output
   * result owns an RGBA8Unorm texture sized to the filter region (or the source dimensions if no
   * region is given), which the caller releases through @p textureAllocator once the frame
   * command buffer has submitted. Every intermediate the execution allocated is released back to
   * @p textureAllocator before this call returns.
   *
   * Filter compute, copy and clear commands are recorded into command-encoder chunks this
   * execution owns. Each chunk is replayed into the leased frame command encoder, so a graph
   * that stays below the per-command-buffer pass bound costs no queue submission of its own;
   * crossing that bound rotates the frame command encoder, which does reach the queue. The
   * caller restores its following frame encoder before compositing the result.
   *
   * @param graph The filter graph to execute.
   * @param sourceGraphic The input texture (layer snapshot), borrowed for the call.
   * @param sourceGraphicDesc Descriptor \p sourceGraphic was allocated with; its extent, format
   *   and usage decide the working resolution and whether tiling can copy from the source.
   * @param filterRegion The filter region in user-space coordinates.
   * @param deviceFromFilter The combined transform from filter/user-space to
   *   device-pixel coordinates, captured at `pushFilterLayer` time. Used to
   *   derive per-axis scale factors and to project directional parameters
   *   (e.g. feOffset dx/dy) through rotation/skew.
   * @param textureAllocator Renderer-owned filter texture pool boundary.
   * @param executionBudget Optional shared per-frame budget. Direct callers may omit it to apply
   *   only the graph-local limit.
   * @param admittedPlan Optional immutable plan already reserved by the caller. Execution keeps
   *   this layout even if shared scratch state or planning preferences change. Invalid plans or
   *   failed execution-time budgets fail the execution instead of bypassing the filter.
   * @param hostLease Lease of the caller's frame command encoder, or `std::nullopt` when the
   *   execution submits to the queue on its own. When given, it must be the lease currently
   *   installed on the device, and the caller must have submitted its source rendering before
   *   the call; a stale or foreign lease fails the execution before anything is replayed.
   * @return The outcome of the execution; see \ref FilterExecutionResult.
   */
  FilterExecutionResult execute(
      const svg::components::FilterGraph& graph, const gpu::Texture& sourceGraphic,
      const gpu::TextureDescriptor& sourceGraphicDesc, const Box2d& filterRegion,
      const Transform2d& deviceFromFilter, FilterTextureAllocator& textureAllocator,
      svg::components::FilterExecutionBudget* executionBudget = nullptr,
      std::optional<FilterTilePlan> admittedPlan = std::nullopt,
      std::optional<GeodeWgpuAdapterDevice::HostEncoderLease> hostLease = std::nullopt);

  /**
   * Begin a new frame for this engine by resetting the per-frame uniform scratch cursor and the
   * count of filter passes in the host command buffer the frame records through.
   *
   * The renderer calls this once per frame from its own beginFrame, BEFORE
   * the filter texture pool runs its stale-bucket eviction, and before any
   * filter pass of the frame records. Uniform slots written after this call
   * reuse stable (buffer, offset) pairs across frames. The cursor itself is
   * mutex-guarded, but callers must still serialize one frame per device at
   * a time: two sibling renderers drawing concurrently on the same device
   * would alias uniform slots and rewind the cursor under recorded passes
   * (the renderer's architecture already serializes the render worker per
   * device).
   */
  void beginFrame();

  /// Current parameter/table allocation bytes, including buffers awaiting frame completion.
  uint64_t retainedBufferBytes() const;

  /// Plans bounded working textures without changing sample coordinates or resolution.
  /// @param graph Graph to execute. @param width Source width. @param height Source height.
  /// @param deviceFromFilter Original filter-to-device transform.
  FilterTilePlan executionPlan(const svg::components::FilterGraph& graph, uint32_t width,
                               uint32_t height, const Transform2d& deviceFromFilter) const;

  /// Lower the working extent for deterministic tile-boundary tests.
  /// @param extent Maximum dimension, between 16 and 512 pixels.
  void setMaximumTileExtentForTesting(uint32_t extent);

  /// Invokes p hook after each accepted command chunk. Test seam for later-chunk failures.
  void setChunkSubmittedHookForTesting(std::function<void(size_t)> hook);

  /// Records exactly p passCount transparent-clear passes through the chunking state machine.
  bool recordPassesForTesting(
      size_t passCount, FilterTextureAllocator& textureAllocator,
      std::optional<GeodeWgpuAdapterDevice::HostEncoderLease> hostLease = std::nullopt);

  /// Observed allocation footprint of the most recent execution; does not own resources.
  FilterExecutionMemory lastExecutionMemory() const { return lastExecutionMemory_; }

private:
  friend struct FilterResourceArena;
  friend struct FilterGraphExecution;
  friend struct FilterNodeExecution;

  /// Two-pass separable Gaussian blur via compute shader.
  /// @param input The input texture.
  /// @param stdDeviationX Standard deviation in X (pixels).
  /// @param stdDeviationY Standard deviation in Y (pixels).
  /// @param edgeMode Edge handling mode (0=None, 1=Duplicate, 2=Wrap).
  /// @return The blurred texture.
  FilterTexture applyGaussianBlur(FilterResourceArena& arena, FilterTexture input,
                                  double stdDeviationX, double stdDeviationY, uint32_t edgeMode,
                                  const Box2d* outputClip = nullptr);

  /// Run a single blur pass (horizontal or vertical).
  /// @param input Source texture for this pass.
  /// @param width Texture width.
  /// @param height Texture height.
  /// @param stdDeviation Standard deviation for this axis.
  /// @param axis 0 = horizontal, 1 = vertical.
  /// @param edgeMode Edge handling mode.
  /// @return Output texture for this pass.
  FilterTexture runBlurPass(FilterResourceArena& arena, FilterTexture input, FilterTexture output,
                            uint32_t width, uint32_t height, float stdDeviation, uint32_t axis,
                            uint32_t edgeMode, const Box2d* clip = nullptr);

  /// One pass of a 3-pass box blur (used to approximate a Gaussian for sigma
  /// >= 2.0, matching tiny-skia's behaviour).
  /// @param input The input texture.
  /// @param width Texture width.
  /// @param height Texture height.
  /// @param boxLeft Number of samples on the negative side of the centre tap.
  /// @param boxRight Number of samples on the positive side of the centre tap.
  /// @param axis 0 = horizontal, 1 = vertical.
  /// @param edgeMode Edge handling mode.
  /// @return Output texture for this pass.
  FilterTexture runBoxBlurPass(FilterResourceArena& arena, FilterTexture input,
                               FilterTexture output, uint32_t width, uint32_t height,
                               int32_t boxLeft, int32_t boxRight, uint32_t axis, uint32_t edgeMode,
                               const Box2d* clip = nullptr);

  /// Shift pixels by (dx, dy) via compute shader.
  /// @param input The input texture.
  /// @param primitive The feOffset parameters.
  /// @return The offset texture.
  FilterTexture applyOffset(FilterResourceArena& arena, FilterTexture input,
                            const svg::components::filter_primitive::Offset& primitive);

  /// Apply a 4x5 color matrix to each pixel via compute shader.
  /// @param input The input texture.
  /// @param primitive The feColorMatrix parameters.
  /// @return The transformed texture.
  FilterTexture applyColorMatrix(FilterResourceArena& arena, FilterTexture input,
                                 const svg::components::filter_primitive::ColorMatrix& primitive);

  /// Extract SourceAlpha (0,0,0,A) from a SourceGraphic texture.
  /// @param input The source-graphic texture.
  /// @return A texture whose RGB are zero and alpha matches the input alpha.
  FilterTexture applySourceAlpha(FilterResourceArena& arena, FilterTexture input);

  /// Fill the output with a constant flood color via compute shader.
  /// @param width Output texture width.
  /// @param height Output texture height.
  /// @param primitive The feFlood parameters.
  /// @return The flood-filled texture.
  FilterTexture applyFlood(FilterResourceArena& arena, uint32_t width, uint32_t height,
                           const svg::components::filter_primitive::Flood& primitive);

  /// Alpha-over composite of N input textures via sequential compute dispatches.
  /// @param node The feMerge filter node (inputs resolve to merge children).
  /// @param namedBuffers Named intermediate textures.
  /// @param currentBuffer The "previous" output buffer.
  /// @param sourceGraphic The original source-graphic texture.
  /// @param linearRGB If true, composite in linearRGB: convert each input
  ///   sRGB→linear before the alpha-over passes and convert the result
  ///   linear→sRGB (matches tiny-skia's `color-interpolation-filters` handling).
  /// @return The composited texture.
  FilterTexture applyMerge(FilterResourceArena& arena, const svg::components::FilterNode& node,
                           const std::unordered_map<std::string, FilterTexture>& namedBuffers,
                           FilterTexture currentBuffer, FilterTexture sourceGraphic,
                           const FilterTexture* sourceAlpha, bool linearRGB);

  /// Run a single alpha-over composite pass (src over dst → output).
  /// @param src Source texture.
  /// @param dst Destination texture.
  /// @param width Output texture width.
  /// @param height Output texture height.
  /// @return The composited texture.
  FilterTexture runMergePass(FilterResourceArena& arena, FilterTexture src, FilterTexture dst,
                             uint32_t width, uint32_t height);

  /// Porter-Duff compositing of two inputs via compute shader.
  /// @param in1 First input texture (source).
  /// @param in2 Second input texture (destination/backdrop).
  /// @param primitive The feComposite parameters (operator + k1..k4).
  /// @return The composited texture.
  FilterTexture applyComposite(FilterResourceArena& arena, FilterTexture in1, FilterTexture in2,
                               const svg::components::filter_primitive::Composite& primitive);

  /// W3C Compositing 1 blend of two inputs via compute shader.
  /// @param in1 First input texture (source).
  /// @param in2 Second input texture (backdrop).
  /// @param primitive The feBlend parameters (blend mode).
  /// @return The blended texture.
  FilterTexture applyBlend(FilterResourceArena& arena, FilterTexture in1, FilterTexture in2,
                           const svg::components::filter_primitive::Blend& primitive);

  /// Morphological erode / dilate via min / max rectangular kernel.
  /// @param input The input texture.
  /// @param primitive The feMorphology parameters (operator, radiusX, radiusY).
  /// @param pixelRadiusX Horizontal radius in pixels.
  /// @param pixelRadiusY Vertical radius in pixels.
  /// @return The morphed texture.
  FilterTexture applyMorphology(FilterResourceArena& arena, FilterTexture input,
                                const svg::components::filter_primitive::Morphology& primitive,
                                int pixelRadiusX, int pixelRadiusY);

  /// Per-channel LUT transform (feComponentTransfer).
  /// @param input The input texture.
  /// @param primitive The feComponentTransfer parameters (4 channel functions).
  /// @return The transformed texture.
  FilterTexture applyComponentTransfer(
      FilterResourceArena& arena, FilterTexture input,
      const svg::components::filter_primitive::ComponentTransfer& primitive);

  /// NxM kernel convolution (feConvolveMatrix).
  /// @param input The input texture.
  /// @param primitive The feConvolveMatrix parameters.
  /// @return The convolved texture.
  FilterTexture applyConvolveMatrix(
      FilterResourceArena& arena, FilterTexture input,
      const svg::components::filter_primitive::ConvolveMatrix& primitive);

  /// Perlin noise / fractal noise generator (feTurbulence).
  /// @param width Output texture width.
  /// @param height Output texture height.
  /// @param primitive The feTurbulence parameters.
  /// @param deviceFromFilter Transform from filter/user space into device space.
  /// @return The noise texture.
  FilterTexture applyTurbulence(FilterResourceArena& arena, uint32_t width, uint32_t height,
                                const svg::components::filter_primitive::Turbulence& primitive,
                                const Transform2d& deviceFromFilter);

  /// Per-pixel channel-driven displacement (feDisplacementMap).
  /// @param in1 Source image texture.
  /// @param in2 Displacement map texture.
  /// @param primitive The feDisplacementMap parameters.
  /// @param pixelScale Displacement scale in pixels.
  /// @return The displaced texture.
  FilterTexture applyDisplacementMap(
      FilterResourceArena& arena, FilterTexture in1, FilterTexture in2,
      const svg::components::filter_primitive::DisplacementMap& primitive, double pixelScale);

  /// Lambertian diffuse lighting (feDiffuseLighting).
  /// @param input The input texture (alpha = height map).
  /// @param primitive The feDiffuseLighting parameters.
  /// @param scaleX User-to-pixel scale X.
  /// @param scaleY User-to-pixel scale Y.
  /// @param linearRGB If true, convert the lighting color sRGB→linear and the
  ///   output linear→sRGB (matches tiny-skia's linearRGB color-interpolation).
  /// @return The lit texture.
  FilterTexture applyDiffuseLighting(
      FilterResourceArena& arena, FilterTexture input,
      const svg::components::filter_primitive::DiffuseLighting& primitive,
      const svg::components::FilterGraph& graph, const Transform2d& deviceFromFilter,
      const Box2d& sampleSubregion, bool linearRGB);

  /// Phong specular lighting (feSpecularLighting).
  /// @param input The input texture (alpha = height map).
  /// @param primitive The feSpecularLighting parameters.
  /// @param scaleX User-to-pixel scale X.
  /// @param scaleY User-to-pixel scale Y.
  /// @param linearRGB If true, convert the lighting color sRGB→linear and the
  ///   output linear→sRGB (matches tiny-skia's linearRGB color-interpolation).
  /// @return The lit texture.
  FilterTexture applySpecularLighting(
      FilterResourceArena& arena, FilterTexture input,
      const svg::components::filter_primitive::SpecularLighting& primitive,
      const svg::components::FilterGraph& graph, const Transform2d& deviceFromFilter,
      const Box2d& sampleSubregion, bool linearRGB);

  /// Drop-shadow composite (feDropShadow): blur alpha, offset, flood, source-over.
  /// @param input The input texture.
  /// @param primitive The feDropShadow parameters.
  /// @param pixelStdDevX Blur standard deviation in pixel units (X).
  /// @param pixelStdDevY Blur standard deviation in pixel units (Y).
  /// @param pixelDx Offset in pixel units (X).
  /// @param pixelDy Offset in pixel units (Y).
  /// @return The drop-shadowed texture.
  FilterTexture applyDropShadow(FilterResourceArena& arena, FilterTexture input,
                                const svg::components::filter_primitive::DropShadow& primitive,
                                double pixelStdDevX, double pixelStdDevY, double pixelDx,
                                double pixelDy);

  /// Blit an external image into a freshly-allocated filter-sized texture (feImage).
  /// @param primitive The feImage parameters (imageData, preserveAspectRatio, fragmentId, ...).
  /// @param width Output texture width (filter-region pixels).
  /// @param height Output texture height (filter-region pixels).
  /// @param graph The enclosing filter graph (for bbox / scale / subregion resolution).
  /// @param node The enclosing filter node (for primitive subregion overrides).
  /// @param deviceFromFilter The combined CTM from filter/user-space to device pixels.
  ///   Used by fragment references to project through rotation/skew.
  /// @param placementRegionUser The feImage placement rectangle in user space,
  ///   already resolved for percent/OBB units and with absent x/y/width/height
  ///   defaulted to the filter region. The image is fit into this rect per
  ///   preserveAspectRatio.
  /// @return The placed-image texture.
  FilterTexture applyImage(FilterResourceArena& arena,
                           const svg::components::filter_primitive::Image& primitive,
                           uint32_t width, uint32_t height,
                           const svg::components::FilterGraph& graph,
                           const svg::components::FilterNode& node,
                           const Transform2d& deviceFromFilter, const Box2d& placementRegionUser);

  /// Fills an image primitive's output from a transparent sample, or returns empty on refusal.
  /// @param arena Frame resources. @param output Existing image destination.
  /// @param destinationExtent Dimensions to fill.
  FilterTexture renderTransparentImage(FilterResourceArena& arena, FilterTexture output,
                                       gpu::Extent2d destinationExtent);

  /// Wraparound tile of an input subregion across the full output (feTile).
  /// @param input The input texture.
  /// @param srcX Source rectangle X origin in pixels.
  /// @param srcY Source rectangle Y origin in pixels.
  /// @param srcW Source rectangle width in pixels.
  /// @param srcH Source rectangle height in pixels.
  /// @return The tiled texture.
  FilterTexture applyTile(FilterResourceArena& arena, FilterTexture input, int32_t srcX,
                          int32_t srcY, int32_t srcW, int32_t srcH);

  /// Clip a primitive's output to its user-space subregion via the inverse CTM.
  /// Pixels whose center maps outside the subregion are zeroed.
  /// @param input The primitive's output texture.
  /// @param filterFromDevice Inverse of the deviceFromFilter transform.
  /// @param usrX0 User-space subregion left edge.
  /// @param usrY0 User-space subregion top edge.
  /// @param usrX1 User-space subregion right edge.
  /// @param usrY1 User-space subregion bottom edge.
  /// @param resolve True for the final RGBA8 clip and half-up quantization.
  /// @return A new texture with out-of-subregion pixels cleared.
  FilterTexture applySubregionClip(FilterResourceArena& arena, FilterTexture input,
                                   const Transform2d& filterFromDevice, double usrX0, double usrY0,
                                   double usrX1, double usrY1, bool resolve = false);

  /// Convert a texture between sRGB and linearRGB color spaces.
  /// Used to implement `color-interpolation-filters: linearRGB` (the SVG default).
  /// @param input The input texture in premultiplied sRGB (or linear, for the reverse).
  /// @param srgbToLinear True to convert sRGB→linear, false for linear→sRGB.
  /// @return A new texture in the target color space.
  FilterTexture applyColorSpaceConversion(FilterResourceArena& arena, FilterTexture input,
                                          bool srgbToLinear);

  GeodeDevice& device_;

  // Gaussian blur pipeline.
  RuntimeComputeProgram blurProgram_;

  // feOffset pipeline, recorded through the GPU runtime.
  RuntimeComputeProgram offsetProgram_;

  // feColorMatrix pipeline, recorded through the GPU runtime.
  RuntimeComputeProgram colorMatrixProgram_;

  // feFlood pipeline, recorded through the GPU runtime.
  RuntimeComputeProgram floodProgram_;

  /// Source-over merge pipeline recorded through the GPU runtime.
  RuntimeComputeProgram mergeProgram_;

  /// Porter-Duff and arithmetic pipeline recorded through the GPU runtime.
  RuntimeComputeProgram compositeProgram_;

  // feBlend W3C blend-mode pipeline (two inputs + output + uniform).
  RuntimeComputeProgram blendProgram_;

  // feMorphology erode/dilate pipeline (input + output + uniform).
  RuntimeComputeProgram morphologyProgram_;

  // feComponentTransfer LUT pipeline (input + output + storage buffer).
  RuntimeComputeProgram componentTransferProgram_;

  /// Matrix-convolution pipeline recorded through the GPU runtime.
  RuntimeComputeProgram convolveMatrixProgram_;

  // feTurbulence noise pipeline (output + parameter and table storage buffers).
  RuntimeComputeProgram turbulenceProgram_;

  // feDisplacementMap pipeline (two inputs + output + uniform).
  RuntimeComputeProgram displacementMapProgram_;

  // feDiffuseLighting pipeline (input + output + storage buffer).
  RuntimeComputeProgram diffuseLightingProgram_;

  // feSpecularLighting pipeline (input + output + storage buffer).
  RuntimeComputeProgram specularLightingProgram_;

  // feDropShadow compose pipeline (two inputs + output + uniform).
  RuntimeComputeProgram dropShadowProgram_;

  // feImage placement pipeline (input texture + output + uniform).
  RuntimeComputeProgram imageProgram_;

  // feTile wraparound pipeline (input + output + uniform).
  RuntimeComputeProgram tileProgram_;

  // Per-primitive subregion clipping pipeline, recorded through the GPU runtime.
  RuntimeComputeProgram subregionClipProgram_;
  RuntimeComputeProgram filterResolveProgram_;

  // sRGB to linear color space conversion pipeline, recorded through the GPU runtime.
  RuntimeComputeProgram colorSpaceConvertProgram_;
  gpu::Buffer colorTransferTable_;

  bool verbose_ = false;

  /// Per-frame parameter scratch buffer and bump-allocated slot cursor (see
  /// FilterResourceCache). Pass bind groups are still created per pass:
  /// the pooled textures a pass binds rotate across frames, so their
  /// identities are not stable cache keys.
  std::unique_ptr<FilterResourceCache> resourceCache_;
  FilterExecutionMemory lastExecutionMemory_;
  uint32_t preferredTileExtent_ = 512;
  bool adaptiveTiles_ = true;

  /// Filter passes already replayed into one host command buffer, and the lease naming it.
  struct HostCommandBufferPasses {
    /// Host command buffer \ref passes describes, or `std::nullopt` before one is leased.
    std::optional<GeodeWgpuAdapterDevice::HostEncoderLease> lease;
    size_t passes = 0;  //!< Filter passes replayed into that buffer.
  };

  /// Filter passes the frame has replayed into the host command buffer it is recording through.
  /// An execution's final partial chunk is replayed into that buffer rather than queue-submitted,
  /// so the bound on one command buffer has to count every execution of the frame: a document
  /// with many small filter graphs would otherwise fill it without any single graph reaching the
  /// bound. Cleared by \ref beginFrame, by the rotation that puts the buffer on the queue, and
  /// whenever the runtime confirms a different buffer is leased. One count serves the device, so
  /// two renderers must not interleave executions of different frames on it; \ref beginFrame
  /// already requires callers to serialize one frame per device.
  HostCommandBufferPasses hostCommandBufferPasses_;
  std::function<void(size_t)> chunkSubmittedHookForTesting_;
};

}  // namespace donner::geode
