#pragma once
/// @file

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/CompositedPresentation.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/PenTool.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/svg/renderer/Renderer.h"
#include "nlohmann/json.hpp"

namespace donner::editor::mcp {

/// PNG image content returned alongside a tool's JSON text payload.
struct EncodedImage {
  std::string label;
  std::string mimeType;
  std::string dataBase64;
};

/// Result of an editor-control tool call.
struct ToolCallResult {
  nlohmann::json body;
  std::vector<EncodedImage> images;
  bool isError = false;
};

/// Headless editor session exposed through the editor-control MCP server.
class EditorControlSession final {
public:
  EditorControlSession();
  ~EditorControlSession();

  EditorControlSession(const EditorControlSession&) = delete;
  EditorControlSession& operator=(const EditorControlSession&) = delete;
  EditorControlSession(EditorControlSession&&) = delete;
  EditorControlSession& operator=(EditorControlSession&&) = delete;

  /// Return MCP tool definitions supported by this server.
  static nlohmann::json toolList();

  /**
   * Dispatch one MCP tool call into the instrumented editor session.
   *
   * @param name Tool name from \ref toolList.
   * @param arguments JSON argument object supplied by the MCP client.
   * @return JSON payload plus any PNG images requested by the call.
   */
  ToolCallResult handleToolCall(std::string_view name, const nlohmann::json& arguments);

  /// Image capture switches shared by render-oriented MCP tools.
  struct CaptureOptions {
    /// Attach the final frame as an MCP PNG image.
    bool includeFinalFrame = false;
    /// Attach each split compositor tile as an MCP PNG image.
    bool includeTileImages = false;
    /// Attach the composed display frame as an MCP PNG image.
    bool includeDisplayFrame = false;
    /// Also embed PNG base64 in the JSON text payload.
    bool embedPngBase64 = false;
  };

  /// Document-load options shared by `load_document` and `load_svg`.
  struct LoadOptions {
    /// Requested canvas width in device pixels; 0 derives it from the SVG viewBox.
    int canvasWidth = 0;
    /// Requested canvas height in device pixels; 0 derives it from the SVG viewBox.
    int canvasHeight = 0;
    /// Device pixel ratio recorded in the session diagnostics.
    double devicePixelRatio = 1.0;
    /// Whether to render an initial frame after load.
    bool renderAfterLoad = true;
    /// Initial render capture settings.
    CaptureOptions captureOptions;
  };

  struct DisplayTileView {
    RenderResult::CompositedTile::Kind kind = RenderResult::CompositedTile::Kind::Segment;
    std::string id;
    std::uint64_t generation = 0;
    Vector2i bitmapDimsPx = Vector2i::Zero();
    Vector2d canvasOffsetDoc = Vector2d::Zero();
    Vector2d bitmapDimsDoc = Vector2d::Zero();
    Vector2d dragTranslationDoc = Vector2d::Zero();
    Transform2d documentFromCachedDocument = Transform2d();
    bool isDragTarget = false;
    bool reusedPreviousTexture = false;
    std::string contentHash;
  };

  struct DisplayFrameSnapshot {
    std::string path;
    bool hasCachedTiles = false;
    Entity cachedEntity = entt::null;
    Entity displayedEntity = entt::null;
    bool hasActiveDragPreview = false;
    std::optional<SelectTool::ActiveDragPreview> activeDragPreview;
    std::optional<SelectTool::ActiveDragPreview> displayedDragPreview;
    std::vector<DisplayTileView> tiles;
  };

  struct CapturedRenderResult {
    RenderResult renderResult;
    DisplayFrameSnapshot displayFrame;
  };

private:
  ToolCallResult loadDocument(const nlohmann::json& arguments);
  ToolCallResult loadSvg(const nlohmann::json& arguments);
  ToolCallResult getSvgSource(const nlohmann::json& arguments) const;
  ToolCallResult editSvgSource(const nlohmann::json& arguments);
  ToolCallResult selectBySelector(const nlohmann::json& arguments);
  ToolCallResult clickLayerButton(const nlohmann::json& arguments);
  ToolCallResult setActiveTool(const nlohmann::json& arguments);
  ToolCallResult setStyleProperty(const nlohmann::json& arguments);
  ToolCallResult penPath(const nlohmann::json& arguments);
  ToolCallResult dragSelector(const nlohmann::json& arguments);
  ToolCallResult transformSelector(const nlohmann::json& arguments);
  ToolCallResult renderFrameTool(const nlohmann::json& arguments);
  ToolCallResult sessionState(const nlohmann::json& arguments) const;
  ToolCallResult startRnrRecording(const nlohmann::json& arguments);
  ToolCallResult stopRnrRecording(const nlohmann::json& arguments);
  ToolCallResult replayRnr(const nlohmann::json& arguments);
  ToolCallResult rnrRecordingState(const nlohmann::json& arguments) const;

  ToolCallResult loadSvgSource(std::string_view source, const LoadOptions& options,
                               std::string_view sourcePath);
  [[nodiscard]] bool loadCurrentSourceText(const LoadOptions& options, std::string_view sourcePath,
                                           bool resetRenderVersion, nlohmann::json* loadInfo,
                                           std::string* error);
  [[nodiscard]] nlohmann::json sourceStateJson() const;

  class HeadlessTextureCache {
  public:
    void reset();
    void uploadComposited(const RenderResult::CompositedPreview& preview);

    [[nodiscard]] const std::vector<DisplayTileView>& tiles() const { return tiles_; }
    [[nodiscard]] std::optional<svg::RendererBitmap> composeDisplayFrame(
        const DisplayFrameSnapshot& display, const Box2d& viewBox,
        const Vector2i& canvasSize) const;

  private:
    struct CachedTextureEntry {
      RenderResult::CompositedTile::Kind kind = RenderResult::CompositedTile::Kind::Segment;
      std::uint64_t generation = 0;
      Vector2i bitmapDimsPx = Vector2i::Zero();
      std::string contentHash;
      svg::RendererBitmap bitmap;
    };

    std::unordered_map<std::string, CachedTextureEntry> tileTextures_;
    std::vector<DisplayTileView> tiles_;
  };

  [[nodiscard]] bool renderCurrentFrame(std::vector<CapturedRenderResult>* results,
                                        std::string* error);
  [[nodiscard]] std::optional<svg::RendererBitmap> composeDisplayFrameBitmap(
      const DisplayFrameSnapshot& display) const;
  DisplayFrameSnapshot recordDisplayFrame(const RenderResult& result);
  [[nodiscard]] DisplayFrameSnapshot currentDisplayFrame() const;
  void appendRnrFrame(const Vector2d& documentPoint, int mouseButtonMask, int modifierMask,
                      std::vector<repro::ReproEvent> events);
  void appendRnrActionFrame(repro::ReproAction action);
  [[nodiscard]] repro::ReproViewport currentReproViewport() const;
  [[nodiscard]] Vector2d currentRecordingScreenPoint(const Vector2d& documentPoint) const;
  [[nodiscard]] std::optional<std::filesystem::path> resolveRnrSvgPath(
      const std::filesystem::path& rnrPath, std::string_view recordingSvgPath) const;
  [[nodiscard]] bool syncCanvasSize(const ViewportState& viewport);
  [[nodiscard]] bool drainPendingWritebacks();
  void syncSourceTextFromDocumentIfChanged();
  [[nodiscard]] bool setActiveToolForReplay(std::string_view tool, std::string* error);
  [[nodiscard]] bool applyStylePropertyForReplay(std::string_view propertyName,
                                                 std::string_view propertyValue);
  [[nodiscard]] bool applyReproAction(const repro::ReproAction& action, std::string* error);
  void replayMouseDown(const Vector2d& documentPoint, MouseModifiers modifiers);
  void replayMouseMove(const Vector2d& documentPoint, bool buttonHeld, MouseModifiers modifiers);
  void replayMouseUp(const Vector2d& documentPoint);
  [[nodiscard]] std::optional<svg::SVGElement> querySelector(std::string_view selector);
  [[nodiscard]] std::optional<Box2d> elementWorldBounds(const svg::SVGElement& element) const;
  [[nodiscard]] nlohmann::json selectedElementJson() const;

  [[nodiscard]] bool ensureDocumentLoaded(std::string* error) const;
  [[nodiscard]] bool waitUntilIdle(std::string* error);

  EditorApp app_;
  std::unique_ptr<SelectTool> selectTool_;
  PenTool penTool_;
  enum class ActiveReplayTool : std::uint8_t {
    Select,
    Pen,
  };
  ActiveReplayTool activeReplayTool_ = ActiveReplayTool::Select;
  svg::Renderer renderer_;
  AsyncRenderer asyncRenderer_;
  CompositedPresentation displayPresentation_;
  HeadlessTextureCache displayTextures_;
  struct RnrRecordingState {
    bool active = false;
    std::optional<std::filesystem::path> outputPath;
    repro::ReproFile file;
    std::uint64_t nextFrameIndex = 0;
    double timestampSeconds = 0.0;
    double frameDeltaMs = 1000.0 / 60.0;
  };

  RnrRecordingState rnrRecording_;
  std::string currentSourcePath_;
  std::string currentSourceText_;
  std::uint64_t sourceRevision_ = 0;
  std::uint64_t loadedSourceRevision_ = 0;
  std::optional<std::string> lastParseError_;
  std::uint64_t nextRenderVersion_ = 1;
  int canvasWidth_ = 0;
  int canvasHeight_ = 0;
  double devicePixelRatio_ = 1.0;
};

}  // namespace donner::editor::mcp
