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
  // Target dimensions in pixels. Used to map fragment positions to the
  // Phase 3b clip-mask texture's normalized UVs.
  targetSize: vec2f,
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
  // Phase 3d: SVG `mix-blend-mode` selector. `0` = plain source-over
  // (or `maskMode` when set). `1..=16` map to the enumeration in
  // `donner::svg::MixBlendMode` in the same order. When non-zero, the
  // fragment shader samples the `dstSnapshotTexture` at binding 4 and
  // composites the content through the matching W3C Compositing 1
  // formula before writing. `maskMode` and `blendMode` are mutually
  // exclusive; the host sets at most one per draw.
  blendMode: u32,
  // Nonzero when a Phase 3b path-clip mask is bound at binding 5/6 and
  // should gate the SOURCE content before mask/blend compositing.
  hasClipMask: u32,
  _blendPad0: u32,
  _blendPad1: u32,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var imageSampler: sampler;
@group(0) @binding(2) var imageTexture: texture_2d<f32>;
// Phase 3c luminance mask input — bound to a 1x1 dummy when
// `maskMode == 0`. Sampled with the same `imageSampler` so texels are
// interpolated between source pixels consistently with the content.
@group(0) @binding(3) var maskTexture: texture_2d<f32>;
// Phase 3d destination snapshot for `mix-blend-mode`. Bound to a
// 1x1 dummy when `blendMode == 0`. When non-zero, this is a copy of
// the parent render target captured before the blend blit pass, so
// the blend formula can read the backdrop without the feedback loop
// of sampling the pass's own color attachment.
@group(0) @binding(4) var dstSnapshotTexture: texture_2d<f32>;
// Phase 3b path-clip mask input. Bound to a 1x1 dummy when
// `hasClipMask == 0`. Sampled in target-pixel space rather than source
// UV space so it applies equally to whole-target blits and partial image draws.
@group(0) @binding(5) var clipMaskTexture: texture_2d<f32>;
@group(0) @binding(6) var clipMaskSampler: sampler;

fn clip_mask_coverage(pixel_center: vec2f) -> f32 {
  let dims = vec2i(textureDimensions(clipMaskTexture));
  let texel = clamp(vec2i(round(pixel_center - vec2f(0.5))), vec2i(0), dims - vec2i(1));
  let sample = textureLoad(clipMaskTexture, texel, 0);
  return clamp((sample.r + sample.g + sample.b + sample.a) * 0.25, 0.0, 1.0);
}

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

// ============================================================================
// W3C Compositing 1 — mix-blend-mode formulas
// ============================================================================
//
// All blend functions `B(Cb, Cs)` operate on STRAIGHT-alpha RGB values.
// The final W3C composite is applied by `composite_with_blend` below:
//
//   Cs' = (1 - αb) * Cs + αb * B(Cb, Cs)           // blended source
//   Co  = αs * Cs' + (1 - αs) * αb * Cb            // premultiplied output
//   αo  = αs + αb - αs * αb                        // Porter-Duff "over"
//
// which reduces to ordinary source-over when `B(Cb, Cs) == Cs` (the
// Normal case). The host demultiplies both inputs inside
// `composite_with_blend` before calling any of the helpers below so
// these functions can use the straight-alpha formulas verbatim.
//
// Non-separable modes (hue, saturation, color, luminosity) get their
// own dedicated `blend_*_non_separable` helpers because they operate
// on the full RGB triple rather than per-channel.

fn blend_multiply(cb: vec3f, cs: vec3f) -> vec3f {
  return cb * cs;
}

fn blend_screen(cb: vec3f, cs: vec3f) -> vec3f {
  return cb + cs - cb * cs;
}

fn blend_hard_light(cb: vec3f, cs: vec3f) -> vec3f {
  // Overlay(cs, cb) = HardLight(cb, cs). Per W3C §9.1.7 the spec
  // definition is: if cs <= 0.5 then 2*cb*cs else Screen(cb, 2*cs - 1).
  let lo = 2.0 * cb * cs;
  let hi = vec3f(1.0) - 2.0 * (vec3f(1.0) - cb) * (vec3f(1.0) - cs);
  return select(hi, lo, cs <= vec3f(0.5));
}

fn blend_overlay(cb: vec3f, cs: vec3f) -> vec3f {
  // Overlay(cb, cs) = HardLight(cs, cb) — roles swapped.
  return blend_hard_light(cs, cb);
}

fn blend_darken(cb: vec3f, cs: vec3f) -> vec3f {
  return min(cb, cs);
}

fn blend_lighten(cb: vec3f, cs: vec3f) -> vec3f {
  return max(cb, cs);
}

fn blend_color_dodge_channel(cb: f32, cs: f32) -> f32 {
  if (cb == 0.0) {
    return 0.0;
  }
  if (cs >= 1.0) {
    return 1.0;
  }
  return min(1.0, cb / (1.0 - cs));
}

fn blend_color_dodge(cb: vec3f, cs: vec3f) -> vec3f {
  return vec3f(
    blend_color_dodge_channel(cb.x, cs.x),
    blend_color_dodge_channel(cb.y, cs.y),
    blend_color_dodge_channel(cb.z, cs.z),
  );
}

fn blend_color_burn_channel(cb: f32, cs: f32) -> f32 {
  if (cb >= 1.0) {
    return 1.0;
  }
  if (cs <= 0.0) {
    return 0.0;
  }
  return 1.0 - min(1.0, (1.0 - cb) / cs);
}

fn blend_color_burn(cb: vec3f, cs: vec3f) -> vec3f {
  return vec3f(
    blend_color_burn_channel(cb.x, cs.x),
    blend_color_burn_channel(cb.y, cs.y),
    blend_color_burn_channel(cb.z, cs.z),
  );
}

fn blend_soft_light_channel(cb: f32, cs: f32) -> f32 {
  // W3C Compositing 1 §9.1.9 soft-light.
  //
  //   if (cs <= 0.5):
  //     B = cb - (1 - 2*cs) * cb * (1 - cb)
  //   else:
  //     if (cb <= 0.25):
  //       D = ((16*cb - 12) * cb + 4) * cb
  //     else:
  //       D = sqrt(cb)
  //     B = cb + (2*cs - 1) * (D - cb)
  if (cs <= 0.5) {
    return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
  }
  var d: f32;
  if (cb <= 0.25) {
    d = ((16.0 * cb - 12.0) * cb + 4.0) * cb;
  } else {
    d = sqrt(cb);
  }
  return cb + (2.0 * cs - 1.0) * (d - cb);
}

fn blend_soft_light(cb: vec3f, cs: vec3f) -> vec3f {
  return vec3f(
    blend_soft_light_channel(cb.x, cs.x),
    blend_soft_light_channel(cb.y, cs.y),
    blend_soft_light_channel(cb.z, cs.z),
  );
}

fn blend_difference(cb: vec3f, cs: vec3f) -> vec3f {
  return abs(cb - cs);
}

fn blend_exclusion(cb: vec3f, cs: vec3f) -> vec3f {
  return cb + cs - 2.0 * cb * cs;
}

// --- Non-separable modes (HSL) --------------------------------------------
//
// Lum, Sat, SetLum, SetSat, ClipColor follow W3C Compositing 1 §9.2.
// The coefficients are the SVG / W3C spec values — NOT BT.709 — and are
// applied to STRAIGHT RGB.

fn lum_of(c: vec3f) -> f32 {
  return 0.3 * c.x + 0.59 * c.y + 0.11 * c.z;
}

fn clip_color(c_in: vec3f) -> vec3f {
  let l = lum_of(c_in);
  let n = min(c_in.x, min(c_in.y, c_in.z));
  let x = max(c_in.x, max(c_in.y, c_in.z));
  var c = c_in;
  if (n < 0.0) {
    c = l + ((c - l) * l) / (l - n);
  }
  if (x > 1.0) {
    c = l + ((c - l) * (1.0 - l)) / (x - l);
  }
  return c;
}

fn set_lum(c_in: vec3f, l: f32) -> vec3f {
  let d = l - lum_of(c_in);
  return clip_color(c_in + vec3f(d));
}

fn sat_of(c: vec3f) -> f32 {
  return max(c.x, max(c.y, c.z)) - min(c.x, min(c.y, c.z));
}

fn set_sat(c_in: vec3f, s: f32) -> vec3f {
  // Sort channels and rewrite mid/max relative to the new saturation.
  // This is the `SetSat` algorithm from §9.2 expressed without
  // pointer-chasing: identify min/mid/max indices by successive
  // min/max operations, then build the output componentwise.
  let r = c_in.x;
  let g = c_in.y;
  let b = c_in.z;
  let cmax = max(r, max(g, b));
  let cmin = min(r, min(g, b));
  let cmid = r + g + b - cmax - cmin;

  var new_min = 0.0;
  var new_mid = 0.0;
  var new_max = 0.0;
  if (cmax > cmin) {
    new_mid = ((cmid - cmin) * s) / (cmax - cmin);
    new_max = s;
  }

  // Rebuild componentwise by comparing each input channel to the
  // identified extremes. Exact-equality checks match the W3C
  // reference — ties preserve the input ordering.
  var out: vec3f;
  out.x = select(new_min, select(new_max, new_mid, r == cmid), r == cmax);
  out.y = select(new_min, select(new_max, new_mid, g == cmid), g == cmax);
  out.z = select(new_min, select(new_max, new_mid, b == cmid), b == cmax);
  // `set_sat` is followed by `set_lum` so tiny ordering ambiguities
  // get absorbed by the luminosity restoration step.
  return out;
}

fn blend_hue(cb: vec3f, cs: vec3f) -> vec3f {
  return set_lum(set_sat(cs, sat_of(cb)), lum_of(cb));
}

fn blend_saturation(cb: vec3f, cs: vec3f) -> vec3f {
  return set_lum(set_sat(cb, sat_of(cs)), lum_of(cb));
}

fn blend_color(cb: vec3f, cs: vec3f) -> vec3f {
  return set_lum(cs, lum_of(cb));
}

fn blend_luminosity(cb: vec3f, cs: vec3f) -> vec3f {
  return set_lum(cb, lum_of(cs));
}

// Dispatch. The caller passes demultiplied `cb` / `cs` and gets back
// the blended straight-alpha RGB to plug into the Compositing-1
// composite equation.
fn apply_blend_fn(mode: u32, cb: vec3f, cs: vec3f) -> vec3f {
  switch (mode) {
    case 1u { return blend_multiply(cb, cs); }
    case 2u { return blend_screen(cb, cs); }
    case 3u { return blend_overlay(cb, cs); }
    case 4u { return blend_darken(cb, cs); }
    case 5u { return blend_lighten(cb, cs); }
    case 6u { return blend_color_dodge(cb, cs); }
    case 7u { return blend_color_burn(cb, cs); }
    case 8u { return blend_hard_light(cb, cs); }
    case 9u { return blend_soft_light(cb, cs); }
    case 10u { return blend_difference(cb, cs); }
    case 11u { return blend_exclusion(cb, cs); }
    case 12u { return blend_hue(cb, cs); }
    case 13u { return blend_saturation(cb, cs); }
    case 14u { return blend_color(cb, cs); }
    case 15u { return blend_luminosity(cb, cs); }
    default { return cs; }
  }
}

fn composite_with_blend(mode: u32, src_pm: vec4f, dst_pm: vec4f) -> vec4f {
  // Demultiply both sides so the blend formulas see straight-alpha
  // inputs. Skip the divide when alpha is zero so we don't emit NaN.
  var cs = vec3f(0.0);
  if (src_pm.a > 0.0) {
    cs = src_pm.rgb / src_pm.a;
  }
  var cb = vec3f(0.0);
  if (dst_pm.a > 0.0) {
    cb = dst_pm.rgb / dst_pm.a;
  }
  let as_ = src_pm.a;
  let ab = dst_pm.a;

  let blended = apply_blend_fn(mode, cb, cs);

  // Blended source colour per Compositing 1 §5.8.
  let cs_prime = (1.0 - ab) * cs + ab * blended;

  // Porter-Duff `over` with the blended source, emitting a
  // premultiplied result:
  //   co = αs * Cs' + (1 - αs) * αb * Cb
  //   αo = αs + αb - αs * αb
  let co = as_ * cs_prime + (1.0 - as_) * ab * cb;
  let ao = as_ + ab - as_ * ab;
  return vec4f(co, ao);
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

  if (uniforms.hasClipMask != 0u) {
    let clipCoverage = clip_mask_coverage(in.clip_pos.xy);
    color = color * clipCoverage;
  }

  if (uniforms.blendMode != 0u) {
    // Phase 3d `mix-blend-mode`. `color` is the layer being composited
    // (premultiplied), `dstSnapshotTexture` is the frozen parent
    // target captured before the blend blit pass. The fragment
    // output REPLACES the parent pixel — the pipeline is configured
    // with `srcFactor=One, dstFactor=Zero` so this shader output
    // lands verbatim in the render target.
    let dstSample = textureSample(dstSnapshotTexture, imageSampler, in.uv);
    return composite_with_blend(uniforms.blendMode, color, dstSample);
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
