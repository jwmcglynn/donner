/**
 * @file
 * Stub wgpu-native extensions header for Emscripten builds.
 *
 * Under native builds the real `wgpu.h` ships inside the wgpu-native prebuilt
 * archive. This stub provides the same type/function declarations so that the
 * vendored `webgpu.hpp` wrapper compiles under Emscripten (where Dawn's
 * `webgpu.h` is the authoritative header, provided by the emdawnwebgpu package).
 *
 * Compatibility notes:
 *  - Dawn's webgpu.h does not have `WGPUChainedStructOut`; we typedef it.
 *  - Dawn already declares `wgpuRenderPassEncoderMultiDrawIndirect` and
 *    `wgpuRenderPassEncoderMultiDrawIndexedIndirect` with 6 parameters (count
 *    buffer built-in), whereas wgpu-native uses 4. We redirect the 4-arg
 *    calls to private shim wrappers via macros so webgpu.hpp compiles.
 *  - Function bodies are in `wgpu_native_stubs.cc`.
 */
#ifndef WGPU_H_
#define WGPU_H_

#include <webgpu/webgpu.h>

/* ── Dawn compatibility: missing wgpu-native enum values ────────────── */
/* webgpu.hpp references enum values that exist in wgpu-native's webgpu.h
 * but not in Dawn's. Define them here with values that don't collide with
 * Dawn's existing assignments. These are never returned by the browser's
 * WebGPU implementation — they exist only for compile-time compatibility. */

/* MapAsyncStatus — Dawn has Success(1), CallbackCancelled(2), Error(3), Aborted(4) */
#define WGPUMapAsyncStatus_InstanceDropped ((WGPUMapAsyncStatus)0x00000005)
#define WGPUMapAsyncStatus_Unknown ((WGPUMapAsyncStatus)0x00000006)

/* PopErrorScopeStatus — Dawn has Success(1), CallbackCancelled(2), Error(3) */
#define WGPUPopErrorScopeStatus_InstanceDropped ((WGPUPopErrorScopeStatus)0x00000004)
#define WGPUPopErrorScopeStatus_EmptyStack ((WGPUPopErrorScopeStatus)0x00000005)

/* QueueWorkDoneStatus — Dawn has Success(1), CallbackCancelled(2), Error(3) */
#define WGPUQueueWorkDoneStatus_InstanceDropped ((WGPUQueueWorkDoneStatus)0x00000004)
#define WGPUQueueWorkDoneStatus_Unknown ((WGPUQueueWorkDoneStatus)0x00000005)

/* RequestAdapterStatus — Dawn has Success(1), CallbackCancelled(2), Unavailable(3), Error(4) */
#define WGPURequestAdapterStatus_InstanceDropped ((WGPURequestAdapterStatus)0x00000005)
#define WGPURequestAdapterStatus_Unknown ((WGPURequestAdapterStatus)0x00000006)

/* RequestDeviceStatus — Dawn has Success(1), CallbackCancelled(2), Error(3) */
#define WGPURequestDeviceStatus_InstanceDropped ((WGPURequestDeviceStatus)0x00000004)
#define WGPURequestDeviceStatus_Unknown ((WGPURequestDeviceStatus)0x00000005)

/* CompilationInfoRequestStatus — Dawn has Success(1), CallbackCancelled(2) */
#define WGPUCompilationInfoRequestStatus_Error ((WGPUCompilationInfoRequestStatus)0x00000003)
#define WGPUCompilationInfoRequestStatus_InstanceDropped \
    ((WGPUCompilationInfoRequestStatus)0x00000004)
#define WGPUCompilationInfoRequestStatus_Unknown ((WGPUCompilationInfoRequestStatus)0x00000005)

/* CreatePipelineAsyncStatus — Dawn has Success(1), CC(2), ValidationError(3), InternalError(4) */
#define WGPUCreatePipelineAsyncStatus_InstanceDropped ((WGPUCreatePipelineAsyncStatus)0x00000005)
#define WGPUCreatePipelineAsyncStatus_Unknown ((WGPUCreatePipelineAsyncStatus)0x00000006)

/* DeviceLostReason — Dawn has Unknown(1), Destroyed(2), CC(3), FailedCreation(4) */
#define WGPUDeviceLostReason_InstanceDropped ((WGPUDeviceLostReason)0x00000005)

/* SurfaceGetCurrentTextureStatus — Dawn goes up to Lost(5), Error(6) */
#define WGPUSurfaceGetCurrentTextureStatus_OutOfMemory \
    ((WGPUSurfaceGetCurrentTextureStatus)0x00000007)
#define WGPUSurfaceGetCurrentTextureStatus_DeviceLost \
    ((WGPUSurfaceGetCurrentTextureStatus)0x00000008)

/* VertexStepMode — Dawn has Undefined(0), Vertex(1), Instance(2) */
#define WGPUVertexStepMode_VertexBufferNotUsed ((WGPUVertexStepMode)0x00000003)

/* WaitStatus — Dawn has Success(1), TimedOut(2), Error(3) */
#define WGPUWaitStatus_UnsupportedTimeout ((WGPUWaitStatus)0x00000004)
#define WGPUWaitStatus_UnsupportedCount ((WGPUWaitStatus)0x00000005)
#define WGPUWaitStatus_UnsupportedMixedSources ((WGPUWaitStatus)0x00000006)

/* FeatureName — Dawn starts at 1 (CoreFeaturesAndLimits) */
#define WGPUFeatureName_Undefined ((WGPUFeatureName)0x00000000)

/* ── Dawn compatibility: WGPUChainedStructOut ───────────────────────── */

/* Dawn's webgpu.h uses WGPUChainedStruct for output chains; wgpu-native has
 * a separate WGPUChainedStructOut. Alias so structs compile. */
typedef WGPUChainedStruct WGPUChainedStructOut;

/* ── Dawn compatibility: multi-draw function signatures ─────────────── */
/* Dawn already declares these with 6 params (includes count-buffer args).
 * webgpu.hpp calls them with 4 params (wgpu-native style). Redirect the
 * names to private shims that either no-op or forward to Dawn's 6-arg
 * versions with NULL count-buffer defaults. */

#define wgpuRenderPassEncoderMultiDrawIndirect \
    wgpuRenderPassEncoderMultiDrawIndirect_wgpuNativeCompat
#define wgpuRenderPassEncoderMultiDrawIndexedIndirect \
    wgpuRenderPassEncoderMultiDrawIndexedIndirect_wgpuNativeCompat

/* ── Dawn compatibility: function signature differences ─────────────── */
/* wgpu-native returns WGPUAdapterInfo by value; Dawn takes a pointer param
 * and returns WGPUStatus. Redirect to a compat shim. */
#define wgpuDeviceGetAdapterInfo wgpuDeviceGetAdapterInfo_wgpuNativeCompat

/* wgpu-native returns WGPUStatus; Dawn returns void. */
#define wgpuInstanceGetWGSLLanguageFeatures \
    wgpuInstanceGetWGSLLanguageFeatures_wgpuNativeCompat

/* ── Enumerations ───────────────────────────────────────────────────── */

typedef enum WGPUNativeSType {
    WGPUSType_DeviceExtras = 0x00030001,
    WGPUSType_NativeLimits = 0x00030002,
    WGPUSType_PipelineLayoutExtras = 0x00030003,
    WGPUSType_ShaderModuleGLSLDescriptor = 0x00030004,
    WGPUSType_InstanceExtras = 0x00030006,
    WGPUSType_BindGroupEntryExtras = 0x00030007,
    WGPUSType_BindGroupLayoutEntryExtras = 0x00030008,
    WGPUSType_QuerySetDescriptorExtras = 0x00030009,
    WGPUSType_SurfaceConfigurationExtras = 0x0003000A,
    WGPUNativeSType_Force32 = 0x7FFFFFFF
} WGPUNativeSType;

typedef enum WGPUNativeFeature {
    WGPUNativeFeature_PushConstants = 0x00030001,
    WGPUNativeFeature_TextureAdapterSpecificFormatFeatures = 0x00030002,
    WGPUNativeFeature_MultiDrawIndirect = 0x00030003,
    WGPUNativeFeature_MultiDrawIndirectCount = 0x00030004,
    WGPUNativeFeature_VertexWritableStorage = 0x00030005,
    WGPUNativeFeature_TextureBindingArray = 0x00030006,
    WGPUNativeFeature_SampledTextureAndStorageBufferArrayNonUniformIndexing = 0x00030007,
    WGPUNativeFeature_PipelineStatisticsQuery = 0x00030008,
    WGPUNativeFeature_StorageResourceBindingArray = 0x00030009,
    WGPUNativeFeature_PartiallyBoundBindingArray = 0x0003000A,
    WGPUNativeFeature_TextureFormat16bitNorm = 0x0003000B,
    WGPUNativeFeature_TextureCompressionAstcHdr = 0x0003000C,
    WGPUNativeFeature_MappablePrimaryBuffers = 0x0003000E,
    WGPUNativeFeature_BufferBindingArray = 0x0003000F,
    WGPUNativeFeature_UniformBufferAndStorageTextureArrayNonUniformIndexing = 0x00030010,
    WGPUNativeFeature_SpirvShaderPassthrough = 0x00030017,
    WGPUNativeFeature_VertexAttribute64bit = 0x00030019,
    WGPUNativeFeature_TextureFormatNv12 = 0x0003001A,
    WGPUNativeFeature_RayTracingAccelerationStructure = 0x0003001B,
    WGPUNativeFeature_RayQuery = 0x0003001C,
    WGPUNativeFeature_ShaderF64 = 0x0003001D,
    WGPUNativeFeature_ShaderI16 = 0x0003001E,
    WGPUNativeFeature_ShaderPrimitiveIndex = 0x0003001F,
    WGPUNativeFeature_ShaderEarlyDepthTest = 0x00030020,
    WGPUNativeFeature_Subgroup = 0x00030021,
    WGPUNativeFeature_SubgroupVertex = 0x00030022,
    WGPUNativeFeature_SubgroupBarrier = 0x00030023,
    WGPUNativeFeature_TimestampQueryInsideEncoders = 0x00030024,
    WGPUNativeFeature_TimestampQueryInsidePasses = 0x00030025,
    WGPUNativeFeature_Force32 = 0x7FFFFFFF
} WGPUNativeFeature;

typedef enum WGPULogLevel {
    WGPULogLevel_Off = 0x00000000,
    WGPULogLevel_Error = 0x00000001,
    WGPULogLevel_Warn = 0x00000002,
    WGPULogLevel_Info = 0x00000003,
    WGPULogLevel_Debug = 0x00000004,
    WGPULogLevel_Trace = 0x00000005,
    WGPULogLevel_Force32 = 0x7FFFFFFF
} WGPULogLevel;

typedef WGPUFlags WGPUInstanceBackend;
static const WGPUInstanceBackend WGPUInstanceBackend_All = 0x00000000;
static const WGPUInstanceBackend WGPUInstanceBackend_Vulkan = 1 << 0;
static const WGPUInstanceBackend WGPUInstanceBackend_GL = 1 << 1;
static const WGPUInstanceBackend WGPUInstanceBackend_Metal = 1 << 2;
static const WGPUInstanceBackend WGPUInstanceBackend_DX12 = 1 << 3;
static const WGPUInstanceBackend WGPUInstanceBackend_DX11 = 1 << 4;
static const WGPUInstanceBackend WGPUInstanceBackend_BrowserWebGPU = 1 << 5;
static const WGPUInstanceBackend WGPUInstanceBackend_Primary =
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5);
static const WGPUInstanceBackend WGPUInstanceBackend_Secondary = (1 << 1) | (1 << 4);
static const WGPUInstanceBackend WGPUInstanceBackend_Force32 = 0x7FFFFFFF;

typedef WGPUFlags WGPUInstanceFlag;
static const WGPUInstanceFlag WGPUInstanceFlag_Default = 0x00000000;
static const WGPUInstanceFlag WGPUInstanceFlag_Debug = 1 << 0;
static const WGPUInstanceFlag WGPUInstanceFlag_Validation = 1 << 1;
static const WGPUInstanceFlag WGPUInstanceFlag_DiscardHalLabels = 1 << 2;
static const WGPUInstanceFlag WGPUInstanceFlag_Force32 = 0x7FFFFFFF;

typedef enum WGPUDx12Compiler {
    WGPUDx12Compiler_Undefined = 0x00000000,
    WGPUDx12Compiler_Fxc = 0x00000001,
    WGPUDx12Compiler_Dxc = 0x00000002,
    WGPUDx12Compiler_Force32 = 0x7FFFFFFF
} WGPUDx12Compiler;

typedef enum WGPUGles3MinorVersion {
    WGPUGles3MinorVersion_Automatic = 0x00000000,
    WGPUGles3MinorVersion_Version0 = 0x00000001,
    WGPUGles3MinorVersion_Version1 = 0x00000002,
    WGPUGles3MinorVersion_Version2 = 0x00000003,
    WGPUGles3MinorVersion_Force32 = 0x7FFFFFFF
} WGPUGles3MinorVersion;

typedef enum WGPUPipelineStatisticName {
    WGPUPipelineStatisticName_VertexShaderInvocations = 0x00000000,
    WGPUPipelineStatisticName_ClipperInvocations = 0x00000001,
    WGPUPipelineStatisticName_ClipperPrimitivesOut = 0x00000002,
    WGPUPipelineStatisticName_FragmentShaderInvocations = 0x00000003,
    WGPUPipelineStatisticName_ComputeShaderInvocations = 0x00000004,
    WGPUPipelineStatisticName_Force32 = 0x7FFFFFFF
} WGPUPipelineStatisticName;

typedef enum WGPUNativeQueryType {
    WGPUNativeQueryType_PipelineStatistics = 0x00030000,
    WGPUNativeQueryType_Force32 = 0x7FFFFFFF
} WGPUNativeQueryType;

typedef enum WGPUNativeTextureFormat {
    WGPUNativeTextureFormat_R16Unorm = 0x00030001,
    WGPUNativeTextureFormat_R16Snorm = 0x00030002,
    WGPUNativeTextureFormat_Rg16Unorm = 0x00030003,
    WGPUNativeTextureFormat_Rg16Snorm = 0x00030004,
    WGPUNativeTextureFormat_Rgba16Unorm = 0x00030005,
    WGPUNativeTextureFormat_Rgba16Snorm = 0x00030006,
    WGPUNativeTextureFormat_NV12 = 0x00030007,
} WGPUNativeTextureFormat;

/* ── Dawn compatibility: missing platform/feature structs ────────────── */
/* wgpu-native's webgpu.h defines platform surface-source structs and
 * timestamp-write helpers that Dawn omits. Add stubs so webgpu.hpp's
 * STRUCT() wrappers compile. None of these are used at runtime under
 * Emscripten — the browser's own surface model applies. */

typedef struct WGPUComputePassTimestampWrites {
    WGPUQuerySet querySet;
    uint32_t beginningOfPassWriteIndex;
    uint32_t endOfPassWriteIndex;
} WGPUComputePassTimestampWrites;

typedef struct WGPURenderPassTimestampWrites {
    WGPUQuerySet querySet;
    uint32_t beginningOfPassWriteIndex;
    uint32_t endOfPassWriteIndex;
} WGPURenderPassTimestampWrites;

typedef struct WGPUSurfaceSourceAndroidNativeWindow {
    WGPUChainedStruct chain;
    void * window;
} WGPUSurfaceSourceAndroidNativeWindow;

typedef struct WGPUSurfaceSourceMetalLayer {
    WGPUChainedStruct chain;
    void * layer;
} WGPUSurfaceSourceMetalLayer;

typedef struct WGPUSurfaceSourceWaylandSurface {
    WGPUChainedStruct chain;
    void * display;
    void * surface;
} WGPUSurfaceSourceWaylandSurface;

typedef struct WGPUSurfaceSourceWindowsHWND {
    WGPUChainedStruct chain;
    void * hinstance;
    void * hwnd;
} WGPUSurfaceSourceWindowsHWND;

typedef struct WGPUSurfaceSourceXCBWindow {
    WGPUChainedStruct chain;
    void * connection;
    uint32_t window;
} WGPUSurfaceSourceXCBWindow;

typedef struct WGPUSurfaceSourceXlibWindow {
    WGPUChainedStruct chain;
    void * display;
    uint64_t window;
} WGPUSurfaceSourceXlibWindow;

typedef struct WGPUInstanceCapabilities {
    WGPUChainedStructOut * nextInChain;
    WGPUBool timedWaitAnyEnable;
    size_t timedWaitAnyMaxCount;
} WGPUInstanceCapabilities;

typedef struct WGPUProgrammableStageDescriptor {
    WGPUChainedStruct const * nextInChain;
    WGPUShaderModule module;
    WGPUStringView entryPoint;
    size_t constantCount;
    WGPUConstantEntry const * constants;
} WGPUProgrammableStageDescriptor;

/* ── Structures (wgpu-native extensions) ───────────────────────────── */

typedef struct WGPUInstanceExtras {
    WGPUChainedStruct chain;
    WGPUInstanceBackend backends;
    WGPUInstanceFlag flags;
    WGPUDx12Compiler dx12ShaderCompiler;
    WGPUGles3MinorVersion gles3MinorVersion;
    WGPUStringView dxilPath;
    WGPUStringView dxcPath;
} WGPUInstanceExtras;

typedef struct WGPUDeviceExtras {
    WGPUChainedStruct chain;
    WGPUStringView tracePath;
} WGPUDeviceExtras;

typedef struct WGPUNativeLimits {
    WGPUChainedStructOut chain;
    uint32_t maxPushConstantSize;
    uint32_t maxNonSamplerBindings;
} WGPUNativeLimits;

typedef struct WGPUPushConstantRange {
    WGPUShaderStage stages;
    uint32_t start;
    uint32_t end;
} WGPUPushConstantRange;

typedef struct WGPUPipelineLayoutExtras {
    WGPUChainedStruct chain;
    size_t pushConstantRangeCount;
    WGPUPushConstantRange const * pushConstantRanges;
} WGPUPipelineLayoutExtras;

typedef uint64_t WGPUSubmissionIndex;

typedef struct WGPUShaderDefine {
    WGPUStringView name;
    WGPUStringView value;
} WGPUShaderDefine;

typedef struct WGPUShaderModuleGLSLDescriptor {
    WGPUChainedStruct chain;
    WGPUShaderStage stage;
    WGPUStringView code;
    uint32_t defineCount;
    WGPUShaderDefine * defines;
} WGPUShaderModuleGLSLDescriptor;

typedef struct WGPUShaderModuleDescriptorSpirV {
    WGPUStringView label;
    uint32_t sourceSize;
    uint32_t const * source;
} WGPUShaderModuleDescriptorSpirV;

typedef struct WGPURegistryReport {
   size_t numAllocated;
   size_t numKeptFromUser;
   size_t numReleasedFromUser;
   size_t elementSize;
} WGPURegistryReport;

typedef struct WGPUHubReport {
    WGPURegistryReport adapters;
    WGPURegistryReport devices;
    WGPURegistryReport queues;
    WGPURegistryReport pipelineLayouts;
    WGPURegistryReport shaderModules;
    WGPURegistryReport bindGroupLayouts;
    WGPURegistryReport bindGroups;
    WGPURegistryReport commandBuffers;
    WGPURegistryReport renderBundles;
    WGPURegistryReport renderPipelines;
    WGPURegistryReport computePipelines;
    WGPURegistryReport pipelineCaches;
    WGPURegistryReport querySets;
    WGPURegistryReport buffers;
    WGPURegistryReport textures;
    WGPURegistryReport textureViews;
    WGPURegistryReport samplers;
} WGPUHubReport;

typedef struct WGPUGlobalReport {
    WGPURegistryReport surfaces;
    WGPUHubReport hub;
} WGPUGlobalReport;

typedef struct WGPUInstanceEnumerateAdapterOptions {
    WGPUChainedStruct const * nextInChain;
    WGPUInstanceBackend backends;
} WGPUInstanceEnumerateAdapterOptions;

typedef struct WGPUBindGroupEntryExtras {
    WGPUChainedStruct chain;
    WGPUBuffer const * buffers;
    size_t bufferCount;
    WGPUSampler const * samplers;
    size_t samplerCount;
    WGPUTextureView const * textureViews;
    size_t textureViewCount;
} WGPUBindGroupEntryExtras;

typedef struct WGPUBindGroupLayoutEntryExtras {
    WGPUChainedStruct chain;
    uint32_t count;
} WGPUBindGroupLayoutEntryExtras;

typedef struct WGPUQuerySetDescriptorExtras {
    WGPUChainedStruct chain;
    WGPUPipelineStatisticName const * pipelineStatistics;
    size_t pipelineStatisticCount;
} WGPUQuerySetDescriptorExtras;

typedef struct WGPUSurfaceConfigurationExtras {
    WGPUChainedStruct chain;
    uint32_t desiredMaximumFrameLatency;
} WGPUSurfaceConfigurationExtras;

typedef void (*WGPULogCallback)(WGPULogLevel level, WGPUStringView message, void * userdata);

/* ── Functions (stubs — see wgpu_native_stubs.cc) ───────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

void wgpuGenerateReport(WGPUInstance instance, WGPUGlobalReport * report);
size_t wgpuInstanceEnumerateAdapters(
    WGPUInstance instance,
    WGPU_NULLABLE WGPUInstanceEnumerateAdapterOptions const * options,
    WGPUAdapter * adapters);

WGPUSubmissionIndex wgpuQueueSubmitForIndex(
    WGPUQueue queue, size_t commandCount, WGPUCommandBuffer const * commands);

WGPUBool wgpuDevicePoll(
    WGPUDevice device, WGPUBool wait,
    WGPU_NULLABLE WGPUSubmissionIndex const * wrappedSubmissionIndex);
WGPUShaderModule wgpuDeviceCreateShaderModuleSpirV(
    WGPUDevice device, WGPUShaderModuleDescriptorSpirV const * descriptor);

void wgpuSetLogCallback(WGPULogCallback callback, void * userdata);
void wgpuSetLogLevel(WGPULogLevel level);
uint32_t wgpuGetVersion(void);

void wgpuRenderPassEncoderSetPushConstants(
    WGPURenderPassEncoder encoder, WGPUShaderStage stages,
    uint32_t offset, uint32_t sizeBytes, void const * data);
void wgpuComputePassEncoderSetPushConstants(
    WGPUComputePassEncoder encoder, uint32_t offset,
    uint32_t sizeBytes, void const * data);
void wgpuRenderBundleEncoderSetPushConstants(
    WGPURenderBundleEncoder encoder, WGPUShaderStage stages,
    uint32_t offset, uint32_t sizeBytes, void const * data);

/* wgpu-native's 4-arg multi-draw signatures (redirected via macro above). */
void wgpuRenderPassEncoderMultiDrawIndirect_wgpuNativeCompat(
    WGPURenderPassEncoder encoder, WGPUBuffer buffer,
    uint64_t offset, uint32_t count);
void wgpuRenderPassEncoderMultiDrawIndexedIndirect_wgpuNativeCompat(
    WGPURenderPassEncoder encoder, WGPUBuffer buffer,
    uint64_t offset, uint32_t count);

/* wgpu-native signature: returns AdapterInfo by value. Dawn uses 2-arg. */
WGPUAdapterInfo wgpuDeviceGetAdapterInfo_wgpuNativeCompat(WGPUDevice device);

/* wgpu-native signature: returns WGPUStatus. Dawn returns void. */
WGPUStatus wgpuInstanceGetWGSLLanguageFeatures_wgpuNativeCompat(
    WGPUInstance instance, WGPUSupportedWGSLLanguageFeatures * features);

/* wgpu-native has this; Dawn stubs it in JS but it's not used at runtime. */
WGPUStatus wgpuGetInstanceCapabilities(WGPUInstanceCapabilities * capabilities);

void wgpuRenderPassEncoderMultiDrawIndirectCount(
    WGPURenderPassEncoder encoder, WGPUBuffer buffer,
    uint64_t offset, WGPUBuffer count_buffer,
    uint64_t count_buffer_offset, uint32_t max_count);
void wgpuRenderPassEncoderMultiDrawIndexedIndirectCount(
    WGPURenderPassEncoder encoder, WGPUBuffer buffer,
    uint64_t offset, WGPUBuffer count_buffer,
    uint64_t count_buffer_offset, uint32_t max_count);

void wgpuComputePassEncoderBeginPipelineStatisticsQuery(
    WGPUComputePassEncoder computePassEncoder,
    WGPUQuerySet querySet, uint32_t queryIndex);
void wgpuComputePassEncoderEndPipelineStatisticsQuery(
    WGPUComputePassEncoder computePassEncoder);
void wgpuRenderPassEncoderBeginPipelineStatisticsQuery(
    WGPURenderPassEncoder renderPassEncoder,
    WGPUQuerySet querySet, uint32_t queryIndex);
void wgpuRenderPassEncoderEndPipelineStatisticsQuery(
    WGPURenderPassEncoder renderPassEncoder);

void wgpuComputePassEncoderWriteTimestamp(
    WGPUComputePassEncoder computePassEncoder,
    WGPUQuerySet querySet, uint32_t queryIndex);
void wgpuRenderPassEncoderWriteTimestamp(
    WGPURenderPassEncoder renderPassEncoder,
    WGPUQuerySet querySet, uint32_t queryIndex);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WGPU_H_ */
