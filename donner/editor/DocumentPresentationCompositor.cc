#include "donner/editor/DocumentPresentationCompositor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "donner/editor/EditorShellPresentation.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/RenderPanePresenter.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor {

#ifndef DONNER_EDITOR_WGPU
namespace {

struct TileKey {
  ImTextureID texture = 0;
  std::string id;
  Entity layerEntity = entt::null;
  std::uint64_t generation = 0;
  Vector2i bitmapDimsPx = Vector2i::Zero();
  Vector2d canvasOffsetDoc = Vector2d::Zero();
  Vector2d bitmapDimsDoc = Vector2d::Zero();
  Vector2d dragTranslationDoc = Vector2d::Zero();
  Vector2d uvBottomRight = Vector2d(1.0, 1.0);
  Transform2d documentFromCachedDocument = Transform2d();
  bool metadataOnly = false;
  bool isDragTarget = false;
};

TileKey MakeTileKey(const GlTextureCache::TileView& tile) {
  return TileKey{
      .texture = tile.texture,
      .id = tile.id,
      .layerEntity = tile.layerEntity,
      .generation = tile.generation,
      .bitmapDimsPx = tile.bitmapDimsPx,
      .canvasOffsetDoc = tile.canvasOffsetDoc,
      .bitmapDimsDoc = tile.bitmapDimsDoc,
      .dragTranslationDoc = tile.dragTranslationDoc,
      .uvBottomRight = tile.uvBottomRight,
      .documentFromCachedDocument = tile.documentFromCachedDocument,
      .metadataOnly = tile.metadataOnly,
      .isDragTarget = tile.isDragTarget,
  };
}

bool EqualTransform(const Transform2d& lhs, const Transform2d& rhs) {
  return std::equal(std::begin(lhs.data), std::end(lhs.data), std::begin(rhs.data));
}

bool EqualTileKey(const TileKey& lhs, const TileKey& rhs) {
  return lhs.texture == rhs.texture && lhs.id == rhs.id && lhs.layerEntity == rhs.layerEntity &&
         lhs.generation == rhs.generation && lhs.bitmapDimsPx == rhs.bitmapDimsPx &&
         lhs.canvasOffsetDoc == rhs.canvasOffsetDoc && lhs.bitmapDimsDoc == rhs.bitmapDimsDoc &&
         lhs.dragTranslationDoc == rhs.dragTranslationDoc &&
         lhs.uvBottomRight == rhs.uvBottomRight &&
         EqualTransform(lhs.documentFromCachedDocument, rhs.documentFromCachedDocument) &&
         lhs.metadataOnly == rhs.metadataOnly && lhs.isDragTarget == rhs.isDragTarget;
}

struct SavedSoftwareUnpack {
  GLint buffer = 0;
  GLint rowLength = 0;
  GLint alignment = 0;
  GLint skipRows = 0;
  GLint skipPixels = 0;
  SavedSoftwareUnpack() {
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  }
  ~SavedSoftwareUnpack() {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(buffer));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
  }
};

struct RequestKey {
  std::uint64_t frameId = 0;
  ViewportState viewport;
  Box2d imageClipRect;
  std::vector<TileKey> overviewTiles;
  std::vector<TileKey> tiles;
};

bool EqualViewport(const ViewportState& lhs, const ViewportState& rhs) {
  return lhs.paneOrigin == rhs.paneOrigin && lhs.paneSize == rhs.paneSize &&
         lhs.documentViewBox == rhs.documentViewBox &&
         lhs.devicePixelRatio == rhs.devicePixelRatio && lhs.zoom == rhs.zoom &&
         lhs.panDocPoint == rhs.panDocPoint && lhs.panScreenPoint == rhs.panScreenPoint;
}

bool EqualTileKeys(const std::vector<TileKey>& lhs, const std::vector<TileKey>& rhs) {
  return lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), EqualTileKey);
}

bool EqualRequest(const RequestKey& lhs, const RequestKey& rhs) {
  return lhs.frameId == rhs.frameId && EqualViewport(lhs.viewport, rhs.viewport) &&
         lhs.imageClipRect == rhs.imageClipRect &&
         EqualTileKeys(lhs.overviewTiles, rhs.overviewTiles) && EqualTileKeys(lhs.tiles, rhs.tiles);
}

RequestKey MakeRequestKey(const ViewportState& viewport, const Box2d& imageClipRect,
                          std::span<const GlTextureCache::TileView> overviewTiles,
                          std::span<const GlTextureCache::TileView> tiles) {
  RequestKey key{
      .viewport = viewport,
      .imageClipRect = imageClipRect,
  };
  key.overviewTiles.reserve(overviewTiles.size());
  for (const GlTextureCache::TileView& tile : overviewTiles) {
    key.overviewTiles.push_back(MakeTileKey(tile));
  }
  key.tiles.reserve(tiles.size());
  for (const GlTextureCache::TileView& tile : tiles) {
    key.tiles.push_back(MakeTileKey(tile));
  }
  return key;
}

GLuint CompileShader(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  if (shader == 0) {
    return 0;
  }
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) {
    return shader;
  }
  std::array<char, 1024> log{};
  GLsizei length = 0;
  glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), &length, log.data());
  std::fprintf(stderr, "DocumentPresentationCompositor shader compile failed: %.*s\n",
               static_cast<int>(length), log.data());
  glDeleteShader(shader);
  return 0;
}

GLuint LinkProgram(const char* vertexSource, const char* fragmentSource) {
  const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSource);
  const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
  if (vertex == 0 || fragment == 0) {
    if (vertex != 0) {
      glDeleteShader(vertex);
    }
    if (fragment != 0) {
      glDeleteShader(fragment);
    }
    return 0;
  }
  const GLuint program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE) {
    return program;
  }
  std::array<char, 1024> log{};
  GLsizei length = 0;
  glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), &length, log.data());
  std::fprintf(stderr, "DocumentPresentationCompositor program link failed: %.*s\n",
               static_cast<int>(length), log.data());
  glDeleteProgram(program);
  return 0;
}

struct SavedGlState {
  GLint framebuffer = 0;
  GLint program = 0;
  GLint activeTexture = 0;
  GLint texture0 = 0;
  GLint arrayBuffer = 0;
  GLint vertexArray = 0;
  std::array<GLint, 4> viewport{};
  std::array<GLint, 4> scissorBox{};
  std::array<GLfloat, 4> clearColor{};
  GLint blendSrcRgb = 0;
  GLint blendDstRgb = 0;
  GLint blendSrcAlpha = 0;
  GLint blendDstAlpha = 0;
  GLint blendEquationRgb = 0;
  GLint blendEquationAlpha = 0;
  GLboolean blend = GL_FALSE;
  GLboolean scissor = GL_FALSE;
  GLboolean depth = GL_FALSE;
  GLboolean cull = GL_FALSE;
  GLboolean stencil = GL_FALSE;

  SavedGlState() {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
    glGetIntegerv(GL_VIEWPORT, viewport.data());
    glGetIntegerv(GL_SCISSOR_BOX, scissorBox.data());
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor.data());
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
    blend = glIsEnabled(GL_BLEND);
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    depth = glIsEnabled(GL_DEPTH_TEST);
    cull = glIsEnabled(GL_CULL_FACE);
    stencil = glIsEnabled(GL_STENCIL_TEST);
  }

  ~SavedGlState() {
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
    glUseProgram(static_cast<GLuint>(program));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
    glBindVertexArray(static_cast<GLuint>(vertexArray));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
    glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
    glBlendFuncSeparate(static_cast<GLenum>(blendSrcRgb), static_cast<GLenum>(blendDstRgb),
                        static_cast<GLenum>(blendSrcAlpha), static_cast<GLenum>(blendDstAlpha));
    glBlendEquationSeparate(static_cast<GLenum>(blendEquationRgb),
                            static_cast<GLenum>(blendEquationAlpha));
    const auto restoreEnable = [](GLenum capability, GLboolean enabled) {
      if (enabled == GL_TRUE) {
        glEnable(capability);
      } else {
        glDisable(capability);
      }
    };
    restoreEnable(GL_BLEND, blend);
    restoreEnable(GL_SCISSOR_TEST, scissor);
    restoreEnable(GL_DEPTH_TEST, depth);
    restoreEnable(GL_CULL_FACE, cull);
    restoreEnable(GL_STENCIL_TEST, stencil);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture0));
    glActiveTexture(static_cast<GLenum>(activeTexture));
  }
};

struct Vertex {
  float x = 0.0f;
  float y = 0.0f;
  float u = 0.0f;
  float v = 0.0f;
};

Box2d QuadBounds(const PresentedTileQuad& quad) {
  Box2d bounds = Box2d::CreateEmpty(quad.topLeft);
  bounds.addPoint(quad.topRight);
  bounds.addPoint(quad.bottomRight);
  bounds.addPoint(quad.bottomLeft);
  return bounds;
}

bool IsValidSize(const Vector2i& size) {
  return size.x > 0 && size.y > 0 && size.x <= ViewportState::kMaxCanvasDim &&
         size.y <= ViewportState::kMaxCanvasDim;
}

std::optional<Vector2i> CompositeOutputSize(const ViewportState& viewport) {
  const Vector2d pixels = viewport.paneSize * viewport.devicePixelRatio;
  if (!std::isfinite(pixels.x) || !std::isfinite(pixels.y) || pixels.x <= 0.0 || pixels.y <= 0.0 ||
      pixels.x > ViewportState::kMaxCanvasDim || pixels.y > ViewportState::kMaxCanvasDim) {
    return std::nullopt;
  }
  return Vector2i(static_cast<int>(std::ceil(pixels.x)), static_cast<int>(std::ceil(pixels.y)));
}

}  // namespace
#endif

struct DocumentPresentationCompositor::Impl {
#ifndef DONNER_EDITOR_WGPU
  GLuint framebuffer = 0;
  GLuint premultipliedTexture = 0;
  GLuint resolvedTexture = 0;
  GLuint vertexArray = 0;
  GLuint vertexBuffer = 0;
  GLuint tileProgram = 0;
  GLuint resolveProgram = 0;
  GLuint chromeTexture = 0;
  GLuint replacementTexture = 0;
  bool failNextPaintUpload = false;
  Vector2i chromeSize = Vector2i::Zero();
  Vector2i replacementSize = Vector2i::Zero();
  Vector2i allocationSize = Vector2i::Zero();
  std::optional<RequestKey> lastRequest;
  DocumentCompositeTextureView view;
  std::uint64_t compositionCount = 0;

  ~Impl() {
    if (chromeTexture != 0) {
      glDeleteTextures(1, &chromeTexture);
    }
    if (replacementTexture != 0) {
      glDeleteTextures(1, &replacementTexture);
    }
    if (tileProgram != 0) {
      glDeleteProgram(tileProgram);
    }
    if (resolveProgram != 0) {
      glDeleteProgram(resolveProgram);
    }
    if (vertexBuffer != 0) {
      glDeleteBuffers(1, &vertexBuffer);
    }
    if (vertexArray != 0) {
      glDeleteVertexArrays(1, &vertexArray);
    }
    if (premultipliedTexture != 0) {
      glDeleteTextures(1, &premultipliedTexture);
    }
    if (resolvedTexture != 0) {
      glDeleteTextures(1, &resolvedTexture);
    }
    if (framebuffer != 0) {
      glDeleteFramebuffers(1, &framebuffer);
    }
  }

  bool ensurePrograms() {
    if (tileProgram != 0 && resolveProgram != 0 && vertexArray != 0 && vertexBuffer != 0) {
      return true;
    }
    constexpr const char* kVersion = "#version 330 core\n";
    const std::string vertexSource = std::string(kVersion) +
                                     R"glsl(layout(location = 0) in vec2 positionPx;
layout(location = 1) in vec2 textureUv;
uniform vec2 outputSizePx;
out vec2 uv;
void main() {
  vec2 ndc = positionPx / outputSizePx * 2.0 - 1.0;
  gl_Position = vec4(ndc, 0.0, 1.0);
  uv = textureUv;
}
)glsl";
    const std::string tileFragmentSource = std::string(kVersion) +
                                           R"glsl(in vec2 uv;
uniform sampler2D sourceTexture;
out vec4 outputColor;
void main() {
  vec4 color = texture(sourceTexture, uv);
  outputColor = vec4(color.rgb * color.a, color.a);
}
)glsl";
    const std::string resolveFragmentSource = std::string(kVersion) +
                                              R"glsl(in vec2 uv;
uniform sampler2D sourceTexture;
out vec4 outputColor;
void main() {
  vec4 color = texture(sourceTexture, uv);
  outputColor = color.a > 0.0 ? vec4(color.rgb / color.a, color.a) : vec4(0.0);
}
)glsl";
    tileProgram = LinkProgram(vertexSource.c_str(), tileFragmentSource.c_str());
    resolveProgram = LinkProgram(vertexSource.c_str(), resolveFragmentSource.c_str());
    if (tileProgram == 0 || resolveProgram == 0) {
      return false;
    }
    glGenVertexArrays(1, &vertexArray);
    glGenBuffers(1, &vertexBuffer);
    if (vertexArray == 0 || vertexBuffer == 0) {
      return false;
    }
    glBindVertexArray(vertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(Vertex) * 6u), nullptr,
                 GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, u)));
    return true;
  }

  bool ensureTextures(const Vector2i& size) {
    if (!IsValidSize(size)) {
      return false;
    }
    if (framebuffer == 0) {
      glGenFramebuffers(1, &framebuffer);
    }
    if (premultipliedTexture == 0) {
      glGenTextures(1, &premultipliedTexture);
    }
    if (resolvedTexture == 0) {
      glGenTextures(1, &resolvedTexture);
    }
    if (framebuffer == 0 || premultipliedTexture == 0 || resolvedTexture == 0) {
      return false;
    }
    if (allocationSize == size) {
      return true;
    }
    const auto allocate = [&](GLuint texture) {
      glBindTexture(GL_TEXTURE_2D, texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   nullptr);
      return glGetError() == GL_NO_ERROR;
    };
    if (!allocate(premultipliedTexture) || !allocate(resolvedTexture)) {
      return false;
    }
    allocationSize = size;
    lastRequest.reset();
    return true;
  }

  bool attach(GLuint texture) const {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  }

  void setClip(const Box2d& clip, const Vector2i& size) const {
    const int left = std::clamp(static_cast<int>(std::floor(clip.topLeft.x)), 0, size.x);
    const int top = std::clamp(static_cast<int>(std::floor(clip.topLeft.y)), 0, size.y);
    const int right = std::clamp(static_cast<int>(std::ceil(clip.bottomRight.x)), 0, size.x);
    const int bottom = std::clamp(static_cast<int>(std::ceil(clip.bottomRight.y)), 0, size.y);
    glScissor(left, top, std::max(0, right - left), std::max(0, bottom - top));
  }

  void drawQuad(GLuint texture, const PresentedTileQuad& quad, const Vector2d& uvBottomRight,
                GLuint program, const Vector2i& outputSize) const {
    const float u = static_cast<float>(uvBottomRight.x);
    const float v = static_cast<float>(uvBottomRight.y);
    const std::array<Vertex, 6> vertices = {
        Vertex{static_cast<float>(quad.topLeft.x), static_cast<float>(quad.topLeft.y), 0.0f, 0.0f},
        Vertex{static_cast<float>(quad.topRight.x), static_cast<float>(quad.topRight.y), u, 0.0f},
        Vertex{static_cast<float>(quad.bottomRight.x), static_cast<float>(quad.bottomRight.y), u,
               v},
        Vertex{static_cast<float>(quad.topLeft.x), static_cast<float>(quad.topLeft.y), 0.0f, 0.0f},
        Vertex{static_cast<float>(quad.bottomRight.x), static_cast<float>(quad.bottomRight.y), u,
               v},
        Vertex{static_cast<float>(quad.bottomLeft.x), static_cast<float>(quad.bottomLeft.y), 0.0f,
               v},
    };
    glUseProgram(program);
    glUniform2f(glGetUniformLocation(program, "outputSizePx"), static_cast<float>(outputSize.x),
                static_cast<float>(outputSize.y));
    glUniform1i(glGetUniformLocation(program, "sourceTexture"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(vertexArray);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(vertices)), vertices.data());
    glDrawArrays(GL_TRIANGLES, 0, 6);
  }
  bool uploadSoftwarePaint(GLuint& texture, Vector2i& size, const SelectionChromeSnapshot& snapshot,
                           const Transform2d& outputFromDocument, const Vector2i& outputSize) {
    svg::Renderer renderer;
    svg::RenderViewport viewport;
    viewport.size = Vector2d(outputSize.x, outputSize.y);
    viewport.devicePixelRatio = 1.0;
    renderer.beginFrame(viewport);
    auto placed = snapshot;
    placed.canvasFromDoc = outputFromDocument;
    OverlayRenderer::drawChromeFromSnapshot(renderer, placed);
    renderer.endFrame();
    const auto bitmap = renderer.takeSnapshot();
    if (bitmap.empty() || std::exchange(failNextPaintUpload, false)) {
      return false;
    }
    if (texture == 0) {
      glGenTextures(1, &texture);
    }
    if (texture == 0) {
      return false;
    }
    SavedSoftwareUnpack unpack;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (size != bitmap.dimensions) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bitmap.dimensions.x, bitmap.dimensions.y, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, nullptr);
      if (glGetError() != GL_NO_ERROR) {
        return false;
      }
      size = bitmap.dimensions;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(bitmap.rowBytes / 4u));
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE,
                    bitmap.pixels.data());
    return glGetError() == GL_NO_ERROR;
  }

  bool prepareSoftwarePaint(const FramePresentation& frame, const Vector2i& outputSize) {
    const double dpr = frame.viewport().devicePixelRatio;
    const Transform2d outputFromDocument =
        frame.framebufferFromDocument(Vector2d(dpr, dpr)) *
        Transform2d::Translate(-frame.viewport().paneOrigin * dpr);
    if (frame.replacementPaint().has_value() &&
        !uploadSoftwarePaint(replacementTexture, replacementSize, *frame.replacementPaint(),
                             outputFromDocument, outputSize)) {
      return false;
    }
    return !frame.chromeEnabled() || uploadSoftwarePaint(chromeTexture, chromeSize, frame.chrome(),
                                                         outputFromDocument, outputSize);
  }

  void drawSoftwarePaint(GLuint texture, const Vector2i& outputSize) const {
    const PresentedTileQuad quad{
        .topLeft = Vector2d::Zero(),
        .topRight = Vector2d(outputSize.x, 0.0),
        .bottomRight = Vector2d(outputSize.x, outputSize.y),
        .bottomLeft = Vector2d(0.0, outputSize.y),
    };
    drawQuad(texture, quad, Vector2d(1.0, 1.0), tileProgram, outputSize);
  }

  template <typename DrawTile>
  void drawActivePaint(const FramePresentation& frame, const Vector2d& paneOriginPx,
                       const Vector2i& outputSize, const DrawTile& drawTile) {
    bool replacementDrawn = false;
    for (const GlTextureCache::TileView& tile : frame.tiles()) {
      if (frame.replacementPaint().has_value() &&
          tile.layerEntity == frame.replacementPaint()->livePathPreview->entity) {
        if (!replacementDrawn) {
          drawSoftwarePaint(replacementTexture, outputSize);
          replacementDrawn = true;
        }
      } else {
        drawTile(tile);
      }
    }
    if (frame.chromeEnabled()) {
      const auto paneClip = frame.paneClipRect();
      setClip(Box2d(paneClip.topLeft * frame.viewport().devicePixelRatio - paneOriginPx,
                    paneClip.bottomRight * frame.viewport().devicePixelRatio - paneOriginPx),
              outputSize);
      drawSoftwarePaint(chromeTexture, outputSize);
    }
  }

  void drawFrameTiles(const FramePresentation& frame, const Vector2i& outputSize) {
    const auto& viewport = frame.viewport();
    const auto& imageClipRect = *frame.documentClipRect();
    const auto& tiles = frame.tiles();
    const auto& overviewTiles = frame.overviewTiles();
    const double dpr = viewport.devicePixelRatio;
    glViewport(0, 0, outputSize.x, outputSize.y);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    const Vector2d paneOriginPx = viewport.paneOrigin * dpr;
    const Transform2d outputFromDocument =
        frame.framebufferFromDocument(Vector2d(dpr, dpr)) * Transform2d::Translate(-paneOriginPx);
    const Box2d outputClipRect(imageClipRect.topLeft * dpr - paneOriginPx,
                               imageClipRect.bottomRight * dpr - paneOriginPx);

    const auto computeTileQuad = [&](const GlTextureCache::TileView& tile) {
      if (tile.texture == 0) {
        return std::optional<PresentedTileQuad>();
      }
      return ComputePresentedTileQuad(PresentedGeometryFromTileView(tile), outputFromDocument,
                                      std::nullopt);
    };
    const auto drawTile = [&](const GlTextureCache::TileView& tile) {
      const std::optional<PresentedTileQuad> quad = computeTileQuad(tile);
      if (!quad.has_value()) {
        return;
      }
      drawQuad(static_cast<GLuint>(tile.texture), *quad, tile.uvBottomRight, tileProgram,
               outputSize);
    };

    glEnable(GL_SCISSOR_TEST);
    if (!overviewTiles.empty()) {
      std::vector<Box2d> activeTileBounds;
      activeTileBounds.reserve(tiles.size() * 2u);
      for (const GlTextureCache::TileView& tile : tiles) {
        if (const std::optional<PresentedTileQuad> quad = computeTileQuad(tile)) {
          activeTileBounds.push_back(QuadBounds(*quad));
        }
      }
      for (const Box2d& overviewClipRect :
           SubtractPresentedTileBoundsFromClip(outputClipRect, activeTileBounds)) {
        setClip(overviewClipRect, outputSize);
        for (const GlTextureCache::TileView& tile : overviewTiles) {
          drawTile(tile);
        }
      }
    }
    setClip(outputClipRect, outputSize);
    drawActivePaint(frame, paneOriginPx, outputSize, drawTile);
  }

#endif
};

DocumentPresentationCompositor::DocumentPresentationCompositor()
    : impl_(std::make_unique<Impl>()) {}

DocumentPresentationCompositor::~DocumentPresentationCompositor() = default;

DocumentCompositeTextureView DocumentPresentationCompositor::compose(
    const FramePresentation& frame) {
#ifdef DONNER_EDITOR_WGPU
  (void)frame;
  return {};
#else
  if (!frame.documentClipRect().has_value()) {
    return {};
  }
  const auto& viewport = frame.viewport();
  const auto& imageClipRect = *frame.documentClipRect();
  const auto& tiles = frame.tiles();
  const auto& overviewTiles = frame.overviewTiles();
  const auto requestedSize = CompositeOutputSize(viewport);
  if (!requestedSize.has_value()) {
    return {};
  }
  const Vector2i outputSize = *requestedSize;
  RequestKey request = MakeRequestKey(viewport, imageClipRect, overviewTiles, tiles);
  request.frameId = frame.frameId();
  if (impl_->lastRequest.has_value() && EqualRequest(*impl_->lastRequest, request) &&
      impl_->view.texture != 0) {
    return impl_->view;
  }

  SavedGlState savedState;
  SavedSoftwareUnpack unpackState;
  if (!impl_->prepareSoftwarePaint(frame, outputSize)) {
    return {};
  }
  if (!impl_->ensurePrograms() || !impl_->ensureTextures(outputSize) ||
      !impl_->attach(impl_->premultipliedTexture)) {
    std::fprintf(stderr, "DocumentPresentationCompositor failed to initialize GL resources\n");
    reset();
    return {};
  }

  impl_->drawFrameTiles(frame, outputSize);

  if (!impl_->attach(impl_->resolvedTexture)) {
    std::fprintf(stderr, "DocumentPresentationCompositor resolve framebuffer is incomplete\n");
    reset();
    return {};
  }
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glClear(GL_COLOR_BUFFER_BIT);
  const PresentedTileQuad fullQuad{
      .topLeft = Vector2d::Zero(),
      .topRight = Vector2d(outputSize.x, 0.0),
      .bottomRight = Vector2d(outputSize.x, outputSize.y),
      .bottomLeft = Vector2d(0.0, outputSize.y),
  };
  impl_->drawQuad(impl_->premultipliedTexture, fullQuad, Vector2d(1.0, 1.0), impl_->resolveProgram,
                  outputSize);

  impl_->lastRequest = std::move(request);
  ++impl_->compositionCount;
  impl_->view = DocumentCompositeTextureView{
      .texture = static_cast<ImTextureID>(impl_->resolvedTexture),
      .dimensions = outputSize,
      .screenRect = Box2d(viewport.paneOrigin, viewport.paneOrigin + viewport.paneSize),
  };
  return impl_->view;
#endif
}

void DocumentPresentationCompositor::failNextSoftwarePaintUploadForTesting() {
#ifndef DONNER_EDITOR_WGPU
  impl_->failNextPaintUpload = true;
#endif
}

void DocumentPresentationCompositor::reset() {
#ifndef DONNER_EDITOR_WGPU
  impl_->lastRequest.reset();
  impl_->view = {};
#endif
}

std::uint64_t DocumentPresentationCompositor::retainedBytes() const {
#ifdef DONNER_EDITOR_WGPU
  return 0;
#else
  const auto bytes = [](const Vector2i& size) {
    return static_cast<std::uint64_t>(size.x) * static_cast<std::uint64_t>(size.y) * 4u;
  };
  return bytes(impl_->allocationSize) * 2u + bytes(impl_->chromeSize) +
         bytes(impl_->replacementSize);
#endif
}

std::uint64_t DocumentPresentationCompositor::compositionCountForTesting() const {
#ifdef DONNER_EDITOR_WGPU
  return 0;
#else
  return impl_->compositionCount;
#endif
}

}  // namespace donner::editor
