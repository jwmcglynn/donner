#pragma once
/// @file
/// Desktop geode Canvas-like shim interface for Dawn-backed runners.

#include <cstdint>
#include <string>
#include <vector>

#include <webgpu/webgpu.h>

namespace donner::geode {

struct GeodeCanvasOptions {
  /// Width of the render target in pixels.
  uint32_t width = 0;
  /// Height of the render target in pixels.
  uint32_t height = 0;
  /// Whether the render target is offscreen; if false, a swapchain-backed surface is expected.
  bool offscreen = true;
};

struct Point {
  float x = 0.0f;
  float y = 0.0f;
};

enum class PathCommandKind {
  kMoveTo,
  kLineTo,
  kQuadraticTo,
  kClosePath,
};

struct PathCommand {
  PathCommandKind kind = PathCommandKind::kMoveTo;
  Point p0{};
  Point p1{};
  Point p2{};
};

struct CanvasState {
  float strokeWidth = 1.0f;
  bool fillEnabled = true;
  bool strokeEnabled = false;
};

/// GPU-ready geode segment encoding for a single curve fragment.
struct GeodeSegment {
  PathCommandKind kind = PathCommandKind::kLineTo;
  Point p0{};
  Point p1{};
  Point p2{};
};

/// Encoded draw intent containing segments, bounds, and state for GPU upload.
struct EncodedDraw {
  std::vector<GeodeSegment> segments;
  CanvasState state{};
  Point boundsMin{};
  Point boundsMax{};
  bool isFill = true;
};

/// GPU upload payload combining packed geode segments and per-draw uniforms.
struct GpuUpload {
  struct DrawUniforms {
    float boundsMin[2] = {0.0f, 0.0f};
    float boundsMax[2] = {0.0f, 0.0f};
    float viewport[2] = {0.0f, 0.0f};
    float strokeWidth = 1.0f;
    uint32_t segmentOffset = 0;
    uint32_t segmentCount = 0;
    uint32_t isFill = 1;
  };

  std::vector<uint8_t> geodeBuffer;
  std::vector<DrawUniforms> drawUniforms;
};

/// Precomputed bindings and draw parameters for Dawn submission.
struct DawnRenderPlan {
  /// Buffer payloads for upload to Dawn buffers.
  struct Buffers {
    /// Packed geode segments; bind as storage at group 0 binding 0.
    std::vector<uint8_t> segments;
    /// Per-draw uniforms packed with alignment for dynamic offsets at group 0 binding 1.
    std::vector<uint8_t> uniforms;
  };

  /// Per-draw metadata consumed by the render loop.
  struct DrawCall {
    /// Byte offset into the uniform buffer for this draw (aligned for dynamic offsets).
    uint32_t uniformOffset = 0;
    /// Offset of the first segment for this draw within the segments buffer.
    uint32_t segmentOffset = 0;
    /// Number of segments referenced by this draw.
    uint32_t segmentCount = 0;
  };

  Buffers buffers;
  std::vector<DrawCall> draws;
  /// Render target width/height for viewport configuration.
  uint32_t width = 0;
  uint32_t height = 0;
  /// Whether the render target is offscreen (true) or swapchain-backed (false).
  bool offscreen = true;
};

/// Buffer upload requirements and render target metadata for Dawn submission.
struct DawnSubmission {
  /// Buffer bindings to allocate and upload prior to issuing draws.
  struct BufferUpload {
    enum class Binding {
      /// Storage buffer for geode segments.
      kSegments,
      /// Uniform buffer for per-draw parameters.
      kUniforms,
    };

    /// Binding slot this buffer satisfies.
    Binding binding = Binding::kSegments;
    /// Buffer size in bytes.
    uint64_t size = 0;
    /// Usage flags for the buffer (maps to WGPUBufferUsage bits when creating buffers).
    uint64_t usage = 0;
  };

  /// Render target surface parameters.
  struct Surface {
    /// Texture view for rendering; optional for offscreen paths.
    void* textureView = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool offscreen = true;
  };

  DawnRenderPlan renderPlan;
  std::vector<BufferUpload> buffers;
  Surface surface;
};

/// Live Dawn GPU objects allocated from a submission package.
struct DawnSubmissionResources {
  /// Storage buffer for packed geode segments.
  WGPUBuffer segments = nullptr;
  /// Uniform buffer for per-draw parameters with dynamic offsets.
  WGPUBuffer uniforms = nullptr;
  /// Color target; owned when offscreen, null for swapchain-backed submissions.
  WGPUTexture colorTexture = nullptr;
  /// Render target view for the current frame.
  WGPUTextureView colorTextureView = nullptr;
  /// Whether `colorTexture` should be released by the caller.
  bool ownsColorTexture = false;
};

/// Dawn pipeline objects for rendering geode draws.
struct DawnGeodePipeline {
  /// WGSL shader module containing both vertex and fragment entry points.
  WGPUShaderModule shaderModule = nullptr;
  /// Bind group layout that matches the shader resource bindings.
  WGPUBindGroupLayout bindGroupLayout = nullptr;
  /// Pipeline layout describing bind groups.
  WGPUPipelineLayout pipelineLayout = nullptr;
  /// Render pipeline for drawing encoded geode quads.
  WGPURenderPipeline pipeline = nullptr;
};

class GeodeCanvas {
 public:
  virtual ~GeodeCanvas();

  /// Clears any pending path commands.
  virtual void beginPath();

  /// Moves the current point without emitting a segment.
  virtual void moveTo(const Point& p0);

  /// Emits a line segment from the current point to `p1`.
  virtual void lineTo(const Point& p1);

  /// Emits a quadratic segment from the current point using `p1` as control and `p2` as end point.
  virtual void quadraticCurveTo(const Point& p1, const Point& p2);

  /// Closes the current subpath.
  virtual void closePath();

  /// Renders the pending path using the current state.
  virtual void fill();

  /// Renders the pending path outline using the current state.
  virtual void stroke();

  /// Sets stroke width and fill/stroke toggles.
  virtual void setState(const CanvasState& state);

  /// Returns a PNG-encoded buffer if offscreen; empty if swapchain-backed.
  virtual std::vector<uint8_t> readbackPng();

  /// Returns the encoded draws (segments + bounds) captured so far.
  virtual const std::vector<EncodedDraw>& encodedDraws() const = 0;

  /// Returns GPU upload payload (packed geode buffer + draw uniforms) if available.
  virtual GpuUpload prepareGpuUpload() const;

  /// Builds a Dawn-friendly render plan with aligned uniform offsets per draw.
  virtual DawnRenderPlan prepareDawnRenderPlan() const;

  /// Packages buffer upload requirements and surface metadata for Dawn submissions.
  virtual DawnSubmission prepareDawnSubmission(void* textureView = nullptr) const;
};

/// Factory for Dawn-backed canvas; expects Dawn device setup externally.
GeodeCanvas* CreateDawnGeodeCanvas(const GeodeCanvasOptions& options);

/// Allocates Dawn GPU buffers and render targets for a prepared submission.
/// The swapchain format is required for swapchain-backed submissions; offscreen targets default to
/// RGBA8Unorm.
DawnSubmissionResources CreateDawnSubmissionResources(
    WGPUDevice device, const DawnSubmission& submission,
    WGPUTextureFormat swapchainFormat = WGPUTextureFormat_RGBA8Unorm);

/// Releases owned Dawn resources allocated by `CreateDawnSubmissionResources`.
void DestroyDawnSubmissionResources(const DawnSubmissionResources& resources);

/// Builds the Dawn pipeline used to render geode submissions with the provided color format.
DawnGeodePipeline CreateDawnGeodePipeline(WGPUDevice device, WGPUTextureFormat colorFormat);

/// Destroys Dawn pipeline objects created by `CreateDawnGeodePipeline`.
void DestroyDawnGeodePipeline(const DawnGeodePipeline& pipeline);

/// Creates a bind group for the supplied submission resources.
WGPUBindGroup CreateGeodeBindGroup(
    WGPUDevice device, const DawnGeodePipeline& pipeline,
    const DawnSubmissionResources& resources);

/// Uploads geode segment and uniform data to Dawn buffers.
bool UploadGeodeSubmissionBuffers(
    WGPUQueue queue, const DawnSubmission& submission,
    const DawnSubmissionResources& resources);

/// Encodes a render pass that draws all geode draws in the submission.
WGPUCommandBuffer EncodeGeodeRenderPass(
    WGPUDevice device, const DawnGeodePipeline& pipeline,
    const DawnSubmission& submission, const DawnSubmissionResources& resources);

/// Uploads buffers, encodes the render pass, and submits commands to the Dawn queue.
bool SubmitGeodeSubmission(
    WGPUDevice device, WGPUQueue queue, const DawnGeodePipeline& pipeline,
    const DawnSubmission& submission, const DawnSubmissionResources& resources);

/// Reads back an offscreen geode submission into a PNG-encoded buffer.
/// Returns empty when the submission is swapchain-backed or a readback error occurs.
std::vector<uint8_t> ReadbackOffscreenSubmissionPng(
    WGPUDevice device, WGPUQueue queue, const DawnSubmission& submission,
    const DawnSubmissionResources& resources);

}  // namespace donner::geode

