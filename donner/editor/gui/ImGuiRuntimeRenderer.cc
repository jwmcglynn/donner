#include "donner/editor/gui/ImGuiRuntimeRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "donner/gpu/CheckedArithmetic.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/shader/CompiledShader.h"
#include "donner/gpu/shader/programs/UiDraw.h"

namespace donner::editor {

namespace {

using gpu::shader::programs::UiDrawParams;

static_assert(sizeof(ImDrawVert) == 20,
              "the vertex buffer is uploaded as a direct copy of the UI draw vertices");
static_assert(offsetof(ImDrawVert, pos) == 0);
static_assert(offsetof(ImDrawVert, uv) == 8);
static_assert(offsetof(ImDrawVert, col) == 16);
static_assert(sizeof(ImDrawIdx) == 2, "indices are bound as 16-bit and drawn with a base vertex");

/// The UI draw projection \p device consumes. The transitional adapter takes WGSL; native Metal
/// and Vulkan devices take their own projection, which the browser package never links.
/// @param device Device the pipelines are created on.
const gpu::shader::CompiledShaderView& SelectUiDrawShader(const gpu::Device& device) {
#if (defined(__APPLE__) || defined(__linux__)) && !defined(__EMSCRIPTEN__)
  if (device.shaderSourceKind() != gpu::ShaderSourceKind::Wgsl) {
    return gpu::shader::programs::UiDrawNativeShader();
  }
#endif
  return gpu::shader::programs::UiDrawShader();
}

/// Vertex layout of the UI draw vertices, matching the authored attribute locations.
gpu::VertexBufferLayout UiDrawVertexLayout() {
  return gpu::VertexBufferLayout{sizeof(ImDrawVert),
                                 gpu::VertexStepMode::Vertex,
                                 {{gpu::VertexFormat::Float32x2, offsetof(ImDrawVert, pos), 0},
                                  {gpu::VertexFormat::Float32x2, offsetof(ImDrawVert, uv), 1},
                                  {gpu::VertexFormat::Uint32, offsetof(ImDrawVert, col), 2}}};
}

/// Blend state for a source whose color channels are independent of its alpha.
gpu::BlendState StraightAlphaBlend() {
  return gpu::BlendState{
      {gpu::BlendFactor::SrcAlpha, gpu::BlendFactor::OneMinusSrcAlpha, gpu::BlendOperation::Add},
      {gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha, gpu::BlendOperation::Add}};
}

/// Blend state for a source whose color channels are already scaled by its alpha.
gpu::BlendState PremultipliedAlphaBlend() {
  return gpu::BlendState{
      {gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha, gpu::BlendOperation::Add},
      {gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha, gpu::BlendOperation::Add}};
}

/// Exact bytes of \p value, for a buffer or texture upload.
/// @param value Object to view.
template <typename T>
std::span<const uint8_t> BytesOf(const T& value) {
  return {reinterpret_cast<const uint8_t*>(&value), sizeof(value)};
}

/// Byte alignment every buffer upload's size must satisfy.
constexpr uint64_t kUploadSizeAlignment = 4;

/// Rounds \p byteCount up to \ref kUploadSizeAlignment.
/// @param byteCount Natural payload size in bytes.
constexpr uint64_t AlignedUploadBytes(uint64_t byteCount) {
  return (byteCount + kUploadSizeAlignment - 1) / kUploadSizeAlignment * kUploadSizeAlignment;
}

/// Row stride alignment every texture upload must satisfy.
constexpr uint32_t kUploadRowAlignment = 256;

/// Rounds \p rowBytes up to \ref kUploadRowAlignment.
/// @param rowBytes Natural row stride in bytes.
constexpr uint64_t AlignedRowBytes(uint64_t rowBytes) {
  return (rowBytes + kUploadRowAlignment - 1) / kUploadRowAlignment * kUploadRowAlignment;
}

/// Rounds \p required up to the next power-of-two-ish growth step, at least \p minimum.
/// @param current Current capacity. @param required Bytes needed. @param minimum Smallest capacity.
uint64_t GrownCapacity(uint64_t current, uint64_t required, uint64_t minimum) {
  uint64_t capacity = std::max(current, minimum);
  while (capacity < required) {
    const std::optional<uint64_t> doubled = gpu::CheckedMul(capacity, 2);
    if (!doubled.has_value()) {
      return required;
    }
    capacity = *doubled;
  }
  return capacity;
}

/// True when \p shader exposes the vertex entry, both fragment entries and all three bindings.
/// @param shader Static compiled interface.
bool HasUiDrawInterface(const gpu::shader::CompiledShaderView& shader) {
  return shader.entryPoints.size() == 3 && shader.resource("params") != nullptr &&
         shader.resource("uiSampler") != nullptr && shader.resource("uiTexture") != nullptr;
}

/// A scissor rectangle in device pixels.
struct ScissorRect {
  uint32_t x = 0;       //!< Left edge.
  uint32_t y = 0;       //!< Top edge.
  uint32_t width = 0;   //!< Width.
  uint32_t height = 0;  //!< Height.
};

/// Converts \p command's clip rectangle into device pixels clamped to \p targetSizePx, or returns
/// no rectangle when nothing of it remains on the attachment.
/// @param command Command whose clip rectangle is converted.
/// @param drawData Draw data supplying the display origin and framebuffer scale.
/// @param targetSizePx Attachment extent in device pixels.
std::optional<ScissorRect> ScissorFor(const ImDrawCmd& command, const ImDrawData& drawData,
                                      const gpu::Extent2d& targetSizePx) {
  const float scaleX = drawData.FramebufferScale.x;
  const float scaleY = drawData.FramebufferScale.y;
  const float maxX = static_cast<float>(targetSizePx.width);
  const float maxY = static_cast<float>(targetSizePx.height);

  // A non-finite clip rectangle would survive clamping, because every comparison against a NaN is
  // false, and converting it to an unsigned scissor bound is undefined. Draw data is untrusted, so
  // the command is dropped instead.
  if (!std::isfinite(command.ClipRect.x) || !std::isfinite(command.ClipRect.y) ||
      !std::isfinite(command.ClipRect.z) || !std::isfinite(command.ClipRect.w)) {
    return std::nullopt;
  }

  const uint32_t minLeft = static_cast<uint32_t>(
      std::clamp((command.ClipRect.x - drawData.DisplayPos.x) * scaleX, 0.0f, maxX));
  const uint32_t minTop = static_cast<uint32_t>(
      std::clamp((command.ClipRect.y - drawData.DisplayPos.y) * scaleY, 0.0f, maxY));
  const uint32_t maxRight = static_cast<uint32_t>(
      std::clamp((command.ClipRect.z - drawData.DisplayPos.x) * scaleX, 0.0f, maxX));
  const uint32_t maxBottom = static_cast<uint32_t>(
      std::clamp((command.ClipRect.w - drawData.DisplayPos.y) * scaleY, 0.0f, maxY));

  if (maxRight <= minLeft || maxBottom <= minTop) {
    return std::nullopt;
  }
  return ScissorRect{minLeft, minTop, maxRight - minLeft, maxBottom - minTop};
}

/// The index range and vertex base one command draws from within the frame's combined buffers.
struct DrawRange {
  uint32_t firstIndex = 0;  //!< First index of the command.
  int32_t baseVertex = 0;   //!< Value added to every index value before the vertex fetch.
};

/// Offsets \p command's own list-relative range by its list's start in the combined buffers, and
/// refuses a range that leaves the frame's own geometry.
///
/// The runtime range-checks an indexed draw's index range but not the elements its index values
/// reach, because those live in GPU memory. Draw data is untrusted here, so the base vertex is
/// checked against the frame's vertex count: an index value can still reach past it, but a base
/// that is already outside cannot address anything this frame uploaded.
/// @param command Command whose range is computed.
/// @param listBaseVertex First vertex of the command's list.
/// @param listFirstIndex First index of the command's list.
/// @param frameVertexCount Vertices the frame uploaded.
/// @param frameIndexCount Indices the frame uploaded.
gpu::Result<DrawRange> DrawRangeFor(const ImDrawCmd& command, int32_t listBaseVertex,
                                    uint32_t listFirstIndex, uint32_t frameVertexCount,
                                    uint32_t frameIndexCount) {
  const std::optional<uint64_t> firstIndex = gpu::CheckedAdd(listFirstIndex, command.IdxOffset);
  if (!firstIndex.has_value() || *firstIndex > UINT32_MAX) {
    return gpu::GpuError{gpu::GpuErrorType::OutOfBounds,
                         "UI draw command's first index overflows the frame index range"};
  }
  const std::optional<uint64_t> baseVertex =
      gpu::CheckedAdd(static_cast<uint64_t>(listBaseVertex), command.VtxOffset);
  if (!baseVertex.has_value() || *baseVertex > INT32_MAX) {
    return gpu::GpuError{gpu::GpuErrorType::OutOfBounds,
                         "UI draw command's base vertex overflows the frame vertex range"};
  }
  if (*baseVertex >= frameVertexCount) {
    return gpu::GpuError{gpu::GpuErrorType::OutOfBounds,
                         "UI draw command's base vertex is outside the frame"};
  }
  const std::optional<uint64_t> lastIndex = gpu::CheckedAdd(*firstIndex, command.ElemCount);
  if (!lastIndex.has_value() || *lastIndex > frameIndexCount) {
    return gpu::GpuError{gpu::GpuErrorType::OutOfBounds,
                         "UI draw command's index range ends past the frame"};
  }
  return DrawRange{static_cast<uint32_t>(*firstIndex), static_cast<int32_t>(*baseVertex)};
}

/// Validates the actual index values a command reads against its owning draw list.
gpu::Status ValidateCommandIndices(const ImDrawCmd& command, const ImDrawList& list) {
  const std::optional<uint64_t> indexEnd = gpu::CheckedAdd(command.IdxOffset, command.ElemCount);
  if (!indexEnd.has_value() || *indexEnd > static_cast<uint64_t>(list.IdxBuffer.Size)) {
    return gpu::GpuError{gpu::GpuErrorType::OutOfBounds,
                         "UI draw command's index range leaves its owning draw list"};
  }
  for (uint64_t indexOffset = command.IdxOffset; indexOffset < *indexEnd; ++indexOffset) {
    const ImDrawIdx index = list.IdxBuffer[static_cast<int>(indexOffset)];
    const std::optional<uint64_t> vertex = gpu::CheckedAdd(command.VtxOffset, index);
    if (!vertex.has_value() || *vertex >= static_cast<uint64_t>(list.VtxBuffer.Size)) {
      return gpu::GpuError{
          gpu::GpuErrorType::OutOfBounds,
          "UI draw command index value or base vertex leaves its owning draw list"};
    }
  }
  return gpu::OkStatus();
}

}  // namespace

ImGuiRuntimeRenderer::ImGuiRuntimeRenderer(gpu::Device& device, UiTextureRegistry& registry)
    : device_(&device), registry_(&registry) {}

ImGuiRuntimeRenderer::~ImGuiRuntimeRenderer() {
  // A renderer that is still published when it is destroyed would leave both producers reading
  // freed memory through the accessors below.
  uninstall();
}

gpu::Result<std::unique_ptr<ImGuiRuntimeRenderer>> ImGuiRuntimeRenderer::Create(
    gpu::Device& device, UiTextureRegistry& registry, gpu::TextureFormat targetFormat) {
  std::unique_ptr<ImGuiRuntimeRenderer> renderer(new ImGuiRuntimeRenderer(device, registry));
  gpu::Status status = renderer->initialize(targetFormat);
  if (status.hasError()) {
    return std::move(status).error();
  }
  return renderer;
}

gpu::Status ImGuiRuntimeRenderer::initialize(gpu::TextureFormat targetFormat) {
  const gpu::shader::CompiledShaderView& shader = SelectUiDrawShader(*device_);
  if (!HasUiDrawInterface(shader)) {
    return gpu::GpuError{gpu::GpuErrorType::Unsupported,
                         "UI draw artifact does not expose the expected entry points and bindings"};
  }

  gpu::Status program = createProgram(shader);
  if (program.hasError()) {
    return program;
  }
  gpu::Status pipelines = createPipelines(targetFormat);
  if (pipelines.hasError()) {
    return pipelines;
  }
  return createSharedResources();
}

gpu::Status ImGuiRuntimeRenderer::createProgram(const gpu::shader::CompiledShaderView& shader) {
  gpu::Result<gpu::ShaderModule> shaderModule = device_->createShaderModule(
      gpu::shader::MakeShaderDescriptor(shader, device_->shaderSourceKind(), "uiDraw"));
  if (shaderModule.hasError()) {
    return std::move(shaderModule).error();
  }
  shaderModule_ = std::move(shaderModule).result();

  gpu::Result<gpu::BindGroupLayout> bindGroupLayout = device_->createBindGroupLayout(
      gpu::BindGroupLayoutDescriptor{"uiDraw", gpu::shader::MakeBindingLayout(shader)});
  if (bindGroupLayout.hasError()) {
    return std::move(bindGroupLayout).error();
  }
  bindGroupLayout_ = std::move(bindGroupLayout).result();

  gpu::Result<gpu::PipelineLayout> pipelineLayout =
      device_->createPipelineLayout(gpu::PipelineLayoutDescriptor{"uiDraw", {bindGroupLayout_}});
  if (pipelineLayout.hasError()) {
    return std::move(pipelineLayout).error();
  }
  pipelineLayout_ = std::move(pipelineLayout).result();
  return gpu::OkStatus();
}

gpu::Status ImGuiRuntimeRenderer::createPipelines(gpu::TextureFormat targetFormat) {
  gpu::RenderPipelineDescriptor descriptor{
      "uiDrawStraightAlpha", pipelineLayout_,
      gpu::VertexState{shaderModule_, "vs_main", {UiDrawVertexLayout()}},
      gpu::FragmentState{
          shaderModule_, "fs_straight_alpha", {{targetFormat, StraightAlphaBlend()}}}};

  gpu::Result<gpu::RenderPipeline> straightAlphaPipeline =
      device_->createRenderPipeline(descriptor);
  if (straightAlphaPipeline.hasError()) {
    return std::move(straightAlphaPipeline).error();
  }
  straightAlphaPipeline_ = std::move(straightAlphaPipeline).result();

  descriptor.label = "uiDrawPremultipliedAlpha";
  descriptor.fragment.entryPoint = "fs_premultiplied_alpha";
  descriptor.fragment.targets.front().blend = PremultipliedAlphaBlend();
  gpu::Result<gpu::RenderPipeline> premultipliedAlphaPipeline =
      device_->createRenderPipeline(descriptor);
  if (premultipliedAlphaPipeline.hasError()) {
    return std::move(premultipliedAlphaPipeline).error();
  }
  premultipliedAlphaPipeline_ = std::move(premultipliedAlphaPipeline).result();
  return gpu::OkStatus();
}

gpu::Status ImGuiRuntimeRenderer::createSharedResources() {
  gpu::Result<gpu::Sampler> sampler = device_->createSampler(
      gpu::SamplerDescriptor{"uiDraw", gpu::FilterMode::Linear, gpu::FilterMode::Linear,
                             gpu::AddressMode::ClampToEdge, gpu::AddressMode::ClampToEdge});
  if (sampler.hasError()) {
    return std::move(sampler).error();
  }
  sampler_ = std::move(sampler).result();

  gpu::Result<gpu::Buffer> uniformBuffer = device_->createBuffer(gpu::BufferDescriptor{
      "uiDrawParams", sizeof(UiDrawParams), gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst});
  if (uniformBuffer.hasError()) {
    return std::move(uniformBuffer).error();
  }
  uniformBuffer_ = std::move(uniformBuffer).result();
  return gpu::OkStatus();
}

gpu::Status ImGuiRuntimeRenderer::uploadFontTexture(ImFontAtlas& atlas, gpu::Texture& texture,
                                                    gpu::Extent2d& size) {
  unsigned char* pixels = nullptr;
  int width = 0;
  int height = 0;
  atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
  if (pixels == nullptr || width <= 0 || height <= 0) {
    return gpu::GpuError{gpu::GpuErrorType::InvalidDescriptor, "font atlas has no pixels"};
  }

  size = gpu::Extent2d{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
  const std::optional<uint64_t> sourceRowBytes = gpu::CheckedMul(size.width, 4);
  // An upload's row stride must be 256-aligned and an atlas width does not have to land on one,
  // so the rows are repacked into an aligned staging buffer rather than uploaded in place.
  const std::optional<uint64_t> bytesPerRow =
      sourceRowBytes.has_value() ? std::optional<uint64_t>(AlignedRowBytes(*sourceRowBytes))
                                 : std::nullopt;
  const std::optional<uint64_t> totalBytes =
      bytesPerRow.has_value() ? gpu::CheckedMul(*bytesPerRow, size.height) : std::nullopt;
  if (!totalBytes.has_value() || *bytesPerRow > UINT32_MAX) {
    return gpu::GpuError{gpu::GpuErrorType::LimitExceeded, "font atlas upload size overflows"};
  }

  std::vector<uint8_t> staging(static_cast<size_t>(*totalBytes), 0);
  for (uint32_t row = 0; row < size.height; ++row) {
    std::memcpy(staging.data() + static_cast<size_t>(row) * static_cast<size_t>(*bytesPerRow),
                pixels + static_cast<size_t>(row) * static_cast<size_t>(*sourceRowBytes),
                static_cast<size_t>(*sourceRowBytes));
  }

  gpu::Result<gpu::Texture> created = device_->createTexture(
      gpu::TextureDescriptor{"uiFontAtlas", size, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
  if (created.hasError()) {
    return std::move(created).error();
  }

  gpu::Status written = device_->writeTexture(
      created.result(), staging,
      gpu::TexelCopyBufferLayout{0, static_cast<uint32_t>(*bytesPerRow), size.height}, size);
  if (written.hasError()) {
    return written;
  }

  texture = std::move(created).result();
  return gpu::OkStatus();
}

gpu::Status ImGuiRuntimeRenderer::buildFontAtlas(ImFontAtlas& atlas) {
  gpu::Texture texture;
  gpu::Extent2d size;
  gpu::Status uploaded = uploadFontTexture(atlas, texture, size);
  if (uploaded.hasError()) {
    return uploaded;
  }

  gpu::Result<gpu::TextureView> view =
      device_->createTextureView(texture, gpu::TextureViewDescriptor{"uiFontAtlas"});
  if (view.hasError()) {
    return std::move(view).error();
  }
  gpu::TextureView replacementView = std::move(view).result();

  gpu::Result<UiTextureId> registered = registry_->registerTexture(
      UiTextureDescriptor{replacementView, size, UiTextureAlphaMode::Straight});
  if (registered.hasError()) {
    return std::move(registered).error();
  }
  const UiTextureId replacementId = registered.result();

  if (fontAtlasTexture_.isValid()) {
    const UiTextureId oldId = fontAtlasTexture_;
    gpu::Status retired = registry_->retire(fontAtlasTexture_);
    if (retired.hasError()) {
      // The replacement registration is already visible to the registry. Retire it before
      // returning the original failure, and retain its backing even if that rollback itself
      // fails, so no live registration can observe destroyed local handles.
      std::ignore = registry_->retire(replacementId);
      retainTextureBackingUntilReleased(replacementId, std::move(texture),
                                        std::move(replacementView));
      return retired;
    }
    retainTextureBackingUntilReleased(oldId, std::move(fontAtlasTextureResource_),
                                      std::move(fontAtlasView_));
  }

  fontAtlasTextureResource_ = std::move(texture);
  fontAtlasView_ = std::move(replacementView);
  fontAtlasTexture_ = replacementId;
  atlas.SetTexID(fontAtlasTexture_.imTextureId());
  return gpu::OkStatus();
}

namespace {

/// The renderer installed on \ref gInstalledContext.
///
/// Deliberately not `ImGuiIO::BackendRendererUserData`: while UI code that has not migrated still
/// calls the previous renderer backend's functions, that backend would read whatever occupies its
/// own slot as its own data type. Keeping this pointer separate leaves those calls seeing no
/// backend and doing nothing, instead of interpreting an unrelated object.
ImGuiContext* gInstalledContext = nullptr;
ImGuiRuntimeRenderer* gInstalledRenderer = nullptr;

}  // namespace

void ImGuiRuntimeRenderer::install() {
  ImGuiIO& io = ImGui::GetIO();
  gInstalledContext = ImGui::GetCurrentContext();
  gInstalledRenderer = this;
  io.BackendRendererName = "donner_gpu_runtime";
  // Draw lists are packed into one vertex range per frame and reached with a base vertex, so a
  // list past 64k vertices does not have to be split.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
}

void ImGuiRuntimeRenderer::uninstall() {
  if (gInstalledRenderer != this) {
    return;
  }
  if (ImGui::GetCurrentContext() != nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
  }
  gInstalledContext = nullptr;
  gInstalledRenderer = nullptr;
  importDevice_ = nullptr;
}

ImGuiRuntimeRenderer* CurrentImGuiRuntimeRenderer() {
  if (ImGui::GetCurrentContext() == nullptr || gInstalledContext != ImGui::GetCurrentContext()) {
    return nullptr;
  }
  return gInstalledRenderer;
}

UiTextureRegistry* CurrentUiTextureRegistry() {
  ImGuiRuntimeRenderer* renderer = CurrentImGuiRuntimeRenderer();
  return renderer != nullptr ? &renderer->registry() : nullptr;
}

std::vector<UiTextureId> ImGuiRuntimeRenderer::advanceFrame() {
  std::vector<UiTextureId> released = registry_->advanceFrame();
  for (const UiTextureId id : released) {
    std::erase_if(textureBindings_, [id](const TextureBinding& cached) { return cached.id == id; });
  }
  for (const UiTextureId id : released) {
    std::erase_if(retiredTextureBackings_,
                  [id](const RetiredTextureBacking& backing) { return backing.id == id; });
  }
  return released;
}

void ImGuiRuntimeRenderer::retainTextureBackingUntilReleased(UiTextureId id, gpu::Texture texture,
                                                             gpu::TextureView view) {
  retiredTextureBackings_.push_back(RetiredTextureBacking{id, std::move(texture), std::move(view)});
}

void ImGuiRuntimeRenderer::resetRendererState() {
  textureBindings_.clear();
}

gpu::Status ImGuiRuntimeRenderer::ensureCapacity(gpu::Buffer& buffer, uint64_t& capacityBytes,
                                                 uint64_t requiredBytes, uint64_t maxBytes,
                                                 gpu::BufferUsage usage, const char* label) {
  if (requiredBytes > maxBytes) {
    return gpu::GpuError{gpu::GpuErrorType::LimitExceeded,
                         "UI frame geometry exceeds the uiDrawVertices or uiDrawIndices bound"};
  }
  if (capacityBytes >= requiredBytes && buffer.isValid()) {
    return gpu::OkStatus();
  }

  const uint64_t capacity = std::min(GrownCapacity(capacityBytes, requiredBytes, 4096), maxBytes);
  gpu::Result<gpu::Buffer> created =
      device_->createBuffer(gpu::BufferDescriptor{label, capacity, usage});
  if (created.hasError()) {
    return std::move(created).error();
  }

  buffer = std::move(created).result();
  capacityBytes = capacity;
  return gpu::OkStatus();
}

gpu::Status ImGuiRuntimeRenderer::writeProjection(const ImDrawData& drawData) {
  const float left = drawData.DisplayPos.x;
  const float right = drawData.DisplayPos.x + drawData.DisplaySize.x;
  const float top = drawData.DisplayPos.y;
  const float bottom = drawData.DisplayPos.y + drawData.DisplaySize.y;

  const float columns[16] = {2.0f / (right - left),
                             0.0f,
                             0.0f,
                             0.0f,
                             0.0f,
                             2.0f / (top - bottom),
                             0.0f,
                             0.0f,
                             0.0f,
                             0.0f,
                             0.5f,
                             0.0f,
                             (right + left) / (left - right),
                             (top + bottom) / (bottom - top),
                             0.5f,
                             1.0f};
  UiDrawParams params = {};
  std::memcpy(params.clipFromLogical, columns, sizeof(columns));
  return device_->writeBuffer(uniformBuffer_, 0, BytesOf(params));
}

gpu::Result<const gpu::BindGroup*> ImGuiRuntimeRenderer::bindGroupFor(
    UiTextureId id, const UiTextureBinding& binding) {
  for (const TextureBinding& cached : textureBindings_) {
    if (cached.id == id) {
      return &cached.bindGroup;
    }
  }

  gpu::Result<gpu::BindGroup> created = device_->createBindGroup(gpu::BindGroupDescriptor{
      "uiDraw",
      bindGroupLayout_,
      {gpu::BindGroupEntry{0, gpu::BufferBinding{uniformBuffer_, 0, sizeof(UiDrawParams)}},
       gpu::BindGroupEntry{1, gpu::SamplerBinding{sampler_}},
       gpu::BindGroupEntry{2, gpu::TextureViewBinding{binding.view}}}});
  if (created.hasError()) {
    return std::move(created).error();
  }

  textureBindings_.push_back(TextureBinding{id, std::move(created).result()});
  return &textureBindings_.back().bindGroup;
}

gpu::Status ImGuiRuntimeRenderer::uploadGeometry(const ImDrawData& drawData,
                                                 FrameGeometry& geometry) {
  const std::optional<uint64_t> vertexBytes =
      gpu::CheckedMul(static_cast<uint64_t>(drawData.TotalVtxCount), sizeof(ImDrawVert));
  const std::optional<uint64_t> indexBytes =
      gpu::CheckedMul(static_cast<uint64_t>(drawData.TotalIdxCount), sizeof(ImDrawIdx));
  if (!vertexBytes.has_value() || !indexBytes.has_value()) {
    return gpu::GpuError{gpu::GpuErrorType::LimitExceeded, "UI frame geometry size overflows"};
  }
  // An odd index count is two bytes short of the four a buffer upload must be a multiple of, so
  // the payload is padded rather than refused.
  const uint64_t paddedIndexBytes = AlignedUploadBytes(*indexBytes);

  gpu::Status vertexCapacity =
      ensureCapacity(vertexBuffer_, vertexCapacityBytes_, *vertexBytes, kMaxVertexBytes,
                     gpu::BufferUsage::Vertex | gpu::BufferUsage::CopyDst, "uiDrawVertices");
  if (vertexCapacity.hasError()) {
    return vertexCapacity;
  }
  gpu::Status indexCapacity =
      ensureCapacity(indexBuffer_, indexCapacityBytes_, paddedIndexBytes, kMaxIndexBytes,
                     gpu::BufferUsage::Index | gpu::BufferUsage::CopyDst, "uiDrawIndices");
  if (indexCapacity.hasError()) {
    return indexCapacity;
  }

  frameVertexCount_ = static_cast<uint32_t>(drawData.TotalVtxCount);
  frameIndexCount_ = static_cast<uint32_t>(drawData.TotalIdxCount);
  vertexStaging_.clear();
  indexStaging_.clear();
  geometry.baseVertex.assign(static_cast<size_t>(drawData.CmdListsCount), 0);
  geometry.firstIndex.assign(static_cast<size_t>(drawData.CmdListsCount), 0);
  for (int listIndex = 0; listIndex < drawData.CmdListsCount; ++listIndex) {
    const ImDrawList* list = drawData.CmdLists[listIndex];
    for (const ImDrawCmd& command : list->CmdBuffer) {
      if (command.UserCallback == nullptr && command.ElemCount != 0) {
        if (gpu::Status valid = ValidateCommandIndices(command, *list); valid.hasError()) {
          return valid;
        }
      }
    }
    geometry.baseVertex[static_cast<size_t>(listIndex)] =
        static_cast<int32_t>(vertexStaging_.size() / sizeof(ImDrawVert));
    geometry.firstIndex[static_cast<size_t>(listIndex)] =
        static_cast<uint32_t>(indexStaging_.size() / sizeof(ImDrawIdx));

    const auto* vertices = reinterpret_cast<const uint8_t*>(list->VtxBuffer.Data);
    vertexStaging_.insert(
        vertexStaging_.end(), vertices,
        vertices + static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert));
    const auto* indices = reinterpret_cast<const uint8_t*>(list->IdxBuffer.Data);
    indexStaging_.insert(indexStaging_.end(), indices,
                         indices + static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
  }

  indexStaging_.resize(static_cast<size_t>(paddedIndexBytes), 0);
  gpu::Status vertexWritten = device_->writeBuffer(vertexBuffer_, 0, vertexStaging_);
  if (vertexWritten.hasError()) {
    return vertexWritten;
  }
  return device_->writeBuffer(indexBuffer_, 0, indexStaging_);
}

gpu::Status ImGuiRuntimeRenderer::bindFrameState(gpu::RenderPassEncoder& pass,
                                                 float framebufferWidth, float framebufferHeight) {
  gpu::Status viewport =
      pass.setViewport(0.0f, 0.0f, framebufferWidth, framebufferHeight, 0.0f, 1.0f);
  if (viewport.hasError()) {
    return viewport;
  }
  gpu::Status vertexBound = pass.setVertexBuffer(0, vertexBuffer_, 0);
  if (vertexBound.hasError()) {
    return vertexBound;
  }
  return pass.setIndexBuffer(indexBuffer_, gpu::IndexFormat::Uint16, 0);
}

gpu::Status ImGuiRuntimeRenderer::selectPipeline(gpu::RenderPassEncoder& pass,
                                                 UiTextureAlphaMode alphaMode,
                                                 const gpu::RenderPipeline** activePipeline) {
  const gpu::RenderPipeline& pipeline = alphaMode == UiTextureAlphaMode::Premultiplied
                                            ? premultipliedAlphaPipeline_
                                            : straightAlphaPipeline_;
  if (*activePipeline == &pipeline) {
    return gpu::OkStatus();
  }
  gpu::Status bound = pass.setPipeline(pipeline);
  if (bound.hasError()) {
    return bound;
  }
  *activePipeline = &pipeline;
  return gpu::OkStatus();
}

gpu::Status ImGuiRuntimeRenderer::recordCommand(const ImDrawCmd& command,
                                                const ImDrawData& drawData,
                                                gpu::RenderPassEncoder& pass,
                                                const gpu::Extent2d& targetSizePx,
                                                int32_t listBaseVertex, uint32_t listFirstIndex,
                                                const gpu::RenderPipeline** activePipeline) {
  const std::optional<ScissorRect> scissor = ScissorFor(command, drawData, targetSizePx);
  if (command.ElemCount == 0 || !scissor.has_value()) {
    return gpu::OkStatus();
  }

  const UiTextureId textureId = UiTextureId::FromImTextureId(command.GetTexID());
  gpu::Result<UiTextureBinding> binding = registry_->lookup(textureId);
  if (binding.hasError()) {
    return std::move(binding).error();
  }

  gpu::Status pipeline = selectPipeline(pass, binding.result().alphaMode, activePipeline);
  if (pipeline.hasError()) {
    return pipeline;
  }

  // Reached only after the lookup above accepted the registration, which is what lets a cached
  // binding be dropped with its registration: a retired one never gets this far.
  gpu::Result<const gpu::BindGroup*> group = bindGroupFor(textureId, binding.result());
  if (group.hasError()) {
    return std::move(group).error();
  }
  gpu::Status bound = pass.setBindGroup(0, *group.result());
  if (bound.hasError()) {
    return bound;
  }

  gpu::Status scissorSet =
      pass.setScissorRect(scissor->x, scissor->y, scissor->width, scissor->height);
  if (scissorSet.hasError()) {
    return scissorSet;
  }

  gpu::Result<DrawRange> range =
      DrawRangeFor(command, listBaseVertex, listFirstIndex, frameVertexCount_, frameIndexCount_);
  if (range.hasError()) {
    return std::move(range).error();
  }
  return pass.drawIndexed(command.ElemCount, 1, range.result().firstIndex,
                          range.result().baseVertex, 0);
}

gpu::Status ImGuiRuntimeRenderer::recordCommandLists(
    const ImDrawData& drawData, gpu::RenderPassEncoder& pass, const gpu::Extent2d& targetSizePx,
    const FrameGeometry& geometry, float framebufferWidth, float framebufferHeight) {
  const gpu::RenderPipeline* activePipeline = nullptr;
  // A command this renderer cannot record is skipped rather than ending the frame, and the first
  // such failure is returned once every other command has been recorded. One draw naming a
  // texture the registry refuses therefore costs that draw, not the whole interface.
  std::optional<gpu::GpuError> firstError;
  for (int listIndex = 0; listIndex < drawData.CmdListsCount; ++listIndex) {
    const ImDrawList* list = drawData.CmdLists[listIndex];
    for (int commandIndex = 0; commandIndex < list->CmdBuffer.Size; ++commandIndex) {
      const ImDrawCmd& command = list->CmdBuffer[commandIndex];
      if (command.UserCallback == ImDrawCallback_ResetRenderState) {
        activePipeline = nullptr;
        // A failed rebind leaves the pass without the state the commands after it need, so it
        // ends the frame instead of being skipped.
        gpu::Status reset = bindFrameState(pass, framebufferWidth, framebufferHeight);
        if (reset.hasError()) {
          return reset;
        }
        continue;
      }

      gpu::Status recorded =
          command.UserCallback != nullptr
              ? gpu::Status(gpu::GpuError{
                    gpu::GpuErrorType::Unsupported,
                    "UI draw command carries a user callback this renderer does not run"})
              : recordCommand(command, drawData, pass, targetSizePx,
                              geometry.baseVertex[static_cast<size_t>(listIndex)],
                              geometry.firstIndex[static_cast<size_t>(listIndex)], &activePipeline);
      if (recorded.hasError() && !firstError.has_value()) {
        firstError = recorded.error();
      }
    }
  }
  if (firstError.has_value()) {
    return *firstError;
  }
  return gpu::OkStatus();
}

gpu::Status ImGuiRuntimeRenderer::render(const ImDrawData& drawData, gpu::RenderPassEncoder& pass,
                                         const gpu::Extent2d& targetSizePx) {
  const float framebufferWidth = drawData.DisplaySize.x * drawData.FramebufferScale.x;
  const float framebufferHeight = drawData.DisplaySize.y * drawData.FramebufferScale.y;
  const bool emptyFrame =
      drawData.CmdListsCount <= 0 || drawData.TotalVtxCount <= 0 || drawData.TotalIdxCount <= 0;
  if (framebufferWidth <= 0.0f || framebufferHeight <= 0.0f || emptyFrame) {
    return gpu::OkStatus();
  }

  FrameGeometry geometry;
  gpu::Status uploaded = uploadGeometry(drawData, geometry);
  if (uploaded.hasError()) {
    return uploaded;
  }
  gpu::Status projection = writeProjection(drawData);
  if (projection.hasError()) {
    return projection;
  }
  gpu::Status frameState = bindFrameState(pass, framebufferWidth, framebufferHeight);
  if (frameState.hasError()) {
    return frameState;
  }
  return recordCommandLists(drawData, pass, targetSizePx, geometry, framebufferWidth,
                            framebufferHeight);
}

}  // namespace donner::editor
