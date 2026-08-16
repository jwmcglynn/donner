#pragma once
/// @file

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

/// One axis-aligned document composite suitable for a single ImGui image draw.
struct DocumentCompositeTextureView {
  ImTextureID texture = 0;
  Vector2i dimensions = Vector2i::Zero();
  Box2d screenRect;
};

}  // namespace donner::editor
