#include "donner/editor/EditorSampleCatalog.h"

#include <array>
#include <span>

#include "donner/editor/EditorSplash.h"

namespace donner::editor {
namespace {

constexpr std::string_view kBasicShapesSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="400" viewBox="0 0 640 400">
  <rect width="640" height="400" fill="#f7f8fa"/>
  <rect x="32" y="32" width="180" height="120" rx="16" fill="#2f6fed"/>
  <circle cx="320" cy="92" r="60" fill="#f0b429"/>
  <ellipse cx="500" cy="92" rx="72" ry="48" fill="#35a16b"/>
  <line x1="48" y1="220" x2="192" y2="348" stroke="#db3a34" stroke-width="16" stroke-linecap="round"/>
  <polyline points="256,330 320,220 384,330" fill="none" stroke="#8a4fff" stroke-width="14" stroke-linejoin="round"/>
  <polygon points="500,220 584,270 500,348 416,270" fill="#e76f51"/>
</svg>)svg";

constexpr std::string_view kTextStyleSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="360" viewBox="0 0 640 360">
  <rect width="640" height="360" fill="#17202a"/>
  <style>
    .heading { fill: #f8f9fa; font-family: sans-serif; font-size: 42px; font-weight: bold; }
    .body { fill: #9fe3c0; font-family: sans-serif; font-size: 22px; }
    .rule { stroke: #f0b429; stroke-width: 5; }
  </style>
  <text class="heading" x="48" y="110">Donner editor</text>
  <text class="body" x="48" y="158">Text and style inspection</text>
  <line class="rule" x1="48" y1="202" x2="592" y2="202"/>
  <text x="48" y="270" fill="#f8f9fa" font-family="sans-serif" font-size="24px">Editable SVG source</text>
  <text x="48" y="310" fill="#aeb6bf" font-family="sans-serif" font-size="18px" font-style="italic">Selectors, inheritance, and typography</text>
</svg>)svg";

constexpr std::string_view kGradientsClipSvg =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="400" viewBox="0 0 640 400">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#2f6fed"/>
      <stop offset="1" stop-color="#9b5de5"/>
    </linearGradient>
    <radialGradient id="sun" cx="35%" cy="35%" r="70%">
      <stop offset="0" stop-color="#fff3b0"/>
      <stop offset="1" stop-color="#f0b429"/>
    </radialGradient>
    <clipPath id="window-clip">
      <rect x="64" y="48" width="512" height="304" rx="28"/>
    </clipPath>
  </defs>
  <rect width="640" height="400" fill="#e9edf2"/>
  <g clip-path="url(#window-clip)">
    <rect x="64" y="48" width="512" height="304" fill="url(#sky)"/>
    <circle cx="210" cy="142" r="74" fill="url(#sun)"/>
    <path d="M0 330 C120 240 220 290 320 250 C420 210 500 260 640 190 L640 400 L0 400 Z" fill="#35a16b" opacity=".9"/>
    <path d="M0 370 C140 300 270 350 390 300 C490 260 560 320 640 280 L640 400 L0 400 Z" fill="#174f3a" opacity=".8"/>
  </g>
  <rect x="64" y="48" width="512" height="304" rx="28" fill="none" stroke="#ffffff" stroke-width="8"/>
</svg>)svg";

std::string_view SourceFromEmbedded(std::span<const unsigned char> bytes) noexcept {
  if (!bytes.empty() && bytes.back() == '\0') {
    bytes = bytes.first(bytes.size() - 1);
  }
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

const std::array<EditorSample, 4> kEditorSamples = {{
    {"donner-splash", "Donner Splash", SourceFromEmbedded(embedded::kEditorSplashSvg)},
    {"basic-shapes", "Basic Shapes", kBasicShapesSvg},
    {"text-style", "Text and Style", kTextStyleSvg},
    {"gradients-clip", "Gradients and Clip", kGradientsClipSvg},
}};

}  // namespace

std::span<const EditorSample> GetEditorSampleCatalog() noexcept {
  return kEditorSamples;
}

const EditorSample* FindEditorSample(std::string_view id) noexcept {
  for (const EditorSample& sample : kEditorSamples) {
    if (sample.id == id) {
      return &sample;
    }
  }
  return nullptr;
}

}  // namespace donner::editor
