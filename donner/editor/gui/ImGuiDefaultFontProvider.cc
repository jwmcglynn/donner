/// @file

#include <cstdio>
#include <cstring>
#include <limits>

#include "donner/base/PackagedResource.h"
#include "embed_resources/ImGuiDefaultFont.h"
#include "imgui.h"

// Strong link contract for the external-resource ImGui implementation. The returned allocation
// belongs to ImGui, so clearing/replacing a resource session cannot invalidate the atlas's font.
extern "C" void* DonnerImGuiLoadDefaultFont(int* fontDataSize) {
  *fontDataSize = 0;
  const donner::ResourceLease font(donner::embedded::kProggyCleanTtf);
  if (font.empty() || font.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    std::fprintf(stderr, "ImGui default font resource unavailable: %s\n",
                 donner::GetPackagedResourceSession()->error().c_str());
    return nullptr;
  }
  void* copy = ImGui::MemAlloc(font.size());
  if (copy == nullptr) {
    std::fprintf(stderr, "ImGui default font allocation failed\n");
    return nullptr;
  }
  std::memcpy(copy, font.data(), font.size());
  *fontDataSize = static_cast<int>(font.size());
  return copy;
}
