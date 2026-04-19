/**
 * @file
 * Stub implementations of wgpu-native extension functions for Emscripten.
 *
 * Browser WebGPU does not expose the wgpu-native extension API. This
 * translation unit provides link-time stubs so that the vendored
 * `webgpu.hpp` wrapper (which wraps these functions) compiles and links
 * under Emscripten. The critical function is `wgpuDevicePoll` which
 * delegates to `emscripten_sleep` so that ASYNCIFY-based waiting works
 * for buffer-map and device-readback paths.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <webgpu/webgpu.h>

// Include wgpu.h for types but NOT the multi-draw rename macros — we need
// to define the renamed symbols directly below.
#include <webgpu/wgpu.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

extern "C" {

WGPUBool wgpuDevicePoll(WGPUDevice /*device*/, WGPUBool /*wait*/,
                         WGPUSubmissionIndex const* /*wrappedSubmissionIndex*/) {
  // On the browser, GPU work completes via the microtask queue. Yield to
  // the event loop so callbacks can fire (requires ASYNCIFY).
#ifdef __EMSCRIPTEN__
  emscripten_sleep(0);
#endif
  return 1;  // "queue empty"
}

WGPUShaderModule wgpuDeviceCreateShaderModuleSpirV(
    WGPUDevice /*device*/,
    WGPUShaderModuleDescriptorSpirV const* /*descriptor*/) {
  // SPIR-V passthrough is not supported in browser WebGPU.
  return nullptr;
}

WGPUSubmissionIndex wgpuQueueSubmitForIndex(WGPUQueue queue,
                                            size_t commandCount,
                                            WGPUCommandBuffer const* commands) {
  // Delegate to the standard submit and return a dummy index.
  wgpuQueueSubmit(queue, commandCount, commands);
  return 0;
}

void wgpuGenerateReport(WGPUInstance /*instance*/,
                         WGPUGlobalReport* report) {
  if (report) {
    std::memset(report, 0, sizeof(*report));
  }
}

size_t wgpuInstanceEnumerateAdapters(
    WGPUInstance /*instance*/,
    WGPUInstanceEnumerateAdapterOptions const* /*options*/,
    WGPUAdapter* /*adapters*/) {
  return 0;
}

void wgpuSetLogCallback(WGPULogCallback /*callback*/, void* /*userdata*/) {}
void wgpuSetLogLevel(WGPULogLevel /*level*/) {}
uint32_t wgpuGetVersion(void) { return 0; }

void wgpuRenderPassEncoderSetPushConstants(WGPURenderPassEncoder, WGPUShaderStage,
                                           uint32_t, uint32_t, void const*) {}
void wgpuComputePassEncoderSetPushConstants(WGPUComputePassEncoder, uint32_t,
                                            uint32_t, void const*) {}
void wgpuRenderBundleEncoderSetPushConstants(WGPURenderBundleEncoder, WGPUShaderStage,
                                             uint32_t, uint32_t, void const*) {}

// 4-arg wgpu-native-compatible shims (macro-renamed in wgpu.h).
void wgpuRenderPassEncoderMultiDrawIndirect_wgpuNativeCompat(
    WGPURenderPassEncoder, WGPUBuffer, uint64_t, uint32_t) {}
void wgpuRenderPassEncoderMultiDrawIndexedIndirect_wgpuNativeCompat(
    WGPURenderPassEncoder, WGPUBuffer, uint64_t, uint32_t) {}

// wgpu-native returns WGPUAdapterInfo by value; Dawn's signature takes a
// pointer. This compat shim adapts to Dawn's 2-arg version.
extern WGPUStatus wgpuDeviceGetAdapterInfo_dawn(WGPUDevice device,
                                                 WGPUAdapterInfo* info);
// Rename Dawn's actual symbol at link time via asm labels below.
WGPUAdapterInfo wgpuDeviceGetAdapterInfo_wgpuNativeCompat(WGPUDevice device) {
    WGPUAdapterInfo info = {};
    // The Dawn implementation is exposed in JS, not as a C symbol we can call
    // directly under the renamed name. For proof-of-life, return empty info.
    (void)device;
    return info;
}

WGPUStatus wgpuInstanceGetWGSLLanguageFeatures_wgpuNativeCompat(
    WGPUInstance /*instance*/, WGPUSupportedWGSLLanguageFeatures* /*features*/) {
    // Dawn returns void; wgpu-native returns Status. Return success.
    return (WGPUStatus)1; // WGPUStatus_Success
}

WGPUStatus wgpuGetInstanceCapabilities(WGPUInstanceCapabilities* capabilities) {
    if (capabilities) {
        capabilities->nextInChain = nullptr;
        capabilities->timedWaitAnyEnable = 0;
        capabilities->timedWaitAnyMaxCount = 0;
    }
    return (WGPUStatus)1; // WGPUStatus_Success
}

void wgpuRenderPassEncoderMultiDrawIndirectCount(WGPURenderPassEncoder, WGPUBuffer,
                                                 uint64_t, WGPUBuffer,
                                                 uint64_t, uint32_t) {}
void wgpuRenderPassEncoderMultiDrawIndexedIndirectCount(WGPURenderPassEncoder, WGPUBuffer,
                                                        uint64_t, WGPUBuffer,
                                                        uint64_t, uint32_t) {}

void wgpuComputePassEncoderBeginPipelineStatisticsQuery(WGPUComputePassEncoder,
                                                        WGPUQuerySet, uint32_t) {}
void wgpuComputePassEncoderEndPipelineStatisticsQuery(WGPUComputePassEncoder) {}
void wgpuRenderPassEncoderBeginPipelineStatisticsQuery(WGPURenderPassEncoder,
                                                       WGPUQuerySet, uint32_t) {}
void wgpuRenderPassEncoderEndPipelineStatisticsQuery(WGPURenderPassEncoder) {}

void wgpuComputePassEncoderWriteTimestamp(WGPUComputePassEncoder,
                                          WGPUQuerySet, uint32_t) {}
void wgpuRenderPassEncoderWriteTimestamp(WGPURenderPassEncoder,
                                         WGPUQuerySet, uint32_t) {}

}  // extern "C"
