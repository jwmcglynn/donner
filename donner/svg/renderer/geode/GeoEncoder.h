#pragma once
/// @file
/// Drawing API for the Geode GPU renderer.

#include <webgpu/webgpu_cpp.h>

#include <memory>

#include "donner/base/FillRule.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/css/Color.h"

namespace donner {
class Path;
}  // namespace donner

namespace donner::geode {

class GeodeDevice;
class GeodePipeline;

/**
 * Drawing API for the Geode GPU renderer.
 *
 * `GeoEncoder` is a per-frame command builder. Construct one against a target
 * texture, issue draw calls (`fillPath`, `clear`, etc.), then call `finish()`
 * to submit the command buffer to the GPU.
 *
 * The encoder owns no GPU buffers itself — each draw call allocates fresh
 * vertex / band / curve / uniform buffers. This is the simplest possible
 * implementation; later phases will add buffer pooling and the ECS-backed
 * `GeodePathCacheComponent` for paths whose geometry hasn't changed.
 *
 * Typical usage:
 *
 *     GeoEncoder encoder(device, pipeline, targetTexture);
 *     encoder.clear(css::RGBA::White);
 *     encoder.setTransform(Transform2d::Scale(2.0));
 *     encoder.fillPath(myPath, css::RGBA::Red, FillRule::NonZero);
 *     encoder.finish();
 */
class GeoEncoder {
public:
  /**
   * Create an encoder targeting the given texture.
   *
   * @param device The Geode device (owns the wgpu::Device + queue).
   * @param pipeline The Slug fill pipeline (must match the target's format).
   * @param target Texture to render into. Must be created with
   *   `RenderAttachment` usage. The encoder takes a reference; the caller
   *   must keep the texture alive until `finish()` returns.
   */
  GeoEncoder(GeodeDevice& device, const GeodePipeline& pipeline, const wgpu::Texture& target);

  ~GeoEncoder();

  GeoEncoder(const GeoEncoder&) = delete;
  GeoEncoder& operator=(const GeoEncoder&) = delete;
  GeoEncoder(GeoEncoder&&) noexcept;
  GeoEncoder& operator=(GeoEncoder&&) noexcept;

  /**
   * Clear the target texture to the given color.
   *
   * Must be called before any draw calls — clear is implemented as the load
   * op of the first render pass, so calling it after a draw is a no-op.
   * Subsequent calls override the previous clear color.
   */
  void clear(const css::RGBA& color);

  /// Set the model-view transform for subsequent draw calls.
  void setTransform(const Transform2d& transform);

  /**
   * Fill a path with a solid color.
   *
   * The path is encoded into Slug band data on the CPU, uploaded to GPU
   * buffers, and a draw call is recorded. The fill is applied with the
   * current transform.
   *
   * @param path The path to fill.
   * @param color Solid fill color (NOT premultiplied — the encoder handles
   *   premultiplication for the blend pipeline).
   * @param rule Fill rule (NonZero or EvenOdd).
   */
  void fillPath(const Path& path, const css::RGBA& color, FillRule rule);

  /**
   * Submit all encoded commands to the GPU queue.
   *
   * After this call, the encoder is in a "finished" state and no further
   * draws can be issued. The caller is responsible for any synchronization
   * (e.g., MapAsync + Tick loop) needed to actually use the rendered output.
   */
  void finish();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::geode
