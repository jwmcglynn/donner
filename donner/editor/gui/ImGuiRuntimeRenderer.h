#pragma once
/// @file
/// \c donner::editor::ImGuiRuntimeRenderer - draws UI draw data through the GPU runtime.

#include <cstdint>
#include <memory>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/gui/UiTextureRegistry.h"
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/GpuResult.h"
#include "donner/gpu/Handles.h"

namespace donner::gpu {
class Device;
class RenderPassEncoder;
}  // namespace donner::gpu

namespace donner::gpu::shader {
struct CompiledShaderView;
}  // namespace donner::gpu::shader

namespace donner::editor {

/**
 * Draws one frame of UI draw data through \c donner::gpu.
 *
 * The renderer owns the compiled UI draw program, one pipeline per alpha interpretation, the
 * per-frame vertex and index buffers, the projection uniform, the sampler, and the font atlas it
 * registers. It resolves the texture of every draw command through a \ref UiTextureRegistry, so a
 * command naming a stale, foreign or retired registration is refused before any GPU work is
 * recorded rather than reaching a backend as an unchecked address.
 *
 * Vertex and index storage grows to fit a frame and never shrinks within a frame, which matches
 * how the UI layer itself grows its draw lists. Growth is bounded: a frame needing more than
 * \ref kMaxVertexBytes or \ref kMaxIndexBytes is refused with
 * \ref gpu::GpuErrorType::LimitExceeded instead of allocating whatever the draw data asked for.
 */
class ImGuiRuntimeRenderer {
public:
  /// Largest vertex payload one frame may upload.
  static constexpr uint64_t kMaxVertexBytes = uint64_t(64) * 1024 * 1024;
  /// Largest index payload one frame may upload.
  static constexpr uint64_t kMaxIndexBytes = uint64_t(32) * 1024 * 1024;

  /**
   * Creates a renderer drawing into \p targetFormat attachments.
   *
   * @param device Device that owns every resource created here; must outlive the renderer.
   * @param registry Registry every draw command's texture is resolved through; must outlive the
   *   renderer.
   * @param targetFormat Color attachment format the pipelines are built for.
   */
  static gpu::Result<std::unique_ptr<ImGuiRuntimeRenderer>> Create(gpu::Device& device,
                                                                   UiTextureRegistry& registry,
                                                                   gpu::TextureFormat targetFormat);

  /// Destructor.
  ~ImGuiRuntimeRenderer();

  ImGuiRuntimeRenderer(const ImGuiRuntimeRenderer&) = delete;
  ImGuiRuntimeRenderer& operator=(const ImGuiRuntimeRenderer&) = delete;
  ImGuiRuntimeRenderer(ImGuiRuntimeRenderer&&) = delete;
  ImGuiRuntimeRenderer& operator=(ImGuiRuntimeRenderer&&) = delete;

  /**
   * Uploads \p atlas as the font texture, registers it, and publishes its identifier as the
   * atlas texture id. Replaces and retires a previously built atlas, so rebuilding after a font
   * change does not strand the old registration.
   *
   * @param atlas Font atlas whose RGBA32 pixels are uploaded.
   */
  gpu::Status buildFontAtlas(ImFontAtlas& atlas);

  /**
   * Records \p drawData into \p pass. Nothing is recorded for empty draw data or a degenerate
   * framebuffer.
   *
   * A command this renderer cannot record is skipped and the rest of the frame is still
   * recorded; the first such failure is returned once the frame is complete. A draw naming a
   * texture the registry refuses, or carrying a user callback other than the render-state reset,
   * therefore costs that draw rather than the whole interface, and still reports why. A failure
   * that leaves the pass unusable for the commands after it - exceeding the upload bounds, or a
   * failed state rebind - ends the frame instead.
   *
   * @param drawData Draw data for the frame.
   * @param pass Open render pass whose attachment matches the renderer's target format.
   * @param targetSizePx Attachment extent in device pixels; scissor rectangles are clamped to it.
   */
  gpu::Status render(const ImDrawData& drawData, gpu::RenderPassEncoder& pass,
                     const gpu::Extent2d& targetSizePx);

  /**
   * Advances one presentation frame: releases the registrations whose retirement frames have
   * passed and drops the bind group cached for each, so a cache entry and its device bind group
   * do not outlive the registration they were built for. Returns the released identifiers so the
   * caller can drop whatever backing it held for them.
   */
  std::vector<UiTextureId> advanceFrame();

  /**
   * Drops the cached per-texture bind groups and the recorded pipeline selection, so the next
   * \ref render rebinds from scratch. The buffers and their capacity are retained.
   */
  void resetRendererState();

  /// Number of bind groups currently cached, one per registration drawn since the last reset.
  size_t cachedBindingCount() const { return textureBindings_.size(); }

  /**
   * Publishes this renderer on the current ImGui context, so UI code that owns a texture can
   * reach the registry its draw commands are resolved through without every panel being handed
   * one.
   *
   * Transitional: the renderer is held in its own pointer rather than the context's
   * renderer-backend slot, because UI code that still calls the previous backend's functions
   * would read whatever occupies that slot as its own data type. This reverts to the idiomatic
   * slot once the last caller of the previous backend is gone.
   *
   * The caller keeps ownership; \ref uninstall must run before the renderer is destroyed.
   */
  void install();

  /// Stops publishing this renderer on the current ImGui context, if it is the published one.
  void uninstall();

  /// Identifier of the registered font atlas, null until \ref buildFontAtlas succeeds.
  UiTextureId fontAtlasTexture() const { return fontAtlasTexture_; }

  /// Registry this renderer resolves draw commands through.
  UiTextureRegistry& registry() const { return *registry_; }

  /// Device this renderer draws on. A texture registered for it must belong to this device, which
  /// is not necessarily the device its pixels were rendered on.
  gpu::Device& device() const { return *device_; }

  /// Bytes currently allocated for vertices.
  uint64_t vertexCapacityBytes() const { return vertexCapacityBytes_; }

  /// Bytes currently allocated for indices.
  uint64_t indexCapacityBytes() const { return indexCapacityBytes_; }

private:
  /// One cached bind group and the registration it was built for.
  struct TextureBinding {
    UiTextureId id;            //!< Registration the group samples.
    gpu::BindGroup bindGroup;  //!< Group holding the uniform, sampler and that view.
  };

  /// Where each draw list's geometry starts within the frame's combined buffers.
  struct FrameGeometry {
    std::vector<int32_t> baseVertex;   //!< First vertex of each list.
    std::vector<uint32_t> firstIndex;  //!< First index of each list.
  };

  /// Constructs an empty renderer; \ref Create fills it in.
  /// @param device Owning device. @param registry Registry draw commands resolve through.
  ImGuiRuntimeRenderer(gpu::Device& device, UiTextureRegistry& registry);

  /// Creates the shader module, layouts, pipelines and shared resources.
  /// @param targetFormat Color attachment format.
  gpu::Status initialize(gpu::TextureFormat targetFormat);

  /// Creates the shader module and the bind group and pipeline layouts.
  /// @param shader Compiled artifact the layouts are reflected from.
  gpu::Status createProgram(const gpu::shader::CompiledShaderView& shader);

  /// Creates one pipeline per alpha interpretation.
  /// @param targetFormat Color attachment format.
  gpu::Status createPipelines(gpu::TextureFormat targetFormat);

  /// Creates the sampler and the projection uniform buffer.
  gpu::Status createSharedResources();

  /// Uploads \p atlas into a new texture.
  /// @param atlas Font atlas to upload. @param texture Receives the created texture.
  /// @param size Receives the atlas extent.
  gpu::Status uploadFontTexture(ImFontAtlas& atlas, gpu::Texture& texture, gpu::Extent2d& size);

  /// Concatenates every draw list into the frame's vertex and index buffers.
  /// @param drawData Draw data for the frame. @param geometry Receives per-list start offsets.
  gpu::Status uploadGeometry(const ImDrawData& drawData, FrameGeometry& geometry);

  /// Applies the viewport and the frame's vertex and index bindings.
  /// @param pass Open render pass. @param framebufferWidth Viewport width in device pixels.
  /// @param framebufferHeight Viewport height in device pixels.
  gpu::Status bindFrameState(gpu::RenderPassEncoder& pass, float framebufferWidth,
                             float framebufferHeight);

  /// Selects the pipeline for \p alphaMode, recording a change only when it differs.
  /// @param pass Open render pass. @param alphaMode Alpha interpretation to draw with.
  /// @param activePipeline Pipeline currently recorded, updated in place.
  gpu::Status selectPipeline(gpu::RenderPassEncoder& pass, UiTextureAlphaMode alphaMode,
                             const gpu::RenderPipeline** activePipeline);

  /// Records every draw list's commands.
  /// @param drawData Draw data for the frame. @param pass Open render pass.
  /// @param targetSizePx Attachment extent in device pixels. @param geometry Per-list offsets.
  /// @param framebufferWidth Viewport width. @param framebufferHeight Viewport height.
  gpu::Status recordCommandLists(const ImDrawData& drawData, gpu::RenderPassEncoder& pass,
                                 const gpu::Extent2d& targetSizePx, const FrameGeometry& geometry,
                                 float framebufferWidth, float framebufferHeight);

  /// Records one draw command.
  /// @param command Command to record. @param drawData Draw data for the frame.
  /// @param pass Open render pass. @param targetSizePx Attachment extent in device pixels.
  /// @param listBaseVertex First vertex of the command's list. @param listFirstIndex First index
  ///   of the command's list. @param activePipeline Pipeline currently recorded, updated in place.
  gpu::Status recordCommand(const ImDrawCmd& command, const ImDrawData& drawData,
                            gpu::RenderPassEncoder& pass, const gpu::Extent2d& targetSizePx,
                            int32_t listBaseVertex, uint32_t listFirstIndex,
                            const gpu::RenderPipeline** activePipeline);

  /// Grows \p buffer to hold \p requiredBytes, refusing past \p maxBytes.
  /// @param buffer Buffer to resize in place. @param capacityBytes Current capacity, updated.
  /// @param requiredBytes Bytes this frame needs. @param maxBytes Refusal bound.
  /// @param usage Usage flags the buffer is recreated with. @param label Diagnostic label.
  gpu::Status ensureCapacity(gpu::Buffer& buffer, uint64_t& capacityBytes, uint64_t requiredBytes,
                             uint64_t maxBytes, gpu::BufferUsage usage, const char* label);

  /// Returns the bind group sampling \p binding's view, creating and caching it on first use.
  /// The cache is keyed by \p id so \ref advanceFrame can drop it with its registration.
  /// @param id Registration the binding was resolved from.
  /// @param binding Validated registration to bind.
  gpu::Result<const gpu::BindGroup*> bindGroupFor(UiTextureId id, const UiTextureBinding& binding);

  /// Writes the projection that maps logical UI coordinates to clip space.
  /// @param drawData Draw data whose display rectangle defines the projection.
  gpu::Status writeProjection(const ImDrawData& drawData);

  gpu::Device* device_;
  UiTextureRegistry* registry_;

  gpu::ShaderModule shaderModule_;
  gpu::BindGroupLayout bindGroupLayout_;
  gpu::PipelineLayout pipelineLayout_;
  gpu::RenderPipeline straightAlphaPipeline_;
  gpu::RenderPipeline premultipliedAlphaPipeline_;
  gpu::Sampler sampler_;
  gpu::Buffer uniformBuffer_;

  gpu::Buffer vertexBuffer_;
  gpu::Buffer indexBuffer_;
  uint64_t vertexCapacityBytes_ = 0;
  uint64_t indexCapacityBytes_ = 0;
  /// Geometry the frame being recorded uploaded, so a draw range that leaves it is refused.
  uint32_t frameVertexCount_ = 0;
  uint32_t frameIndexCount_ = 0;

  gpu::Texture fontAtlasTextureResource_;
  gpu::TextureView fontAtlasView_;
  UiTextureId fontAtlasTexture_;

  std::vector<TextureBinding> textureBindings_;
  std::vector<uint8_t> vertexStaging_;
  std::vector<uint8_t> indexStaging_;
};

/**
 * Returns the renderer installed on the current ImGui context, or null when the UI is not drawing
 * through the GPU runtime, which is the case for the desktop build that still presents through
 * OpenGL.
 */
ImGuiRuntimeRenderer* CurrentImGuiRuntimeRenderer();

/**
 * Returns the texture registry of the renderer installed on the current ImGui context, or null
 * when there is none. UI code registers the textures it wants drawn through this.
 */
UiTextureRegistry* CurrentUiTextureRegistry();

}  // namespace donner::editor
