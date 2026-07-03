#pragma once
/// @file
/// Shared argument-parsing, JSON-serialization, and bitmap helpers used by the
/// EditorControlSession tool implementations split across sibling .cc files.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/SelectTool.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "nlohmann/json.hpp"
#include "tools/mcp-servers/editor-control/EditorControlSession.h"

namespace donner::editor::mcp {

/// @cond INTERNAL

inline constexpr int kDefaultCanvasWidth = 892;
inline constexpr int kDefaultCanvasHeight = 512;
inline constexpr int kMaxDragFrames = 240;

Entity SelectedGraphicsEntity(EditorApp& app);

std::optional<PresentedDragBaseline> PresentedBaselineFromSelectPreviews(
    const std::optional<SelectTool::ActiveDragPreview>& activePreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedPreview);

PresentedFrameTileGeometry PresentedGeometryFromDisplayTile(
    const EditorControlSession::DisplayTileView& tile);

ToolCallResult MakeErrorResult(std::string message);

std::string Base64Encode(std::span<const uint8_t> bytes);

bool ReadRequiredString(const nlohmann::json& arguments, std::string_view key, std::string* out,
                        std::string* error);
bool ReadOptionalString(const nlohmann::json& arguments, std::string_view key,
                        std::string_view fallback, std::string* out, std::string* error);
bool ReadOptionalBool(const nlohmann::json& arguments, std::string_view key, bool fallback,
                      bool* out, std::string* error);
bool ReadOptionalInt(const nlohmann::json& arguments, std::string_view key, int fallback, int* out,
                     std::string* error);
bool ReadOptionalUint64(const nlohmann::json& arguments, std::string_view key,
                        std::uint64_t fallback, std::uint64_t* out, std::string* error);
bool ReadNonNegativeIntMember(const nlohmann::json& object, std::string_view key, int* out,
                              std::string* error);
bool ReadOptionalDouble(const nlohmann::json& arguments, std::string_view key, double fallback,
                        double* out, std::string* error);
bool ReadCaptureOptions(const nlohmann::json& arguments, bool includeFinalFrameDefault,
                        EditorControlSession::CaptureOptions* out, std::string* error);
bool ReadLoadOptions(const nlohmann::json& arguments, EditorControlSession::LoadOptions* out,
                     std::string* error);

Box2d DocumentViewBoxOr(const svg::SVGDocument& document, const Box2d& fallback);

uint32_t EntityToJsonValue(Entity entity);
nlohmann::json VectorToJson(const Vector2d& vector);
nlohmann::json VectorToJson(const Vector2i& vector);
nlohmann::json BoxToJson(const Box2d& box);
nlohmann::json TransformToJson(const Transform2d& destinationFromSource);

std::string ReproContentHash(std::string_view source);
std::optional<std::string> BitmapContentHash(const svg::RendererBitmap& bitmap);
std::size_t BitmapRowBytes(const svg::RendererBitmap& bitmap);
nlohmann::json BitmapSummary(const svg::RendererBitmap& bitmap);

int AttachBitmapImage(ToolCallResult* out, const std::string& label,
                      const svg::RendererBitmap& bitmap, bool embedBase64,
                      nlohmann::json* metadata);

nlohmann::json DisplayFrameJson(const EditorControlSession::DisplayFrameSnapshot& display);
nlohmann::json RenderResultsJson(
    const std::vector<EditorControlSession::CapturedRenderResult>& results, ToolCallResult* out,
    const EditorControlSession::CaptureOptions& capture, std::string_view imagePrefix);

/// @endcond

}  // namespace donner::editor::mcp
