#pragma once
/// @file

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

/// One axis-aligned document composite suitable for a single ImGui image draw.
struct DocumentCompositeTextureView {
  ImTextureID texture = 0;                 //!< ImGui handle for the composited document texture.
  Vector2i dimensions = Vector2i::Zero();  //!< Texture payload dimensions in pixels.
  Box2d screenRect;  //!< Screen-space rectangle where the document composite is presented.
};

}  // namespace donner::editor
