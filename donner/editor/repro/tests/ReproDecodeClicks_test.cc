/// @file
///
/// Offline diagnostic: given a `.donner-repro` and the SVG it was
/// recorded against, map each recorded `mdown` / `mup` event from
/// logical window coordinates through the editor's real pane layout +
/// viewport math, then hit-test the result against the document to
/// identify which SVG element was clicked.
///
/// Doesn't play back the recording — just decodes click-landings so a
/// human (or a follow-up test) knows which elements the user actually
/// interacted with. The recorded `(mx, my)` are in logical window
/// pixels, below every layer of editor / ImGui coordinate math; this
/// test applies the same math the live editor would to translate them
/// into document space.
///
/// Usage:
///   bazel test //donner/editor/repro/tests:repro_decode_clicks_test
///
/// Prints results to stderr; always passes. Edit the
/// `kReproPath` / `kSvgPath` constants for other recordings.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "donner/base/Box.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Vector2.h"
#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor::repro {

namespace {

// Pane layout constants (mirror `EditorShell.cc:27-28`). A repro-replay
// harness ultimately needs to run the real layout code — these
// constants drift with the editor UI. For the diagnostic use case
// where we just want to know roughly what was clicked, matching the
// two constants is enough.
constexpr double kSourcePaneWidth = 560.0;
constexpr double kInspectorPaneWidth = 320.0;
// Approximate menu bar height. `ImGui::GetFrameHeight()` returns
// `FontSize + FramePadding.y * 2`. Rough value at default style.
constexpr double kMenuBarHeightApprox = 22.0;

// Compute the same pane origin / size the editor would for a window
// of `windowSize`. Matches `EditorShell::runFrame` layout math.
struct PaneLayout {
  Vector2d origin;
  Vector2d size;
};
PaneLayout computeRenderPaneLayout(Vector2d windowSize) {
  const double paneHeight = std::max(0.0, windowSize.y - kMenuBarHeightApprox);
  const double paneWidth =
      std::max(0.0, windowSize.x - kSourcePaneWidth - kInspectorPaneWidth);
  return {
      Vector2d(kSourcePaneWidth, kMenuBarHeightApprox),
      Vector2d(paneWidth, paneHeight),
  };
}

std::string loadFile(const std::filesystem::path& path) {
  std::ifstream is(path, std::ios::binary);
  if (!is) return {};
  std::ostringstream ss;
  ss << is.rdbuf();
  return ss.str();
}

}  // namespace

TEST(ReproDecodeClicks, MapClicksToDocumentElements) {
  const char* kReproPath = "/tmp/clouds_bug.donner-repro";
  const char* kSvgPath = "donner_splash.svg";

  auto file = ReadReproFile(kReproPath);
  if (!file.has_value()) {
    GTEST_SKIP() << "could not load " << kReproPath
                 << " (record one first with: donner-editor --save-repro "
                 << kReproPath << " donner_splash.svg)";
  }

  std::fprintf(stderr, "\n[decode] repro: %s (frames=%zu)\n", kReproPath,
               file->frames.size());
  std::fprintf(stderr, "[decode] svg: %s  window: %dx%d  scale: %.2f  exp: %d\n",
               file->metadata.svgPath.c_str(), file->metadata.windowWidth,
               file->metadata.windowHeight, file->metadata.displayScale,
               file->metadata.experimentalMode ? 1 : 0);

  // Compute render-pane layout from the recorded window size.
  const Vector2d windowSize(file->metadata.windowWidth, file->metadata.windowHeight);
  const auto layout = computeRenderPaneLayout(windowSize);
  std::fprintf(stderr, "[decode] render pane: origin=(%.1f, %.1f) size=(%.1f, %.1f)\n",
               layout.origin.x, layout.origin.y, layout.size.x, layout.size.y);

  // Load the SVG the user was editing. Resolve against bazel runfiles
  // (splash lives at repo root in runfiles).
  const std::string svgSource = loadFile(kSvgPath);
  if (svgSource.empty()) {
    GTEST_SKIP() << "could not load " << kSvgPath << " (expected in runfiles)";
  }
  ParseWarningSink sink;
  auto parseResult = svg::parser::SVGParser::ParseSVG(svgSource, sink);
  ASSERT_FALSE(parseResult.hasError()) << parseResult.error().reason;
  svg::SVGDocument doc = std::move(parseResult).result();
  doc.setCanvasSize(static_cast<int>(layout.size.x), static_cast<int>(layout.size.y));

  // Set up the viewport the editor would have at startup: resetTo100Percent
  // (zoom=1, document center anchored at pane center). Matches
  // `EditorShell::runFrame` at `viewportInitialized_ = true`.
  ViewportState viewport;
  viewport.paneOrigin = layout.origin;
  viewport.paneSize = layout.size;
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 892.0, 512.0);
  viewport.devicePixelRatio = file->metadata.displayScale;
  viewport.resetTo100Percent();
  std::fprintf(stderr,
               "[decode] viewport: zoom=%.3f  panDocPoint=(%.1f,%.1f)  "
               "panScreenPoint=(%.1f,%.1f)\n",
               viewport.zoom, viewport.panDocPoint.x, viewport.panDocPoint.y,
               viewport.panScreenPoint.x, viewport.panScreenPoint.y);

  // Set up EditorApp for hit-testing.
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(svgSource))
      << "EditorApp failed to load splash even though parser succeeded";

  // Walk the recording. For each mdown, map screen→doc and hit-test.
  int clickIdx = 0;
  for (const auto& frame : file->frames) {
    for (const auto& ev : frame.events) {
      if (ev.kind != ReproEvent::Kind::MouseDown) continue;
      const Vector2d screen(frame.mouseX, frame.mouseY);
      const Vector2d docPoint = viewport.screenToDocument(screen);

      // Is the click inside the pane at all?
      const bool insidePane = screen.x >= layout.origin.x &&
                              screen.x <= layout.origin.x + layout.size.x &&
                              screen.y >= layout.origin.y &&
                              screen.y <= layout.origin.y + layout.size.y;

      auto hit = app.hitTest(docPoint);
      std::string hitName = "(none)";
      std::string hitId;
      std::string hitClass;
      std::string hitAncestors;
      if (hit.has_value()) {
        std::ostringstream tagStream;
        tagStream << hit->tagName();
        hitName = tagStream.str();
        if (auto id = hit->id(); !id.empty()) {
          hitId = " id=\"" + std::string(id) + "\"";
        }
        // class attribute — helpful when elements carry only a class
        // rather than an id (splash is full of these, e.g. cls-70 /
        // cls-8 / cls-90).
        if (auto classAttr = hit->getAttribute(xml::XMLQualifiedNameRef("class"));
            classAttr.has_value() && !classAttr->empty()) {
          hitClass = " class=\"" + std::string(*classAttr) + "\"";
        }
        // Walk up the parent chain noting any id'd ancestors, so the
        // user can tell which group the element lives inside.
        auto ancestor = hit->parentElement();
        int depth = 0;
        while (ancestor.has_value() && depth < 6) {
          auto ancId = ancestor->id();
          if (!ancId.empty()) {
            hitAncestors += " ← " + std::string(ancId);
          }
          ancestor = ancestor->parentElement();
          ++depth;
        }
      }

      std::fprintf(stderr,
                   "[decode] click #%d  f=%zu  t=%.3fs  btn=%d\n"
                   "         window=(%.1f, %.1f)  insidePane=%s\n"
                   "         document=(%.2f, %.2f)\n"
                   "         hit=%s%s%s%s\n",
                   ++clickIdx, static_cast<size_t>(frame.index),
                   frame.timestampSeconds, ev.mouseButton, screen.x, screen.y,
                   insidePane ? "yes" : "NO", docPoint.x, docPoint.y, hitName.c_str(),
                   hitId.c_str(), hitClass.c_str(), hitAncestors.c_str());
    }
  }
  std::fprintf(stderr, "[decode] %d click(s) decoded\n", clickIdx);
}

}  // namespace donner::editor::repro
