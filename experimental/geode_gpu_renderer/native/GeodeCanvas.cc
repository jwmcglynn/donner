/// @file
/// Stub implementation of the desktop geode Canvas-like shim.

#include "GeodeCanvas.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>

#include <webgpu/webgpu.h>
#include <stb/stb_image_write.h>

namespace donner::geode {

GeodeCanvas::~GeodeCanvas() = default;

void GeodeCanvas::beginPath() {}

void GeodeCanvas::moveTo(const Point& p0) {
  (void)p0;
}

void GeodeCanvas::lineTo(const Point& p1) {
  (void)p1;
}

void GeodeCanvas::quadraticCurveTo(const Point& p1, const Point& p2) {
  (void)p1;
  (void)p2;
}

void GeodeCanvas::closePath() {}

void GeodeCanvas::fill() {}

void GeodeCanvas::stroke() {}

void GeodeCanvas::setState(const CanvasState& state) {
  (void)state;
}

std::vector<uint8_t> GeodeCanvas::readbackPng() { return {}; }

GpuUpload GeodeCanvas::prepareGpuUpload() const { return {}; }

DawnRenderPlan GeodeCanvas::prepareDawnRenderPlan() const { return {}; }

DawnSubmission GeodeCanvas::prepareDawnSubmission(void* textureView) const {
  (void)textureView;
  return {};
}

namespace {

constexpr char kGeodeWgsl[] = R"(struct GeodeSegment {
  p0 : vec2f;
  p1 : vec2f;
  p2 : vec2f;
  kind : u32; // 0 = line, 1 = quadratic
  pad : u32;
};

struct FrameUniforms {
  boundsMin : vec2f;
  boundsMax : vec2f;
  viewportSize : vec2f;
  segmentCount : u32;
  _pad : vec3u;
};

struct VertexOutput {
  @builtin(position) position : vec4f;
  @location(0) localPosition : vec2f;
};

@group(0) @binding(0) var<storage, read> segments : array<GeodeSegment>;
@group(0) @binding(1) var<uniform> frame : FrameUniforms;

fn toClipSpace(pos : vec2f) -> vec4f {
  let ndc = vec2f(
    (pos.x / frame.viewportSize.x) * 2.0 - 1.0,
    1.0 - (pos.y / frame.viewportSize.y) * 2.0,
  );
  return vec4f(ndc, 0.0, 1.0);
}

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex : u32) -> VertexOutput {
  var quad = array<vec2f, 6>(
    vec2f(frame.boundsMin.x, frame.boundsMin.y),
    vec2f(frame.boundsMax.x, frame.boundsMin.y),
    vec2f(frame.boundsMin.x, frame.boundsMax.y),
    vec2f(frame.boundsMax.x, frame.boundsMin.y),
    vec2f(frame.boundsMax.x, frame.boundsMax.y),
    vec2f(frame.boundsMin.x, frame.boundsMax.y),
  );

  var out : VertexOutput;
  out.localPosition = quad[vertexIndex];
  out.position = toClipSpace(quad[vertexIndex]);
  return out;
}

fn signedDistanceToLine(point : vec2f, a : vec2f, b : vec2f) -> f32 {
  let ab = b - a;
  let t = clamp(dot(point - a, ab) / dot(ab, ab), 0.0, 1.0);
  let closest = a + ab * t;
  let perp = vec2f(-ab.y, ab.x);
  let sign = sign(dot(point - closest, perp));
  return length(point - closest) * sign;
}

fn evalQuadratic(p0 : vec2f, p1 : vec2f, p2 : vec2f, t : f32) -> vec2f {
  let u = 1.0 - t;
  return u * u * p0 + 2.0 * u * t * p1 + t * t * p2;
}

fn signedDistanceToQuadratic(point : vec2f, p0 : vec2f, p1 : vec2f, p2 : vec2f) -> f32 {
  var t = clamp(dot(point - p0, p2 - p0) / dot(p2 - p0, p2 - p0), 0.0, 1.0);
  var i = 0u;
  loop {
    let pos = evalQuadratic(p0, p1, p2, t);
    let tangent =
      normalize(2.0 * (p1 - p0) * (1.0 - t) + 2.0 * (p2 - p1) * t);
    let normal = vec2f(-tangent.y, tangent.x);
    let d1 =
      2.0 * dot(pos - point, (p0 - p1) * (1.0 - t) + (p2 - p1) * t);
    let curvature = (p0 - p1) * (1.0 - t) + (p2 - p1) * t;
    let d2 = 2.0 * dot(curvature, curvature) +
             2.0 * dot(pos - point, p0 - 2.0 * p1 + p2);
    if (abs(d2) > 1e-5) {
      t = clamp(t - d1 / d2, 0.0, 1.0);
    }
    i = i + 1u;
    if (i >= 5u) {
      break;
    }
  }

  let pos = evalQuadratic(p0, p1, p2, t);
  let tangent = normalize(2.0 * (p1 - p0) * (1.0 - t) + 2.0 * (p2 - p1) * t);
  let normal = vec2f(-tangent.y, tangent.x);
  let signFactor = sign(dot(point - pos, normal));
  return length(point - pos) * signFactor;
}

fn coverageAtPixel(position : vec2f) -> f32 {
  var dist = 1e6;

  for (var i = 0u; i < frame.segmentCount; i = i + 1u) {
    let seg = segments[i];
    if (seg.kind == 0u) {
      dist = min(dist, signedDistanceToLine(position, seg.p0, seg.p1));
    } else {
      dist = min(dist, signedDistanceToQuadratic(position, seg.p0, seg.p1, seg.p2));
    }
  }

  let aaWidth = 1.0;
  let coverage = clamp(0.5 - dist / aaWidth, 0.0, 1.0);
  return coverage;
}

@fragment
fn fs_main(in : VertexOutput) -> @location(0) vec4f {
  let alpha = coverageAtPixel(in.localPosition);
  let color = vec3f(0.12, 0.63, 0.35);
  return vec4f(color * alpha, alpha);
})";

std::vector<GeodeSegment> EncodePathCommands(const std::vector<PathCommand>& commands) {
  std::vector<GeodeSegment> segments;
  Point currentPoint{};
  Point subpathStart{};
  bool hasCurrentPoint = false;

  for (const PathCommand& command : commands) {
    switch (command.kind) {
      case PathCommandKind::kMoveTo: {
        currentPoint = command.p0;
        subpathStart = command.p0;
        hasCurrentPoint = true;
        break;
      }
      case PathCommandKind::kLineTo: {
        if (!hasCurrentPoint) {
          currentPoint = command.p1;
          subpathStart = command.p1;
          hasCurrentPoint = true;
          break;
        }

        GeodeSegment segment;
        segment.kind = PathCommandKind::kLineTo;
        segment.p0 = currentPoint;
        segment.p1 = command.p1;
        segment.p2 = command.p1;
        segments.push_back(segment);
        currentPoint = command.p1;
        break;
      }
      case PathCommandKind::kQuadraticTo: {
        if (!hasCurrentPoint) {
          currentPoint = command.p2;
          subpathStart = command.p2;
          hasCurrentPoint = true;
          break;
        }

        GeodeSegment segment;
        segment.kind = PathCommandKind::kQuadraticTo;
        segment.p0 = currentPoint;
        segment.p1 = command.p1;
        segment.p2 = command.p2;
        segments.push_back(segment);
        currentPoint = command.p2;
        break;
      }
      case PathCommandKind::kClosePath: {
        if (!hasCurrentPoint) {
          break;
        }

        GeodeSegment segment;
        segment.kind = PathCommandKind::kLineTo;
        segment.p0 = currentPoint;
        segment.p1 = command.p1;
        segment.p2 = command.p1;
        segments.push_back(segment);
        currentPoint = command.p1;
        break;
      }
    }
  }

  return segments;
}

struct PackedSegment {
  float p0x;
  float p0y;
  float p1x;
  float p1y;
  float p2x;
  float p2y;
  uint32_t kind;
  uint32_t pad;
};

std::vector<uint8_t> EncodeGeodeSegmentsBinary(const std::vector<GeodeSegment>& segments) {
  std::vector<PackedSegment> packed;
  packed.reserve(segments.size());

  for (const GeodeSegment& segment : segments) {
    PackedSegment packedSegment{};
    packedSegment.p0x = segment.p0.x;
    packedSegment.p0y = segment.p0.y;
    packedSegment.p1x = segment.p1.x;
    packedSegment.p1y = segment.p1.y;
    packedSegment.p2x = segment.p2.x;
    packedSegment.p2y = segment.p2.y;
    packedSegment.kind = static_cast<uint32_t>(segment.kind);
    packed.push_back(packedSegment);
  }

  std::vector<uint8_t> buffer(packed.size() * sizeof(PackedSegment));
  if (!buffer.empty()) {
    std::memcpy(buffer.data(), packed.data(), buffer.size());
  }

  return buffer;
}

struct Bounds {
  Point min{};
  Point max{};
};

Bounds ComputeBounds(const std::vector<GeodeSegment>& segments) {
  Bounds bounds;
  bounds.min.x = std::numeric_limits<float>::infinity();
  bounds.min.y = std::numeric_limits<float>::infinity();
  bounds.max.x = -std::numeric_limits<float>::infinity();
  bounds.max.y = -std::numeric_limits<float>::infinity();

  for (const GeodeSegment& segment : segments) {
    const Point points[] = {segment.p0, segment.p1, segment.p2};
    for (const Point& point : points) {
      bounds.min.x = std::min(bounds.min.x, point.x);
      bounds.min.y = std::min(bounds.min.y, point.y);
      bounds.max.x = std::max(bounds.max.x, point.x);
      bounds.max.y = std::max(bounds.max.y, point.y);
    }
  }

  if (segments.empty()) {
    bounds.min = {0.0f, 0.0f};
    bounds.max = {0.0f, 0.0f};
  }

  return bounds;
}

uint32_t AlignTo(uint32_t value, uint32_t alignment) {
  if (alignment == 0) {
    return value;
  }

  return (value + alignment - 1) / alignment * alignment;
}

std::vector<uint8_t> EncodeRgbaToPng(
    const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  std::vector<uint8_t> png;
  stbi_write_png_to_func(
      [](void* context, void* data, int len) {
        auto* output = static_cast<std::vector<uint8_t>*>(context);
        const auto* bytes = static_cast<const uint8_t*>(data);
        output->insert(output->end(), bytes, bytes + len);
      },
      &png, static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
      static_cast<int>(width * 4u));

  return png;
}

class DawnGeodeCanvas : public GeodeCanvas {
 public:
  explicit DawnGeodeCanvas(GeodeCanvasOptions options) : options_(std::move(options)) {}
  ~DawnGeodeCanvas() override = default;

  void beginPath() override {
    currentPath_.clear();
    hasCurrentPoint_ = false;
    subpathStart_ = {};
  }

  void moveTo(const Point& p0) override {
    currentPoint_ = p0;
    subpathStart_ = p0;
    hasCurrentPoint_ = true;
    PathCommand command;
    command.kind = PathCommandKind::kMoveTo;
    command.p0 = p0;
    currentPath_.push_back(command);
  }

  void lineTo(const Point& p1) override {
    if (!hasCurrentPoint_) {
      moveTo(p1);
      return;
    }

    PathCommand command;
    command.kind = PathCommandKind::kLineTo;
    command.p0 = currentPoint_;
    command.p1 = p1;
    currentPath_.push_back(command);
    currentPoint_ = p1;
  }

  void quadraticCurveTo(const Point& p1, const Point& p2) override {
    if (!hasCurrentPoint_) {
      moveTo(p2);
      return;
    }

    PathCommand command;
    command.kind = PathCommandKind::kQuadraticTo;
    command.p0 = currentPoint_;
    command.p1 = p1;
    command.p2 = p2;
    currentPath_.push_back(command);
    currentPoint_ = p2;
  }

  void closePath() override {
    if (!hasCurrentPoint_) {
      return;
    }

    PathCommand command;
    command.kind = PathCommandKind::kClosePath;
    command.p0 = currentPoint_;
    command.p1 = subpathStart_;
    currentPath_.push_back(command);
    currentPoint_ = subpathStart_;
  }

  void fill() override { recordDraw(/*isFill=*/true); }

  void stroke() override { recordDraw(/*isFill=*/false); }

  void setState(const CanvasState& state) override { state_ = state; }

  std::vector<uint8_t> readbackPng() override {
    (void)options_;
    return {};
  }

  const std::vector<EncodedDraw>& encodedDraws() const override { return encodedDraws_; }

  GpuUpload prepareGpuUpload() const override {
    GpuUpload upload;
    uint32_t segmentOffset = 0;

    std::vector<GeodeSegment> aggregateSegments;
    size_t totalSegments = 0;
    for (const EncodedDraw& draw : encodedDraws_) {
      totalSegments += draw.segments.size();
    }
    aggregateSegments.reserve(totalSegments);

    for (const EncodedDraw& draw : encodedDraws_) {
      aggregateSegments.insert(
          aggregateSegments.end(), draw.segments.begin(), draw.segments.end());

      GpuUpload::DrawUniforms uniforms;
      uniforms.boundsMin[0] = draw.boundsMin.x;
      uniforms.boundsMin[1] = draw.boundsMin.y;
      uniforms.boundsMax[0] = draw.boundsMax.x;
      uniforms.boundsMax[1] = draw.boundsMax.y;
      uniforms.viewport[0] = static_cast<float>(options_.width);
      uniforms.viewport[1] = static_cast<float>(options_.height);
      uniforms.strokeWidth = draw.state.strokeWidth;
      uniforms.segmentOffset = segmentOffset;
      uniforms.segmentCount = static_cast<uint32_t>(draw.segments.size());
      uniforms.isFill = draw.isFill ? 1u : 0u;
      upload.drawUniforms.push_back(uniforms);

      segmentOffset += static_cast<uint32_t>(draw.segments.size());
    }

    upload.geodeBuffer = EncodeGeodeSegmentsBinary(aggregateSegments);
    return upload;
  }

  DawnRenderPlan prepareDawnRenderPlan() const override {
    DawnRenderPlan plan;
    GpuUpload upload = prepareGpuUpload();

    plan.width = options_.width;
    plan.height = options_.height;
    plan.offscreen = options_.offscreen;
    plan.buffers.segments = std::move(upload.geodeBuffer);

    constexpr uint32_t kUniformAlignment = 256u;
    const uint32_t uniformSize = static_cast<uint32_t>(sizeof(GpuUpload::DrawUniforms));
    uint32_t cursor = 0;

    plan.draws.reserve(upload.drawUniforms.size());
    for (const GpuUpload::DrawUniforms& uniforms : upload.drawUniforms) {
      const uint32_t alignedOffset = AlignTo(cursor, kUniformAlignment);
      const uint32_t requiredSize = alignedOffset + uniformSize;
      if (plan.buffers.uniforms.size() < requiredSize) {
        plan.buffers.uniforms.resize(requiredSize);
      }

      std::memcpy(plan.buffers.uniforms.data() + alignedOffset, &uniforms, uniformSize);

      DawnRenderPlan::DrawCall draw;
      draw.uniformOffset = alignedOffset;
      draw.segmentOffset = uniforms.segmentOffset;
      draw.segmentCount = uniforms.segmentCount;
      plan.draws.push_back(draw);

      cursor = requiredSize;
    }

    return plan;
  }

  DawnSubmission prepareDawnSubmission(void* textureView) const override {
    DawnSubmission submission;
    submission.renderPlan = prepareDawnRenderPlan();

    constexpr uint64_t kUsageCopyDst = WGPUBufferUsage_CopyDst;
    constexpr uint64_t kUsageStorage = WGPUBufferUsage_Storage;
    constexpr uint64_t kUsageUniform = WGPUBufferUsage_Uniform;

    DawnSubmission::BufferUpload segments;
    segments.binding = DawnSubmission::BufferUpload::Binding::kSegments;
    segments.size = submission.renderPlan.buffers.segments.size();
    segments.usage = kUsageCopyDst | kUsageStorage;
    submission.buffers.push_back(segments);

    DawnSubmission::BufferUpload uniforms;
    uniforms.binding = DawnSubmission::BufferUpload::Binding::kUniforms;
    uniforms.size = submission.renderPlan.buffers.uniforms.size();
    uniforms.usage = kUsageCopyDst | kUsageUniform;
    submission.buffers.push_back(uniforms);

    submission.surface.textureView = submission.renderPlan.offscreen ? nullptr : textureView;
    submission.surface.width = submission.renderPlan.width;
    submission.surface.height = submission.renderPlan.height;
    submission.surface.offscreen = submission.renderPlan.offscreen;

    return submission;
  }

 private:
  void recordDraw(bool isFill) {
    if (currentPath_.empty()) {
      return;
    }

    std::vector<GeodeSegment> segments = EncodePathCommands(currentPath_);
    if (segments.empty()) {
      return;
    }

    Bounds bounds = ComputeBounds(segments);

    EncodedDraw draw;
    draw.segments = std::move(segments);
    draw.state = state_;
    draw.boundsMin = bounds.min;
    draw.boundsMax = bounds.max;
    draw.isFill = isFill;
    encodedDraws_.push_back(std::move(draw));
  }

  GeodeCanvasOptions options_;
  CanvasState state_{};
  std::vector<PathCommand> currentPath_{};
  Point currentPoint_{};
  Point subpathStart_{};
  bool hasCurrentPoint_ = false;
  std::vector<EncodedDraw> encodedDraws_{};
};

}  // namespace

GeodeCanvas* CreateDawnGeodeCanvas(const GeodeCanvasOptions& options) {
  return new DawnGeodeCanvas(options);
}

DawnSubmissionResources CreateDawnSubmissionResources(
    WGPUDevice device, const DawnSubmission& submission, WGPUTextureFormat swapchainFormat) {
  DawnSubmissionResources resources;
  if (device == nullptr) {
    return resources;
  }

  for (const DawnSubmission::BufferUpload& buffer : submission.buffers) {
    const WGPUBufferUsageFlags usage = static_cast<WGPUBufferUsageFlags>(buffer.usage);
    if (buffer.binding == DawnSubmission::BufferUpload::Binding::kSegments) {
      if (buffer.size == 0) {
        continue;
      }

      WGPUBufferDescriptor descriptor{};
      descriptor.size = buffer.size;
      descriptor.usage = usage;
      resources.segments = wgpuDeviceCreateBuffer(device, &descriptor);
    } else if (buffer.binding == DawnSubmission::BufferUpload::Binding::kUniforms) {
      if (buffer.size == 0) {
        continue;
      }

      WGPUBufferDescriptor descriptor{};
      descriptor.size = buffer.size;
      descriptor.usage = usage;
      resources.uniforms = wgpuDeviceCreateBuffer(device, &descriptor);
    }
  }

  if (submission.surface.offscreen) {
    if (submission.surface.width == 0 || submission.surface.height == 0) {
      return resources;
    }

    WGPUTextureDescriptor descriptor{};
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.format = swapchainFormat;
    descriptor.size.width = submission.surface.width;
    descriptor.size.height = submission.surface.height;
    descriptor.size.depthOrArrayLayers = 1;
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;
    descriptor.usage = WGPUTextureUsage_RenderAttachment |
                       WGPUTextureUsage_TextureBinding |
                       WGPUTextureUsage_CopySrc;

    resources.colorTexture = wgpuDeviceCreateTexture(device, &descriptor);
    if (resources.colorTexture != nullptr) {
      resources.colorTextureView = wgpuTextureCreateView(resources.colorTexture, nullptr);
      resources.ownsColorTexture = true;
    }
  } else {
    resources.colorTextureView =
        static_cast<WGPUTextureView>(submission.surface.textureView);
    resources.ownsColorTexture = false;
  }

  return resources;
}

void DestroyDawnSubmissionResources(const DawnSubmissionResources& resources) {
  if (resources.segments != nullptr) {
    wgpuBufferRelease(resources.segments);
  }

  if (resources.uniforms != nullptr) {
    wgpuBufferRelease(resources.uniforms);
  }

  if (resources.ownsColorTexture) {
    if (resources.colorTextureView != nullptr) {
      wgpuTextureViewRelease(resources.colorTextureView);
    }

    if (resources.colorTexture != nullptr) {
      wgpuTextureRelease(resources.colorTexture);
    }
  }
}

namespace {

WGPUShaderModule CreateWgslModule(WGPUDevice device, const char* source) {
  if (device == nullptr || source == nullptr) {
    return nullptr;
  }

  WGPUShaderModuleWGSLDescriptor wgsl{};
  wgsl.chain.sType = WGPUSType_ShaderModuleWGSLDescriptor;
  wgsl.code = source;

  WGPUShaderModuleDescriptor descriptor{};
  descriptor.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wgsl);
  return wgpuDeviceCreateShaderModule(device, &descriptor);
}

WGPUBindGroupLayout CreateBindGroupLayout(WGPUDevice device) {
  if (device == nullptr) {
    return nullptr;
  }

  WGPUBindGroupLayoutEntry entries[2] = {};

  entries[0].binding = 0;
  entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
  entries[0].buffer.type = WGPUBufferBindingType_Storage;
  entries[0].buffer.hasDynamicOffset = false;
  entries[0].buffer.minBindingSize = 0;

  entries[1].binding = 1;
  entries[1].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
  entries[1].buffer.type = WGPUBufferBindingType_Uniform;
  entries[1].buffer.hasDynamicOffset = true;
  entries[1].buffer.minBindingSize = sizeof(GpuUpload::DrawUniforms);

  WGPUBindGroupLayoutDescriptor descriptor{};
  descriptor.entryCount = 2;
  descriptor.entries = entries;
  return wgpuDeviceCreateBindGroupLayout(device, &descriptor);
}

WGPUPipelineLayout CreatePipelineLayout(WGPUDevice device, WGPUBindGroupLayout layout) {
  if (device == nullptr || layout == nullptr) {
    return nullptr;
  }

  WGPUPipelineLayoutDescriptor descriptor{};
  descriptor.bindGroupLayoutCount = 1;
  descriptor.bindGroupLayouts = &layout;
  return wgpuDeviceCreatePipelineLayout(device, &descriptor);
}

}  // namespace

DawnGeodePipeline CreateDawnGeodePipeline(WGPUDevice device, WGPUTextureFormat colorFormat) {
  DawnGeodePipeline pipeline;
  if (device == nullptr) {
    return pipeline;
  }

  pipeline.shaderModule = CreateWgslModule(device, kGeodeWgsl);
  pipeline.bindGroupLayout = CreateBindGroupLayout(device);
  pipeline.pipelineLayout = CreatePipelineLayout(device, pipeline.bindGroupLayout);

  if (pipeline.shaderModule == nullptr || pipeline.pipelineLayout == nullptr) {
    return pipeline;
  }

  WGPUVertexState vertexState{};
  vertexState.module = pipeline.shaderModule;
  vertexState.entryPoint = "vs_main";
  vertexState.bufferCount = 0;
  vertexState.buffers = nullptr;

  WGPUColorTargetState colorTarget{};
  colorTarget.format = colorFormat;
  colorTarget.writeMask = WGPUColorWriteMask_All;

  WGPUFragmentState fragmentState{};
  fragmentState.module = pipeline.shaderModule;
  fragmentState.entryPoint = "fs_main";
  fragmentState.targetCount = 1;
  fragmentState.targets = &colorTarget;

  WGPUPrimitiveState primitive{};
  primitive.topology = WGPUPrimitiveTopology_TriangleList;
  primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
  primitive.frontFace = WGPUFrontFace_CCW;
  primitive.cullMode = WGPUCullMode_None;

  WGPUMultisampleState multisample{};
  multisample.count = 1;
  multisample.mask = ~0u;
  multisample.alphaToCoverageEnabled = false;

  WGPURenderPipelineDescriptor descriptor{};
  descriptor.layout = pipeline.pipelineLayout;
  descriptor.vertex = vertexState;
  descriptor.primitive = primitive;
  descriptor.multisample = multisample;
  descriptor.fragment = &fragmentState;

  pipeline.pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
  return pipeline;
}

void DestroyDawnGeodePipeline(const DawnGeodePipeline& pipeline) {
  if (pipeline.pipeline != nullptr) {
    wgpuRenderPipelineRelease(pipeline.pipeline);
  }

  if (pipeline.pipelineLayout != nullptr) {
    wgpuPipelineLayoutRelease(pipeline.pipelineLayout);
  }

  if (pipeline.bindGroupLayout != nullptr) {
    wgpuBindGroupLayoutRelease(pipeline.bindGroupLayout);
  }

  if (pipeline.shaderModule != nullptr) {
    wgpuShaderModuleRelease(pipeline.shaderModule);
  }
}

WGPUBindGroup CreateGeodeBindGroup(
    WGPUDevice device, const DawnGeodePipeline& pipeline,
    const DawnSubmissionResources& resources) {
  if (device == nullptr || pipeline.bindGroupLayout == nullptr) {
    return nullptr;
  }

  if (resources.segments == nullptr || resources.uniforms == nullptr) {
    return nullptr;
  }

  WGPUBindGroupEntry entries[2] = {};
  entries[0].binding = 0;
  entries[0].buffer = resources.segments;
  entries[0].offset = 0;
  entries[0].size = WGPU_WHOLE_SIZE;

  entries[1].binding = 1;
  entries[1].buffer = resources.uniforms;
  entries[1].offset = 0;
  entries[1].size = WGPU_WHOLE_SIZE;

  WGPUBindGroupDescriptor descriptor{};
  descriptor.layout = pipeline.bindGroupLayout;
  descriptor.entryCount = 2;
  descriptor.entries = entries;
  return wgpuDeviceCreateBindGroup(device, &descriptor);
}

bool UploadGeodeSubmissionBuffers(
    WGPUQueue queue, const DawnSubmission& submission,
    const DawnSubmissionResources& resources) {
  if (queue == nullptr) {
    return false;
  }

  bool ok = true;
  for (const DawnSubmission::BufferUpload& buffer : submission.buffers) {
    if (buffer.binding == DawnSubmission::BufferUpload::Binding::kSegments) {
      if (buffer.size == 0) {
        continue;
      }

      if (resources.segments == nullptr) {
        ok = false;
        continue;
      }

      wgpuQueueWriteBuffer(
          queue, resources.segments, 0, submission.renderPlan.buffers.segments.data(),
          submission.renderPlan.buffers.segments.size());
    } else if (buffer.binding == DawnSubmission::BufferUpload::Binding::kUniforms) {
      if (buffer.size == 0) {
        continue;
      }

      if (resources.uniforms == nullptr) {
        ok = false;
        continue;
      }

      wgpuQueueWriteBuffer(
          queue, resources.uniforms, 0, submission.renderPlan.buffers.uniforms.data(),
          submission.renderPlan.buffers.uniforms.size());
    }
  }

  return ok;
}

WGPUCommandBuffer EncodeGeodeRenderPass(
    WGPUDevice device, const DawnGeodePipeline& pipeline,
    const DawnSubmission& submission, const DawnSubmissionResources& resources) {
  if (device == nullptr || pipeline.pipeline == nullptr) {
    return nullptr;
  }

  if (resources.colorTextureView == nullptr) {
    return nullptr;
  }

  WGPUBindGroup bindGroup = CreateGeodeBindGroup(device, pipeline, resources);
  if (bindGroup == nullptr) {
    return nullptr;
  }

  WGPUCommandEncoderDescriptor encoderDescriptor{};
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, &encoderDescriptor);

  WGPURenderPassColorAttachment colorAttachment{};
  colorAttachment.view = resources.colorTextureView;
  colorAttachment.resolveTarget = nullptr;
  colorAttachment.loadOp = WGPULoadOp_Clear;
  colorAttachment.storeOp = WGPUStoreOp_Store;
  colorAttachment.clearValue = {0.04f, 0.04f, 0.08f, 1.0f};

  WGPURenderPassDescriptor passDescriptor{};
  passDescriptor.colorAttachmentCount = 1;
  passDescriptor.colorAttachments = &colorAttachment;

  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDescriptor);
  wgpuRenderPassEncoderSetPipeline(pass, pipeline.pipeline);

  std::vector<uint32_t> dynamicOffsets(1, 0u);
  for (const DawnRenderPlan::DrawCall& draw : submission.renderPlan.draws) {
    dynamicOffsets[0] = draw.uniformOffset;
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup, 1, dynamicOffsets.data());
    wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
  }

  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);

  WGPUCommandBufferDescriptor commandBufferDescriptor{};
  WGPUCommandBuffer commandBuffer =
      wgpuCommandEncoderFinish(encoder, &commandBufferDescriptor);
  wgpuCommandEncoderRelease(encoder);
  wgpuBindGroupRelease(bindGroup);
  return commandBuffer;
}

bool SubmitGeodeSubmission(
    WGPUDevice device, WGPUQueue queue, const DawnGeodePipeline& pipeline,
    const DawnSubmission& submission, const DawnSubmissionResources& resources) {
  if (device == nullptr || queue == nullptr) {
    return false;
  }

  if (!UploadGeodeSubmissionBuffers(queue, submission, resources)) {
    return false;
  }

  WGPUCommandBuffer commandBuffer =
      EncodeGeodeRenderPass(device, pipeline, submission, resources);
  if (commandBuffer == nullptr) {
    return false;
  }

  wgpuQueueSubmit(queue, 1, &commandBuffer);
  wgpuCommandBufferRelease(commandBuffer);
  return true;
}

std::vector<uint8_t> ReadbackOffscreenSubmissionPng(
    WGPUDevice device, WGPUQueue queue, const DawnSubmission& submission,
    const DawnSubmissionResources& resources) {
  if (device == nullptr || queue == nullptr) {
    return {};
  }

  if (!submission.surface.offscreen || resources.colorTexture == nullptr) {
    return {};
  }

  const uint32_t width = submission.surface.width;
  const uint32_t height = submission.surface.height;
  if (width == 0 || height == 0) {
    return {};
  }

  const uint32_t bytesPerRow = AlignTo(width * 4u, 256u);
  const uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * height;

  WGPUBufferDescriptor bufferDescriptor{};
  bufferDescriptor.size = bufferSize;
  bufferDescriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer readbackBuffer = wgpuDeviceCreateBuffer(device, &bufferDescriptor);
  if (readbackBuffer == nullptr) {
    return {};
  }

  WGPUCommandEncoderDescriptor encoderDescriptor{};
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, &encoderDescriptor);

  WGPUImageCopyTexture srcTexture{};
  srcTexture.texture = resources.colorTexture;
  srcTexture.mipLevel = 0;
  srcTexture.origin = {0, 0, 0};
  srcTexture.aspect = WGPUTextureAspect_All;

  WGPUImageCopyBuffer dstBuffer{};
  dstBuffer.buffer = readbackBuffer;
  dstBuffer.layout.offset = 0;
  dstBuffer.layout.bytesPerRow = bytesPerRow;
  dstBuffer.layout.rowsPerImage = height;

  WGPUExtent3D copySize{};
  copySize.width = width;
  copySize.height = height;
  copySize.depthOrArrayLayers = 1;

  wgpuCommandEncoderCopyTextureToBuffer(encoder, &srcTexture, &dstBuffer, &copySize);

  WGPUCommandBufferDescriptor commandBufferDescriptor{};
  WGPUCommandBuffer commandBuffer =
      wgpuCommandEncoderFinish(encoder, &commandBufferDescriptor);
  wgpuCommandEncoderRelease(encoder);
  wgpuQueueSubmit(queue, 1, &commandBuffer);
  wgpuCommandBufferRelease(commandBuffer);

  struct MapContext {
    std::atomic<bool> done{false};
    WGPUBufferMapAsyncStatus status = WGPUBufferMapAsyncStatus_Unknown;
  };

  MapContext mapContext;
  wgpuBufferMapAsync(
      readbackBuffer, WGPUMapMode_Read, 0, bufferSize,
      [](WGPUBufferMapAsyncStatus status, void* userdata) {
        auto* context = static_cast<MapContext*>(userdata);
        context->status = status;
        context->done.store(true);
      },
      &mapContext);

  while (!mapContext.done.load()) {
    wgpuDevicePoll(device, true, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (mapContext.status != WGPUBufferMapAsyncStatus_Success) {
    wgpuBufferRelease(readbackBuffer);
    return {};
  }

  const uint8_t* mapped = static_cast<const uint8_t*>(
      wgpuBufferGetConstMappedRange(readbackBuffer, 0, bufferSize));
  if (mapped == nullptr) {
    wgpuBufferUnmap(readbackBuffer);
    wgpuBufferRelease(readbackBuffer);
    return {};
  }

  std::vector<uint8_t> rgba;
  rgba.resize(static_cast<size_t>(width) * height * 4u);

  for (uint32_t y = 0; y < height; ++y) {
    const size_t srcOffset = static_cast<size_t>(y) * bytesPerRow;
    const size_t dstOffset = static_cast<size_t>(y) * width * 4u;
    std::memcpy(rgba.data() + dstOffset, mapped + srcOffset, width * 4u);
  }

  wgpuBufferUnmap(readbackBuffer);
  wgpuBufferRelease(readbackBuffer);
  return EncodeRgbaToPng(rgba, width, height);
}

}  // namespace donner::geode

