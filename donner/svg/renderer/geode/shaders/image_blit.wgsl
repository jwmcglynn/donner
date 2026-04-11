// Geode image blit pipeline: renders a textured quad.
//
// Shared by:
//   - `drawImage`: SVG <image> elements (via GeoEncoder::drawImage).
//   - Phase 2H patterns: pattern tile sampled as repeating fill.
//
// Unlike the Slug fill pipeline, this shader does *no* coverage computation —
// it's a straightforward 2-triangle textured quad. The vertex shader maps
// unit-square corners into target-pixel space using the host-supplied
// destination rectangle and MVP, then the fragment shader samples the
// texture and multiplies by the opacity uniform.
//
// The pipeline blend state is premultiplied-source-over (same as Slug fill),
// so the fragment shader premultiplies RGB by (alpha * opacity) before
// writing. The input texture itself is uploaded as straight-alpha RGBA8
// (that's what `ImageResource` stores); the premultiply happens here.

struct Uniforms {
  // Model-view-projection matrix — maps target-pixel space to clip space.
  // Built by the host exactly like the Slug fill pipeline's MVP.
  mvp: mat4x4f,
  // Destination rectangle in target-pixel space (x0, y0, x1, y1).
  // Quad corners come from (unit.x ? x1 : x0, unit.y ? y1 : y0).
  destRect: vec4f,
  // Source UV rectangle (u0, v0, u1, v1), in normalized [0,1] texture space.
  // For a full-image blit this is (0,0,1,1). For pattern tile sampling
  // (Phase 2H) the caller may pass a sub-rect.
  srcRect: vec4f,
  // Overall multiplier applied to the sampled texel. Used for
  // `ImageParams::opacity * paint.opacity` on the draw path.
  opacity: f32,
  // 0 = texture stores STRAIGHT alpha (default; `drawImage` for SVG
  // `<image>` elements sourced from `ImageResource`). The fragment
  // shader will premultiply by `alpha * opacity` before writing.
  // 1 = texture already stores PREMULTIPLIED alpha (used by
  // `blitFullTarget` for layer/pattern compositing — offscreen render
  // targets always end up premultiplied because the Geode render
  // pipeline's blend state is premultiplied source-over). The shader
  // will multiply the entire texel by `opacity` and write the result
  // as-is.
  sourceIsPremult: u32,
  // Nonzero when this blit should apply a luminance mask from the
  // texture bound at binding 3. Used by `RendererGeode::popMask` to
  // composite mask content through a `<mask>` element's luminance.
  // When 0, the mask texture binding carries the 1x1 dummy and the
  // shader skips the sampling entirely.
  maskMode: u32,
  // When `maskMode != 0`, pixels outside `maskBounds` (x0, y0, x1, y1)
  // in target-pixel space are discarded. When zero, the field is
  // unused. Used to honour the `<mask>` element's x/y/width/height
  // attributes.
  applyMaskBounds: u32,
  // Mask bounds rectangle (x0, y0, x1, y1) in target-pixel space —
  // only read when `applyMaskBounds != 0`. Sits at offset 112 so it
  // remains 16-byte (`vec4f`) aligned without explicit padding.
  maskBounds: vec4f,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var imageSampler: sampler;
@group(0) @binding(2) var imageTexture: texture_2d<f32>;
// Phase 3c luminance mask input — bound to a 1x1 dummy when
// `maskMode == 0`. Sampled with the same `imageSampler` so texels are
// interpolated between source pixels consistently with the content.
@group(0) @binding(3) var maskTexture: texture_2d<f32>;

// The vertex shader uses `@builtin(vertex_index)` to pick one of the six
// corners of the quad — no vertex buffer is needed. Layout:
//
//   0: (0,0)   1: (1,0)   2: (0,1)
//   3: (1,0)   4: (1,1)   5: (0,1)
//
// Two triangles covering the unit square.

struct VertexOutput {
  @builtin(position) clip_pos: vec4f,
  @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VertexOutput {
  // Lookup tables for the two triangles.
  var corners = array<vec2f, 6>(
    vec2f(0.0, 0.0),
    vec2f(1.0, 0.0),
    vec2f(0.0, 1.0),
    vec2f(1.0, 0.0),
    vec2f(1.0, 1.0),
    vec2f(0.0, 1.0),
  );
  let unit = corners[vid];

  // Map unit corner into target-pixel destination rectangle.
  let dest_pos = vec2f(
    mix(uniforms.destRect.x, uniforms.destRect.z, unit.x),
    mix(uniforms.destRect.y, uniforms.destRect.w, unit.y),
  );

  // Map unit corner into source UV rectangle for sampling.
  let src_uv = vec2f(
    mix(uniforms.srcRect.x, uniforms.srcRect.z, unit.x),
    mix(uniforms.srcRect.y, uniforms.srcRect.w, unit.y),
  );

  var out: VertexOutput;
  out.clip_pos = uniforms.mvp * vec4f(dest_pos, 0.0, 1.0);
  out.uv = src_uv;
  return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
  let sampled = textureSample(imageTexture, imageSampler, in.uv);

  // Base colour — premultiplied by the pipeline's blend expectations.
  var color: vec4f;
  if (uniforms.sourceIsPremult != 0u) {
    // Source is already premultiplied. Just scale the whole texel by
    // the external opacity so the output stays premultiplied and
    // composites correctly through the pipeline's source-over blend.
    color = sampled * uniforms.opacity;
  } else {
    // The uploaded texture stores straight-alpha RGBA8. Premultiply by
    // (alpha * opacity) so the fragment matches the pipeline's
    // premultiplied-source-over blend state.
    let a = sampled.a * uniforms.opacity;
    color = vec4f(sampled.rgb * a, a);
  }

  if (uniforms.maskMode != 0u) {
    // SVG `<mask>` luminance. tiny-skia's mask.rs computes
    //   luma = 0.2126*R + 0.7152*G + 0.0722*B   (BT.709, on STRAIGHT RGB)
    //   mask_value = luma * alpha
    // Working on premultiplied input, `r_premult = r_straight * a`, so:
    //   0.2126*R_pm + 0.7152*G_pm + 0.0722*B_pm
    //     = a * (0.2126*R + 0.7152*G + 0.0722*B)
    //     = luma * a = mask_value
    // exactly the tiny-skia formula — no division, no branching.
    let maskSample = textureSample(maskTexture, imageSampler, in.uv);
    let maskValue = maskSample.r * 0.2126
                  + maskSample.g * 0.7152
                  + maskSample.b * 0.0722;

    // Honour the `<mask>` element's x/y/width/height attributes by
    // discarding anything outside the bounds rectangle in target-
    // pixel space (the pipeline's `@builtin(position)` is in
    // framebuffer coords, matching how the host computes the rect).
    if (uniforms.applyMaskBounds != 0u) {
      let px = in.clip_pos.xy;
      if (px.x < uniforms.maskBounds.x || px.x >= uniforms.maskBounds.z ||
          px.y < uniforms.maskBounds.y || px.y >= uniforms.maskBounds.w) {
        return vec4f(0.0);
      }
    }

    // `color` is already premultiplied; multiplying the whole texel
    // by a scalar mask value keeps that invariant and matches
    // tinyskia's `applyMask` (which uses premultiplied blend-in).
    return color * maskValue;
  }

  return color;
}
