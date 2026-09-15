#include "donner/gpu/browser/EmscriptenBrowserBridge.h"

#include <emscripten/emscripten.h>

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "donner/gpu/browser/BrowserWireCodes.h"

namespace donner::gpu::browser {

// The browser side of each of these lives in `library_donner_gpu.js`, which is linked with
// `--js-library`. Each returns one of the BridgeStatus values below; the two sides agree on those
// numbers and on the descriptor codes in BrowserWireCodes.h, and nothing else.
//
// Structured descriptors are described one item at a time rather than handed over as a packed
// buffer, matching how recorded commands are replayed: the browser side then has no length,
// offset or arity arithmetic to get wrong, because every item it sees is one it was called for.
extern "C" {

int donner_gpu_check_protocol(const unsigned int* codes, int count);
void donner_gpu_release_device();
int donner_gpu_begin_device_request();
int donner_gpu_device_request_state();
int donner_gpu_read_request_error(char* destination, int capacity);
int donner_gpu_owns_device();
int donner_gpu_is_device_lost();
int donner_gpu_read_lost_reason(char* destination, int capacity);
double donner_gpu_completed_serial();

int donner_gpu_create_buffer(unsigned int id, double byteSize, unsigned int usageBits);
int donner_gpu_create_texture(unsigned int id, unsigned int width, unsigned int height,
                              unsigned int formatCode, unsigned int usageBits);
int donner_gpu_create_texture_view(unsigned int id, unsigned int textureId);
int donner_gpu_create_sampler(unsigned int id, unsigned int magFilterCode,
                              unsigned int minFilterCode, unsigned int addressUCode,
                              unsigned int addressVCode);
int donner_gpu_create_shader_module(unsigned int id, const char* wgsl, int byteCount);
int donner_gpu_destroy_object(unsigned int kindCode, unsigned int id);

int donner_gpu_bind_group_layout_begin();
int donner_gpu_bind_group_layout_entry(unsigned int binding, unsigned int visibilityBits,
                                       unsigned int bindingTypeCode,
                                       unsigned int storageTextureFormat);
int donner_gpu_bind_group_layout_finish(unsigned int id);

int donner_gpu_bind_group_begin(unsigned int layoutId);
int donner_gpu_bind_group_entry(unsigned int binding, unsigned int resourceKindCode,
                                unsigned int resourceId, double offsetBytes, double sizeBytes);
int donner_gpu_bind_group_finish(unsigned int id);

int donner_gpu_pipeline_layout_begin();
int donner_gpu_pipeline_layout_group(unsigned int bindGroupLayoutId);
int donner_gpu_pipeline_layout_finish(unsigned int id);

int donner_gpu_render_pipeline_begin(unsigned int layoutId, unsigned int vertexModuleId,
                                     const char* vertexEntryPoint, int vertexEntryPointBytes,
                                     unsigned int fragmentModuleId, const char* fragmentEntryPoint,
                                     int fragmentEntryPointBytes, unsigned int topologyCode,
                                     unsigned int cullModeCode);
int donner_gpu_render_pipeline_vertex_buffer(unsigned int strideBytes, unsigned int stepModeCode);
int donner_gpu_render_pipeline_vertex_attribute(unsigned int formatCode, unsigned int offsetBytes,
                                                unsigned int shaderLocation);
int donner_gpu_render_pipeline_color_target(
    unsigned int formatCode, unsigned int blendEnabled, unsigned int colorSrcFactor,
    unsigned int colorDstFactor, unsigned int colorOperation, unsigned int alphaSrcFactor,
    unsigned int alphaDstFactor, unsigned int alphaOperation, unsigned int writeMaskBits);
int donner_gpu_render_pipeline_finish(unsigned int id);

int donner_gpu_create_compute_pipeline(unsigned int id, unsigned int layoutId,
                                       unsigned int moduleId, const char* entryPoint,
                                       int entryPointBytes);

int donner_gpu_write_buffer(unsigned int bufferId, double offsetBytes, const void* data,
                            double byteCount);
int donner_gpu_write_texture(unsigned int textureId, const void* data, double byteCount,
                             double layoutOffsetBytes, unsigned int bytesPerRow,
                             unsigned int rowsPerImage, unsigned int destinationX,
                             unsigned int destinationY, unsigned int width, unsigned int height);

int donner_gpu_begin_command_buffer(double submissionSerial);
int donner_gpu_begin_render_pass();
int donner_gpu_render_pass_attachment(unsigned int viewId, unsigned int loadOpCode,
                                      unsigned int storeOpCode, double clearRed, double clearGreen,
                                      double clearBlue, double clearAlpha);
int donner_gpu_begin_render_pass_finish();
int donner_gpu_end_render_pass();
int donner_gpu_begin_compute_pass();
int donner_gpu_end_compute_pass();
int donner_gpu_set_render_pipeline(unsigned int pipelineId);
int donner_gpu_set_compute_pipeline(unsigned int pipelineId);
int donner_gpu_set_bind_group(unsigned int index, unsigned int bindGroupId);
int donner_gpu_set_vertex_buffer(unsigned int slot, unsigned int bufferId, double offsetBytes);
int donner_gpu_set_index_buffer(unsigned int bufferId, unsigned int indexFormatCode,
                                double offsetBytes);
int donner_gpu_set_scissor_rect(unsigned int x, unsigned int y, unsigned int width,
                                unsigned int height);
int donner_gpu_set_viewport(double x, double y, double width, double height, double minDepth,
                            double maxDepth);
int donner_gpu_draw(unsigned int vertexCount, unsigned int instanceCount, unsigned int firstVertex,
                    unsigned int firstInstance);
int donner_gpu_draw_indexed(unsigned int indexCount, unsigned int instanceCount,
                            unsigned int firstIndex, int baseVertex, unsigned int firstInstance);
int donner_gpu_dispatch_workgroups(unsigned int countX, unsigned int countY, unsigned int countZ);
int donner_gpu_copy_texture_to_buffer(unsigned int textureId, unsigned int bufferId,
                                      double layoutOffsetBytes, unsigned int bytesPerRow,
                                      unsigned int rowsPerImage, unsigned int width,
                                      unsigned int height);
int donner_gpu_copy_texture_to_texture(unsigned int sourceTextureId,
                                       unsigned int destinationTextureId, unsigned int sourceX,
                                       unsigned int sourceY, unsigned int destinationX,
                                       unsigned int destinationY, unsigned int width,
                                       unsigned int height);
int donner_gpu_end_command_buffer(double submissionSerial);

int donner_gpu_map_buffer_async(unsigned int mappingId, unsigned int bufferId, double offsetBytes,
                                double byteCount);
int donner_gpu_mapping_state(unsigned int mappingId);
int donner_gpu_copy_mapped_bytes(unsigned int mappingId, void* destination, double byteCount);
int donner_gpu_unmap_buffer(unsigned int mappingId);

int donner_gpu_create_surface(unsigned int id, const char* canvasSelector, int selectorBytes);
int donner_gpu_surface_capabilities(unsigned int surfaceId, unsigned int* preferredFormatCode,
                                    unsigned int* usageBits);
int donner_gpu_configure_surface(unsigned int surfaceId, unsigned int formatCode,
                                 unsigned int usageBits, unsigned int width, unsigned int height,
                                 unsigned int alphaModeCode);
int donner_gpu_acquire_current_texture(unsigned int surfaceId, unsigned int textureId,
                                       unsigned int* surfaceStatusCode);
int donner_gpu_abandon_current_texture(unsigned int surfaceId);

}  // extern "C"

namespace {

/// Largest message this bridge reads back from the browser. A browser's own failure text is a
/// diagnostic, so it is truncated rather than allowed to size an allocation.
constexpr int kMaxMessageBytes = 512;

/// Translates a status the browser side returned. A code this protocol assigns no meaning becomes
/// \ref BridgeStatus::Failed rather than being cast into an enumerator, so a JavaScript half that
/// has drifted from this one refuses operations instead of appearing to succeed.
/// @param status Value the browser side returned.
BridgeStatus StatusFromBrowser(int status) {
  if (status < 0) {
    return BridgeStatus::Failed;
  }
  return BridgeStatusFromWire(static_cast<uint32_t>(status)).value_or(BridgeStatus::Failed);
}

/// Translates a device-request state the browser side returned.
/// @param state Value the browser side returned.
BrowserDeviceRequestState RequestStateFromBrowser(int state) {
  if (state < 0) {
    return BrowserDeviceRequestState::Failed;
  }
  return RequestStateFromWire(static_cast<uint32_t>(state))
      .value_or(BrowserDeviceRequestState::Failed);
}

/// Translates a mapping state the browser side returned.
/// @param state Value the browser side returned.
MapSliceState MappingStateFromBrowser(int state) {
  if (state < 0) {
    return MapSliceState::Failed;
  }
  return MapSliceStateFromWire(static_cast<uint32_t>(state)).value_or(MapSliceState::Failed);
}

/// Translates a surface outcome the browser side returned. An unassigned code becomes
/// \ref SurfaceStatus::Lost: a frame whose outcome cannot be read is not one to draw into, and
/// Lost is the outcome whose recovery - build a new surface - is safe to take when the report
/// itself is not trustworthy.
/// @param status Value the browser side returned.
SurfaceStatus SurfaceStatusFromBrowser(unsigned int status) {
  return SurfaceStatusFromWire(status).value_or(SurfaceStatus::Lost);
}

/// Reads a bounded message the browser side exposes through \p reader.
/// @param reader Entry point writing into a caller-provided buffer.
RcString ReadMessage(int (*reader)(char*, int)) {
  std::array<char, kMaxMessageBytes> buffer = {};
  const int written = reader(buffer.data(), static_cast<int>(buffer.size()));
  if (written <= 0) {
    return RcString();
  }
  const size_t length =
      static_cast<size_t>(written) < buffer.size() ? static_cast<size_t>(written) : buffer.size();
  return RcString(std::string_view(buffer.data(), length));
}

}  // namespace

EmscriptenBrowserBridge::EmscriptenBrowserBridge() = default;

EmscriptenBrowserBridge::~EmscriptenBrowserBridge() {
  // Hand the browser device back, so a later bridge in this same context starts from nothing
  // rather than inheriting this one's objects, its completed serial, or the fact that it was lost.
  donner_gpu_release_device();
}

BridgeStatus EmscriptenBrowserBridge::beginDeviceRequest() {
  // The two halves agree on what their numbers mean before anything is built on them. Checking
  // here rather than later is what keeps a disagreement from reaching a browser call as a value
  // that was read as something else: nothing else on this interface may run until the request is
  // ready, so this is the one place every later call is downstream of.
  const std::span<const uint32_t> codes = ProtocolCodeTable();
  if (const BridgeStatus status = StatusFromBrowser(
          donner_gpu_check_protocol(codes.data(), static_cast<int>(codes.size())));
      status != BridgeStatus::Success) {
    return status;
  }
  return StatusFromBrowser(donner_gpu_begin_device_request());
}

BrowserDeviceRequestState EmscriptenBrowserBridge::deviceRequestState() const {
  return RequestStateFromBrowser(donner_gpu_device_request_state());
}

RcString EmscriptenBrowserBridge::deviceRequestError() const {
  return ReadMessage(&donner_gpu_read_request_error);
}

bool EmscriptenBrowserBridge::ownsDevice() const {
  return donner_gpu_owns_device() != 0;
}

bool EmscriptenBrowserBridge::isDeviceLost() const {
  return donner_gpu_is_device_lost() != 0;
}

RcString EmscriptenBrowserBridge::deviceLostReason() const {
  return ReadMessage(&donner_gpu_read_lost_reason);
}

uint64_t EmscriptenBrowserBridge::completedSerial() const {
  const double serial = donner_gpu_completed_serial();
  return serial > 0.0 ? static_cast<uint64_t>(serial) : 0;
}

BridgeStatus EmscriptenBrowserBridge::createBuffer(BrowserObjectId id, uint64_t byteSize,
                                                   uint32_t usageBits) {
  return StatusFromBrowser(donner_gpu_create_buffer(id, static_cast<double>(byteSize), usageBits));
}

BridgeStatus EmscriptenBrowserBridge::createTexture(BrowserObjectId id, uint32_t width,
                                                    uint32_t height, uint32_t formatCode,
                                                    uint32_t usageBits) {
  return StatusFromBrowser(donner_gpu_create_texture(id, width, height, formatCode, usageBits));
}

BridgeStatus EmscriptenBrowserBridge::createTextureView(BrowserObjectId id,
                                                        BrowserObjectId textureId) {
  return StatusFromBrowser(donner_gpu_create_texture_view(id, textureId));
}

BridgeStatus EmscriptenBrowserBridge::createSampler(BrowserObjectId id, uint32_t magFilterCode,
                                                    uint32_t minFilterCode, uint32_t addressUCode,
                                                    uint32_t addressVCode) {
  return StatusFromBrowser(
      donner_gpu_create_sampler(id, magFilterCode, minFilterCode, addressUCode, addressVCode));
}

BridgeStatus EmscriptenBrowserBridge::createBindGroupLayout(
    BrowserObjectId id, std::span<const BrowserBindGroupLayoutEntry> entries) {
  if (const BridgeStatus status = StatusFromBrowser(donner_gpu_bind_group_layout_begin());
      status != BridgeStatus::Success) {
    return status;
  }
  for (const BrowserBindGroupLayoutEntry& entry : entries) {
    if (const BridgeStatus status = StatusFromBrowser(
            donner_gpu_bind_group_layout_entry(entry.binding, entry.visibilityBits,
                                               entry.bindingTypeCode, entry.storageTextureFormat));
        status != BridgeStatus::Success) {
      return status;
    }
  }
  return StatusFromBrowser(donner_gpu_bind_group_layout_finish(id));
}

BridgeStatus EmscriptenBrowserBridge::createBindGroup(
    BrowserObjectId id, BrowserObjectId layoutId, std::span<const BrowserBindGroupEntry> entries) {
  if (const BridgeStatus status = StatusFromBrowser(donner_gpu_bind_group_begin(layoutId));
      status != BridgeStatus::Success) {
    return status;
  }
  for (const BrowserBindGroupEntry& entry : entries) {
    if (const BridgeStatus status = StatusFromBrowser(donner_gpu_bind_group_entry(
            entry.binding, static_cast<unsigned int>(entry.resource), entry.resourceId,
            static_cast<double>(entry.offsetBytes), static_cast<double>(entry.sizeBytes)));
        status != BridgeStatus::Success) {
      return status;
    }
  }
  return StatusFromBrowser(donner_gpu_bind_group_finish(id));
}

BridgeStatus EmscriptenBrowserBridge::createPipelineLayout(
    BrowserObjectId id, std::span<const BrowserObjectId> groupLayoutIds) {
  if (const BridgeStatus status = StatusFromBrowser(donner_gpu_pipeline_layout_begin());
      status != BridgeStatus::Success) {
    return status;
  }
  for (const BrowserObjectId layoutId : groupLayoutIds) {
    if (const BridgeStatus status = StatusFromBrowser(donner_gpu_pipeline_layout_group(layoutId));
        status != BridgeStatus::Success) {
      return status;
    }
  }
  return StatusFromBrowser(donner_gpu_pipeline_layout_finish(id));
}

BridgeStatus EmscriptenBrowserBridge::createShaderModule(BrowserObjectId id,
                                                         std::string_view wgsl) {
  return StatusFromBrowser(
      donner_gpu_create_shader_module(id, wgsl.data(), static_cast<int>(wgsl.size())));
}

BridgeStatus EmscriptenBrowserBridge::createRenderPipeline(
    BrowserObjectId id, const BrowserRenderPipelineRequest& request) {
  const std::string vertexEntryPoint = request.vertexEntryPoint.str();
  const std::string fragmentEntryPoint = request.fragmentEntryPoint.str();
  if (const BridgeStatus status = StatusFromBrowser(donner_gpu_render_pipeline_begin(
          request.layoutId, request.vertexModuleId, vertexEntryPoint.data(),
          static_cast<int>(vertexEntryPoint.size()), request.fragmentModuleId,
          fragmentEntryPoint.data(), static_cast<int>(fragmentEntryPoint.size()),
          request.topologyCode, request.cullModeCode));
      status != BridgeStatus::Success) {
    return status;
  }

  for (const BrowserVertexBufferLayout& layout : request.vertexBuffers) {
    if (const BridgeStatus status = StatusFromBrowser(
            donner_gpu_render_pipeline_vertex_buffer(layout.strideBytes, layout.stepModeCode));
        status != BridgeStatus::Success) {
      return status;
    }
    for (const BrowserVertexAttribute& attribute : layout.attributes) {
      if (const BridgeStatus status = StatusFromBrowser(donner_gpu_render_pipeline_vertex_attribute(
              attribute.formatCode, attribute.offsetBytes, attribute.shaderLocation));
          status != BridgeStatus::Success) {
        return status;
      }
    }
  }

  for (const BrowserColorTarget& target : request.colorTargets) {
    if (const BridgeStatus status = StatusFromBrowser(donner_gpu_render_pipeline_color_target(
            target.formatCode, target.blendEnabled ? 1u : 0u, target.colorBlend.srcFactorCode,
            target.colorBlend.dstFactorCode, target.colorBlend.operationCode,
            target.alphaBlend.srcFactorCode, target.alphaBlend.dstFactorCode,
            target.alphaBlend.operationCode, target.writeMaskBits));
        status != BridgeStatus::Success) {
      return status;
    }
  }

  return StatusFromBrowser(donner_gpu_render_pipeline_finish(id));
}

BridgeStatus EmscriptenBrowserBridge::createComputePipeline(
    BrowserObjectId id, const BrowserComputePipelineRequest& request) {
  const std::string entryPoint = request.entryPoint.str();
  return StatusFromBrowser(donner_gpu_create_compute_pipeline(id, request.layoutId,
                                                              request.moduleId, entryPoint.data(),
                                                              static_cast<int>(entryPoint.size())));
}

BridgeStatus EmscriptenBrowserBridge::destroyObject(BrowserObjectKind kind, BrowserObjectId id) {
  mappings_.erase(id);
  const std::optional<uint32_t> kindCode = WireBrowserObjectKind(kind);
  if (!kindCode.has_value()) {
    return BridgeStatus::WrongObjectKind;
  }
  return StatusFromBrowser(donner_gpu_destroy_object(*kindCode, id));
}

BridgeStatus EmscriptenBrowserBridge::writeBuffer(BrowserObjectId bufferId, uint64_t offsetBytes,
                                                  std::span<const uint8_t> data) {
  return StatusFromBrowser(donner_gpu_write_buffer(bufferId, static_cast<double>(offsetBytes),
                                                   data.data(), static_cast<double>(data.size())));
}

BridgeStatus EmscriptenBrowserBridge::writeTexture(BrowserObjectId textureId,
                                                   std::span<const uint8_t> data,
                                                   const BrowserTexelLayout& layout,
                                                   const BrowserCopyRegion& region) {
  return StatusFromBrowser(donner_gpu_write_texture(
      textureId, data.data(), static_cast<double>(data.size()),
      static_cast<double>(layout.offsetBytes), layout.bytesPerRow, layout.rowsPerImage,
      region.destinationX, region.destinationY, region.width, region.height));
}

BridgeStatus EmscriptenBrowserBridge::beginCommandBuffer(uint64_t submissionSerial) {
  return StatusFromBrowser(donner_gpu_begin_command_buffer(static_cast<double>(submissionSerial)));
}

BridgeStatus EmscriptenBrowserBridge::beginRenderPass(
    std::span<const BrowserColorAttachment> colorAttachments) {
  if (const BridgeStatus status = StatusFromBrowser(donner_gpu_begin_render_pass());
      status != BridgeStatus::Success) {
    return status;
  }
  for (const BrowserColorAttachment& attachment : colorAttachments) {
    if (const BridgeStatus status = StatusFromBrowser(donner_gpu_render_pass_attachment(
            attachment.viewId, attachment.loadOpCode, attachment.storeOpCode,
            attachment.clearColor[0], attachment.clearColor[1], attachment.clearColor[2],
            attachment.clearColor[3]));
        status != BridgeStatus::Success) {
      return status;
    }
  }
  return StatusFromBrowser(donner_gpu_begin_render_pass_finish());
}

BridgeStatus EmscriptenBrowserBridge::endRenderPass() {
  return StatusFromBrowser(donner_gpu_end_render_pass());
}

BridgeStatus EmscriptenBrowserBridge::beginComputePass() {
  return StatusFromBrowser(donner_gpu_begin_compute_pass());
}

BridgeStatus EmscriptenBrowserBridge::endComputePass() {
  return StatusFromBrowser(donner_gpu_end_compute_pass());
}

BridgeStatus EmscriptenBrowserBridge::setRenderPipeline(BrowserObjectId pipelineId) {
  return StatusFromBrowser(donner_gpu_set_render_pipeline(pipelineId));
}

BridgeStatus EmscriptenBrowserBridge::setComputePipeline(BrowserObjectId pipelineId) {
  return StatusFromBrowser(donner_gpu_set_compute_pipeline(pipelineId));
}

BridgeStatus EmscriptenBrowserBridge::setBindGroup(uint32_t index, BrowserObjectId bindGroupId) {
  return StatusFromBrowser(donner_gpu_set_bind_group(index, bindGroupId));
}

BridgeStatus EmscriptenBrowserBridge::setVertexBuffer(uint32_t slot, BrowserObjectId bufferId,
                                                      uint64_t offsetBytes) {
  return StatusFromBrowser(
      donner_gpu_set_vertex_buffer(slot, bufferId, static_cast<double>(offsetBytes)));
}

BridgeStatus EmscriptenBrowserBridge::setIndexBuffer(BrowserObjectId bufferId,
                                                     uint32_t indexFormatCode,
                                                     uint64_t offsetBytes) {
  return StatusFromBrowser(
      donner_gpu_set_index_buffer(bufferId, indexFormatCode, static_cast<double>(offsetBytes)));
}

BridgeStatus EmscriptenBrowserBridge::setScissorRect(uint32_t x, uint32_t y, uint32_t width,
                                                     uint32_t height) {
  return StatusFromBrowser(donner_gpu_set_scissor_rect(x, y, width, height));
}

BridgeStatus EmscriptenBrowserBridge::setViewport(float x, float y, float width, float height,
                                                  float minDepth, float maxDepth) {
  return StatusFromBrowser(donner_gpu_set_viewport(x, y, width, height, minDepth, maxDepth));
}

BridgeStatus EmscriptenBrowserBridge::draw(uint32_t vertexCount, uint32_t instanceCount,
                                           uint32_t firstVertex, uint32_t firstInstance) {
  return StatusFromBrowser(donner_gpu_draw(vertexCount, instanceCount, firstVertex, firstInstance));
}

BridgeStatus EmscriptenBrowserBridge::drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                                  uint32_t firstIndex, int32_t baseVertex,
                                                  uint32_t firstInstance) {
  return StatusFromBrowser(
      donner_gpu_draw_indexed(indexCount, instanceCount, firstIndex, baseVertex, firstInstance));
}

BridgeStatus EmscriptenBrowserBridge::dispatchWorkgroups(uint32_t countX, uint32_t countY,
                                                         uint32_t countZ) {
  return StatusFromBrowser(donner_gpu_dispatch_workgroups(countX, countY, countZ));
}

BridgeStatus EmscriptenBrowserBridge::copyTextureToBuffer(BrowserObjectId textureId,
                                                          BrowserObjectId bufferId,
                                                          const BrowserTexelLayout& layout,
                                                          const BrowserCopyRegion& region) {
  return StatusFromBrowser(donner_gpu_copy_texture_to_buffer(
      textureId, bufferId, static_cast<double>(layout.offsetBytes), layout.bytesPerRow,
      layout.rowsPerImage, region.width, region.height));
}

BridgeStatus EmscriptenBrowserBridge::copyTextureToTexture(BrowserObjectId sourceTextureId,
                                                           BrowserObjectId destinationTextureId,
                                                           const BrowserCopyRegion& region) {
  return StatusFromBrowser(donner_gpu_copy_texture_to_texture(
      sourceTextureId, destinationTextureId, region.sourceX, region.sourceY, region.destinationX,
      region.destinationY, region.width, region.height));
}

BridgeStatus EmscriptenBrowserBridge::endCommandBuffer(uint64_t submissionSerial) {
  return StatusFromBrowser(donner_gpu_end_command_buffer(static_cast<double>(submissionSerial)));
}

BridgeStatus EmscriptenBrowserBridge::mapBufferAsync(BrowserObjectId mappingId,
                                                     BrowserObjectId bufferId, uint64_t offsetBytes,
                                                     uint64_t byteCount) {
  const BridgeStatus status = StatusFromBrowser(donner_gpu_map_buffer_async(
      mappingId, bufferId, static_cast<double>(offsetBytes), static_cast<double>(byteCount)));
  if (status == BridgeStatus::Success) {
    mappings_[mappingId] = MappedRange{byteCount, {}, false};
  }
  return status;
}

MapSliceState EmscriptenBrowserBridge::mappingState(BrowserObjectId mappingId) const {
  return MappingStateFromBrowser(donner_gpu_mapping_state(mappingId));
}

void EmscriptenBrowserBridge::yieldToBrowser(double seconds) {
  // emscripten_sleep unwinds and rewinds this thread through Asyncify, which is what lets the
  // browser run the promise callbacks that settle a mapping. A negative or absent budget still
  // yields once, because the point is to hand the loop back at all, not to wait a particular
  // length of time.
  //
  // Both WebAssembly binaries enable Asyncify today, but each does so for the transitional WebGPU
  // path: the editor's link line justifies it by that path's synchronous device creation and
  // buffer-map waits, and a second copy arrives with the bindings this bridge replaces. Whether a
  // shipped build keeps Asyncify for this yield, moves to stack switching, or makes readback
  // asynchronous so nothing waits here is a cutover decision that reaches renderer code outside
  // this package and is not made by this change.
  //
  // The duration is bounded again here rather than trusted. This is a public interface, so the
  // value reaching it is whatever a caller passed, and converting a double that does not fit the
  // unsigned argument would be undefined; the runtime clamps too, and neither side relies on the
  // other to have done it.
  static constexpr double kMaxSleepMilliseconds = 60.0 * 1000.0;
  double milliseconds = seconds > 0.0 ? seconds * 1000.0 : 0.0;
  if (!(milliseconds < kMaxSleepMilliseconds)) {
    milliseconds = kMaxSleepMilliseconds;
  }
  emscripten_sleep(static_cast<unsigned int>(milliseconds));
}

BridgeStatus EmscriptenBrowserBridge::mappedBytes(BrowserObjectId mappingId,
                                                  std::span<const uint8_t>& bytes) const {
  const auto it = mappings_.find(mappingId);
  if (it == mappings_.end()) {
    return BridgeStatus::UnknownObject;
  }
  if (!it->second.copied) {
    // The mapped range is copied out of the browser's heap into this module's own, so a readback
    // costs its own size again here; the runtime's 1 GiB buffer cap therefore bounds a single
    // mapping's copy at 1 GiB on top of the buffer itself. On a 32-bit heap a length that does not
    // fit a size_t cannot be allocated at all, so it is refused rather than truncated into one.
    if (it->second.byteCount > std::numeric_limits<size_t>::max()) {
      return BridgeStatus::Failed;
    }
    it->second.bytes.resize(static_cast<size_t>(it->second.byteCount));
    const BridgeStatus status = StatusFromBrowser(donner_gpu_copy_mapped_bytes(
        mappingId, it->second.bytes.data(), static_cast<double>(it->second.byteCount)));
    if (status != BridgeStatus::Success) {
      it->second.bytes.clear();
      return status;
    }
    it->second.copied = true;
  }
  bytes = std::span<const uint8_t>(it->second.bytes);
  return BridgeStatus::Success;
}

BridgeStatus EmscriptenBrowserBridge::unmapBuffer(BrowserObjectId mappingId) {
  mappings_.erase(mappingId);
  return StatusFromBrowser(donner_gpu_unmap_buffer(mappingId));
}

BridgeStatus EmscriptenBrowserBridge::createSurface(BrowserObjectId id,
                                                    std::string_view canvasSelector) {
  return StatusFromBrowser(donner_gpu_create_surface(id, canvasSelector.data(),
                                                     static_cast<int>(canvasSelector.size())));
}

BridgeStatus EmscriptenBrowserBridge::surfaceCapabilities(
    BrowserObjectId surfaceId, BrowserSurfaceCapabilities& capabilities) const {
  unsigned int preferredFormatCode = 0;
  unsigned int usageBits = 0;
  const BridgeStatus status = StatusFromBrowser(
      donner_gpu_surface_capabilities(surfaceId, &preferredFormatCode, &usageBits));
  if (status != BridgeStatus::Success) {
    return status;
  }

  // A browser canvas offers one preferred format and no choice of frame pacing: it presents when
  // the page is composited, which is what Fifo describes. Reporting the set that actually exists
  // is more useful than reporting a menu the browser does not have.
  capabilities.formatCodes.clear();
  if (preferredFormatCode != 0) {
    capabilities.formatCodes.push_back(preferredFormatCode);
  }
  capabilities.usageBits = usageBits;
  capabilities.presentModeCodes = {1};
  capabilities.alphaModeCodes = {1, 2};
  return BridgeStatus::Success;
}

BridgeStatus EmscriptenBrowserBridge::configureSurface(BrowserObjectId surfaceId,
                                                       uint32_t formatCode, uint32_t usageBits,
                                                       uint32_t width, uint32_t height,
                                                       uint32_t alphaModeCode) {
  return StatusFromBrowser(
      donner_gpu_configure_surface(surfaceId, formatCode, usageBits, width, height, alphaModeCode));
}

BridgeStatus EmscriptenBrowserBridge::acquireCurrentTexture(BrowserObjectId surfaceId,
                                                            BrowserObjectId textureId,
                                                            SurfaceStatus& status) {
  unsigned int surfaceStatusCode = 0;
  const BridgeStatus bridgeStatus = StatusFromBrowser(
      donner_gpu_acquire_current_texture(surfaceId, textureId, &surfaceStatusCode));
  if (bridgeStatus == BridgeStatus::Success) {
    status = SurfaceStatusFromBrowser(surfaceStatusCode);
  }
  return bridgeStatus;
}

BridgeStatus EmscriptenBrowserBridge::abandonCurrentTexture(BrowserObjectId surfaceId) {
  return StatusFromBrowser(donner_gpu_abandon_current_texture(surfaceId));
}

}  // namespace donner::gpu::browser
