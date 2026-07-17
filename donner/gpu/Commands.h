#pragma once
/// @file
/// Validated command value types recorded by \c donner::gpu::CommandEncoder.
///
/// Commands store only validated Donner value objects and slot-based resource identifiers, never
/// raw pointers or native handles (design 0053 "Command model"), so recorded streams serialize
/// deterministically and contain no process state.

#include <cstdint>
#include <variant>

#include "donner/gpu/Descriptors.h"

namespace donner::gpu {

/// Recorded `beginRenderPass`. Attachment references inside the descriptor are validated before
/// recording.
struct BeginRenderPassCommand {
  RenderPassDescriptor descriptor;  //!< Validated pass descriptor.
};

/// Recorded `setPipeline`.
struct SetPipelineCommand {
  uint32_t pipelineSlot = 0;  //!< Render pipeline slot index.
};

/// Recorded `setBindGroup`.
struct SetBindGroupCommand {
  uint32_t index = 0;          //!< Bind group index.
  uint32_t bindGroupSlot = 0;  //!< Bind group slot index.
};

/// Recorded `setVertexBuffer`.
struct SetVertexBufferCommand {
  uint32_t slot = 0;         //!< Vertex buffer slot index in the pipeline layout.
  uint32_t bufferSlot = 0;   //!< Buffer slot index.
  uint64_t offsetBytes = 0;  //!< Byte offset of the first element.
};

/// Recorded `setScissorRect`.
struct SetScissorRectCommand {
  uint32_t x = 0;       //!< Left edge in pixels.
  uint32_t y = 0;       //!< Top edge in pixels.
  uint32_t width = 0;   //!< Width in pixels.
  uint32_t height = 0;  //!< Height in pixels.
};

/// Recorded `setViewport`.
struct SetViewportCommand {
  float x = 0;         //!< Left edge in pixels.
  float y = 0;         //!< Top edge in pixels.
  float width = 0;     //!< Width in pixels.
  float height = 0;    //!< Height in pixels.
  float minDepth = 0;  //!< Minimum depth of the viewport range.
  float maxDepth = 1;  //!< Maximum depth of the viewport range.
};

/// Recorded `draw`.
struct DrawCommand {
  uint32_t vertexCount = 0;    //!< Number of vertices.
  uint32_t instanceCount = 1;  //!< Number of instances.
  uint32_t firstVertex = 0;    //!< First vertex index.
  uint32_t firstInstance = 0;  //!< First instance index.
};

/// Recorded render pass end.
struct EndRenderPassCommand {};

/// Recorded `copyTextureToBuffer` (readback staging copy).
struct CopyTextureToBufferCommand {
  uint32_t textureSlot = 0;      //!< Source texture slot index.
  uint32_t bufferSlot = 0;       //!< Destination buffer slot index.
  TexelCopyBufferLayout layout;  //!< Destination row layout.
  Extent2d copySize;             //!< Copy extent in texels.
};

/// One recorded command.
using Command = std::variant<BeginRenderPassCommand, SetPipelineCommand, SetBindGroupCommand,
                             SetVertexBufferCommand, SetScissorRectCommand, SetViewportCommand,
                             DrawCommand, EndRenderPassCommand, CopyTextureToBufferCommand>;

}  // namespace donner::gpu
