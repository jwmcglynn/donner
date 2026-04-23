/// @file
///
/// Offline diagnostic: given a v2 `.donner-repro` and the SVG it was
/// recorded against, print each recorded `mdown` / `mup` event along
/// with the hit-test checkpoint baked into the recording, and
/// cross-check against a live hit-test so drift between record-time
/// and analysis-time DOMs is visible.
///
/// Doesn't play back the recording — just decodes click-landings so a
/// human (or a follow-up test) knows which elements the user actually
/// interacted with. v2 files carry the authoritative document-space
/// coordinate for every frame (via `frame.mouseDocX` / `mouseDocY`),
/// so this test no longer reconstructs pane layout from hand-tuned
/// constants — it reads the coord straight out of the recording.
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

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Vector2.h"
#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor::repro {

namespace {

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

  // Load the SVG the user was editing. Resolve against bazel runfiles
  // (splash lives at repo root in runfiles).
  const std::string svgSource = loadFile(kSvgPath);
  if (svgSource.empty()) {
    GTEST_SKIP() << "could not load " << kSvgPath << " (expected in runfiles)";
  }
  ParseWarningSink sink;
  auto parseResult = svg::parser::SVGParser::ParseSVG(svgSource, sink);
  ASSERT_FALSE(parseResult.hasError()) << parseResult.error().reason;

  // Set up EditorApp for hit-testing.
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(svgSource))
      << "EditorApp failed to load splash even though parser succeeded";

  // Walk the recording. v2 carries authoritative doc-space coords on
  // every frame — no pane-layout math required.
  int clickIdx = 0;
  for (const auto& frame : file->frames) {
    for (const auto& ev : frame.events) {
      if (ev.kind != ReproEvent::Kind::MouseDown) continue;
      const Vector2d screen(frame.mouseX, frame.mouseY);
      if (!frame.mouseDocX.has_value() || !frame.mouseDocY.has_value()) {
        std::fprintf(stderr,
                     "[decode] click #%d  f=%zu  window=(%.1f,%.1f): no recorded doc "
                     "coord — cursor was outside the render pane; skipped.\n",
                     ++clickIdx, static_cast<size_t>(frame.index), screen.x, screen.y);
        continue;
      }
      const Vector2d docPoint(*frame.mouseDocX, *frame.mouseDocY);

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

      std::string recordedSummary;
      if (ev.hit.has_value()) {
        if (ev.hit->empty) {
          recordedSummary = "<empty>";
        } else {
          recordedSummary = ev.hit->tag;
          if (!ev.hit->id.empty()) recordedSummary += " #" + ev.hit->id;
        }
      } else {
        recordedSummary = "<no checkpoint>";
      }

      std::fprintf(stderr,
                   "[decode] click #%d  f=%zu  t=%.3fs  btn=%d\n"
                   "         window=(%.1f, %.1f)  document=(%.2f, %.2f)\n"
                   "         recorded hit: %s\n"
                   "         live hit:     %s%s%s%s\n",
                   ++clickIdx, static_cast<size_t>(frame.index),
                   frame.timestampSeconds, ev.mouseButton, screen.x, screen.y,
                   docPoint.x, docPoint.y, recordedSummary.c_str(), hitName.c_str(),
                   hitId.c_str(), hitClass.c_str(), hitAncestors.c_str());
    }
  }
  std::fprintf(stderr, "[decode] %d click(s) decoded\n", clickIdx);
}

}  // namespace donner::editor::repro
