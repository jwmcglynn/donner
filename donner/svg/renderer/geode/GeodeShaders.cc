#include "donner/svg/renderer/geode/GeodeShaders.h"

#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "embed_resources/FilterBlendWgsl.h"
#include "embed_resources/FilterColorMatrixWgsl.h"
#include "embed_resources/FilterComponentTransferWgsl.h"
#include "embed_resources/FilterCompositeWgsl.h"
#include "embed_resources/FilterConvolveMatrixWgsl.h"
#include "embed_resources/FilterDiffuseLightingWgsl.h"
#include "embed_resources/FilterDisplacementMapWgsl.h"
#include "embed_resources/FilterDropShadowWgsl.h"
#include "embed_resources/FilterFloodWgsl.h"
#include "embed_resources/FilterImageWgsl.h"
#include "embed_resources/FilterMergeWgsl.h"
#include "embed_resources/FilterMorphologyWgsl.h"
#include "embed_resources/FilterOffsetWgsl.h"
#include "embed_resources/FilterSpecularLightingWgsl.h"
#include "embed_resources/FilterSubregionClipWgsl.h"
#include "embed_resources/FilterTileWgsl.h"
#include "embed_resources/FilterTurbulenceWgsl.h"
#include "embed_resources/GaussianBlurWgsl.h"
#include "embed_resources/ImageBlitWgsl.h"
#include "embed_resources/SlugFillAlphaCoverageWgsl.h"
#include "embed_resources/SlugFillWgsl.h"
#include "embed_resources/SlugGradientAlphaCoverageWgsl.h"
#include "embed_resources/SlugGradientWgsl.h"
#include "embed_resources/SlugMaskAlphaCoverageWgsl.h"
#include "embed_resources/SlugMaskWgsl.h"

namespace donner::geode {

namespace {

/// Build a `wgpu::ShaderModule` from a raw byte buffer of WGSL source.
/// The embedded WGSL blobs in `//donner/svg/renderer/geode:*_wgsl` land
/// here as `std::span<const unsigned char>` — wgpu-native's shader-
/// module descriptor wants a pointer + length as a `WGPUStringView`
/// (since the newer WebGPU headers use string views everywhere instead
/// of NUL-terminated C strings).
///
/// `wgpu::ShaderSourceWGSL{wgpu::Default}` runs the generated
/// `setDefault()` which fills in `chain.sType = SType::ShaderSourceWGSL`
/// so the downstream `nextInChain = &source.chain` pointer walks the
/// right struct type.
wgpu::ShaderModule createShaderFromWgsl(const wgpu::Device& device, const char* label,
                                        const unsigned char* source, size_t sourceSize) {
  wgpu::ShaderSourceWGSL wgslSource{wgpu::Default};
  wgslSource.code.data = reinterpret_cast<const char*>(source);
  wgslSource.code.length = sourceSize;

  wgpu::ShaderModuleDescriptor desc{wgpu::Default};
  desc.label = wgpuLabel(label);
  desc.nextInChain = &wgslSource.chain;

  return device.createShaderModule(desc);
}

}  // namespace

wgpu::ShaderModule createSlugFillShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugFill", donner::embedded::kSlugFillWgsl.data(),
                              donner::embedded::kSlugFillWgsl.size());
}

wgpu::ShaderModule createSlugGradientShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugGradient", donner::embedded::kSlugGradientWgsl.data(),
                              donner::embedded::kSlugGradientWgsl.size());
}

wgpu::ShaderModule createSlugMaskShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugMask", donner::embedded::kSlugMaskWgsl.data(),
                              donner::embedded::kSlugMaskWgsl.size());
}

wgpu::ShaderModule createImageBlitShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "ImageBlit", donner::embedded::kImageBlitWgsl.data(),
                              donner::embedded::kImageBlitWgsl.size());
}

wgpu::ShaderModule createSlugFillAlphaCoverageShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugFillAlphaCoverage",
                              donner::embedded::kSlugFillAlphaCoverageWgsl.data(),
                              donner::embedded::kSlugFillAlphaCoverageWgsl.size());
}

wgpu::ShaderModule createSlugGradientAlphaCoverageShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugGradientAlphaCoverage",
                              donner::embedded::kSlugGradientAlphaCoverageWgsl.data(),
                              donner::embedded::kSlugGradientAlphaCoverageWgsl.size());
}

wgpu::ShaderModule createSlugMaskAlphaCoverageShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "SlugMaskAlphaCoverage",
                              donner::embedded::kSlugMaskAlphaCoverageWgsl.data(),
                              donner::embedded::kSlugMaskAlphaCoverageWgsl.size());
}

wgpu::ShaderModule createGaussianBlurShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "GaussianBlur", donner::embedded::kGaussianBlurWgsl.data(),
                              donner::embedded::kGaussianBlurWgsl.size());
}

wgpu::ShaderModule createFilterOffsetShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterOffset", donner::embedded::kFilterOffsetWgsl.data(),
                              donner::embedded::kFilterOffsetWgsl.size());
}

wgpu::ShaderModule createFilterColorMatrixShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterColorMatrix",
                              donner::embedded::kFilterColorMatrixWgsl.data(),
                              donner::embedded::kFilterColorMatrixWgsl.size());
}

wgpu::ShaderModule createFilterFloodShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterFlood", donner::embedded::kFilterFloodWgsl.data(),
                              donner::embedded::kFilterFloodWgsl.size());
}

wgpu::ShaderModule createFilterMergeShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterMerge", donner::embedded::kFilterMergeWgsl.data(),
                              donner::embedded::kFilterMergeWgsl.size());
}

wgpu::ShaderModule createFilterCompositeShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterComposite",
                              donner::embedded::kFilterCompositeWgsl.data(),
                              donner::embedded::kFilterCompositeWgsl.size());
}

wgpu::ShaderModule createFilterBlendShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterBlend", donner::embedded::kFilterBlendWgsl.data(),
                              donner::embedded::kFilterBlendWgsl.size());
}

wgpu::ShaderModule createFilterMorphologyShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterMorphology",
                              donner::embedded::kFilterMorphologyWgsl.data(),
                              donner::embedded::kFilterMorphologyWgsl.size());
}

wgpu::ShaderModule createFilterComponentTransferShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterComponentTransfer",
                              donner::embedded::kFilterComponentTransferWgsl.data(),
                              donner::embedded::kFilterComponentTransferWgsl.size());
}

wgpu::ShaderModule createFilterConvolveMatrixShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterConvolveMatrix",
                              donner::embedded::kFilterConvolveMatrixWgsl.data(),
                              donner::embedded::kFilterConvolveMatrixWgsl.size());
}

wgpu::ShaderModule createFilterTurbulenceShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterTurbulence",
                              donner::embedded::kFilterTurbulenceWgsl.data(),
                              donner::embedded::kFilterTurbulenceWgsl.size());
}

wgpu::ShaderModule createFilterDisplacementMapShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterDisplacementMap",
                              donner::embedded::kFilterDisplacementMapWgsl.data(),
                              donner::embedded::kFilterDisplacementMapWgsl.size());
}

wgpu::ShaderModule createFilterDiffuseLightingShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterDiffuseLighting",
                              donner::embedded::kFilterDiffuseLightingWgsl.data(),
                              donner::embedded::kFilterDiffuseLightingWgsl.size());
}

wgpu::ShaderModule createFilterSpecularLightingShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterSpecularLighting",
                              donner::embedded::kFilterSpecularLightingWgsl.data(),
                              donner::embedded::kFilterSpecularLightingWgsl.size());
}

wgpu::ShaderModule createFilterDropShadowShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterDropShadow",
                              donner::embedded::kFilterDropShadowWgsl.data(),
                              donner::embedded::kFilterDropShadowWgsl.size());
}

wgpu::ShaderModule createFilterImageShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterImage", donner::embedded::kFilterImageWgsl.data(),
                              donner::embedded::kFilterImageWgsl.size());
}

wgpu::ShaderModule createFilterTileShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterTile", donner::embedded::kFilterTileWgsl.data(),
                              donner::embedded::kFilterTileWgsl.size());
}

wgpu::ShaderModule createFilterSubregionClipShader(const wgpu::Device& device) {
  return createShaderFromWgsl(device, "FilterSubregionClip",
                              donner::embedded::kFilterSubregionClipWgsl.data(),
                              donner::embedded::kFilterSubregionClipWgsl.size());
}

}  // namespace donner::geode
