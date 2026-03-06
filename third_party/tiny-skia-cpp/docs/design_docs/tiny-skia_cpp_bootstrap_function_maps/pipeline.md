# Function Mapping Tables

Legend: `☐` Not started, `🧩` Stub only, `🟡` Implemented/tested (Rust completeness not yet vetted), `🟢` Rust-completeness vetted (line-by-line against Rust), `✅` Verified parity sign-off (user-requested audit complete), `⏸` Blocked.

### `third_party/tiny-skia/src/pipeline/mod.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `Stage` variants | `pipeline::Stage` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineStageOrderingMatchesRustReference` |
| `STAGES_COUNT` | `pipeline::kStagesCount` | 🟢 | Line-by-line audited: Covered by `ColorTest.RasterPipelineCompileRecordsStageCountForExecutionPlan` |
| `Context` | `pipeline::Context` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineContextDefaultsMatchExpectedMembers` |
| `AAMaskCtx::copy_at_xy` | `AAMaskCtx::copyAtXY` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineAAMaskCtxCopyAtXYReturnsExpectedPairs` |
| `MaskCtx` | `pipeline::MaskCtx` | 🟢 | Line-by-line audited: Covered by `ColorTest.MaskCtxOffsetMatchesPackedCoordinateFormula` |
| `SamplerCtx` | `pipeline::SamplerCtx` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineContextDefaultsMatchExpectedMembers` |
| `UniformColorCtx::from (implicit)` | `pipeline::UniformColorCtx` | 🟢 | Line-by-line audited: Covered by `ColorTest.RasterPipelineBuilderCompileBuildsExpectedKindAndContext` |
| `GradientColor::new` | `GradientColor::newFromRGBA` | 🟢 | Line-by-line audited: Covered by `ColorTest.GradientColorNewFromRGBARoundTripsRGBAValues` |
| `GradientCtx::push_const_color` | `GradientCtx::pushConstColor` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineGradientCtxPushConstColorAppendsBiasAndZeroFactor` |
| `RasterPipelineBuilder::new` | `RasterPipelineBuilder` ctor | 🟢 | Line-by-line audited: Covered by `ColorTest.RasterPipelineBuilderCompileBuildsExpectedKindAndContext` |
| `RasterPipelineBuilder::set_force_hq_pipeline` | `setForceHqPipeline` | 🟢 | Line-by-line audited: Covered by `ColorTest.RasterPipelineBuilderCompileForcesHighForUnsupportedStage` |
| `RasterPipelineBuilder::push` | `push` | 🟢 | Line-by-line audited: Covered by `ColorTest.RasterPipelineBuilderPushAppendsStage` |
| `RasterPipelineBuilder::push_transform` | `pushTransform` | 🟢 | Line-by-line audited: Covered by `ColorTest.RasterPipelineBuilderPushTransformSkipsIdentity` and `RasterPipelineBuilderPushTransformStoresNonIdentityTransform` |
| `RasterPipelineBuilder::push_uniform_color` | `pushUniformColor` | 🟢 | Line-by-line audited: Covered by `ColorTest.RasterPipelineBuilderCompileBuildsExpectedKindAndContext` |
| `RasterPipelineBuilder::compile` | `compile` | 🟢 | Line-by-line audited: Covered by `ColorTest.RasterPipelineBuilderCompileUsesLowpByDefaultForSupportedStages`, `RasterPipelineBuilderCompileForcesHighForUnsupportedStage`, and `RasterPipelineBuilderCompileStageOverflowSkipsBeyondCapacitySafely` |
| `RasterPipeline::run` | `RasterPipeline::run` | 🟢 | Line-by-line audited: kind-dispatch, highp/lowp start call wiring, mutable context handoff, and lowp no-`pixmap_src` behavior match Rust |
| `RasterPipeline` | `pipeline::RasterPipeline` | 🟢 | Line-by-line audited: Covered by `ColorTest.RasterPipelineBuilderCompileBuildsExpectedKindAndContext` |

### `third_party/tiny-skia/src/pipeline/highp.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `STAGE_WIDTH` | `highp::kStageWidth` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `StageFn` | `highp::StageFn` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `fn_ptr_eq` | `highp::fnPtrEq` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `fn_ptr` | `highp::fnPtr` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `start` | `highp::start` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpStartUsesFullAndTailChunks` |
| `just_return` | `highp::justReturn` | 🟢 | Line-by-line audited: terminal no-op stage parity (`just_return` intentionally ends stage chain) |
| `destination_atop` | `highp::destination_atop` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `destination_in` | `highp::destination_in` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `destination_out` | `highp::destination_out` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `destination_over` | `highp::destination_over` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `source_atop` | `highp::source_atop` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `source_in` | `highp::source_in` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `source_out` | `highp::source_out` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `source_over` | `highp::source_over` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `clear` | `highp::clear` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `modulate` | `highp::modulate` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `multiply` | `highp::multiply` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `plus` | `highp::plus` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `screen` | `highp::screen` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `xor` | `highp::x_or` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `color_burn` | `highp::color_burn` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `color_dodge` | `highp::color_dodge` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `load_dst` | `highp::load_dst` | 🟢 | Line-by-line audited: Pixel I/O loads RGBA destination via load_8888 helper (byte→float [0,1]). Validated in audit 2026-03-02. |
| `store` | `highp::store` | 🟢 | Line-by-line audited: Pixel I/O stores RGBA via store_8888 helper (float [0,1]→byte with lround). Validated in audit 2026-03-02. |
| `mask_u8` | `highp::mask_u8` | 🟢 | Line-by-line audited: Reads external mask coverage, scales source channels, early-returns if all-zero. Validated in audit 2026-03-02. |
| `source_over_rgba` | `highp::source_over_rgba` | 🟢 | Line-by-line audited: Combined load+SourceOver+store in one stage matches Rust. Validated in audit 2026-03-02. |
| `gather` | `highp::gather` | 🟢 | Line-by-line audited: Texture sampling via SamplerCtx with tiling modes matches Rust. Validated in audit 2026-03-02. |
| `transform` | `highp::transform` | 🟢 | Line-by-line audited: Affine transform via TransformCtx matches Rust. Validated in audit 2026-03-02. |
| `reflect` | `highp::reflect` | 🟢 | Line-by-line audited: Reflect tiling for texture coordinates matches Rust. Validated in audit 2026-03-02. |
| `repeat` | `highp::repeat` | 🟢 | Line-by-line audited: Repeat tiling for texture coordinates matches Rust. Validated in audit 2026-03-02. |
| `bilinear` | `highp::bilinear` | 🟢 | Line-by-line audited: Bilinear interpolation via SamplerCtx matches Rust. Validated in audit 2026-03-02. |
| `bicubic` | `highp::bicubic` | 🟢 | Line-by-line audited: Bicubic interpolation via SamplerCtx (Mitchell-Netravali) matches Rust. Validated in audit 2026-03-02. |
| `pad_x1` | `highp::pad_x1` | 🟢 | Line-by-line audited: Clamp gradient t to [0,1] matches Rust. Validated in audit 2026-03-02. |
| `reflect_x1` | `highp::reflect_x1` | 🟢 | Line-by-line audited: Reflect gradient t in [0,1] matches Rust. Validated in audit 2026-03-02. |
| `repeat_x1` | `highp::repeat_x1` | 🟢 | Line-by-line audited: Repeat gradient t in [0,1] matches Rust. Validated in audit 2026-03-02. |
| `gradient` | `highp::gradient` | 🟢 | Line-by-line audited: Multi-stop gradient interpolation via GradientCtx matches Rust. Validated in audit 2026-03-02. |
| `evenly_spaced_2_stop_gradient` | `highp::evenly_spaced_2_stop_gradient` | 🟢 | Line-by-line audited: 2-stop gradient fast path matches Rust. Validated in audit 2026-03-02. |
| `xy_to_unit_angle` | `highp::xy_to_unit_angle` | 🟢 | Line-by-line audited: atan2 approximation for sweep gradients matches Rust. Validated in audit 2026-03-02. |
| `xy_to_radius` | `highp::xy_to_radius` | 🟢 | Line-by-line audited: Euclidean distance for radial gradients matches Rust. Validated in audit 2026-03-02. |
| `xy_to_2pt_conical_focal_on_circle` | `highp::xy_to_2pt_conical_focal_on_circle` | 🟢 | Line-by-line audited: 2pt-conical focal-on-circle mapping matches Rust. Validated in audit 2026-03-02. |
| `xy_to_2pt_conical_well_behaved` | `highp::xy_to_2pt_conical_well_behaved` | 🟢 | Line-by-line audited: 2pt-conical well-behaved mapping matches Rust. Validated in audit 2026-03-02. |
| `xy_to_2pt_conical_smaller` | `highp::xy_to_2pt_conical_smaller` | 🟢 | Line-by-line audited: 2pt-conical smaller-radius mapping matches Rust. Validated in audit 2026-03-02. |
| `xy_to_2pt_conical_greater` | `highp::xy_to_2pt_conical_greater` | 🟢 | Line-by-line audited: 2pt-conical greater-radius mapping matches Rust. Validated in audit 2026-03-02. |
| `xy_to_2pt_conical_strip` | `highp::xy_to_2pt_conical_strip` | 🟢 | Line-by-line audited: 2pt-conical strip mapping matches Rust. Validated in audit 2026-03-02. |
| `mask_2pt_conical_nan` | `highp::mask_2pt_conical_nan` | 🟢 | Line-by-line audited: NaN masking for 2pt-conical matches Rust. Validated in audit 2026-03-02. |
| `mask_2pt_conical_degenerates` | `highp::mask_2pt_conical_degenerates` | 🟢 | Line-by-line audited: Degenerate masking for 2pt-conical matches Rust. Validated in audit 2026-03-02. |
| `apply_vector_mask` | `highp::apply_vector_mask` | 🟢 | Line-by-line audited: Applies float mask to source channels matches Rust. Validated in audit 2026-03-02. |
| `alter_2pt_conical_compensate_focal` | `highp::alter_2pt_conical_compensate_focal` | 🟢 | Line-by-line audited: Focal compensation for 2pt-conical matches Rust. Validated in audit 2026-03-02. |
| `alter_2pt_conical_unswap` | `highp::alter_2pt_conical_unswap` | 🟢 | Line-by-line audited: Unswap transform for 2pt-conical matches Rust. Validated in audit 2026-03-02. |
| `negate_x` | `highp::negate_x` | 🟢 | Line-by-line audited: Negates x coordinate matches Rust. Validated in audit 2026-03-02. |
| `apply_concentric_scale_bias` | `highp::apply_concentric_scale_bias` | 🟢 | Line-by-line audited: Scale+bias for concentric gradients matches Rust. Validated in audit 2026-03-02. |
| `gamma_expand_2` | `highp::gamma_expand_2` | 🟢 | Line-by-line audited: Gamma 2.0 expand for source matches Rust. Validated in audit 2026-03-02. |
| `gamma_expand_dst_2` | `highp::gamma_expand_dst_2` | 🟢 | Line-by-line audited: Gamma 2.0 expand for destination matches Rust. Validated in audit 2026-03-02. |
| `gamma_compress_2` | `highp::gamma_compress_2` | 🟢 | Line-by-line audited: Gamma 2.0 compress matches Rust. Validated in audit 2026-03-02. |
| `gamma_expand_22` | `highp::gamma_expand_22` | 🟢 | Line-by-line audited: Gamma 2.2 expand for source matches Rust. Validated in audit 2026-03-02. |
| `gamma_expand_dst_22` | `highp::gamma_expand_dst_22` | 🟢 | Line-by-line audited: Gamma 2.2 expand for destination matches Rust. Validated in audit 2026-03-02. |
| `gamma_compress_22` | `highp::gamma_compress_22` | 🟢 | Line-by-line audited: Gamma 2.2 compress matches Rust. Validated in audit 2026-03-02. |
| `gamma_expand_srgb` | `highp::gamma_expand_srgb` | 🟢 | Line-by-line audited: sRGB gamma expand for source matches Rust. Validated in audit 2026-03-02. |
| `gamma_expand_dst_srgb` | `highp::gamma_expand_dst_srgb` | 🟢 | Line-by-line audited: sRGB gamma expand for destination matches Rust. Validated in audit 2026-03-02. |
| `gamma_compress_srgb` | `highp::gamma_compress_srgb` | 🟢 | Line-by-line audited: sRGB gamma compress matches Rust. Validated in audit 2026-03-02. |
| `darken` | `highp::darken` | 🟢 | Line-by-line audited: Darken blend mode matches Rust. Validated in audit 2026-03-02. |
| `lighten` | `highp::lighten` | 🟢 | Line-by-line audited: Lighten blend mode matches Rust. Validated in audit 2026-03-02. |
| `difference` | `highp::difference` | 🟢 | Line-by-line audited: Difference blend mode matches Rust. Validated in audit 2026-03-02. |
| `exclusion` | `highp::exclusion` | 🟢 | Line-by-line audited: Exclusion blend mode matches Rust. Validated in audit 2026-03-02. |
| `hard_light` | `highp::hard_light` | 🟢 | Line-by-line audited: Hard light blend mode matches Rust. Validated in audit 2026-03-02. |
| `overlay` | `highp::overlay` | 🟢 | Line-by-line audited: Overlay blend mode matches Rust. Validated in audit 2026-03-02. |
| `soft_light` | `highp::soft_light` | 🟢 | Line-by-line audited: Soft light blend mode matches Rust. Validated in audit 2026-03-02. |
| `hue` | `highp::hue` | 🟢 | Line-by-line audited: Hue blend mode (non-separable) matches Rust. Validated in audit 2026-03-02. |
| `saturation` | `highp::saturation` | 🟢 | Line-by-line audited: Saturation blend mode (non-separable) matches Rust. Validated in audit 2026-03-02. |
| `color` | `highp::color` | 🟢 | Line-by-line audited: Color blend mode (non-separable) matches Rust. Validated in audit 2026-03-02. |
| `luminosity` | `highp::luminosity` | 🟢 | Line-by-line audited: Luminosity blend mode (non-separable) matches Rust. Validated in audit 2026-03-02. |
| `STAGES` | `highp::STAGES` | 🟢 | Line-by-line audited: Covered by stage-table execution in `ColorTest.PipelineHighpStartUsesFullAndTailChunks` |

### `third_party/tiny-skia/src/pipeline/lowp.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `STAGE_WIDTH` | `lowp::kStageWidth` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `StageFn` | `lowp::StageFn` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `fn_ptr_eq` | `lowp::fnPtrEq` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `fn_ptr` | `lowp::fnPtr` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `start` | `lowp::start` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineLowpStartUsesFullAndTailChunks` |
| `just_return` | `lowp::justReturn` | 🟢 | Line-by-line audited: terminal no-op stage parity (`just_return` intentionally ends stage chain) |
| `destination_atop` | `lowp::destination_atop` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `destination_in` | `lowp::destination_in` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `destination_out` | `lowp::destination_out` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `destination_over` | `lowp::destination_over` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `source_atop` | `lowp::source_atop` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `source_in` | `lowp::source_in` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `source_out` | `lowp::source_out` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `source_over` | `lowp::source_over` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `clear` | `lowp::clear` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `modulate` | `lowp::modulate` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `multiply` | `lowp::multiply` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `plus` | `lowp::plus` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `screen` | `lowp::screen` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `xor` | `lowp::x_or` | 🟢 | Line-by-line audited: Covered by `ColorTest.PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults` |
| `load_dst` | `lowp::load_dst` | 🟢 | Line-by-line audited: Pixel I/O loads RGBA destination via load_8888_lowp (byte→float [0,255]) matches Rust. Validated in audit 2026-03-02. |
| `store` | `lowp::store` | 🟢 | Line-by-line audited: Pixel I/O stores RGBA via store_8888_lowp (float [0,255]→byte truncation) matches Rust. Validated in audit 2026-03-02. |
| `load_dst_u8` | `lowp::load_dst_u8` | 🟢 | Line-by-line audited: Loads single-byte mask destination into da matches Rust. Validated in audit 2026-03-02. |
| `store_u8` | `lowp::store_u8` | 🟢 | Line-by-line audited: Stores alpha channel as single byte matches Rust. Validated in audit 2026-03-02. |
| `load_mask_u8` | `lowp::load_mask_u8` | 🟢 | Line-by-line audited: Reads mask coverage into source alpha, zeros RGB matches Rust. Validated in audit 2026-03-02. |
| `mask_u8` | `lowp::mask_u8` | 🟢 | Line-by-line audited: Reads external mask coverage, scales source channels with lowp div255, early-returns if all-zero matches Rust. Validated in audit 2026-03-02. |
| `source_over_rgba` | `lowp::source_over_rgba` | 🟢 | Line-by-line audited: Combined load+SourceOver+store in lowp [0,255] range matches Rust. Validated in audit 2026-03-02. |
| `darken` | `lowp::darken` | 🟢 | Line-by-line audited: Darken blend in [0,255] range with div255 approximation matches Rust. Validated in audit 2026-03-02. |
| `lighten` | `lowp::lighten` | 🟢 | Line-by-line audited: Lighten blend in [0,255] range with div255 approximation matches Rust. Validated in audit 2026-03-02. |
| `difference` | `lowp::difference` | 🟢 | Line-by-line audited: Difference blend in [0,255] range matches Rust. Validated in audit 2026-03-02. |
| `exclusion` | `lowp::exclusion` | 🟢 | Line-by-line audited: Exclusion blend in [0,255] range matches Rust. Validated in audit 2026-03-02. |
| `hard_light` | `lowp::hard_light` | 🟢 | Line-by-line audited: Hard light blend in [0,255] range matches Rust. Validated in audit 2026-03-02. |
| `overlay` | `lowp::overlay` | 🟢 | Line-by-line audited: Overlay blend in [0,255] range matches Rust. Validated in audit 2026-03-02. |
| `transform` | `lowp::transform` | 🟢 | Line-by-line audited: Affine transform via TransformCtx matches Rust. Validated in audit 2026-03-02. |
| `pad_x1` | `lowp::pad_x1` | 🟢 | Line-by-line audited: Clamp gradient t to [0,1] matches Rust. Validated in audit 2026-03-02. |
| `reflect_x1` | `lowp::reflect_x1` | 🟢 | Line-by-line audited: Reflect gradient t in [0,1] matches Rust. Validated in audit 2026-03-02. |
| `repeat_x1` | `lowp::repeat_x1` | 🟢 | Line-by-line audited: Repeat gradient t in [0,1] matches Rust. Validated in audit 2026-03-02. |
| `gradient` | `lowp::gradient` | 🟢 | Line-by-line audited: Multi-stop gradient with [0,1]→[0,255] conversion matches Rust. Validated in audit 2026-03-02. |
| `evenly_spaced_2_stop_gradient` | `lowp::evenly_spaced_2_stop_gradient` | 🟢 | Line-by-line audited: 2-stop gradient with [0,1]→[0,255] conversion matches Rust. Validated in audit 2026-03-02. |
| `xy_to_radius` | `lowp::xy_to_radius` | 🟢 | Line-by-line audited: Euclidean distance for radial gradients matches Rust. Validated in audit 2026-03-02. |
| `STAGES` | `lowp::STAGES` | 🟢 | Line-by-line audited: Fixed missing Luminosity entry. Covered by stage-table execution in `ColorTest.PipelineLowpStartUsesFullAndTailChunks` |


### `third_party/tiny-skia/src/pipeline/blitter.rs`
| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `RasterPipelineBlitter` | `pipeline::RasterPipelineBlitter` | 🟢 | Line-by-line audited: Pipeline-driven blitter struct layout with 3 compiled RasterPipelines, optional memset_color, external mask, and pixmap_src_storage matches Rust. Validated in audit 2026-03-02. |
| `RasterPipelineBlitter::new` | `RasterPipelineBlitter::create` | 🟢 | Line-by-line audited: Builds 3 pipelines with blend mode selection (SourceOver→Source strength reduction, Clear→Source, memset optimization) matches Rust. Validated in audit 2026-03-02. |
| `RasterPipelineBlitter::new_mask` | `RasterPipelineBlitter::createMask` | 🟢 | Line-by-line audited: Builds 3 pipelines for mask mode (UniformColor(white) + Lerp1Float/LerpU8 + Store) matches Rust. Validated in audit 2026-03-02. |
| `Blitter::blit_h` | `RasterPipelineBlitter::blitH` | 🟢 | Line-by-line audited: Delegates to blitRect matches Rust. Validated in audit 2026-03-02. |
| `Blitter::blit_anti_h` | `RasterPipelineBlitter::blitAntiH` | 🟢 | Line-by-line audited: Run-walk with transparent/partial/opaque coverage branches matches Rust. Validated in audit 2026-03-02. |
| `Blitter::blit_v` | `RasterPipelineBlitter::blitV` | 🟢 | Line-by-line audited: Uses blit_mask_rp_ with AAMaskCtx (row_bytes=0 for uniform coverage) matches Rust. Validated in audit 2026-03-02. |
| `Blitter::blit_anti_h2` | `RasterPipelineBlitter::blitAntiH2` | 🟢 | Line-by-line audited: Uses blit_mask_rp_ with AAMaskCtx for 2-pixel horizontal coverage matches Rust. Validated in audit 2026-03-02. |
| `Blitter::blit_anti_v2` | `RasterPipelineBlitter::blitAntiV2` | 🟢 | Line-by-line audited: Uses blit_mask_rp_ with AAMaskCtx for 2-pixel vertical coverage matches Rust. Validated in audit 2026-03-02. |
| `Blitter::blit_rect` | `RasterPipelineBlitter::blitRect` | 🟢 | Line-by-line audited: memset fast path for Source mode + pipeline fallback matches Rust. Validated in audit 2026-03-02. |
| `Blitter::blit_mask` | `RasterPipelineBlitter::blitMask` | 🟢 | Line-by-line audited: Iterates mask data 2 pixels at a time through blit_mask_rp_ via AAMaskCtx matches Rust. Validated in audit 2026-03-02. |

