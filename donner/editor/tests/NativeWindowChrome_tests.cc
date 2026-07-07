#include "donner/editor/NativeWindowChrome.h"

#include <gtest/gtest.h>

namespace donner::editor {

namespace {
// U+25CF BLACK CIRCLE followed by a space, matching ComposeWindowTitle's dot.
constexpr const char* kDotPrefix = "\xE2\x97\x8F ";
}  // namespace

TEST(ComposeWindowTitle, UntitledNoEdits) {
  const WindowChromeState state{.filePath = std::nullopt, .edited = false};
  EXPECT_EQ(ComposeWindowTitle(state, /*showDocumentStateInText=*/true),
            "untitled - Donner SVG Editor");
}

TEST(ComposeWindowTitle, UntitledEditedShowsDotInText) {
  const WindowChromeState state{.filePath = std::nullopt, .edited = true};
  EXPECT_EQ(ComposeWindowTitle(state, /*showDocumentStateInText=*/true),
            std::string(kDotPrefix) + "untitled - Donner SVG Editor");
}

TEST(ComposeWindowTitle, UntitledEditedNativeChromeUsesProductNameOnly) {
  const WindowChromeState state{.filePath = std::nullopt, .edited = true};
  EXPECT_EQ(ComposeWindowTitle(state, /*showDocumentStateInText=*/false), "Donner SVG Editor");
}

TEST(ComposeWindowTitle, NamedFileUsesBasename) {
  const WindowChromeState state{
      .filePath = std::optional<std::string>("/home/user/art/diagram.svg"), .edited = false};
  EXPECT_EQ(ComposeWindowTitle(state, /*showDocumentStateInText=*/true),
            "diagram.svg - Donner SVG Editor");
}

TEST(ComposeWindowTitle, NamedFileEditedShowsDotInText) {
  const WindowChromeState state{
      .filePath = std::optional<std::string>("/home/user/art/diagram.svg"), .edited = true};
  EXPECT_EQ(ComposeWindowTitle(state, /*showDocumentStateInText=*/true),
            std::string(kDotPrefix) + "diagram.svg - Donner SVG Editor");
}

TEST(ComposeWindowTitle, NamedFileNativeChromeUsesExactProductName) {
  const WindowChromeState state{
      .filePath = std::optional<std::string>("/home/user/art/diagram.svg"), .edited = true};
  EXPECT_EQ(ComposeWindowTitle(state, /*showDocumentStateInText=*/false), "Donner SVG Editor");
}

TEST(ComposeWindowTitle, EmptyPathTreatedAsUntitled) {
  const WindowChromeState state{.filePath = std::optional<std::string>(""), .edited = false};
  EXPECT_EQ(ComposeWindowTitle(state, /*showDocumentStateInText=*/true),
            "untitled - Donner SVG Editor");
}

}  // namespace donner::editor
