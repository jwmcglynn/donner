#include "donner/editor/AsyncRenderer.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <thread>
#include <utility>

#ifdef DONNER_WASM_WORKER_SURFACE
#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>
#endif

#include "donner/base/Utils.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/RendererInterface.h"
#ifdef DONNER_WASM_WORKER_SURFACE
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#endif

namespace donner::editor {

namespace {

#ifdef DONNER_WASM_WORKER_SURFACE
constexpr const char* kDirectWorkerDocumentCanvasSelector = "#donner-document-canvas";
constexpr const char* kDirectWorkerDocumentBackCanvasSelector = "#donner-document-canvas-back";
constexpr const char* kDirectWorkerDocumentCanvasSelectors =
    "#donner-document-canvas,#donner-document-canvas-back";
constexpr const char* kBitmapWorkerDocumentCanvasSelector = "#donner-worker-document-canvas";

EM_JS(int, UseBitmapWorkerSurfaceBridge, (),
      { return globalThis['__donnerWorkerSurfaceMode'] == 'bitmap-bridge' ? 1 : 0; });

EM_JS(int, UseWorkerSurfaceDiagnostic, (), {
  return new URLSearchParams(window.location.search).has('workerSurfaceDiagnostic') ? 1 : 0;
});

EM_JS(void, RequireWasmWorkerRuntimeForReveal, (), {
  window['__donnerRequiresWorkerRuntime'] = true;
  window['__donnerWorkerRuntimeStats'] = window['__donnerWorkerRuntimeStats'] || {
    'ready' : false,
    'initializationMs' : 0,
    'maskPipelineMs' : 0,
    'initializationCount' : 0,
    'workerDeviceCreations' : 0,
    'readyAtMs' : 0,
  };
});

EM_JS(int, StageWorkerDocumentBitmap,
      (const char* selector, int width, int height, double frameToken, int surfaceSlot), {
        try {
          const canvasTarget = findCanvasEventTarget(UTF8ToString(selector));
          const canvas = canvasTarget && (canvasTarget['offscreenCanvas'] || canvasTarget);
          if (!canvas || typeof canvas.transferToImageBitmap != 'function') {
            Module['printErr'](
                'Donner bitmap bridge: transferred canvas cannot create an ImageBitmap');
            return 0;
          }
          const bitmap = canvas.transferToImageBitmap();
          postMessage({
            'cmd' : 'callHandler',
            'handler' : 'stageDonnerDocumentBitmap',
            'args' : [ frameToken, surfaceSlot, bitmap, width, height ],
          },
                      [bitmap]);
          return 1;
        } catch (error) {
          Module['printErr']('Donner bitmap bridge failed: ' + error);
          return 0;
        }
      });

EM_JS(void, CommitWorkerDocumentBitmap, (double frameToken, int surfaceSlot), {
  if (typeof Module['commitDonnerDocumentBitmap'] == 'function') {
    Module['commitDonnerDocumentBitmap'](frameToken, surfaceSlot);
  }
});

EM_JS(void, DiscardWorkerDocumentBitmap, (double frameToken), {
  if (typeof document != 'undefined') {
    if (typeof Module['discardDonnerDocumentBitmap'] == 'function') {
      Module['discardDonnerDocumentBitmap'](frameToken);
    }
    return;
  }
  postMessage({
    'cmd' : 'callHandler',
    'handler' : 'discardDonnerDocumentBitmap',
    'args' : [frameToken],
  });
});

EM_JS(void, PublishWasmWorkerRuntimeStats,
      (double initializationMs, double maskPipelineMs, int initializationCount,
       int workerDeviceCreations, int headlessDeviceCreations),
      {
        postMessage({
          'cmd' : 'callHandler',
          'handler' : 'publishDonnerWorkerRuntimeStats',
          'args' : [
            initializationMs, maskPipelineMs, initializationCount, workerDeviceCreations,
            headlessDeviceCreations
          ],
        });
      });

EM_JS(void, ReportWasmWorkerRuntimeInitializationFailure, (), {
  postMessage({
    'cmd' : 'callHandler',
    'handler' : 'reportDonnerWorkerRuntimeInitializationFailure',
    'args' : [],
  });
});

EM_JS(void, PublishWorkerSurfaceDiagnostic,
      (double frameToken, int samples, int coloredPixels, int nonBlackPixels, int maxChannel,
       int textStyleBackgroundPixels, int textStyleGlyphPixels),
      {
        postMessage({
          'cmd' : 'callHandler',
          'handler' : 'publishDonnerWorkerSurfaceDiagnostic',
          'args' : [
            frameToken, samples, coloredPixels, nonBlackPixels, maxChannel,
            textStyleBackgroundPixels,
            textStyleGlyphPixels
          ],
        });
      });

EM_JS(void, ReportWorkerSurfaceFailure, (int code),
      { Module['printErr']('Donner worker surface presentation failed at stage ' + code); });

EM_JS(void, ReportWorkerTaskWakeFailure,
      (double failureCount, int shuttingDown, int renderRequestDropped, int thumbnailDropped,
       int surfaceUnavailable),
      {
        const args = [
          failureCount, shuttingDown, renderRequestDropped, thumbnailDropped,
          surfaceUnavailable
        ];
        if (typeof document == 'undefined') {
          postMessage({
            'cmd' : 'callHandler',
            'handler' : 'reportDonnerWorkerTaskWakeFailure',
            'args' : args,
          });
        } else if (typeof Module['reportDonnerWorkerTaskWakeFailure'] == 'function') {
          Module['reportDonnerWorkerTaskWakeFailure'](failureCount, shuttingDown,
                                                      renderRequestDropped, thumbnailDropped,
                                                      surfaceUnavailable);
        } else {
          Module['printErr']('Donner renderer worker wake failure handler is unavailable');
        }
      });

EM_JS(void, PublishDirectSurfaceTaskBoundaryAcknowledgment, (double frameToken),
      { window['__donnerDirectSurfaceTaskBoundaryToken'] = frameToken; });

// Run `callback(userdata)` in the worker's next event-loop task, so the browser has committed the
// WebGPU canvas this task presented before the acknowledgment observes it.
//
// A `MessagePort` message is a task like `setTimeout` but carries no timer clamp. That matters
// here because the wake chain re-enters through a timer callback: the next proxied worker task
// arrives as a microtask of this acknowledgment (Emscripten's mailbox resolves an
// `Atomics.waitAsync` promise), so a `setTimeout`-based hop inherits and keeps incrementing the
// nesting level until every hop pays the browsers' 4 ms nested-timer minimum. Measured on this
// exact chain shape, a `setTimeout(0)` hop costs ~6 ms per frame in both Chromium and Gecko while
// the port hop costs ~0 ms, and that per-frame cost is what pushes the handoff past the animation
// frame the result was rendered for.
EM_JS(void, ScheduleWorkerTaskBoundaryCallback, (void* callback, void* userdata), {
  let state = globalThis['__donnerWorkerTaskBoundary'];
  if (typeof state == 'undefined') {
    state = false;
    if (typeof MessageChannel == 'function') {
      const channel = new MessageChannel();
      const queue = [];
      channel.port1.onmessage = function() {
        const entry = queue.shift();
        if (entry) {
          callUserCallback(function() { getWasmTableEntry(entry[0])(entry[1]); });
        }
      };
      channel.port1.start();
      state = {'port' : channel.port2, 'queue' : queue};
    }
    globalThis['__donnerWorkerTaskBoundary'] = state;
  }
  if (!state) {
    // No MessageChannel: any real task boundary still satisfies the presentation contract.
    setTimeout(
        function() { callUserCallback(function() { getWasmTableEntry(callback)(userdata); }); }, 0);
    return;
  }
  state['queue'].push([ callback, userdata ]);
  state['port'].postMessage(0);
});
EM_JS_DEPS(donner_worker_task_boundary, "$getWasmTableEntry,$callUserCallback");

enum class WorkerSurfacePresentDisposition : std::uint8_t {
  Presented,
  RetryNextWorkerTask,
  TerminalFailure,
};

struct WorkerSurfacePixelStats {
  int samples = 0;
  int coloredPixels = 0;
  int nonBlackPixels = 0;
  int maxChannel = 0;
  int textStyleBackgroundPixels = 0;
  int textStyleGlyphPixels = 0;
};

WorkerSurfacePixelStats MeasureWorkerSurfacePixels(const svg::RendererBitmap& bitmap) {
  WorkerSurfacePixelStats stats;
  if (bitmap.empty() || bitmap.rowBytes == 0u) {
    return stats;
  }
  constexpr int kStride = 2;
  constexpr int kWeight = kStride * kStride;
  for (int y = 0; y < bitmap.dimensions.y; y += kStride) {
    const uint8_t* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
    for (int x = 0; x < bitmap.dimensions.x; x += kStride) {
      const uint8_t* pixel = row + static_cast<std::size_t>(x) * 4u;
      const int maxRgb = std::max({pixel[0], pixel[1], pixel[2]});
      const int minRgb = std::min({pixel[0], pixel[1], pixel[2]});
      stats.samples += kWeight;
      stats.maxChannel = std::max(stats.maxChannel, maxRgb);
      if (pixel[3] > 0 && maxRgb > 12) {
        stats.nonBlackPixels += kWeight;
      }
      if (pixel[3] > 0 && maxRgb > 50 && maxRgb - minRgb > 20) {
        stats.coloredPixels += kWeight;
      }
      const bool textStyleBackground = pixel[3] > 200 && pixel[0] >= 15 && pixel[0] <= 32 &&
                                       pixel[1] >= 24 && pixel[1] <= 45 && pixel[2] >= 34 &&
                                       pixel[2] <= 55;
      if (textStyleBackground) {
        stats.textStyleBackgroundPixels += kWeight;
      }
      const bool neutralLight = pixel[3] > 200 && minRgb > 130 && maxRgb - minRgb < 35;
      const bool mintText = pixel[3] > 200 && pixel[0] > 100 && pixel[1] > 170 && pixel[2] > 140 &&
                            pixel[1] - pixel[0] > 30 && pixel[1] - pixel[2] > 10;
      if (neutralLight || mintText) {
        stats.textStyleGlyphPixels += kWeight;
      }
    }
  }
  return stats;
}

struct WorkerSurfacePresentResult {
  WorkerSurfacePresentDisposition disposition = WorkerSurfacePresentDisposition::TerminalFailure;
  WorkerSurfaceFailureKind terminalFailure = WorkerSurfaceFailureKind::Fatal;
  int surfaceSlot = 0;
  /// Backing-store size the surface canvas is configured at (device pixels).
  Vector2i configuredBackingSize = Vector2i::Zero();
};

WorkerSurfaceFailureKind WorkerSurfaceFailureForStatus(WGPUSurfaceGetCurrentTextureStatus status) {
  if (status == WGPUSurfaceGetCurrentTextureStatus_Timeout) {
    return WorkerSurfaceFailureKind::Timeout;
  }
  if (status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
      status == WGPUSurfaceGetCurrentTextureStatus_Lost) {
    return WorkerSurfaceFailureKind::OutdatedOrLost;
  }
  return WorkerSurfaceFailureKind::Fatal;
}

class WasmWorkerSurfacePresenter {
public:
  WasmWorkerSurfacePresenter(std::shared_ptr<geode::GeodeDevice> device, bool publishBitmap)
      : device_(std::move(device)), publishBitmap_(publishBitmap) {
    if (device_ == nullptr || !device_->instance()) {
      return;
    }

    if (publishBitmap_) {
      addSurface(kBitmapWorkerDocumentCanvasSelector);
    } else {
      // Two DOM surfaces, presented into alternately.
      //
      // A direct WebGPU present commits worker-side, immediately. The CSS
      // layout that matches those pixels - the element box scaled by
      // backing/content and the pane clip - is only applied when the main
      // thread accepts the epoch, one or more MessagePort task boundaries
      // later. Presenting into the canvas that is currently on screen shows
      // epoch N+1 pixels under epoch N geometry for that whole window: during
      // a pinch the document visibly flickers between two scales.
      //
      // Alternating means the epoch being drawn is never the epoch being
      // displayed, and acceptance flips which canvas is visible in the same
      // style flush that writes the new geometry. Pixels and CSS become
      // atomic.
      addSurface(kDirectWorkerDocumentCanvasSelector);
      addSurface(kDirectWorkerDocumentBackCanvasSelector);
    }
  }

  [[nodiscard]] bool hasCompatibleSurfaceTargets() const {
    return !surfaces_.empty() &&
           std::ranges::all_of(surfaces_, [](const std::unique_ptr<SurfaceTarget>& target) {
             return target->valid;
           });
  }

  [[nodiscard]] bool supportsRgba8UnormFallback() const {
    return !surfaces_.empty() &&
           std::ranges::all_of(surfaces_, [](const std::unique_ptr<SurfaceTarget>& target) {
             return target->surface && target->supportsRgba8Unorm;
           });
  }

  [[nodiscard]] WorkerSurfacePresentResult present(
      const svg::RendererTextureSnapshot* textureSnapshot, int requestedSurfaceSlot,
      std::uint64_t frameToken, Vector2i backingCapPx) {
    if (surfaces_.empty() || textureSnapshot == nullptr ||
        textureSnapshot->backend() != svg::RendererTextureSnapshotBackend::Geode) {
      ReportWorkerSurfaceFailure(1);
      return {};
    }
    const Vector2i dimensions = textureSnapshot->dimensions();
    if (dimensions.x <= 0 || dimensions.y <= 0) {
      ReportWorkerSurfaceFailure(2);
      return {};
    }

    // The bitmap bridge owns exactly one worker canvas and stages an
    // ImageBitmap into one of two *main-thread* canvases, so its slot names a
    // destination the worker does not hold. The direct path owns both DOM
    // canvases, and the slot is the surface it presents into.
    const int lastTargetSlot = static_cast<int>(surfaces_.size()) - 1;
    const int surfaceSlot = std::clamp(requestedSurfaceSlot, 0, 1);
    const int targetSlot = publishBitmap_ ? 0 : std::clamp(surfaceSlot, 0, lastTargetSlot);
    SurfaceTarget& target = *surfaces_[targetSlot];
    if (target.recreateBeforeNextAttempt) {
      initializeSurface(target);
    }
    if (!target.surface) {
      return handleFailure(target, surfaceSlot, WorkerSurfaceFailureKind::Setup, 3);
    }
    if (!target.valid) {
      return handleFailure(target, surfaceSlot, WorkerSurfaceFailureKind::Incompatible, 3);
    }

    const auto* geodeTexture =
        static_cast<const svg::RendererGeodeTextureSnapshot*>(textureSnapshot);
    if (geodeTexture == nullptr || !geodeTexture->texture() ||
        geodeTexture->format() != target.format) {
      return handleFailure(target, surfaceSlot, WorkerSurfaceFailureKind::Incompatible, 5);
    }

    // Configure the backing store at the viewport's raster cap, not the
    // content size: resizing a transferred OffscreenCanvas clears it, and the
    // cleared frame reaches the compositor before the drawn one, flashing the
    // editor background on every raster-size change during a zoom gesture.
    // At the cap the size is a function of pane geometry only, so gestures
    // never re-configure. Content is blitted into the top-left corner and the
    // remainder stays transparent; presentation layout scales the element and
    // extends its clip so only the content region shows. The WebKit bitmap
    // bridge snapshots the whole canvas, so it keeps exact sizing.
    Vector2i backing = dimensions;
    if (!publishBitmap_) {
      // Start at the viewport's raster cap and never shrink: the unbounded
      // raster branch can legitimately exceed the per-axis pane cap (a wide
      // document at moderate zoom rasterizes wider than the pane while its
      // area stays below the viewport-bounded alternative), and every
      // reconfigure is a visible background flash. Growing to the running
      // maximum converges within the first gesture; the allocation stays
      // bounded by the hard canvas dimension cap.
      const auto growTo = [](int needed, int configured, int cap) {
        int size = std::max(needed, cap);
        size = std::max(size, configured);
        if (configured > 0 && size > configured) {
          // Each reconfigure is one visible flash, so grow geometrically:
          // repeated small increases during a zoom-in ramp coalesce into one
          // or two reconfigures instead of one per epoch. Overshoot is
          // clamped so it can never exceed the hard canvas dimension cap.
          const int overshoot = std::min(configured + configured / 4,
                                         static_cast<int>(ViewportState::kMaxCanvasDim));
          size = std::max(size, overshoot);
        }
        return size;
      };
      backing.x = growTo(dimensions.x, target.configuredSize.x, backingCapPx.x);
      backing.y = growTo(dimensions.y, target.configuredSize.y, backingCapPx.y);
    }
    if (backing != target.configuredSize) {
      if (emscripten_set_canvas_element_size(target.canvasSelector, backing.x, backing.y) !=
          EMSCRIPTEN_RESULT_SUCCESS) {
        return handleFailure(target, surfaceSlot, WorkerSurfaceFailureKind::Setup, 3);
      }
      wgpu::SurfaceConfiguration config(wgpu::Default);
      config.device = device_->device();
      config.format = target.format;
      config.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopyDst;
      config.width = static_cast<uint32_t>(backing.x);
      config.height = static_cast<uint32_t>(backing.y);
      config.presentMode = wgpu::PresentMode::Fifo;
      config.alphaMode = target.alphaMode;
      target.surface.get().configure(config);
      target.configuredSize = backing;
    }

    wgpu::SurfaceTexture surfaceTexture;
    target.surface.get().getCurrentTexture(&surfaceTexture);
    geode::ScopedWgpuHandle<wgpu::Texture> surfaceTextureHandle(
        wgpu::Texture(surfaceTexture.texture));
    if (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
      return handleFailure(target, surfaceSlot,
                           WorkerSurfaceFailureForStatus(surfaceTexture.status), 4);
    }
    if (!surfaceTextureHandle) {
      return handleFailure(target, surfaceSlot, WorkerSurfaceFailureKind::Setup, 4);
    }

    geode::ScopedWgpuHandle<wgpu::CommandEncoder> encoder(device_->device().createCommandEncoder());
    if (!encoder) {
      return handleFailure(target, surfaceSlot, WorkerSurfaceFailureKind::Fatal, 6);
    }
    wgpu::TexelCopyTextureInfo source = {};
    source.texture = geodeTexture->texture();
    wgpu::TexelCopyTextureInfo destination = {};
    destination.texture = surfaceTextureHandle.get();
    const wgpu::Extent3D copySize = {static_cast<uint32_t>(dimensions.x),
                                     static_cast<uint32_t>(dimensions.y), 1u};
    encoder.get().copyTextureToTexture(source, destination, copySize);
    geode::ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder.get().finish());
    if (!commands) {
      return handleFailure(target, surfaceSlot, WorkerSurfaceFailureKind::Fatal, 7);
    }
    device_->queue().submit(1, &commands.get());
    if (publishBitmap_ &&
        StageWorkerDocumentBitmap(target.canvasSelector, dimensions.x, dimensions.y,
                                  static_cast<double>(frameToken), surfaceSlot) == 0) {
      return handleFailure(target, surfaceSlot, WorkerSurfaceFailureKind::Setup, 8);
    }

    target.consecutiveFailures = 0u;
    return WorkerSurfacePresentResult{
        .disposition = WorkerSurfacePresentDisposition::Presented,
        .surfaceSlot = surfaceSlot,
        .configuredBackingSize = target.configuredSize,
    };
  }

private:
  struct SurfaceTarget {
    const char* canvasSelector = nullptr;
    geode::ScopedWgpuHandle<wgpu::Surface> surface;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    wgpu::CompositeAlphaMode alphaMode = wgpu::CompositeAlphaMode::Auto;
    Vector2i configuredSize = Vector2i::Zero();
    unsigned consecutiveFailures = 0u;
    bool supportsRgba8Unorm = false;
    bool recreateBeforeNextAttempt = false;
    bool valid = false;
  };

  [[nodiscard]] WorkerSurfacePresentResult handleFailure(SurfaceTarget& target, int surfaceSlot,
                                                         WorkerSurfaceFailureKind failure,
                                                         int reportCode) {
    ReportWorkerSurfaceFailure(reportCode);
    const WorkerSurfaceRecoveryAction action =
        WorkerSurfaceRecoveryDecisionFor(failure, target.consecutiveFailures);
    if (action == WorkerSurfaceRecoveryAction::TerminalFailure) {
      target.consecutiveFailures = 0u;
      return WorkerSurfacePresentResult{
          .disposition = WorkerSurfacePresentDisposition::TerminalFailure,
          .terminalFailure = failure,
          .surfaceSlot = surfaceSlot,
      };
    }

    ++target.consecutiveFailures;
    if (action == WorkerSurfaceRecoveryAction::ReconfigureAndRetry) {
      target.configuredSize = Vector2i::Zero();
    } else if (action == WorkerSurfaceRecoveryAction::RecreateAndRetry) {
      target.recreateBeforeNextAttempt = true;
      target.configuredSize = Vector2i::Zero();
    }
    return WorkerSurfacePresentResult{
        .disposition = WorkerSurfacePresentDisposition::RetryNextWorkerTask,
        .surfaceSlot = surfaceSlot,
    };
  }

  void initializeSurface(SurfaceTarget& target) {
    target.surface.reset();
    target.format = wgpu::TextureFormat::Undefined;
    target.alphaMode = wgpu::CompositeAlphaMode::Auto;
    target.configuredSize = Vector2i::Zero();
    target.supportsRgba8Unorm = false;
    target.recreateBeforeNextAttempt = false;
    target.valid = false;

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvasSource =
        WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvasSource.selector.data = target.canvasSelector;
    canvasSource.selector.length = WGPU_STRLEN;
    WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
    descriptor.nextInChain = &canvasSource.chain;
    target.surface.reset(
        wgpu::Surface(wgpuInstanceCreateSurface(device_->instance(), &descriptor)));
    if (!target.surface) {
      return;
    }

    wgpu::SurfaceCapabilities caps;
    target.surface.get().getCapabilities(device_->adapter(), &caps);
    for (size_t i = 0; i < caps.formatCount; ++i) {
      target.supportsRgba8Unorm |= caps.formats[i] == wgpu::TextureFormat::RGBA8Unorm;
      if (caps.formats[i] == device_->textureFormat()) {
        target.format = caps.formats[i];
      }
    }
    if (caps.alphaModeCount > 0u) {
      target.alphaMode = caps.alphaModes[0];
    }
    for (size_t i = 0; i < caps.alphaModeCount; ++i) {
      if (caps.alphaModes[i] == wgpu::CompositeAlphaMode::Premultiplied) {
        target.alphaMode = caps.alphaModes[i];
        break;
      }
    }
    caps.freeMembers();
    target.valid = target.format != wgpu::TextureFormat::Undefined &&
                   target.alphaMode != wgpu::CompositeAlphaMode::Auto;
  }

  void addSurface(const char* canvasSelector) {
    auto target = std::make_unique<SurfaceTarget>();
    target->canvasSelector = canvasSelector;
    initializeSurface(*target);
    surfaces_.push_back(std::move(target));
  }

  std::shared_ptr<geode::GeodeDevice> device_;
  bool publishBitmap_ = false;
  std::vector<std::unique_ptr<SurfaceTarget>> surfaces_;
};
#endif

RenderResult::CompositedPreview BuildFullCanvasCompositedPreview(
    const Box2d& documentViewBox, const svg::RendererBitmap& bitmap,
    std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot, std::uint64_t generation,
    Entity entity, svg::compositor::InteractionHint interactionKind,
    const EditorRasterViewport& rasterViewport,
    std::optional<RenderRequest::DragPreview> representedDragPreview) {
  RenderResult::CompositedTile tile;
  tile.kind = RenderResult::CompositedTile::Kind::Segment;
  tile.id = "full-canvas";
  tile.generation = generation;
  tile.bitmap = bitmap;
  tile.textureSnapshot = std::move(textureSnapshot);
  tile.canvasOffsetDoc = rasterViewport.documentRect.topLeft - documentViewBox.topLeft;
  const Vector2i payloadDims =
      !bitmap.empty() ? bitmap.dimensions
                      : (tile.textureSnapshot != nullptr ? tile.textureSnapshot->dimensions()
                                                         : Vector2i::Zero());
  tile.bitmapDimsPx = payloadDims;
  tile.rasterCanvasSize = rasterViewport.outputSizePx;
  if (rasterViewport.documentRect.width() > 0.0 && rasterViewport.documentRect.height() > 0.0) {
    tile.bitmapDimsDoc = rasterViewport.documentRect.size();
  } else {
    tile.bitmapDimsDoc =
        Vector2d(static_cast<double>(payloadDims.x), static_cast<double>(payloadDims.y));
  }

  return RenderResult::CompositedPreview{
      .tiles = {std::move(tile)},
      .entity = entity,
      .interactionKind = interactionKind,
      .representedDragPreview = std::move(representedDragPreview),
  };
}

}  // namespace

std::optional<RenderResult::CompositedPreview> BuildDirectSurfaceCompositorTileMetadata(
    svg::compositor::CompositorController& compositor, const EditorRasterViewport& rasterViewport,
    const Box2d& documentViewBox, Entity compositorEntity,
    const std::optional<RenderRequest::DragPreview>& dragPreview, bool overviewInfillOnly) {
  // Direct-surface pixels never enter the ImGui texture cache, including overview-infill frames.
  // The compositor overlay still needs the exact tile geometry for every surface presentation.
  (void)overviewInfillOnly;

  using svg::compositor::CompositorTileBitmapPayload;
  std::vector<svg::compositor::CompositorTile> compositorTiles =
      compositor.snapshotTilesForUpload(CompositorTileBitmapPayload::MetadataOnly);
  const Transform2d documentFromOutput = rasterViewport.outputFromDocument.inverse();
  const auto outputPointToPresentedDoc = [&](const Vector2d& outputPoint) {
    return documentFromOutput.transformPosition(outputPoint) - documentViewBox.topLeft;
  };
  const auto outputVectorToDoc = [&](const Vector2d& outputVector) {
    return documentFromOutput.transformVector(outputVector);
  };
  const auto documentFromCachedDocument = [&](const Transform2d& outputFromCachedOutput) {
    return rasterViewport.outputFromDocument * outputFromCachedOutput * documentFromOutput;
  };

  std::vector<RenderResult::CompositedTile> previewTiles;
  previewTiles.reserve(compositorTiles.size());
  for (const svg::compositor::CompositorTile& compositorTile : compositorTiles) {
    if (compositorTile.bitmapDims.x <= 0 || compositorTile.bitmapDims.y <= 0) {
      continue;
    }

    RenderResult::CompositedTile tile;
    tile.kind = compositorTile.layerEntity != entt::null
                    ? RenderResult::CompositedTile::Kind::Layer
                    : (compositorTile.immediate ? RenderResult::CompositedTile::Kind::Immediate
                                                : RenderResult::CompositedTile::Kind::Segment);
    tile.id = std::to_string(compositorTile.tileId);
    tile.layerEntity = compositorTile.layerEntity;
    tile.generation = compositorTile.generation;
    tile.bitmapDimsPx = compositorTile.bitmapDims;
    tile.rasterCanvasSize = rasterViewport.outputSizePx;
    tile.canvasOffsetDoc = outputPointToPresentedDoc(compositorTile.canvasOffsetPx);
    tile.bitmapDimsDoc =
        outputVectorToDoc(Vector2d(static_cast<double>(compositorTile.bitmapDims.x),
                                   static_cast<double>(compositorTile.bitmapDims.y)));
    if (compositorTile.layerEntity != entt::null) {
      tile.documentFromCachedDocument = documentFromCachedDocument(compositorTile.canvasFromBitmap);
      tile.dragTranslationDoc = tile.documentFromCachedDocument.translation();
    }
    tile.isDragTarget = compositorTile.isDragTarget;
    previewTiles.push_back(std::move(tile));
  }
  if (previewTiles.empty()) {
    // A flat document is presented directly by the root renderer and legitimately has no split
    // compositor tiles. Publish its worker surface as one metadata-only full-canvas tile so the
    // View-menu overlay can still identify the actual presentation boundary.
    RenderResult::CompositedTile tile;
    tile.kind = RenderResult::CompositedTile::Kind::Segment;
    tile.id = "full-canvas";
    tile.bitmapDimsPx = rasterViewport.outputSizePx;
    tile.rasterCanvasSize = rasterViewport.outputSizePx;
    tile.canvasOffsetDoc = rasterViewport.documentRect.topLeft - documentViewBox.topLeft;
    tile.bitmapDimsDoc = rasterViewport.documentRect.size();
    if (tile.bitmapDimsPx.x <= 0 || tile.bitmapDimsPx.y <= 0 || tile.bitmapDimsDoc.x <= 0.0 ||
        tile.bitmapDimsDoc.y <= 0.0) {
      return std::nullopt;
    }
    previewTiles.push_back(std::move(tile));
  }

  return RenderResult::CompositedPreview{
      .tiles = std::move(previewTiles),
      .entity = compositorEntity,
      .interactionKind = dragPreview.has_value() ? dragPreview->interactionKind
                                                 : svg::compositor::InteractionHint::Selection,
      .representedDragPreview = dragPreview,
  };
}

namespace {

EditorRasterViewport EffectiveRasterViewportForRequest(svg::SVGDocument& document,
                                                       const EditorRasterViewport& requested) {
  if (requested.outputSizePx.x > 0 && requested.outputSizePx.y > 0 &&
      requested.semanticCanvasSizePx.x > 0 && requested.semanticCanvasSizePx.y > 0) {
    return requested;
  }

  EditorRasterViewport fallback;
  fallback.outputSizePx = document.canvasSize();
  fallback.semanticCanvasSizePx = fallback.outputSizePx;
  if (const std::optional<Box2d> viewBox = document.svgElement().viewBox()) {
    fallback.documentRect = *viewBox;
  } else {
    fallback.documentRect = Box2d::FromXYWH(0.0, 0.0, static_cast<double>(fallback.outputSizePx.x),
                                            static_cast<double>(fallback.outputSizePx.y));
  }
  fallback.outputFromDocument = document.canvasFromDocumentTransform();
  return fallback;
}

bool ContainsEntity(const std::vector<Entity>& entities, Entity entity) {
  return std::ranges::find(entities, entity) != entities.end();
}

void AppendUniqueEntity(std::vector<Entity>* entities, Entity entity) {
  if (entity != entt::null && !ContainsEntity(*entities, entity)) {
    entities->push_back(entity);
  }
}

std::vector<Entity> DragPreviewEntities(const RenderRequest::DragPreview& preview) {
  std::vector<Entity> entities;
  entities.reserve(1u + preview.extraEntities.size());
  AppendUniqueEntity(&entities, preview.entity);
  for (Entity entity : preview.extraEntities) {
    AppendUniqueEntity(&entities, entity);
  }
  return entities;
}

std::vector<Entity> DesiredCompositorEntities(const RenderRequest& request) {
  if (request.dragPreview.has_value()) {
    return DragPreviewEntities(*request.dragPreview);
  }

  std::vector<Entity> entities;
  AppendUniqueEntity(&entities, request.selectedEntity);
  return entities;
}

bool SameEntityList(const std::vector<Entity>& lhs, const std::vector<Entity>& rhs) {
  return lhs == rhs;
}

#ifndef DONNER_WASM_WORKER_SURFACE
bool ContainsAllEntities(const std::vector<Entity>& haystack, const std::vector<Entity>& needles) {
  return std::ranges::all_of(needles,
                             [&](Entity entity) { return ContainsEntity(haystack, entity); });
}
#endif

bool WaitForSampleThumbnailDelay(const svg::compositor::CancellationToken& cancellation,
                                 std::chrono::milliseconds delay) {
  const auto deadline = std::chrono::steady_clock::now() + delay;
  while (!cancellation.isCancelled()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return true;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(1)));
  }
  return false;
}

SampleThumbnailRenderResult RenderSampleThumbnail(
    SampleThumbnailRenderRequest request, svg::RendererInterface& renderer,
    const svg::compositor::CancellationToken& cancellation, std::chrono::milliseconds delay) {
  SampleThumbnailRenderResult result{
      .key = request.key,
      .outcome = SampleThumbnailRenderOutcome::RenderError,
  };
  if (request.dimensions.x <= 0 || request.dimensions.y <= 0) {
    return result;
  }
  if (delay.count() > 0 && !WaitForSampleThumbnailDelay(cancellation, delay)) {
    result.outcome = SampleThumbnailRenderOutcome::Cancelled;
    return result;
  }
  if (cancellation.isCancelled()) {
    result.outcome = SampleThumbnailRenderOutcome::Cancelled;
    return result;
  }

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(request.source, warnings);
  if (parsed.hasError()) {
    result.outcome = SampleThumbnailRenderOutcome::ParseError;
    return result;
  }
  if (cancellation.isCancelled()) {
    result.outcome = SampleThumbnailRenderOutcome::Cancelled;
    return result;
  }

  svg::SVGDocument document = std::move(parsed.result());
  document.setCanvasSize(request.dimensions.x, request.dimensions.y);

  svg::RenderViewport viewport;
  viewport.size = Vector2d(request.dimensions.x, request.dimensions.y);
  viewport.devicePixelRatio = 1.0;
  svg::RendererDriver driver(renderer);
  const bool completed = driver.drawInterruptibly(
      document, viewport, Transform2d(), [&cancellation] { return cancellation.isCancelled(); });
  if (!completed || cancellation.isCancelled()) {
    result.outcome = SampleThumbnailRenderOutcome::Cancelled;
    return result;
  }

  result.bitmap =
      renderer.takeSnapshotInterruptibly([&cancellation] { return cancellation.isCancelled(); });
  if (cancellation.isCancelled()) {
    result.bitmap = {};
    result.outcome = SampleThumbnailRenderOutcome::Cancelled;
    return result;
  }
  if (!result.bitmap.empty()) {
    result.outcome = SampleThumbnailRenderOutcome::Rendered;
  }
  return result;
}

}  // namespace

#ifdef DONNER_WASM_WORKER_SURFACE
struct WasmWorkerRuntime {
  std::shared_ptr<geode::GeodeDevice> device;
  std::unique_ptr<svg::Renderer> renderer;
  std::unique_ptr<svg::RendererInterface> sampleThumbnailRenderer;
  std::unique_ptr<WasmWorkerSurfacePresenter> surfacePresenter;
};

struct WasmWorkerRuntimeInitControl {
  std::mutex mutex;
  AsyncRenderer* owner = nullptr;
};

namespace {
struct WasmWorkerRuntimeInitCallbackContext {
  std::shared_ptr<WasmWorkerRuntimeInitControl> control;
  std::chrono::steady_clock::time_point initializationStart;
  wgpu::TextureFormat textureFormat = wgpu::TextureFormat::BGRA8Unorm;
  int workerDeviceCreations = 1;
};

struct WasmDirectSurfaceTaskBoundaryCallbackContext {
  std::shared_ptr<WasmWorkerRuntimeInitControl> control;
  std::uint64_t frameToken = 0;
};
}  // namespace
#endif

PresentationSnapshotPlan ChoosePresentationSnapshotPlan(bool hasCompositedPreview,
                                                        bool requiresTextureSnapshotPresentation,
                                                        bool captureCpuSnapshot) {
  if (requiresTextureSnapshotPresentation) {
    return PresentationSnapshotPlan{
        .captureCpuSnapshot = captureCpuSnapshot,
        .captureTextureSnapshot = !hasCompositedPreview,
    };
  }

  // Without a composited preview the CPU bitmap *is* the presented frame, so it is always read
  // back. When a preview covers the frame the readback is pure overhead for presentation, but
  // `captureCpuSnapshot` callers (replay harnesses, goldens, thumbnail and diagnostic captures)
  // consume `RenderResult::bitmap` directly and must still receive one.
  return PresentationSnapshotPlan{
      .captureCpuSnapshot = captureCpuSnapshot || !hasCompositedPreview,
  };
}

TextureSnapshotHandoff ChooseTextureSnapshotHandoff(bool consumerOutlivesCurrentFrame) noexcept {
  return consumerOutlivesCurrentFrame ? TextureSnapshotHandoff::TakeOwnership
                                      : TextureSnapshotHandoff::BorrowCurrentFrame;
}

bool DirectSurfacePresentationGenerationIsCurrent(std::uint64_t requestDocumentGeneration,
                                                  std::uint64_t minimumDocumentGeneration) {
  return requestDocumentGeneration >= minimumDocumentGeneration;
}

bool DirectSurfacePlacementViewportIsUsable(const ViewportState& viewport) {
  return viewport.pixelsPerDocUnit() > 0.0 && viewport.paneSize.x > 0.0 &&
         viewport.paneSize.y > 0.0;
}

int NextWorkerSurfacePresentSlot(int lastAcceptedSlot) {
  // The invariant, for both presentation paths: never draw into the slot the
  // main thread is currently showing. An unrecognized slot resolves to 0, which
  // still satisfies it because no such slot can be the accepted one.
  return lastAcceptedSlot == 0 ? 1 : 0;
}

WorkerTaskFollowUp ChooseWorkerTaskFollowUp(bool hasPendingRequest,
                                            bool cancellationPending) noexcept {
  return hasPendingRequest || cancellationPending ? WorkerTaskFollowUp::SchedulePendingRequest
                                                  : WorkerTaskFollowUp::Park;
}

WorkerTaskCompletionDisposition ChooseWorkerTaskCompletionDisposition(
    bool shuttingDown, bool hasPendingRequest, bool cancellationPending,
    bool lowPriorityWorkPending, bool presentationBoundaryPending) noexcept {
  if (shuttingDown) {
    return WorkerTaskCompletionDisposition::ExitWorker;
  }
  if (hasPendingRequest || cancellationPending || lowPriorityWorkPending ||
      presentationBoundaryPending) {
    return WorkerTaskCompletionDisposition::ScheduleFollowUp;
  }
  return WorkerTaskCompletionDisposition::Park;
}

WasmWorkerRuntimeWakeAction ChooseWasmWorkerRuntimeWakeAction(
    WasmWorkerRuntimeInitializationStatus status, bool shuttingDown) noexcept {
  if (status == WasmWorkerRuntimeInitializationStatus::Ready) {
    return WasmWorkerRuntimeWakeAction::ScheduleWorkerTask;
  }
  if (shuttingDown) {
    return WasmWorkerRuntimeWakeAction::DetachAndCancelWorker;
  }
  return status == WasmWorkerRuntimeInitializationStatus::Initializing
             ? WasmWorkerRuntimeWakeAction::DeferUntilRuntimeReady
             : WasmWorkerRuntimeWakeAction::ReportRuntimeUnavailable;
}

WasmWorkerRuntimeOwnerCleanupDisposition ChooseWasmWorkerRuntimeOwnerCleanupDisposition(
    bool workerCancellationRequested, bool runtimeStillOwnedAfterJoin) noexcept {
  return workerCancellationRequested || runtimeStillOwnedAfterJoin
             ? WasmWorkerRuntimeOwnerCleanupDisposition::AbandonThreadAffinedRuntime
             : WasmWorkerRuntimeOwnerCleanupDisposition::ExpectWorkerCleanup;
}

WorkerTaskEnqueueFailurePlan ChooseWorkerTaskEnqueueFailurePlan(
    bool shuttingDown, bool renderStatePending, bool thumbnailPending,
    std::uint64_t consecutiveFailureCount, bool enqueueAttemptFromWorker) noexcept {
  return WorkerTaskEnqueueFailurePlan{
      .resolveRenderState = !shuttingDown && renderStatePending,
      .dropPendingThumbnail = !shuttingDown && thumbnailPending,
      .wakeOwnerForRetry = !shuttingDown && consecutiveFailureCount == 1u,
      .reportSurfaceUnavailable = !shuttingDown && consecutiveFailureCount > 1u,
      .shutdownDisposition = !shuttingDown ? WorkerTaskShutdownDisposition::None
                             : enqueueAttemptFromWorker
                                 ? WorkerTaskShutdownDisposition::ExitCurrentWorker
                                 : WorkerTaskShutdownDisposition::CancelWorkerBeforeJoin,
  };
}

AsyncRenderer::AsyncRenderer(AsyncRendererStartMode startMode) {
#ifdef DONNER_WASM_WORKER_SURFACE
  RequireWasmWorkerRuntimeForReveal();
  useBitmapWorkerSurfaceBridge_ = UseBitmapWorkerSurfaceBridge() != 0;
  publishWorkerSurfaceDiagnostic_ = UseWorkerSurfaceDiagnostic() != 0;
#endif
  if (startMode == AsyncRendererStartMode::Immediate) {
    start();
  }
}

void AsyncRenderer::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::holds_alternative<ShutdownState>(workerState_)) {
    return;
  }
#ifdef DONNER_WASM_WORKER_SURFACE
  if (threadStarted_) {
    return;
  }

  wasmWorkerRuntimeInitControl_ = std::make_shared<WasmWorkerRuntimeInitControl>();
  wasmWorkerRuntimeInitControl_->owner = this;
  const char* workerCanvasSelectors = useBitmapWorkerSurfaceBridge_
                                          ? kBitmapWorkerDocumentCanvasSelector
                                          : kDirectWorkerDocumentCanvasSelectors;
  proxyQueue_ = em_proxying_queue_create();
  UTILS_RELEASE_ASSERT_MSG(proxyQueue_ != nullptr, "Failed to create Wasm renderer proxy queue");
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  emscripten_pthread_attr_settransferredcanvases(&attr, workerCanvasSelectors);
  const int createResult = pthread_create(&thread_, &attr, &AsyncRenderer::workerThreadEntry, this);
  pthread_attr_destroy(&attr);
  UTILS_RELEASE_ASSERT_MSG(createResult == 0,
                           "Failed to start Wasm renderer pthread with document canvas ownership");
  threadStarted_ = true;
#else
  if (thread_.joinable()) {
    return;
  }
  thread_ = std::thread([this] { workerLoop(); });
#endif
}

AsyncRenderer::~AsyncRenderer() {
  shutdown();
}

void AsyncRenderer::shutdown() {
#ifdef DONNER_WASM_WORKER_SURFACE
  std::optional<std::uint64_t> discardedBitmapBridgeFrame;
  WasmWorkerRuntimeWakeAction shutdownWakeAction = WasmWorkerRuntimeWakeAction::ScheduleWorkerTask;
  WasmWorkerOwnerDetachTiming ownerDetachTiming = WasmWorkerOwnerDetachTiming::BeforeWorkerJoin;
  bool workerCancellationRequested = false;
#endif
  bool initiatedShutdown = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Detach first so no later worker completion can copy a callback into its local wake slot. A
    // callback copied before this lock is still safe: the join below waits for it to return while
    // the owner and its window remain alive.
    wakeCallback_ = {};
    if (std::holds_alternative<ShutdownState>(workerState_)) {
      return;
    }
#ifdef DONNER_WASM_WORKER_SURFACE
    if (const auto* done = std::get_if<DoneState>(&workerState_);
        done != nullptr && done->result.bitmapBridgeFrameStaged) {
      discardedBitmapBridgeFrame = done->result.directSurfaceFrames;
    }
#endif
    pendingSampleThumbnail_.reset();
    sampleThumbnailResult_.reset();
    cancelSampleThumbnail_.cancel();
    pendingCompositorWarmup_ = false;
    cancelCompositorWarmup_.cancel();
    cancelRender_.cancel();
    workerState_ = ShutdownState{};
#ifdef DONNER_WASM_WORKER_SURFACE
    shutdownWakeAction = ChooseWasmWorkerRuntimeWakeAction(wasmWorkerRuntimeInitializationStatus_,
                                                           /*shuttingDown=*/true);
    ownerDetachTiming = ChooseWasmWorkerOwnerDetachTiming(wasmWorkerRuntimeInitializationStatus_);
#endif
    initiatedShutdown = true;
  }
#ifdef DONNER_WASM_WORKER_SURFACE
  if (discardedBitmapBridgeFrame.has_value()) {
    DiscardWorkerDocumentBitmap(static_cast<double>(*discardedBitmapBridgeFrame));
  }
#endif
#ifdef DONNER_WASM_WORKER_SURFACE
  const auto detachCallbackOwner = [this]() {
    if (wasmWorkerRuntimeInitControl_ == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(wasmWorkerRuntimeInitControl_->mutex);
    wasmWorkerRuntimeInitControl_->owner = nullptr;
  };
  if (ownerDetachTiming == WasmWorkerOwnerDetachTiming::BeforeWorkerJoin) {
    // A spontaneous adapter/device Promise may outlive pthread cancellation and the pooled worker
    // which started it. Close the initialization owner gate before join; a late callback then owns
    // only its heap control and never dereferences this renderer.
    detachCallbackOwner();
  }
  WorkerTaskScheduleResult shutdownScheduleResult =
      WorkerTaskScheduleResult::DeferredUntilRuntimeReady;
  if (initiatedShutdown && shutdownWakeAction == WasmWorkerRuntimeWakeAction::ScheduleWorkerTask) {
    shutdownScheduleResult = scheduleWorkerTask();
  }
#else
  if (initiatedShutdown) {
    cv_.notify_all();
  }
#endif
#ifdef DONNER_WASM_WORKER_SURFACE
  if (threadStarted_) {
    if (shutdownWakeAction == WasmWorkerRuntimeWakeAction::DetachAndCancelWorker ||
        shutdownScheduleResult == WorkerTaskScheduleResult::EnqueueRejected ||
        shutdownScheduleResult == WorkerTaskScheduleResult::RuntimeUnavailable) {
      // Initialization has no runnable proxy callback yet, or the mailbox rejected the exit task.
      // The non-ready path detached its Promise callback above. A ready runtime has no outstanding
      // initialization Promise, and join still keeps this owner alive while cancellation lands.
      (void)pthread_cancel(thread_);
      workerCancellationRequested = true;
    }
    pthread_join(thread_, nullptr);
    threadStarted_ = false;
    if (ChooseWasmWorkerRuntimeOwnerCleanupDisposition(workerCancellationRequested,
                                                       wasmWorkerRuntime_ != nullptr) ==
        WasmWorkerRuntimeOwnerCleanupDisposition::AbandonThreadAffinedRuntime) {
      // A runtime still owned after join cannot safely release worker-affined WebGPU handles on
      // main. Abandon the one runtime for the remainder of this page.
      (void)wasmWorkerRuntime_.release();
    }
  }
  if (ownerDetachTiming == WasmWorkerOwnerDetachTiming::AfterWorkerJoin) {
    // A ready worker may park behind a surface task-boundary timer which owns the wake gate. Keep
    // its owner attached until that callback observes Shutdown, releases the gate, and exits the
    // pthread. Once join returns, no ready-worker callback can dereference the owner.
    detachCallbackOwner();
  }
  wasmWorkerRuntimeInitControl_.reset();
  if (proxyQueue_ != nullptr) {
    em_proxying_queue_destroy(proxyQueue_);
    proxyQueue_ = nullptr;
  }
#else
  if (thread_.joinable()) {
    thread_.join();
  }
#endif
}

#ifdef DONNER_WASM_WORKER_SURFACE
void* AsyncRenderer::workerThreadEntry(void* self) {
  AsyncRenderer& renderer = *static_cast<AsyncRenderer*>(self);
  renderer.beginWasmWorkerRuntimeInitialization();
  // Keep this pthread's JavaScript runtime alive while returning from the native entry point. Each
  // render then runs as one proxied event-loop task, which gives WebGPU a real implicit
  // presentation boundary without an ImageBitmap readback.
  emscripten_exit_with_live_runtime();
}

void AsyncRenderer::completeWasmWorkerDeviceInitialization(
    std::unique_ptr<geode::GeodeDevice> device, void* userdata) {
  auto* context = static_cast<WasmWorkerRuntimeInitCallbackContext*>(userdata);
  const std::shared_ptr<WasmWorkerRuntimeInitControl> control = context->control;
  WasmWorkerRuntimeFinishAction action = WasmWorkerRuntimeFinishAction::ExitWorker;
  {
    std::lock_guard<std::mutex> lock(control->mutex);
    if (ChooseWasmWorkerRuntimeCallbackDisposition(control->owner != nullptr) ==
        WasmWorkerRuntimeCallbackDisposition::DisposeDetachedResult) {
      // Shutdown detached the owner while the browser Promise was pending. Release any returned
      // WebGPU roots on this callback thread, without touching a pooled pthread or dead renderer.
      device.reset();
      delete context;
      return;
    }
    action = control->owner->finishWasmWorkerRuntimeInitialization(
        std::move(device), context->textureFormat == wgpu::TextureFormat::BGRA8Unorm,
        context->initializationStart, context->workerDeviceCreations);
  }

  if (action == WasmWorkerRuntimeFinishAction::RetryWithRgba8) {
    context->textureFormat = wgpu::TextureFormat::RGBA8Unorm;
    ++context->workerDeviceCreations;
    geode::GeodeDevice::CreateHeadlessAsync(
        context->textureFormat, &AsyncRenderer::completeWasmWorkerDeviceInitialization, context);
    return;
  }

  delete context;
  if (action == WasmWorkerRuntimeFinishAction::ExitWorker) {
    pthread_exit(nullptr);
  }
}

[[noreturn]] void AsyncRenderer::exitWasmWorker() {
  wasmWorkerRuntime_.reset();
  pthread_exit(nullptr);
}

void AsyncRenderer::beginWasmWorkerRuntimeInitialization() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++wasmWorkerRuntimeInitializationCount_;
  }
  auto* context = new WasmWorkerRuntimeInitCallbackContext{
      .control = wasmWorkerRuntimeInitControl_,
      .initializationStart = std::chrono::steady_clock::now(),
  };
  geode::GeodeDevice::CreateHeadlessAsync(
      context->textureFormat, &AsyncRenderer::completeWasmWorkerDeviceInitialization, context);
}

WasmWorkerRuntimeFinishAction AsyncRenderer::finishWasmWorkerRuntimeInitialization(
    std::unique_ptr<geode::GeodeDevice> device, bool usingBgra8PrimaryFormat,
    std::chrono::steady_clock::time_point initializationStart, int workerDeviceCreations) {
  if (device == nullptr) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (std::holds_alternative<ShutdownState>(workerState_)) {
        return WasmWorkerRuntimeFinishAction::ExitWorker;
      }
      wasmWorkerRuntimeInitializationStatus_ = WasmWorkerRuntimeInitializationStatus::Failed;
    }
    handleWasmWorkerRuntimeUnavailable();
    return WasmWorkerRuntimeFinishAction::ExitWorker;
  }

  auto runtime = std::make_unique<WasmWorkerRuntime>();
  runtime->device = std::shared_ptr<geode::GeodeDevice>(std::move(device));
  runtime->renderer = std::make_unique<svg::Renderer>(runtime->device);
  runtime->surfacePresenter =
      std::make_unique<WasmWorkerSurfacePresenter>(runtime->device, useBitmapWorkerSurfaceBridge_);

  // Firefox's shared-texture canvas swapchain prefers BGRA. If another implementation exposes
  // only RGBA, rebuild the worker renderer once in that compatible format during worker startup.
  if (usingBgra8PrimaryFormat && !runtime->surfacePresenter->hasCompatibleSurfaceTargets() &&
      runtime->surfacePresenter->supportsRgba8UnormFallback()) {
    return WasmWorkerRuntimeFinishAction::RetryWithRgba8;
  }

  // The splash and many real documents use clip paths. Compile the otherwise-lazy mask pipeline
  // while the branded loading surface is still visible.
  const auto maskPipelineStart = std::chrono::steady_clock::now();
  (void)runtime->device->maskPipeline();
  const double maskPipelineMs = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - maskPipelineStart)
                                    .count();
  bool schedulePendingWork = false;
  int initializationCount = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::holds_alternative<ShutdownState>(workerState_)) {
      return WasmWorkerRuntimeFinishAction::ExitWorker;
    }
    wasmWorkerRuntime_ = std::move(runtime);
    wasmWorkerRuntimeInitializationStatus_ = WasmWorkerRuntimeInitializationStatus::Ready;
    initializationCount = wasmWorkerRuntimeInitializationCount_;
    const auto* rendering = std::get_if<RenderingState>(&workerState_);
    schedulePendingWork = (rendering != nullptr && rendering->pendingRequest.has_value()) ||
                          std::holds_alternative<CancellingState>(workerState_) ||
                          pendingCompositorWarmup_ || pendingSampleThumbnail_.has_value();
  }

  const double initializationMs = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - initializationStart)
                                      .count();
  PublishWasmWorkerRuntimeStats(initializationMs, maskPipelineMs, initializationCount,
                                workerDeviceCreations,
                                geode::GeodeDevice::headlessCreationCountForTesting());
  if (schedulePendingWork) {
    (void)scheduleWorkerTask();
  }
  return WasmWorkerRuntimeFinishAction::Ready;
}

void AsyncRenderer::runWorkerTask(void* self) {
  AsyncRenderer& renderer = *static_cast<AsyncRenderer*>(self);
  renderer.workerLoop();

  bool hasPendingRequest = false;
  bool cancellationPending = false;
  bool lowPriorityWorkReady = false;
  std::optional<std::uint64_t> presentationBoundaryToken;
  WorkerTaskCompletionDisposition disposition = WorkerTaskCompletionDisposition::Park;
  {
    std::lock_guard<std::mutex> lock(renderer.mutex_);
    const auto* rendering = std::get_if<RenderingState>(&renderer.workerState_);
    hasPendingRequest = rendering != nullptr && rendering->pendingRequest.has_value();
    cancellationPending = std::holds_alternative<CancellingState>(renderer.workerState_);
    lowPriorityWorkReady =
        std::holds_alternative<IdleState>(renderer.workerState_) &&
        (renderer.pendingCompositorWarmup_ || renderer.pendingSampleThumbnail_.has_value());
    if (const auto* pending =
            std::get_if<PendingDirectSurfaceTaskBoundaryState>(&renderer.workerState_)) {
      presentationBoundaryToken = pending->done.result.directSurfaceFrames;
    }
    disposition = ChooseWorkerTaskCompletionDisposition(
        std::holds_alternative<ShutdownState>(renderer.workerState_), hasPendingRequest,
        cancellationPending, lowPriorityWorkReady, presentationBoundaryToken.has_value());
    if (!presentationBoundaryToken.has_value()) {
      // Release the slot while state is still protected. A concurrent request either observes the
      // occupied slot and is covered by this disposition, or owns the newly-released slot itself.
      renderer.workerTaskWakeGate_.completeTask();
    }
  }

  if (disposition == WorkerTaskCompletionDisposition::ExitWorker) {
    renderer.exitWasmWorker();
  }
  if (presentationBoundaryToken.has_value()) {
    // The proxy queue drains callbacks added by a callback before returning to JavaScript. A real
    // worker event-loop turn is required so WebGPU's implicit canvas presentation happens first.
    auto* context = new WasmDirectSurfaceTaskBoundaryCallbackContext{
        .control = renderer.wasmWorkerRuntimeInitControl_,
        .frameToken = *presentationBoundaryToken,
    };
    ScheduleWorkerTaskBoundaryCallback(
        reinterpret_cast<void*>(&AsyncRenderer::acknowledgeDirectSurfaceTaskBoundary), context);
  } else if (disposition == WorkerTaskCompletionDisposition::ScheduleFollowUp) {
    renderer.scheduleWorkerTask();
  }
}

void AsyncRenderer::acknowledgeDirectSurfaceTaskBoundary(void* userdata) {
  std::unique_ptr<WasmDirectSurfaceTaskBoundaryCallbackContext> context(
      static_cast<WasmDirectSurfaceTaskBoundaryCallbackContext*>(userdata));
  const std::shared_ptr<WasmWorkerRuntimeInitControl> control = context->control;
  AsyncRenderer* renderer = nullptr;
  std::function<void()> wake;
  bool notifyStateChange = false;
  bool scheduleFollowUp = false;
  bool exitWorker = false;
  {
    std::lock_guard<std::mutex> ownerLock(control->mutex);
    renderer = control->owner;
    if (renderer == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(renderer->mutex_);
    notifyStateChange =
        renderer->acknowledgeDirectSurfaceTaskBoundaryLocked(context->frameToken, wake);
    const auto* rendering = std::get_if<RenderingState>(&renderer->workerState_);
    const bool hasPendingRequest = rendering != nullptr && rendering->pendingRequest.has_value();
    const bool cancellationPending =
        std::holds_alternative<CancellingState>(renderer->workerState_);
    const bool lowPriorityWorkReady =
        std::holds_alternative<IdleState>(renderer->workerState_) &&
        (renderer->pendingCompositorWarmup_ || renderer->pendingSampleThumbnail_.has_value());
    const bool presentationBoundaryPending =
        std::holds_alternative<PendingDirectSurfaceTaskBoundaryState>(renderer->workerState_);
    const WorkerTaskCompletionDisposition disposition = ChooseWorkerTaskCompletionDisposition(
        std::holds_alternative<ShutdownState>(renderer->workerState_), hasPendingRequest,
        cancellationPending, lowPriorityWorkReady, presentationBoundaryPending);
    renderer->workerTaskWakeGate_.completeTask();
    scheduleFollowUp = disposition == WorkerTaskCompletionDisposition::ScheduleFollowUp;
    exitWorker = disposition == WorkerTaskCompletionDisposition::ExitWorker;
  }
  if (notifyStateChange) {
    renderer->cv_.notify_all();
  }
  if (wake) {
    wake();
  }
  if (exitWorker) {
    renderer->exitWasmWorker();
  }
  if (scheduleFollowUp) {
    renderer->scheduleWorkerTask();
  }
}

WorkerTaskScheduleResult AsyncRenderer::scheduleWorkerTask() {
  if (!threadStarted_ || proxyQueue_ == nullptr) {
    return WorkerTaskScheduleResult::EnqueueRejected;
  }
  WasmWorkerRuntimeWakeAction wakeAction = WasmWorkerRuntimeWakeAction::DeferUntilRuntimeReady;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wakeAction =
        ChooseWasmWorkerRuntimeWakeAction(wasmWorkerRuntimeInitializationStatus_,
                                          std::holds_alternative<ShutdownState>(workerState_));
  }
  if (wakeAction == WasmWorkerRuntimeWakeAction::DeferUntilRuntimeReady ||
      wakeAction == WasmWorkerRuntimeWakeAction::DetachAndCancelWorker) {
    return WorkerTaskScheduleResult::DeferredUntilRuntimeReady;
  }
  if (wakeAction == WasmWorkerRuntimeWakeAction::ReportRuntimeUnavailable) {
    handleWasmWorkerRuntimeUnavailable();
    return WorkerTaskScheduleResult::RuntimeUnavailable;
  }
  if (!workerTaskWakeGate_.trySchedule()) {
    return WorkerTaskScheduleResult::ScheduledOrCoalesced;
  }
  const bool queued =
      emscripten_proxy_async(proxyQueue_, thread_, &AsyncRenderer::runWorkerTask, this);
  if (!queued) {
    handleWorkerTaskEnqueueFailure();
    return WorkerTaskScheduleResult::EnqueueRejected;
  }
  workerTaskWakeFailureCount_.store(0u, std::memory_order_release);
  workerTaskWakeGate_.completeEnqueue(/*queued=*/true);
  return WorkerTaskScheduleResult::ScheduledOrCoalesced;
}

void AsyncRenderer::handleWasmWorkerRuntimeUnavailable() {
  std::function<void()> wake;
  bool notifyStateChange = false;
  bool reportFailure = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::holds_alternative<ShutdownState>(workerState_)) {
      return;
    }
    const bool renderStatePending =
        std::holds_alternative<RenderingState>(workerState_) ||
        std::holds_alternative<CancellingState>(workerState_) ||
        std::holds_alternative<PendingDirectSurfaceTaskBoundaryState>(workerState_);
    const WasmWorkerRuntimeUnavailablePlan plan = ChooseWasmWorkerRuntimeUnavailablePlan(
        renderStatePending, pendingSampleThumbnail_.has_value());
    if (plan.resolveRenderState) {
      cancelRender_.cancel();
      workerState_ = IdleState{};
      notifyStateChange = true;
    }
    if (plan.dropPendingThumbnail) {
      pendingSampleThumbnail_.reset();
      ++sampleThumbnailCounters_.completed;
      ++sampleThumbnailCounters_.cancelled;
      notifyStateChange = true;
    }
    if (pendingCompositorWarmup_ || compositorWarmupActive_) {
      pendingCompositorWarmup_ = false;
      cancelCompositorWarmup_.cancel();
      notifyStateChange = true;
    }
    if (plan.wakeOwner) {
      wake = wakeCallback_;
    }
    reportFailure = !wasmWorkerRuntimeFailureReported_;
    wasmWorkerRuntimeFailureReported_ = true;
  }
  if (notifyStateChange) {
    cv_.notify_all();
  }
  if (wake) {
    wake();
  }
  if (reportFailure) {
    ReportWasmWorkerRuntimeInitializationFailure();
  }
}

void AsyncRenderer::handleWorkerTaskEnqueueFailure() {
  bool shuttingDown = false;
  bool renderRequestDropped = false;
  bool thumbnailDropped = false;
  bool reportSurfaceUnavailable = false;
  WorkerTaskShutdownDisposition shutdownDisposition = WorkerTaskShutdownDisposition::None;
  std::uint64_t failureCount = 0;
  std::function<void()> wake;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    failureCount = workerTaskWakeFailureCount_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    shuttingDown = std::holds_alternative<ShutdownState>(workerState_);
    const bool renderStatePending =
        std::holds_alternative<RenderingState>(workerState_) ||
        std::holds_alternative<CancellingState>(workerState_) ||
        std::holds_alternative<PendingDirectSurfaceTaskBoundaryState>(workerState_);
    const bool enqueueAttemptFromWorker = pthread_equal(pthread_self(), thread_) != 0;
    const WorkerTaskEnqueueFailurePlan plan = ChooseWorkerTaskEnqueueFailurePlan(
        shuttingDown, renderStatePending, pendingSampleThumbnail_.has_value(), failureCount,
        enqueueAttemptFromWorker);
    if (plan.resolveRenderState) {
      cancelRender_.cancel();
      workerState_ = IdleState{};
      renderRequestDropped = true;
    }
    if (plan.dropPendingThumbnail) {
      pendingSampleThumbnail_.reset();
      ++sampleThumbnailCounters_.completed;
      ++sampleThumbnailCounters_.cancelled;
      thumbnailDropped = true;
    }
    // Cache warmup is speculative. A failed wake must never leave an invisible low-priority slot
    // permanently queued or turn surface recovery into an availability failure.
    pendingCompositorWarmup_ = false;
    if (compositorWarmupActive_) {
      cancelCompositorWarmup_.cancel();
    }
    if (plan.wakeOwnerForRetry) {
      wake = wakeCallback_;
    }
    reportSurfaceUnavailable = plan.reportSurfaceUnavailable;
    shutdownDisposition = plan.shutdownDisposition;
    // Resolve all state which may have coalesced behind this nonexistent callback before releasing
    // its slot. A later request can then own a fresh wake instead of inheriting a permanent busy
    // state.
    workerTaskWakeGate_.completeEnqueue(/*queued=*/false);
  }
  cv_.notify_all();
  ReportWorkerTaskWakeFailure(static_cast<double>(failureCount), shuttingDown, renderRequestDropped,
                              thumbnailDropped, reportSurfaceUnavailable);
  if (wake) {
    wake();
  }
  if (shutdownDisposition == WorkerTaskShutdownDisposition::ExitCurrentWorker) {
    exitWasmWorker();
  }
  if (shutdownDisposition == WorkerTaskShutdownDisposition::CancelWorkerBeforeJoin) {
    (void)pthread_cancel(thread_);
  }
}
#endif

void AsyncRenderer::notePublishedCompositedPreview(
    const std::optional<RenderResult::CompositedPreview>& compositedPreview) {
  if (!compositedPreview.has_value() || !compositedPreview->valid()) {
    return;
  }

  // A full-canvas fallback replaces the split tile set in `GlTextureCache`,
  // so future split previews must resend pixels before switching back to
  // metadata-only updates.
  if (compositedPreview->tiles.size() == 1u &&
      compositedPreview->tiles.front().id == "full-canvas") {
    publishedCompositedTiles_.clear();
    return;
  }

  std::unordered_map<std::string, PublishedCompositedTile> nextPublished;
  nextPublished.reserve(compositedPreview->tiles.size());
  for (const RenderResult::CompositedTile& tile : compositedPreview->tiles) {
    if (!tile.bitmap.empty() || tile.textureSnapshot != nullptr) {
      const Vector2i bitmapDims =
          !tile.bitmap.empty() ? tile.bitmap.dimensions : tile.textureSnapshot->dimensions();
      nextPublished[tile.id] = PublishedCompositedTile{
          .kind = tile.kind,
          .generation = tile.generation,
          .bitmapDims = bitmapDims,
          .rasterCanvasSize = tile.rasterCanvasSize,
      };
    } else if (const auto it = publishedCompositedTiles_.find(tile.id);
               it != publishedCompositedTiles_.end()) {
      nextPublished[tile.id] = it->second;
    }
  }
  publishedCompositedTiles_ = std::move(nextPublished);
}

void AsyncRenderer::commitDirectSurfacePresentation(RenderResult& result) {
  if (result.directSurfaceOutcome != DirectSurfacePresentationOutcome::Presented) {
    return;
  }
  const bool generationIsCurrent = DirectSurfacePresentationGenerationIsCurrent(
      result.documentGeneration, minimumDirectSurfaceDocumentGeneration_);
  if (!generationIsCurrent) {
    result.directSurfaceOutcome = DirectSurfacePresentationOutcome::None;
    return;
  }
#ifdef DONNER_WASM_WORKER_SURFACE
  lastPublishedDirectSurfaceSlot_ = result.directSurfaceSlot;
#endif
  lastDirectSurfacePresentation_ = DirectSurfacePresentationState{
      .active = true,
      .rasterViewport = result.rasterViewport,
      .viewport = result.viewport,
      .frameCount = result.directSurfaceFrames,
      .surfaceSlot = result.directSurfaceSlot,
      .selectionChromeBaked = result.directSurfaceSelectionChromeBaked,
      .surfaceBackingSizePx = result.directSurfaceBackingSizePx,
  };
}

bool AsyncRenderer::workerStateBusy(const WorkerState& state) {
  return std::holds_alternative<RenderingState>(state) ||
         std::holds_alternative<CancellingState>(state) ||
         std::holds_alternative<DoneState>(state) ||
         std::holds_alternative<PendingDirectSurfaceTaskBoundaryState>(state);
}

bool AsyncRenderer::workerStateRenderInFlight(const WorkerState& state) {
  return std::holds_alternative<RenderingState>(state) ||
         std::holds_alternative<CancellingState>(state);
}

bool AsyncRenderer::isBusy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workerStateBusy(workerState_) || pendingCompositorWarmup_ || compositorWarmupActive_;
}

bool AsyncRenderer::hasRenderInFlightForTesting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workerStateRenderInFlight(workerState_) || pendingCompositorWarmup_ ||
         compositorWarmupActive_;
}

bool AsyncRenderer::waitUntilNoRenderInFlightForTesting(
    std::chrono::steady_clock::time_point deadline) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_until(lock, deadline, [this] {
    return !workerStateRenderInFlight(workerState_) && !pendingCompositorWarmup_ &&
           !compositorWarmupActive_;
  });
}

void AsyncRenderer::setReplayRenderDelayForTesting(std::chrono::milliseconds delay) {
  const std::chrono::milliseconds clampedDelay = std::max(delay, std::chrono::milliseconds(0));
  replayRenderDelayMsForTesting_.store(clampedDelay.count(), std::memory_order_release);
}

void AsyncRenderer::setReplayResultHoldFramesForTesting(int frameCount) {
  std::lock_guard<std::mutex> lock(mutex_);
  replayResultHoldFramesForTesting_ = std::max(frameCount, 0);
}

void AsyncRenderer::stageDirectSurfaceResultForTesting(RenderResult result) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::holds_alternative<ShutdownState>(workerState_)) {
    return;
  }
  DoneState done;
  done.result = std::move(result);
  workerState_ = std::move(done);
}

void AsyncRenderer::stageDirectSurfaceResultPendingTaskBoundaryForTesting(RenderResult result) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::holds_alternative<ShutdownState>(workerState_)) {
    return;
  }
  PendingDirectSurfaceTaskBoundaryState pending;
  pending.done.result = std::move(result);
  workerState_ = std::move(pending);
}

bool AsyncRenderer::acknowledgeDirectSurfaceTaskBoundaryLocked(std::uint64_t frameToken,
                                                               std::function<void()>& wake) {
  auto* pending = std::get_if<PendingDirectSurfaceTaskBoundaryState>(&workerState_);
  if (pending == nullptr || pending->done.result.directSurfaceFrames != frameToken) {
    return false;
  }
  DoneState done = std::move(pending->done);
  done.directSurfaceTaskBoundaryAcknowledged = true;
  // The result only becomes pollable here, so this is the handoff point the UI-frame wait is
  // measured from. Recording it separates the worker's event-loop hop from that wait.
  done.result.workerTaskBoundaryAt = std::chrono::steady_clock::now();
  workerState_ = std::move(done);
  wake = wakeCallback_;
  return true;
}

bool AsyncRenderer::acknowledgeDirectSurfaceTaskBoundaryForTesting() {
  std::function<void()> wake;
  bool acknowledged = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* pending = std::get_if<PendingDirectSurfaceTaskBoundaryState>(&workerState_);
    if (pending != nullptr) {
      acknowledged = acknowledgeDirectSurfaceTaskBoundaryLocked(
          pending->done.result.directSurfaceFrames, wake);
    }
  }
  if (acknowledged) {
    cv_.notify_all();
  }
  if (wake) {
    wake();
  }
  return acknowledged;
}

bool AsyncRenderer::acknowledgeDirectSurfaceTaskBoundaryForTesting(std::uint64_t frameToken) {
  std::function<void()> wake;
  bool acknowledged = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    acknowledged = acknowledgeDirectSurfaceTaskBoundaryLocked(frameToken, wake);
  }
  if (acknowledged) {
    cv_.notify_all();
  }
  if (wake) {
    wake();
  }
  return acknowledged;
}

void AsyncRenderer::invalidateDirectSurfacePresentation(std::uint64_t documentGeneration) {
  std::lock_guard<std::mutex> lock(mutex_);
  minimumDirectSurfaceDocumentGeneration_ =
      std::max(minimumDirectSurfaceDocumentGeneration_, documentGeneration);
  lastDirectSurfacePresentation_.active = false;
}

void AsyncRenderer::requestRender(const RenderRequest& request) {
  bool signalCancel = false;
#ifdef DONNER_WASM_WORKER_SURFACE
  std::optional<std::uint64_t> discardedBitmapBridgeFrame;
#endif
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::holds_alternative<ShutdownState>(workerState_)) {
      return;
    }
    RenderRequest stagedRequest = request;
    stagedRequest.queuedAt = std::chrono::steady_clock::now();
    if (!request.structuralRemap.empty()) {
      retainedStructuralRemaps_[request.documentGeneration] = request.structuralRemap;
    } else {
      const auto retainedIt = retainedStructuralRemaps_.find(request.documentGeneration);
      if (retainedIt != retainedStructuralRemaps_.end()) {
        stagedRequest.structuralRemap = retainedIt->second;
      }
    }

    // Foreground interaction always outranks speculative cache work. The compositor's normal
    // render path will finish any still-missing payloads against the newest DOM and viewport.
    pendingCompositorWarmup_ = false;
    if (compositorWarmupActive_) {
      cancelCompositorWarmup_.cancel();
    }

    if (auto* rendering = std::get_if<RenderingState>(&workerState_)) {
      // The newest request wins.
      rendering->pendingRequest.emplace(std::move(stagedRequest));
      signalCancel = true;
    } else {
#ifdef DONNER_WASM_WORKER_SURFACE
      if (const auto* done = std::get_if<DoneState>(&workerState_);
          done != nullptr && done->result.bitmapBridgeFrameStaged) {
        discardedBitmapBridgeFrame = done->result.directSurfaceFrames;
      }
#endif
      RenderingState nextRendering;
      nextRendering.pendingRequest.emplace(std::move(stagedRequest));
      signalCancel = std::holds_alternative<CancellingState>(workerState_);
      workerState_ = std::move(nextRendering);
    }
    if (signalCancel) {
      // §M4: tell the in-flight render to bail. Set this while the mutex still
      // exposes the superseding state so the worker cannot start the
      // replacement request and then receive a stale cancel.
      cancelRender_.cancel();
    }
    if (sampleThumbnailActive_) {
      // Main-document presentation always preempts background preview work. The thumbnail
      // traversal polls its independent token between rendered entities.
      cancelSampleThumbnail_.cancel();
    }
  }
#ifdef DONNER_WASM_WORKER_SURFACE
  if (discardedBitmapBridgeFrame.has_value()) {
    DiscardWorkerDocumentBitmap(static_cast<double>(*discardedBitmapBridgeFrame));
  }
#endif
#ifdef DONNER_WASM_WORKER_SURFACE
  scheduleWorkerTask();
#else
  cv_.notify_one();
#endif
}

void AsyncRenderer::cancelInFlight() {
  bool signalCancel = false;
  std::function<void()> wakeAfterSettledCancellation;
#ifdef DONNER_WASM_WORKER_SURFACE
  std::optional<std::uint64_t> discardedBitmapBridgeFrame;
#endif
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingCompositorWarmup_ = false;
    if (compositorWarmupActive_) {
      // Cancellation-token sampling is not an acknowledgment. The worker may have completed its
      // final poll while it still owns DocumentWriteAccess, so remember the waiter until the
      // active flag and guard are released together below.
      compositorWarmupReleaseWakePending_ = true;
      cancelCompositorWarmup_.cancel();
    }
    if (std::holds_alternative<RenderingState>(workerState_)) {
      // Worker is mid-renderFrame. Transition to `Cancelling` (not
      // `Idle`) so the editor's `!isBusy()` gates keep gating registry reads
      // until the worker actually observes the cancel and bails.
      workerState_ = CancellingState{};
      cancelRender_.cancel();
      signalCancel = true;
    } else if (std::holds_alternative<DoneState>(workerState_) ||
               std::holds_alternative<PendingDirectSurfaceTaskBoundaryState>(workerState_)) {
      // Worker raced to completion before we got here. The result
      // is already staged, but the user-input event that triggered this cancel
      // supersedes it. Drop the result and transition to Idle directly.
#ifdef DONNER_WASM_WORKER_SURFACE
      const DoneState* done = std::get_if<DoneState>(&workerState_);
      if (const auto* pending = std::get_if<PendingDirectSurfaceTaskBoundaryState>(&workerState_)) {
        done = &pending->done;
      }
      if (done != nullptr && done->result.bitmapBridgeFrameStaged) {
        discardedBitmapBridgeFrame = done->result.directSurfaceFrames;
      }
#endif
      workerState_ = IdleState{};
    }

    // `isBusy()` and `cancelInFlight()` cannot form an atomic check-then-act pair. If the worker
    // released its guard between those calls, cancellation is already settled and the deferred
    // input still needs a retry frame. Active render/cancellation states provide their own later
    // completion wake; an active compositor warmup uses the durable waiter above.
    const bool cancellationSettled =
        !compositorWarmupActive_ &&
        (std::holds_alternative<IdleState>(workerState_) ||
         std::holds_alternative<DoneState>(workerState_) ||
         std::holds_alternative<PendingDirectSurfaceTaskBoundaryState>(workerState_));
    if (cancellationSettled) {
      wakeAfterSettledCancellation = wakeCallback_;
    }
  }
#ifdef DONNER_WASM_WORKER_SURFACE
  if (discardedBitmapBridgeFrame.has_value()) {
    DiscardWorkerDocumentBitmap(static_cast<double>(*discardedBitmapBridgeFrame));
  }
#endif
  if (signalCancel) {
    // Notify in case the worker was still in `cv_.wait` when we
    // landed - its updated predicate also wakes on `Cancelling`.
    cv_.notify_one();
  }
  if (wakeAfterSettledCancellation) {
    wakeAfterSettledCancellation();
  }
}

std::optional<RenderResult> AsyncRenderer::pollResult() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (auto* done = std::get_if<DoneState>(&workerState_)) {
    if (done->presentationHoldPollsRemaining > 0) {
      --done->presentationHoldPollsRemaining;
      replayResultHoldPollCount_.fetch_add(1, std::memory_order_release);
      const std::function<void()> wake = wakeCallback_;
      lock.unlock();
      // Wasm's expensive main-frame gate is event-driven even though its lightweight scheduler
      // runs every rAF. Request the next UI frame explicitly; otherwise the staged surface could
      // remain behind the prior epoch until another input event arrives.
      if (wake) {
        wake();
      }
      return std::nullopt;
    }

    RenderResult result = std::move(done->result);
#ifdef DONNER_WASM_WORKER_SURFACE
    const bool publishDirectSurfaceBoundaryAcknowledgment =
        done->directSurfaceTaskBoundaryAcknowledged;
#endif
    const HandoffTimings handoff = ComputeHandoffTimings(
        result.workerCompletedAt, result.workerTaskBoundaryAt, std::chrono::steady_clock::now());
    result.workerTiming.pollDelayMs = handoff.pollDelayMs;
    result.workerTiming.taskBoundaryMs = handoff.taskBoundaryMs;
    result.workerTiming.wakeToPollMs = handoff.wakeToPollMs;
    if (!result.overviewInfillOnly) {
      notePublishedCompositedPreview(result.compositedPreview);
    }
    commitDirectSurfacePresentation(result);
    workerState_ = IdleState{};
    if (compositor_ != nullptr && compositor_->hasPendingFirstFrameWarmup()) {
      pendingCompositorWarmup_ = true;
    }
    const bool scheduleLowPriorityWork =
        pendingCompositorWarmup_ || pendingSampleThumbnail_.has_value();
#ifdef DONNER_WASM_WORKER_SURFACE
    const bool commitBitmapBridgeFrame =
        result.bitmapBridgeFrameStaged &&
        result.directSurfaceOutcome == DirectSurfacePresentationOutcome::Presented;
    const bool discardBitmapBridgeFrame =
        result.bitmapBridgeFrameStaged && !commitBitmapBridgeFrame;
#endif
    lock.unlock();
    cv_.notify_all();
#ifdef DONNER_WASM_WORKER_SURFACE
    if (publishDirectSurfaceBoundaryAcknowledgment &&
        result.directSurfaceOutcome == DirectSurfacePresentationOutcome::Presented) {
      PublishDirectSurfaceTaskBoundaryAcknowledgment(
          static_cast<double>(result.directSurfaceFrames));
    }
    if (commitBitmapBridgeFrame) {
      CommitWorkerDocumentBitmap(static_cast<double>(result.directSurfaceFrames),
                                 result.directSurfaceSlot);
    } else if (discardBitmapBridgeFrame) {
      DiscardWorkerDocumentBitmap(static_cast<double>(result.directSurfaceFrames));
    }
    if (scheduleLowPriorityWork) {
      scheduleWorkerTask();
    }
#else
    if (scheduleLowPriorityWork) {
      cv_.notify_one();
    }
#endif
    return result;
  }
  return std::nullopt;
}

bool AsyncRenderer::requestSampleThumbnail(SampleThumbnailRenderRequest request) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::holds_alternative<ShutdownState>(workerState_) ||
#ifdef DONNER_WASM_WORKER_SURFACE
        !CanAcceptWasmSampleThumbnailRequest(wasmWorkerRuntimeInitializationStatus_) ||
#endif
        pendingSampleThumbnail_.has_value() || sampleThumbnailActive_ ||
        sampleThumbnailResult_.has_value()) {
      return false;
    }
    pendingSampleThumbnail_.emplace(std::move(request));
    ++sampleThumbnailCounters_.requested;
  }
#ifdef DONNER_WASM_WORKER_SURFACE
  if (!DidAcceptWasmSampleThumbnailScheduleResult(scheduleWorkerTask())) {
    return false;
  }
#else
  cv_.notify_one();
#endif
  return true;
}

std::optional<SampleThumbnailRenderResult> AsyncRenderer::pollSampleThumbnailResult() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!sampleThumbnailResult_.has_value()) {
    return std::nullopt;
  }
  std::optional<SampleThumbnailRenderResult> result = std::move(sampleThumbnailResult_);
  sampleThumbnailResult_.reset();
  return result;
}

void AsyncRenderer::cancelSampleThumbnailWork() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pendingSampleThumbnail_.has_value()) {
    pendingSampleThumbnail_.reset();
    ++sampleThumbnailCounters_.completed;
    ++sampleThumbnailCounters_.cancelled;
  }
  sampleThumbnailResult_.reset();
  if (sampleThumbnailActive_) {
    discardActiveSampleThumbnailResult_ = true;
    cancelSampleThumbnail_.cancel();
  }
}

SampleThumbnailRenderStats AsyncRenderer::sampleThumbnailRenderStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  SampleThumbnailRenderStats stats = sampleThumbnailCounters_;
  stats.pending = pendingSampleThumbnail_.has_value();
  stats.active = sampleThumbnailActive_;
  stats.resultReady = sampleThumbnailResult_.has_value();
  return stats;
}

void AsyncRenderer::setSampleThumbnailRenderDelayForTesting(std::chrono::milliseconds delay) {
  const std::chrono::milliseconds clamped = std::max(delay, std::chrono::milliseconds(0));
  sampleThumbnailRenderDelayMsForTesting_.store(clamped.count(), std::memory_order_release);
}

void AsyncRenderer::setWakeCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::holds_alternative<ShutdownState>(workerState_)) {
    return;
  }
  wakeCallback_ = std::move(callback);
}

void AsyncRenderer::workerLoop() {
#ifdef DONNER_WASM_WORKER_SURFACE
  // One proxied callback processes at most one frame and then returns to the worker's JavaScript
  // event loop. Firefox commits the transferred WebGPU canvas at that task boundary.
#elif defined(__EMSCRIPTEN__)
  // Emscripten's WebGPU object table is per-worker. Construct and use the
  // renderer on this pthread so wgpu handles never cross JS worker boundaries.
  svg::Renderer workerRenderer;
#endif
#ifndef DONNER_WASM_WORKER_SURFACE
  std::unique_ptr<svg::RendererInterface> sampleThumbnailRenderer;
  svg::RendererInterface* sampleThumbnailRendererRoot = nullptr;
#endif

  while (true) {
    std::optional<RenderRequest> requestStorage;
    std::optional<SampleThumbnailRenderRequest> sampleThumbnailStorage;
    bool runCompositorWarmup = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
#ifdef DONNER_WASM_WORKER_SURFACE
      if (std::holds_alternative<ShutdownState>(workerState_)) {
        lock.unlock();
        exitWasmWorker();
      }
      if (std::holds_alternative<DoneState>(workerState_) ||
          std::holds_alternative<PendingDirectSurfaceTaskBoundaryState>(workerState_) ||
          (std::holds_alternative<IdleState>(workerState_) && !pendingCompositorWarmup_ &&
           !pendingSampleThumbnail_.has_value())) {
        return;
      }
#else
      cv_.wait(lock, [this] {
        return std::holds_alternative<RenderingState>(workerState_) ||
               std::holds_alternative<CancellingState>(workerState_) ||
               std::holds_alternative<ShutdownState>(workerState_) ||
               (std::holds_alternative<IdleState>(workerState_) &&
                (pendingCompositorWarmup_ || pendingSampleThumbnail_.has_value()));
      });
      if (std::holds_alternative<ShutdownState>(workerState_)) {
#ifdef __EMSCRIPTEN__
        // `workerRenderer` is destroyed here in Emscripten builds, i.e. on
        // the worker thread, mirroring its construction.
#endif
        return;
      }
#endif
      if (std::holds_alternative<CancellingState>(workerState_)) {
        // `cancelInFlight` raced with the worker before it could
        // start renderFrame. Transition to Idle and loop back to cv_.wait.
        std::function<void()> wake = wakeCallback_;
        workerState_ = IdleState{};
        lock.unlock();
        cv_.notify_all();
        if (wake) {
          wake();
        }
#ifdef DONNER_WASM_WORKER_SURFACE
        return;
#else
        continue;
#endif
      }
      if (auto* rendering = std::get_if<RenderingState>(&workerState_)) {
        assert(rendering->pendingRequest.has_value() &&
               "Rendering worker state requires a pending request while waiting");
        requestStorage.emplace(std::move(*rendering->pendingRequest));
        rendering->pendingRequest.reset();
      } else {
        assert(std::holds_alternative<IdleState>(workerState_));
        if (pendingCompositorWarmup_) {
          pendingCompositorWarmup_ = false;
          cancelCompositorWarmup_.reset();
          compositorWarmupActive_ = true;
          runCompositorWarmup = true;
        } else {
          assert(pendingSampleThumbnail_.has_value());
          sampleThumbnailStorage.emplace(std::move(*pendingSampleThumbnail_));
          pendingSampleThumbnail_.reset();
          cancelSampleThumbnail_.reset();
          sampleThumbnailActive_ = true;
          discardActiveSampleThumbnailResult_ = false;
          ++sampleThumbnailCounters_.started;
        }
      }
    }

    if (runCompositorWarmup) {
      if (compositor_ != nullptr && compositorDocument_.has_value()) {
        svg::SVGDocument& warmupDocument = *compositorDocument_;
        std::optional<svg::DocumentWriteAccess> documentAccess;
        documentAccess.emplace(warmupDocument.writeAccess());
        (void)compositor_->warmPendingFirstFrameCaches(cancelCompositorWarmup_);
      }

      std::function<void()> wake;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        compositorWarmupActive_ = false;
        const bool releaseWakePending = std::exchange(compositorWarmupReleaseWakePending_, false);
        if (releaseWakePending && !std::holds_alternative<ShutdownState>(workerState_)) {
          // Speculative work changes no visible state, including when a foreground render
          // preempts it. Wake only a caller that explicitly registered against the document
          // guard's release; the foreground render supplies its own completion wake.
          wake = wakeCallback_;
        }
      }
      cv_.notify_all();
      if (wake) {
        wake();
      }
#ifdef DONNER_WASM_WORKER_SURFACE
      return;
#else
      continue;
#endif
    }

    if (sampleThumbnailStorage.has_value()) {
      svg::RendererInterface* offscreenRenderer = nullptr;
#ifdef DONNER_WASM_WORKER_SURFACE
      UTILS_RELEASE_ASSERT(wasmWorkerRuntime_ != nullptr);
      if (wasmWorkerRuntime_->sampleThumbnailRenderer == nullptr) {
        wasmWorkerRuntime_->sampleThumbnailRenderer =
            wasmWorkerRuntime_->renderer->createOffscreenInstance();
        if (wasmWorkerRuntime_->sampleThumbnailRenderer != nullptr) {
          std::lock_guard<std::mutex> lock(mutex_);
          ++sampleThumbnailCounters_.offscreenRendererCreations;
        }
      }
      offscreenRenderer = wasmWorkerRuntime_->sampleThumbnailRenderer.get();
#elif defined(__EMSCRIPTEN__)
      if (sampleThumbnailRenderer == nullptr || sampleThumbnailRendererRoot != &workerRenderer) {
        sampleThumbnailRenderer = workerRenderer.createOffscreenInstance();
        sampleThumbnailRendererRoot = &workerRenderer;
        if (sampleThumbnailRenderer != nullptr) {
          std::lock_guard<std::mutex> lock(mutex_);
          ++sampleThumbnailCounters_.offscreenRendererCreations;
        }
      }
      offscreenRenderer = sampleThumbnailRenderer.get();
#else
      svg::RendererInterface* requestedRoot = sampleThumbnailStorage->nativeRenderer;
      if (requestedRoot != nullptr &&
          (sampleThumbnailRenderer == nullptr || sampleThumbnailRendererRoot != requestedRoot)) {
        sampleThumbnailRenderer = requestedRoot->createOffscreenInstance();
        sampleThumbnailRendererRoot = requestedRoot;
        if (sampleThumbnailRenderer != nullptr) {
          std::lock_guard<std::mutex> lock(mutex_);
          ++sampleThumbnailCounters_.offscreenRendererCreations;
        }
      }
      offscreenRenderer = requestedRoot != nullptr ? sampleThumbnailRenderer.get() : nullptr;
#endif

      SampleThumbnailRenderResult result;
      if (offscreenRenderer == nullptr) {
        result.key = sampleThumbnailStorage->key;
        result.outcome = SampleThumbnailRenderOutcome::RendererUnavailable;
      } else {
        const std::chrono::milliseconds delay(
            sampleThumbnailRenderDelayMsForTesting_.load(std::memory_order_acquire));
        result = RenderSampleThumbnail(std::move(*sampleThumbnailStorage), *offscreenRenderer,
                                       cancelSampleThumbnail_, delay);
      }

      std::function<void()> wake;
      bool notifyStateChange = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        sampleThumbnailActive_ = false;
        if (!std::holds_alternative<ShutdownState>(workerState_)) {
          ++sampleThumbnailCounters_.completed;
          if (discardActiveSampleThumbnailResult_) {
            ++sampleThumbnailCounters_.cancelled;
          } else {
            if (result.outcome == SampleThumbnailRenderOutcome::Rendered) {
              ++sampleThumbnailCounters_.rendered;
            } else if (result.outcome == SampleThumbnailRenderOutcome::Cancelled) {
              ++sampleThumbnailCounters_.cancelled;
            }
            sampleThumbnailResult_.emplace(std::move(result));
          }
          discardActiveSampleThumbnailResult_ = false;
          wake = wakeCallback_;
          notifyStateChange = true;
        }
      }
      if (notifyStateChange) {
        cv_.notify_all();
      }
      if (wake) {
        wake();
      }
#ifdef DONNER_WASM_WORKER_SURFACE
      return;
#else
      continue;
#endif
    }

    assert(requestStorage.has_value());
    RenderRequest& request = *requestStorage;
    const auto workerDequeuedAt = std::chrono::steady_clock::now();
    const double queueWaitMs =
        request.queuedAt.time_since_epoch().count() == 0
            ? 0.0
            : std::chrono::duration<double, std::milli>(workerDequeuedAt - request.queuedAt)
                  .count();
#ifdef DONNER_WASM_WORKER_SURFACE
    UTILS_RELEASE_ASSERT(wasmWorkerRuntime_ != nullptr);
    svg::Renderer& requestRenderer = *wasmWorkerRuntime_->renderer;
#elif defined(__EMSCRIPTEN__)
    svg::Renderer& requestRenderer = workerRenderer;
#else
    // Geode editor builds intentionally use the request renderer so worker texture snapshots are
    // created on the same WGPU device as ImGui presentation.
    svg::Renderer& requestRenderer = request.lease.renderer();
#endif
    // Readback counters live on the backend device shared by the root renderer
    // and compositor offscreens. Start each worker iteration from a clean epoch.
    (void)requestRenderer.consumeReadbackStats();
    svg::SVGDocument& requestDocument = request.lease.document();

    // §M4: every iteration starts with a fresh (non-cancelled) token.
    // The UI thread sets cancel via `requestRender` ONLY when posting
    // while busy, and we're idle here right before the render runs -
    // so any cancel signal from a previous iteration is stale.
    cancelRender_.reset();

    // Execute the render outside the lock so the UI thread can poll
    // `isBusy()` / `pollResult()` while we work.
    ZoneScopedN("AsyncRenderer::workerIteration");
    const auto workerStart = std::chrono::steady_clock::now();
    const auto elapsedSince = [](std::chrono::steady_clock::time_point start) {
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
          .count();
    };
    RenderResult::WorkerTimingBreakdown workerTiming;
    workerTiming.queueWaitMs = queueWaitMs;
    workerTiming.dequeueToStartMs =
        std::chrono::duration<double, std::milli>(workerStart - workerDequeuedAt).count();
    std::optional<RenderResult::CompositedPreview> compositedPreview;

    // §concurrent-dom: serialize this worker render against UI-thread DOM reads. The lease shares
    // the live registry (it does not snapshot), and the worker cannot touch the document in
    // SingleThreaded mode (owner-thread assert). The document is flipped to ConcurrentDom on first
    // render and stays there for the editor's lifetime - UI-thread reads are responsible for
    // holding their own access guard (`withReadAccess` / a scoped `DocumentReadAccess`) where they
    // touch the live document. The worker holds a write guard across the document-reading render
    // work and releases it via `releaseDocumentAccess()` before every `mutex_` section below to
    // avoid a lock-order inversion against UI threads holding `mutex_` while reading the DOM.
    if (requestDocument.threadingMode() != svg::ThreadingMode::ConcurrentDom) {
      requestDocument.setThreadingMode(svg::ThreadingMode::ConcurrentDom);
    }
    std::optional<svg::DocumentWriteAccess> documentAccess;
    documentAccess.emplace(requestDocument.writeAccess());
    const auto releaseDocumentAccess = [&]() { documentAccess.reset(); };
    const EditorRasterViewport rasterViewport =
        EffectiveRasterViewportForRequest(requestDocument, request.rasterViewport);

    // Compositor lifecycle is split into two independent decisions:
    //
    //   1. Do we need a fresh `CompositorController` instance? Only
    //      on first construction or when the renderer pointer changes
    //      (e.g. backend swap). The renderer is owned by the worker
    //      and constructed at the top of `workerLoop`, so in steady
    //      state this is just the first-frame case.
    //
    //   2. Did the document space swap underneath us? `setDocument`
    //      and `setDocumentMaybeStructural` both bump
    //      `documentGeneration` and produce a fresh `Registry` (the
    //      `SVGDocumentHandle` pointer changes). When that happens we
    //      try the structural-remap path FIRST - it preserves cached
    //      filter / bucket bitmaps, `canvasFromBitmap` stamps, and
    //      the pre-warmed bg/fg pair - and only fall back to a
    //      destructive `resetAllLayers(documentReplaced=true)` when
    //      no remap is available or the remap itself fails an
    //      invariant check.
    //
    // The previous implementation collapsed step 1 onto a pointer-
    // identity check that fired on every `setDocumentMaybeStructural`
    // (since the new doc carries a new `Registry` handle), making
    // step 2's structural-remap branch unreachable on the
    // drag-release writeback path. The user-visible symptom was a
    // filter-group "snap back to original position" on drag release:
    // the freshly-reconstructed compositor blitted its zero-offset
    // bitmap of the pre-drag layer state while the editor's cached
    // GL textures still showed the dragged element at its rasterize-
    // time position. Pinned by
    // `RnrReplayTest::FilterSnapbackReproPreservesCompositorAcrossWriteback`.
    const bool needsFreshCompositor = !compositor_ || compositorRenderer_ != &requestRenderer;
    if (needsFreshCompositor) {
      svg::compositor::CompositorConfig compositorConfig;
      // The editor retains compositor textures across frames, so even a geometrically cheap span
      // is less expensive to upload once than to rerasterize during every pointer update. Browser
      // WebGPU makes the difference especially pronounced, but the same policy removes the native
      // renderer's dominant steady-drag CPU cost as well.
      compositorConfig.immediateStaticSpans = false;
      compositorConfig.dynamicImmediateStaticSpans = false;
      // The first full-document draw is already correct. Publish it first, then warm retained
      // caches from the worker's independent low-priority lane after the result is accepted.
      compositorConfig.deferFirstFrameWarmup = true;
#ifdef DONNER_WASM_WORKER_SURFACE
      // With pooled offscreen renderers, a multi-tile rasterize pass no longer
      // performs the per-tile teardown device polls whose ASYNCIFY suspensions
      // incidentally serviced the worker's event loop mid-pass. Canvas-size
      // commits and WebGPU callbacks arrive as worker events, so yield for one
      // event-loop turn at each tile boundary instead. A cancellation
      // delivered by the yield is observed at the compositor's next
      // `isCancelled()` poll, immediately after this callback returns.
      compositorConfig.yieldBetweenTiles = []() { emscripten_sleep(0); };
#endif
      // CompositorController stores its SVGDocument by reference. Bind that reference to the
      // AsyncRenderer-owned value before constructing the controller: RenderLease is destroyed
      // after this request, while deferred warmup and later frames intentionally outlive it.
      compositor_.reset();
      compositorDocument_.emplace(requestDocument);  // Cheap: refcount bump on the Registry.
      compositor_ = std::make_unique<svg::compositor::CompositorController>(
          *compositorDocument_, requestRenderer, compositorConfig);
      compositorRenderer_ = &requestRenderer;
      compositorEntity_ = entt::null;
      compositorEntities_.clear();
      compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
      compositorDocumentGeneration_ = request.documentGeneration;
      publishedCompositedTiles_.clear();
      compositorReconstructCount_.fetch_add(1, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        retainedStructuralRemaps_.erase(request.documentGeneration);
      }
    }

    const bool documentSwapDetected =
        !needsFreshCompositor &&
        (request.documentGeneration != compositorDocumentGeneration_ ||
         (compositorDocument_.has_value() &&
          compositorDocument_->handle().get() != requestDocument.handle().get()));
    if (documentSwapDetected) {
      // Preserve the SVGDocument object's address because CompositorController references it, but
      // update its shared Registry handle before asking the compositor to remap/reset against the
      // replacement entity space.
      assert(compositorDocument_.has_value());
      *compositorDocument_ = requestDocument;
      bool remapped = false;
      if (!request.structuralRemap.empty()) {
        remapped = compositor_->remapAfterStructuralReplace(request.structuralRemap);
        if (remapped && compositorEntity_ != entt::null) {
          const auto it = request.structuralRemap.find(compositorEntity_);
          if (it != request.structuralRemap.end()) {
            compositorEntity_ = it->second;
            std::vector<Entity> remappedEntities;
            remappedEntities.reserve(compositorEntities_.size());
            for (Entity entity : compositorEntities_) {
              const auto entityIt = request.structuralRemap.find(entity);
              if (entityIt != request.structuralRemap.end()) {
                AppendUniqueEntity(&remappedEntities, entityIt->second);
              }
            }
            compositorEntities_ = std::move(remappedEntities);
          } else {
            // The drag/selection target didn't survive the remap - fall
            // through to the reset branch so subsequent promote calls
            // start clean.
            compositorEntity_ = entt::null;
            remapped = false;
          }
        }
      }
      if (!remapped) {
        compositor_->resetAllLayers(/*documentReplaced=*/true);
        compositorEntity_ = entt::null;
        compositorEntities_.clear();
        compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
        compositorResetCount_.fetch_add(1, std::memory_order_release);
      }
      publishedCompositedTiles_.clear();
      compositorDocumentGeneration_ = request.documentGeneration;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        retainedStructuralRemaps_.erase(request.documentGeneration);
      }
    }

    // Geometry debug is a frame-global final pass. Retained segmented tiles can
    // crop or cover its wireframe, so every debug frame stays flat and carries
    // no selection prewarm/promotion state. Toggling back off resets once more,
    // then the normal promotion path below rebuilds the selected layer.
    const bool geometryDebugOverlayRequested =
        geometryDebugOverlay_.load(std::memory_order_acquire);
    requestRenderer.setDebugGeometryOverlay(geometryDebugOverlayRequested);
    // Unsupported backends intentionally ignore the request and report false.
    // Keep their normal retained-presentation path active instead of clearing
    // selection prewarm for a debug pass they cannot draw.
    const bool geometryDebugOverlay = requestRenderer.debugGeometryOverlay();
    const bool geometryDebugOverlayChanged = geometryDebugOverlay != appliedGeometryDebugOverlay_;
    if (geometryDebugOverlayChanged) {
      appliedGeometryDebugOverlay_ = geometryDebugOverlay;
      compositor_->resetAllLayers();
      compositorResetCount_.fetch_add(1, std::memory_order_release);
      compositorEntity_ = entt::null;
      compositorEntities_.clear();
      compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
      publishedCompositedTiles_.clear();
    }

    // Resolve what the compositor should be promoted on this render.
    // Priority: explicit drag targets win over the persistent selection hint;
    // otherwise we keep the selected entity promoted so the next drag
    // arrives with everything pre-warmed. Multi-select drags intentionally
    // promote every selected participant: the presenter applies one shared
    // document-space transform to every drag-target tile, keeping the path
    // overlay and cached content in lockstep while avoiding a full DOM render
    // on each pointer frame.
    const std::vector<Entity> desiredEntities =
        geometryDebugOverlay ? std::vector<Entity>() : DesiredCompositorEntities(request);
    const Entity desiredEntity = desiredEntities.empty() ? entt::null : desiredEntities.front();
    const svg::compositor::InteractionHint desiredKind =
        request.dragPreview.has_value() ? request.dragPreview->interactionKind
                                        : svg::compositor::InteractionHint::Selection;

    // Re-promote when EITHER the entity changes OR the kind changes (the
    // editor flips Selection → ActiveDrag at drag start without changing
    // the entity). The compositor's `promoteEntity` refreshes the kind
    // in place for an already-promoted entity instead of demoting and
    // re-promoting, so the layer's cached bitmap survives the
    // transition. Skipping the kind-change re-promote left the
    // compositor treating an active drag as a Selection prewarm and
    // tripped the descendant-segment dirty cascade every drag frame
    // post-zoom - sustained > 1 s/frame on the splash.
    const bool entityChanged = !SameEntityList(compositorEntities_, desiredEntities);
    // Keep a selected entity in ActiveDrag mode after mouse-up so the
    // layer/segment caches stay hot for release-to-drag cycles. The
    // interaction kind changes back to Selection only when a different
    // entity is promoted.
    const bool kindUpgrade =
        desiredEntity != entt::null &&
        compositorInteractionKind_ == svg::compositor::InteractionHint::Selection &&
        desiredKind == svg::compositor::InteractionHint::ActiveDrag;
    if (entityChanged || kindUpgrade) {
      if (entityChanged) {
        for (Entity oldEntity : compositorEntities_) {
          if (!ContainsEntity(desiredEntities, oldEntity)) {
            compositor_->demoteEntity(oldEntity);
          }
        }
        compositorEntities_.clear();
        compositorEntity_ = entt::null;
        compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
      }

      for (Entity entity : desiredEntities) {
        const svg::compositor::CompositorController::PromoteResult promoteResult =
            compositor_->promoteEntity(entity, desiredKind);
        if (promoteResult.promotedLayer()) {
          AppendUniqueEntity(&compositorEntities_, entity);
          if (compositorEntity_ == entt::null) {
            compositorEntity_ = entity;
          }
          compositorInteractionKind_ = desiredKind;
        } else if (promoteResult.fullCanvasPreviewRequired()) {
          // Valid renderable content under a filter, clip-path, or mask is presented through the
          // full-canvas composited tile built from the final snapshot below.
        }
      }
      if (compositorEntities_.empty()) {
        compositorEntity_ = entt::null;
      }
    }
    if (request.dragPreview.has_value() && request.dragPreview->forceLayerRasterization) {
      for (Entity entity : DragPreviewEntities(*request.dragPreview)) {
        compositor_->markPromotedLayerDirty(entity);
      }
    }
#ifndef DONNER_WASM_WORKER_SURFACE
    const bool desiredPromotionIncomplete =
        !desiredEntities.empty() && !ContainsAllEntities(compositorEntities_, desiredEntities);
#endif

    // The DOM is the sole source of truth for the dragged entity's
    // position - `SelectTool` mutates the `transform` attribute every
    // drag frame, so by the time we reach here the compositor's fast
    // path will diff the new DOM transform against the cached bitmap's
    // rasterize-time transform and either reuse the bitmap via a
    // pure-translation compose offset or mark it dirty for re-rasterize.
    // No emulation layer on top of the DOM.
    svg::RenderViewport viewport;
    const Vector2i semanticCanvasSize = requestDocument.canvasSize();
    [[maybe_unused]] const Box2d documentViewBox = requestDocument.svgElement().viewBox().value_or(
        Box2d::FromXYWH(0, 0, static_cast<double>(semanticCanvasSize.x),
                        static_cast<double>(semanticCanvasSize.y)));
    const Vector2i outputCanvasSize = rasterViewport.outputSizePx;
    viewport.size = Vector2d(outputCanvasSize.x, outputCanvasSize.y);
    viewport.devicePixelRatio = 1.0;
    const Transform2d semanticCanvasFromDocument = requestDocument.canvasFromDocumentTransform();
    const Transform2d surfaceFromCanvas =
        semanticCanvasFromDocument.inverse() * rasterViewport.outputFromDocument;
    // Push the current UI-thread setting for tight-bounded segments
    // into the compositor. Setter is a no-op when unchanged; otherwise
    // it marks all segments dirty so the flip takes effect this frame.
    compositor_->setTightBoundedSegmentsEnabled(
        tightBoundedSegments_.load(std::memory_order_acquire));

    // Keep the compositor hint in ActiveDrag across mouse-up so the
    // layer/segment caches survive quick release->drag-again cycles, but
    // only skip the main-renderer compose while an actual drag request is
    // in flight. Post-release and Selection-prewarm renders must refresh
    // the final CPU snapshot so the full-canvas composited tile, when
    // needed, matches the DOM and tile metadata.
    const bool activeDragRequest =
        request.dragPreview.has_value() &&
        request.dragPreview->interactionKind == svg::compositor::InteractionHint::ActiveDrag;
#ifdef DONNER_WASM_WORKER_SURFACE
    // The worker-owned browser surface consumes the compositor's final texture,
    // so every request must compose on the worker even during an active drag.
    compositor_->setSkipMainComposeDuringSplit(false);
#else
    const bool splitPreviewSafe = !desiredPromotionIncomplete;
    compositor_->setSkipMainComposeDuringSplit(activeDragRequest && splitPreviewSafe &&
                                               !request.captureCpuSnapshot);
#endif
    workerTiming.setupMs = elapsedSince(workerStart);

    // Build a CompositedPreview from the compositor's current tile state.
    // Tiles whose id/generation/dimensions were already published carry
    // metadata only; the GL cache keeps the existing texture and applies
    // updated presentation geometry.
#ifndef DONNER_WASM_WORKER_SURFACE
    const auto buildCompositedPreview = [&]() -> std::optional<RenderResult::CompositedPreview> {
      if (request.overviewInfillOnly) {
        return std::nullopt;
      }
      if (!splitPreviewSafe || !request.dragPreview.has_value() ||
          compositorEntity_ == entt::null || compositor_->layerCount() == 0u) {
        return std::nullopt;
      }
      const std::vector<Entity> dragPreviewEntities = DragPreviewEntities(*request.dragPreview);
      const Transform2d documentFromOutput = rasterViewport.outputFromDocument.inverse();
      const auto outputPointToPresentedDoc = [&](const Vector2d& outputPoint) {
        return documentFromOutput.transformPosition(outputPoint) - documentViewBox.topLeft;
      };
      const auto outputVectorToDoc = [&](const Vector2d& outputVector) {
        return documentFromOutput.transformVector(outputVector);
      };
      const auto documentFromCachedDocument = [&](const Transform2d& outputFromCachedOutput) {
        return rasterViewport.outputFromDocument * outputFromCachedOutput * documentFromOutput;
      };
      const auto publishedTextureMatches = [this](const std::string& tileId,
                                                  RenderResult::CompositedTile::Kind kind,
                                                  std::uint64_t generation,
                                                  const Vector2i& bitmapDims,
                                                  const Vector2i& rasterCanvasSize) {
        const auto publishedIt = publishedCompositedTiles_.find(tileId);
        return publishedIt != publishedCompositedTiles_.end() && publishedIt->second.kind == kind &&
               publishedIt->second.generation == generation &&
               publishedIt->second.bitmapDims.x == bitmapDims.x &&
               publishedIt->second.bitmapDims.y == bitmapDims.y &&
               publishedIt->second.rasterCanvasSize.x == rasterCanvasSize.x &&
               publishedIt->second.rasterCanvasSize.y == rasterCanvasSize.y;
      };
      const auto outputTileId = [](const svg::compositor::CompositorTile& ct) {
        // Immediate (direct-rendered) static segments share the same stable tile
        // identity as composited static segments. The identity must NOT encode
        // the generation: a steady drag frame leaves the underlying segment
        // unchanged, so a generation-suffixed id would make every frame look
        // like a brand-new tile and defeat texture/metadata reuse. Generation
        // is tracked separately on the output tile.
        return std::to_string(ct.tileId);
      };
      const auto outputTileKind = [](const svg::compositor::CompositorTile& ct) {
        using OutKind = RenderResult::CompositedTile::Kind;
        if (ct.layerEntity != entt::null) {
          return OutKind::Layer;
        }
        return ct.immediate ? OutKind::Immediate : OutKind::Segment;
      };

      using svg::compositor::CompositorTileBitmapPayload;
      auto compositorTiles =
          compositor_->snapshotTilesForUpload(CompositorTileBitmapPayload::MetadataOnly);
      const bool metadataReuseRequest =
          activeDragRequest ||
          compositorInteractionKind_ == svg::compositor::InteractionHint::ActiveDrag;
      bool canReuseNonDragTextures = !publishedCompositedTiles_.empty();
      std::size_t activeDragTilesAvailable = 0u;
      bool activeDragTileNeedsPayload = false;
      bool hasImmediateTile = false;
      for (const auto& ct : compositorTiles) {
        if (ct.bitmapDims.x <= 0 || ct.bitmapDims.y <= 0) {
          continue;
        }
        using OutKind = RenderResult::CompositedTile::Kind;
        const OutKind kind = outputTileKind(ct);
        const bool currentActiveDragLayer =
            activeDragRequest && ContainsEntity(dragPreviewEntities, ct.layerEntity);
        const std::string tileId = outputTileId(ct);
        if (kind == OutKind::Immediate) {
          hasImmediateTile = true;
          if (metadataReuseRequest && !publishedTextureMatches(tileId, kind, ct.generation,
                                                               ct.bitmapDims, outputCanvasSize)) {
            canReuseNonDragTextures = false;
            break;
          }
          if (currentActiveDragLayer) {
            ++activeDragTilesAvailable;
          }
          continue;
        }
        if (currentActiveDragLayer) {
          ++activeDragTilesAvailable;
          activeDragTileNeedsPayload = !publishedTextureMatches(tileId, kind, ct.generation,
                                                                ct.bitmapDims, outputCanvasSize);
          continue;
        }
        if (ct.isDragTarget && activeDragRequest) continue;
        if (!publishedTextureMatches(tileId, kind, ct.generation, ct.bitmapDims,
                                     outputCanvasSize)) {
          canReuseNonDragTextures = false;
          break;
        }
      }
      if (activeDragRequest && activeDragTilesAvailable < dragPreviewEntities.size()) {
        canReuseNonDragTextures = false;
      }
      CompositorTileBitmapPayload payload = CompositorTileBitmapPayload::All;
      if (canReuseNonDragTextures) {
        if (metadataReuseRequest && activeDragTileNeedsPayload) {
          payload = CompositorTileBitmapPayload::DragTargetOnly;
        } else if (metadataReuseRequest) {
          payload = CompositorTileBitmapPayload::MetadataOnly;
        } else if (hasImmediateTile) {
          payload = CompositorTileBitmapPayload::ImmediateOnly;
        } else if (activeDragTileNeedsPayload) {
          payload = CompositorTileBitmapPayload::DragTargetOnly;
        } else {
          payload = CompositorTileBitmapPayload::MetadataOnly;
        }
      }
      if (payload != CompositorTileBitmapPayload::MetadataOnly) {
        compositorTiles = compositor_->snapshotTilesForUpload(payload);
      }
      std::vector<RenderResult::CompositedTile> previewTiles;
      previewTiles.reserve(compositorTiles.size());
      for (auto& ct : compositorTiles) {
        if (ct.bitmapDims.x <= 0 || ct.bitmapDims.y <= 0) continue;
        using OutKind = RenderResult::CompositedTile::Kind;
        const std::string tileId = outputTileId(ct);
        const OutKind kind = outputTileKind(ct);
        const bool hasPayload = !ct.bitmap.empty() || ct.textureSnapshot != nullptr;
        const bool metadataOnly =
            !hasPayload &&
            publishedTextureMatches(tileId, kind, ct.generation, ct.bitmapDims, outputCanvasSize);
        if (!metadataOnly && !hasPayload) continue;
        RenderResult::CompositedTile tile;
        tile.kind = kind;
        tile.id = tileId;
        tile.layerEntity = ct.layerEntity;
        tile.generation = ct.generation;
        tile.bitmapDimsPx = ct.bitmapDims;
        tile.rasterCanvasSize = outputCanvasSize;
        tile.canvasOffsetDoc = outputPointToPresentedDoc(ct.canvasOffsetPx);
        tile.bitmapDimsDoc = outputVectorToDoc(
            Vector2d(static_cast<double>(ct.bitmapDims.x), static_cast<double>(ct.bitmapDims.y)));
        if (ct.layerEntity != entt::null) {
          tile.documentFromCachedDocument = documentFromCachedDocument(ct.canvasFromBitmap);
          tile.dragTranslationDoc = tile.documentFromCachedDocument.translation();
        }
        tile.isDragTarget = ct.isDragTarget;
        if (!metadataOnly) {
          tile.bitmap = std::move(ct.bitmap);
          tile.textureSnapshot = std::move(ct.textureSnapshot);
        }
        previewTiles.push_back(std::move(tile));
      }
      if (previewTiles.empty()) {
        return std::nullopt;
      }
      return RenderResult::CompositedPreview{
          .tiles = std::move(previewTiles),
          .entity = compositorEntity_,
          .interactionKind = request.dragPreview->interactionKind,
          .representedDragPreview = request.dragPreview,
      };
    };
#endif

    bool renderCompleted = true;
    const std::chrono::milliseconds replayDelay(
        replayRenderDelayMsForTesting_.load(std::memory_order_acquire));
    if (replayDelay.count() > 0) {
      const auto delayDeadline = std::chrono::steady_clock::now() + replayDelay;
      while (!cancelRender_.isCancelled()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= delayDeadline) {
          break;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(delayDeadline - now);
        std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(1)));
      }
      renderCompleted = !cancelRender_.isCancelled();
    }
    if (renderCompleted) {
      ZoneScopedN("Compositor::renderFrame");
      // The final worker timing below intentionally covers the whole
      // presentation-gating iteration, including any readback or tile
      // snapshot work after renderFrame. Keep this scoped timing in
      // Tracy only for drilling into the compositor itself.
      const auto renderFrameStart = std::chrono::steady_clock::now();
      renderCompleted = compositor_->renderFrame(viewport, cancelRender_, surfaceFromCanvas);
#ifdef DONNER_WASM_WORKER_SURFACE
      if (renderCompleted && request.directSurfaceSelectionChrome.has_value()) {
        // The document and Select-mode chrome must enter the browser compositor
        // as one texture epoch. Appending here avoids a permanent cross-layer
        // race where the worker surface can advance before ImGui publishes the
        // matching outline on the main thread.
        requestRenderer.setPreserveTargetOnBeginFrame(true);
        requestRenderer.beginFrame(viewport);
        svg::ResolvedClip surfaceClip;
        surfaceClip.clipRect = Box2d::FromXYWH(0.0, 0.0, viewport.size.x, viewport.size.y);
        requestRenderer.pushClip(surfaceClip);
        OverlayRenderer::drawChromeFromSnapshot(requestRenderer,
                                                *request.directSurfaceSelectionChrome);
        requestRenderer.popClip();
        requestRenderer.endFrame();
        requestRenderer.setPreserveTargetOnBeginFrame(false);
      }
      if (renderCompleted) {
        // The transparency checkerboard, in destination-over after the compose.
        //
        // Desktop draws it FIRST, scissored to the document rect, and
        // composites tiles over it. The worker surface hands the browser one
        // already-composed texture, so drawing it first is not available: it
        // has to go underneath what is already there. Without this pass every
        // see-through document pixel reaches the browser compositor at alpha
        // zero and the page's solid background shows where the desktop editor
        // shows checkerboard.
        //
        // The pattern is anchored to the page, not to the texture, so it stays
        // put while the surface element pans with the document - the same
        // window-anchored pattern the desktop underlay draws.
        svg::CheckerboardUnderlayParams checkerboard;
        checkerboard.devicePixelRatio = request.viewport.devicePixelRatio;
        checkerboard.originOffsetPx =
            request.viewport.documentToScreen(rasterViewport.documentRect).topLeft *
            request.viewport.devicePixelRatio;
        std::ignore = requestRenderer.drawCheckerboardUnderlay(checkerboard);
      }
#endif
      workerTiming.renderFrameMs = elapsedSince(renderFrameStart);
    }

    // A superseding request can arrive after the compositor's final internal cancellation point.
    // Recheck at the presentation boundary so a stale pointer frame never enters a browser surface
    // handoff that cannot itself be cancelled.
    if (renderCompleted && cancelRender_.isCancelled()) {
      renderCompleted = false;
    }

    if (renderCompleted) {
      // SVG traversal is complete. Snapshot/readback, browser presentation, and diagnostic
      // packaging below use renderer/compositor-owned state only, so release the live DOM before
      // those potentially slow operations. UI input can then acquire the document without waiting
      // for a browser surface handoff to finish.
      releaseDocumentAccess();
    }

    bool directSurfacePresented = false;
    std::optional<WorkerSurfaceFailureKind> directSurfaceTerminalFailure;
    int directSurfaceSlot = 0;
    Vector2i directSurfaceBackingSizePx = Vector2i::Zero();
#ifdef DONNER_WASM_WORKER_SURFACE
    if (renderCompleted) {
      const auto presentStart = std::chrono::steady_clock::now();
      std::shared_ptr<const svg::RendererTextureSnapshot> ownedComposedTexture;
      const svg::RendererTextureSnapshot* composedTexture = nullptr;
      if (ChooseTextureSnapshotHandoff(/*consumerOutlivesCurrentFrame=*/false) ==
          TextureSnapshotHandoff::BorrowCurrentFrame) {
        composedTexture = requestRenderer.borrowTextureSnapshot();
      } else {
        ownedComposedTexture = requestRenderer.takeTextureSnapshot();
        composedTexture = ownedComposedTexture.get();
      }
      // Both presentation paths double-buffer across two DOM canvas slots:
      // present into the slot the main thread has *not* accepted, so the epoch
      // being drawn is never the epoch being displayed. Acceptance then flips
      // visibility and geometry together (see WasmWorkerSurfacePresenter's
      // constructor for why a single direct surface flickers between scales).
      const int requestedSurfaceSlot =
          NextWorkerSurfacePresentSlot(lastPublishedDirectSurfaceSlot_);
      const std::uint64_t frameToken = directSurfaceFrameCount_ + 1u;
      svg::RendererBitmap surfaceDiagnostic;
      const bool captureSurfaceDiagnostic =
          publishWorkerSurfaceDiagnostic_ && !workerSurfaceDiagnosticPublished_;
      if (captureSurfaceDiagnostic) {
        workerSurfaceDiagnosticAttempted_ = true;
        surfaceDiagnostic = requestRenderer.takeSnapshot();
      }
      const WorkerSurfacePresentResult presentation = wasmWorkerRuntime_->surfacePresenter->present(
          composedTexture, requestedSurfaceSlot, frameToken,
          request.viewport.rasterBackingCapPx());
      directSurfaceSlot = presentation.surfaceSlot;
      directSurfaceBackingSizePx = presentation.configuredBackingSize;
      if (presentation.disposition == WorkerSurfacePresentDisposition::RetryNextWorkerTask) {
        // The surface rejected this presentation attempt, so the local snapshot has no accepted
        // frame token to describe. The presenter bounds these same-request retries; allow the next
        // attempt to capture diagnostics for the frame that can actually be accepted.
        workerSurfaceDiagnosticAttempted_ = false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto* rendering = std::get_if<RenderingState>(&workerState_)) {
          if (!rendering->pendingRequest.has_value()) {
            rendering->pendingRequest.emplace(std::move(*requestStorage));
            requestStorage.reset();
          }
        }
        return;
      }
      directSurfacePresented =
          presentation.disposition == WorkerSurfacePresentDisposition::Presented;
      if (presentation.disposition == WorkerSurfacePresentDisposition::TerminalFailure) {
        directSurfaceTerminalFailure = presentation.terminalFailure;
      }
      if (directSurfacePresented) {
        ++directSurfaceFrameCount_;
        if (captureSurfaceDiagnostic && workerSurfaceDiagnosticAttempted_) {
          const WorkerSurfacePixelStats stats = MeasureWorkerSurfacePixels(surfaceDiagnostic);
          PublishWorkerSurfaceDiagnostic(static_cast<double>(frameToken), stats.samples,
                                         stats.coloredPixels, stats.nonBlackPixels,
                                         stats.maxChannel, stats.textStyleBackgroundPixels,
                                         stats.textStyleGlyphPixels);
          workerSurfaceDiagnosticPublished_ = true;
          workerSurfaceDiagnosticAttempted_ = false;
        }
      }
      workerTiming.presentMs = elapsedSince(presentStart);
    }
#endif

    // §M4: a cancelled render leaves compositor dirty flags ready for the next
    // pass. Do not publish a partial result; either loop into the superseding
    // request or park after a cancel-without-replacement.
    if (!renderCompleted) {
      // Release document access before taking `mutex_` to avoid a lock-order inversion.
      releaseDocumentAccess();
      cancelledRenderCount_.fetch_add(1, std::memory_order_release);
      std::function<void()> wake;
      bool notifyStateChange = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::holds_alternative<CancellingState>(workerState_)) {
          workerState_ = IdleState{};
          wake = wakeCallback_;
          notifyStateChange = true;
        }
      }
      if (notifyStateChange) {
        cv_.notify_all();
      }
      if (wake) {
        wake();
      }
#ifdef DONNER_WASM_WORKER_SURFACE
      return;
#else
      continue;
#endif
    }

    // Build a CompositedPreview from the compositor tile set when available.
    // If the splitter cannot provide tiles for this frame, the final snapshot
    // below is wrapped as a single full-canvas tile so presentation still goes
    // through the compositor path.
    {
      const auto buildPreviewStart = std::chrono::steady_clock::now();
#ifdef DONNER_WASM_WORKER_SURFACE
      if (directSurfacePresented) {
        compositedPreview = BuildDirectSurfaceCompositorTileMetadata(
            *compositor_, rasterViewport, documentViewBox, compositorEntity_, request.dragPreview,
            request.overviewInfillOnly);
      }
#else
      compositedPreview = buildCompositedPreview();
#endif
      workerTiming.buildPreviewMs = elapsedSince(buildPreviewStart);
    }

    // Selection chrome is no longer baked into the bitmap - main.cc
    // draws it via the ImGui draw list every frame so clicks don't
    // pay the SVG re-rasterize cost. The `request.selection` field
    // is left in place for back-compat callers but ignored here.
    (void)request.selection;
    svg::RendererBitmap bitmap;
    std::shared_ptr<const svg::RendererTextureSnapshot> fullCanvasTexture;
    PresentationSnapshotPlan snapshotPlan;
    // Worker-surface builds present GPU-native frames and ignore
    // request.captureCpuSnapshot; browser diagnostics read pixels through the
    // async smoke-readback path instead.
#ifndef DONNER_WASM_WORKER_SURFACE
    snapshotPlan = ChoosePresentationSnapshotPlan(
        compositedPreview.has_value(), requestRenderer.requiresTextureSnapshotPresentation(),
        request.captureCpuSnapshot);
#endif
    {
      const auto finalSnapshotStart = std::chrono::steady_clock::now();
      // Read before exporting the texture because texture export detaches the renderer target.
      if (snapshotPlan.captureCpuSnapshot) {
        ZoneScopedN("Renderer::takeSnapshot");
        bitmap = requestRenderer.takeSnapshot();
      }
      if (snapshotPlan.captureTextureSnapshot) {
        ZoneScopedN("Renderer::takeTextureSnapshot");
        fullCanvasTexture = requestRenderer.takeTextureSnapshot();
        UTILS_RELEASE_ASSERT_MSG(
            fullCanvasTexture != nullptr,
            "Geode full-canvas presentation did not produce a GPU texture. Refusing CPU "
            "readback/upload fallback in Geode presentation mode.");
      }
      workerTiming.finalSnapshotMs = elapsedSince(finalSnapshotStart);
    }
    const svg::RendererReadbackStats readbackStats = requestRenderer.consumeReadbackStats();
    workerTiming.readbackCount = readbackStats.count;
    workerTiming.readbackPollIterations = readbackStats.pollIterations;
    workerTiming.usedTimedWaitAny = readbackStats.usedTimedWaitAny;
    if (!compositedPreview.has_value() && (!bitmap.empty() || fullCanvasTexture != nullptr)) {
      const Entity previewEntity =
          request.dragPreview.has_value() ? request.dragPreview->entity : request.selectedEntity;
      const svg::compositor::InteractionHint interactionKind =
          request.dragPreview.has_value() ? request.dragPreview->interactionKind
                                          : svg::compositor::InteractionHint::Selection;
      compositedPreview = BuildFullCanvasCompositedPreview(
          documentViewBox, bitmap, std::move(fullCanvasTexture), request.version, previewEntity,
          interactionKind, rasterViewport, request.dragPreview);
    }

    // All document reads for this iteration are done; release write access before taking `mutex_`
    // to avoid a lock-order inversion against UI-thread DOM reads.
    releaseDocumentAccess();

    std::function<void()> wake;
    bool notifyStateChange = false;
#ifdef DONNER_WASM_WORKER_SURFACE
    bool discardBitmapBridgeFrame = false;
#endif
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Only transition to Done if we were not shut down, cancelled, or
      // superseded mid-render.
      if (auto* rendering = std::get_if<RenderingState>(&workerState_)) {
        if (rendering->pendingRequest.has_value()) {
#ifdef DONNER_WASM_WORKER_SURFACE
          discardBitmapBridgeFrame = directSurfacePresented && useBitmapWorkerSurfaceBridge_;
#endif
        } else {
          DoneState done;
          done.result.bitmap = std::move(bitmap);
          done.result.compositedPreview = std::move(compositedPreview);
          done.result.rasterViewport = rasterViewport;
          done.result.viewport = request.viewport;
          done.result.overviewInfillOnly = request.overviewInfillOnly;
          done.result.version = request.version;
          done.result.documentGeneration = request.documentGeneration;
          done.result.directSurfaceOutcome = directSurfacePresented
                                                 ? DirectSurfacePresentationOutcome::Presented
                                                 : DirectSurfacePresentationOutcome::None;
          done.result.directSurfaceFrames = directSurfaceFrameCount_;
          done.result.directSurfaceEntity = compositorEntity_;
          done.result.directSurfaceDragPreview = request.dragPreview;
          done.result.directSurfaceSlot = directSurfaceSlot;
          done.result.directSurfaceBackingSizePx = directSurfaceBackingSizePx;
#ifdef DONNER_WASM_WORKER_SURFACE
          done.result.bitmapBridgeFrameStaged =
              directSurfacePresented && useBitmapWorkerSurfaceBridge_;
          done.result.directSurfaceSelectionChromeBaked =
              directSurfacePresented && request.directSurfaceSelectionChrome.has_value();
#endif
          done.presentationHoldPollsRemaining = replayResultHoldFramesForTesting_;
#ifdef DONNER_WASM_WORKER_SURFACE
          // Direct WebGPU uses one stable DOM surface. Do not hold its matching
          // overlay metadata for an extra UI frame after the worker task
          // boundary, which would make the visible surface lead the overlay.
#endif
          lastFastPathCounters_ = compositor_->fastPathCountersForTesting();
          lastCompositorRenderFrameStats_ = compositor_->lastRenderFrameStats();
          if (compositorDiagnosticsEnabled_.load(std::memory_order_acquire)) {
            const auto diagnosticsStart = std::chrono::steady_clock::now();
            const auto thumbnailMode =
                activeDragRequest
                    ? svg::compositor::CompositorController::SnapshotThumbnails::Omit
                    : svg::compositor::CompositorController::SnapshotThumbnails::Include;
            lastLayerInspectorRows_ = compositor_->snapshotLayerInspectorRows(thumbnailMode);
            lastSegmentInspectorRows_ = compositor_->snapshotSegmentInspectorRows();
            lastCompositeTiles_ = compositor_->snapshotCompositeTiles(thumbnailMode);
            lastStateSnapshot_ = compositor_->snapshotState();
            workerTiming.diagnosticsMs = elapsedSince(diagnosticsStart);
          }
          lastWorkerCompositorEntity_ = compositorEntity_;
          lastDocumentCanvasSize_ = outputCanvasSize;
          if (directSurfaceTerminalFailure.has_value()) {
            done.result.directSurfaceOutcome =
                DirectSurfaceTerminalOutcomeFor(*directSurfaceTerminalFailure);
          }
          const auto workerEnd = std::chrono::steady_clock::now();
          const double workerMs =
              std::chrono::duration<double, std::milli>(workerEnd - workerStart).count();
          done.result.workerMs = workerMs;
          done.result.workerTiming = workerTiming;
          done.result.workerCompletedAt = workerEnd;
#ifdef DONNER_WASM_WORKER_SURFACE
          if (directSurfacePresented && !useBitmapWorkerSurfaceBridge_) {
            PendingDirectSurfaceTaskBoundaryState pendingBoundary;
            pendingBoundary.done = std::move(done);
            workerState_ = std::move(pendingBoundary);
          } else {
#endif
            workerState_ = std::move(done);
            // Snapshot the callback under the lock so a concurrent
            // `setWakeCallback` swap can't tear the invocation. Fire it
            // outside the lock to keep the hook cheap and avoid any
            // chance of deadlock if the caller re-enters AsyncRenderer.
            wake = wakeCallback_;
#ifdef DONNER_WASM_WORKER_SURFACE
          }
#endif
          notifyStateChange = true;
        }
      } else if (std::holds_alternative<CancellingState>(workerState_)) {
        // `cancelInFlight` raced with the worker's final lap -
        // renderFrame finished naturally but the user-input event
        // wants the result dropped. Drop it and transition to
        // Idle so the worker's cv_.wait at the top of the loop
        // doesn't deadlock.
        workerState_ = IdleState{};
#ifdef DONNER_WASM_WORKER_SURFACE
        discardBitmapBridgeFrame = directSurfacePresented && useBitmapWorkerSurfaceBridge_;
#endif
        wake = wakeCallback_;
        notifyStateChange = true;
      }
    }
    if (notifyStateChange) {
      cv_.notify_all();
    }
    if (wake) {
      wake();
    }
#ifdef DONNER_WASM_WORKER_SURFACE
    if (discardBitmapBridgeFrame) {
      DiscardWorkerDocumentBitmap(static_cast<double>(directSurfaceFrameCount_));
    }
    return;
#endif
  }
}

}  // namespace donner::editor
