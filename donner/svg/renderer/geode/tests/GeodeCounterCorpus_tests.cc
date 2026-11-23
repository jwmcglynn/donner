/// @file
/// Corpus-wide steady-state counter gate for the Geode backend.
///
/// `GeodeCounters` documents what an unchanged second frame must cost: the
/// path-encode cache serves every path (`pathEncodes == 0`), render targets and
/// scratch textures come back from the pool (`textureCreates == 0`), resident
/// geometry is already on the GPU so nothing is re-uploaded (`bufferWrites`
/// drives to 0), and bind groups are reused rather than rebuilt per draw
/// (`bindgroupCreates` bounded).
///
/// Those invariants used to be asserted on a handful of hand-picked fixtures,
/// so a feature that quietly fell off residency (styled `<use>` strokes,
/// markers, text, patterns, filter uniforms, gradient strokes) cost nothing in
/// CI. This suite renders EVERY SVG in the repository's render corpus twice
/// through one shared headless device and asserts the invariants on frame two.
///
/// Scenes that do not hold the invariants today are listed in
/// `kKnownViolations` with the counters they break, the measured values as a
/// ceiling, and a one-line reason. A listed scene asserts the INVERSE: it must
/// STILL violate the counters its entry claims. That makes the list a ratchet -
/// fixing a scene fails this suite until its entry is deleted, so nothing can
/// silently fall back off residency once it is fixed.
///
/// The list gates the built configuration only: masks and ceilings are the
/// measurements of the shipped build, which always compiles in cross-entity
/// ordered batching, and a build without it is not expected to pass.
///
/// Reading the numbers here, three caveats matter:
///
///   - A ceiling only ratchets against the measurement it was taken from. An
///     improvement that halves a counter without reaching the target keeps
///     passing against the old, looser ceiling, so the suite prints a NOTICE
///     whenever an observed value sits far below its recorded ceiling. Treat
///     that notice as "re-measure and tighten this entry".
///   - Counters are sampled BEFORE the frame-two snapshot, unlike
///     `GeodePerf_tests.cc`, which samples after `takeSnapshot()`. Snapshot
///     readback allocates a buffer and a bind group, so the bind-group numbers
///     in the two suites are NOT comparable, and readback cost is deliberately
///     ungated here: this gate is about per-frame render cost.
///   - Every scene prints its observed counters, but bazel hides passing test
///     output. Use `--test_output=all` to see the `[counter-corpus]` lines.
///
/// Upper bounds alone cannot tell a fixed scene from a scene that renders
/// nothing, so each scene also asserts that its snapshot contains visible
/// pixels. Scenes that legitimately render nothing are listed, with a reason,
/// in `kDeliberatelyInert`.
///
/// The corpus pass clamps every scene to 512 px per edge, so it cannot see a
/// cost that appears only on a larger canvas: the texture pool's retention
/// limits depend on texture size. A second pass renders every scene again at
/// twice its natural size, up to a 2048 px edge (`aboveNaturalCanvasSize`),
/// starting from an empty texture pool, against its own ledger,
/// `kKnownViolationsAboveNaturalSize`. Twice a small scene's natural size is still a small canvas,
/// so a third pass renders one frame of every scene at a 2048 px long edge and requires visible
/// pixels there too: a budget that drops content only on a large canvas fails it. Neither the
/// corpus pass nor the above-natural-size pass may run a filter on a reduced-resolution raster.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Vector2.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/resources/SandboxedFileResourceLoader.h"

namespace donner::svg {
namespace {

// ---------------------------------------------------------------------------
// Gated counters.
// ---------------------------------------------------------------------------

/// The subset of `GeodeCounters` this gate asserts, sampled on the second
/// render of an unchanged document.
struct Observed {
  uint64_t pathEncodes = 0;
  uint64_t textureCreates = 0;
  uint64_t bufferWrites = 0;
  uint64_t bindgroupCreates = 0;
};

/// One measured frame: the gated counters plus the liveness signal.
struct FrameSample {
  Observed gated;
  /// Draw calls issued by the measured frame. Not gated; reported because it
  /// is the cheapest way to see how much of a scene reaches the fill path.
  uint64_t drawCalls = 0;

  bool operator==(const FrameSample& other) const {
    return gated.pathEncodes == other.gated.pathEncodes &&
           gated.textureCreates == other.gated.textureCreates &&
           gated.bufferWrites == other.gated.bufferWrites &&
           gated.bindgroupCreates == other.gated.bindgroupCreates && drawCalls == other.drawCalls;
  }
};

/// Bit per gated counter, used by `KnownViolation::violated` to say which
/// invariants a scene is currently allowed (and required) to break.
enum CounterBit : uint32_t {
  kPathEncodes = 1u << 0,
  kTextureCreates = 1u << 1,
  kBufferWrites = 1u << 2,
  kBindgroupCreates = 1u << 3,
};

/// Steady-state ceiling for each gated counter on an unchanged second frame.
///
/// `bindgroupCreates` is the one counter with a legitimately non-zero target.
/// Its documented invariant is "at most one bind group per pipeline layout",
/// and the concrete number 6 is carried over from the bound `GeodePerf_tests`
/// already asserts on its own fixtures rather than derived from first
/// principles. For scale: a corpus frame can reach roughly four distinct
/// render pipelines (solid fill, gradient fill, image blit, clip mask), so 6
/// leaves slack, and the scenes far above it are building a bind group per
/// DRAW, which is the regression the bound exists to catch.
///
/// Everything else is a hard zero. Re-encoding a path, allocating a texture,
/// or re-uploading geometry on a frame whose document did not change is
/// exactly the work the path cache, the texture pool, and GPU residence exist
/// to remove.
constexpr Observed kSteadyState = {
    /*pathEncodes=*/0,
    /*textureCreates=*/0,
    /*bufferWrites=*/0,
    /*bindgroupCreates=*/6,
};

/// Table-driven description of one gated counter so the assertion, the
/// diagnostic text, and the per-scene dump all read from one place.
struct CounterField {
  uint32_t bit;
  const char* name;
  uint64_t Observed::* member;
  /// What the counter proves when it holds, used in failure messages.
  const char* rationale;
};

constexpr CounterField kCounterFields[] = {
    {kPathEncodes, "pathEncodes", &Observed::pathEncodes,
     "the path-encode cache should serve every path on an unchanged frame"},
    {kTextureCreates, "textureCreates", &Observed::textureCreates,
     "render targets and scratch textures should come back from the pool"},
    {kBufferWrites, "bufferWrites", &Observed::bufferWrites,
     "GPU-resident geometry should not be re-uploaded on an unchanged frame"},
    {kBindgroupCreates, "bindgroupCreates", &Observed::bindgroupCreates,
     "bind groups should be cached per pipeline, not rebuilt per draw"},
};

// ---------------------------------------------------------------------------
// Known violations.
// ---------------------------------------------------------------------------

/// One corpus scene that does not hold the steady-state invariants yet.
struct KnownViolation {
  /// Scene name, i.e. the SVG filename without its extension.
  std::string_view scene;
  /// Bitmask of `CounterBit`: the counters this scene must STILL break. The
  /// still-violating assertion fails as soon as any listed counter holds, so a
  /// fixed counter must leave the mask, and an entry with an empty mask is
  /// rejected by `KnownViolationsAreWellFormed`.
  uint32_t violated;
  /// The full measured frame-two counter set. The counters named in
  /// `violated` are enforced as ceilings, so a listed scene cannot get worse
  /// while it waits for a fix; the rest stay held to the shared target.
  Observed ceiling;
  /// One line on why the scene falls off the steady state.
  std::string_view reason;
  /// What has to change for this entry to be deleted.
  std::string_view tracking;
};

// Every corpus scene meets the steady state, so the list is empty. A scene that falls off it is
// listed as `std::to_array<KnownViolation>({...})`, whose size is deduced from its entries.
constexpr std::array<KnownViolation, 0> kKnownViolations = {};

/// The ledger of the above-natural-size pass, measured at `aboveNaturalCanvasSize` from an empty
/// texture pool.
constexpr std::array<KnownViolation, 0> kKnownViolationsAboveNaturalSize = {};

const KnownViolation* findKnownViolation(std::span<const KnownViolation> ledger,
                                         std::string_view scene) {
  for (const KnownViolation& entry : ledger) {
    if (entry.scene == scene) {
      return &entry;
    }
  }
  return nullptr;
}

/// One way of sizing every corpus scene, with the ledger of the scenes it lets miss the steady
/// state.
struct CorpusPass {
  std::string_view name;
  std::span<const KnownViolation> ledger;
};

constexpr CorpusPass kCorpusSizePass = {"corpus size", kKnownViolations};
constexpr CorpusPass kAboveNaturalSizePass = {"above natural size",
                                              kKnownViolationsAboveNaturalSize};
/// Gates visible pixels only, so it has no counter ledger.
constexpr CorpusPass kLargeCanvasPass = {"large canvas", {}};
constexpr std::array<const CorpusPass*, 3> kCorpusPasses = {
    &kCorpusSizePass, &kAboveNaturalSizePass, &kLargeCanvasPass};

// ---------------------------------------------------------------------------
// Deliberately inert scenes.
// ---------------------------------------------------------------------------

/// A corpus scene that renders no visible pixels, with the reason it is
/// expected to stay that way, or the issue that tracks it. Without this list a
/// scene that stopped rendering entirely would sail through every upper bound
/// in this file.
struct InertScene {
  std::string_view scene;
  /// The pass that renders it blank, or null when every pass does.
  const CorpusPass* onlyIn;
  std::string_view reason;
};

constexpr std::array<InertScene, 3> kDeliberatelyInert = {{
    {
        /*scene=*/"external_context_paint",
        /*onlyIn=*/nullptr,
        /*reason=*/
        "helper document for use-external-context-paint.svg: rendered standalone there is no "
        "use context, so context-fill and context-stroke resolve to no paint and nothing is "
        "painted",
    },
    {
        /*scene=*/"size_too_large",
        /*onlyIn=*/&kCorpusSizePass,
        /*reason=*/
        "declares a 100000 px width, which the shared canvas clamp scales down until every "
        "stroke is sub-pixel, so this scene is measured blank by construction",
    },
    {
        /*scene=*/"feimage_fragment_idempotency",
        /*onlyIn=*/&kLargeCanvasPass,
        /*reason=*/
        "its feImage fragment raster exceeds the renderer driver's per-frame filter source-image "
        "budget on a canvas this large, so the filter is not prepared and the element is hidden, "
        "on every backend (#2662)",
    },
}};

bool isDeliberatelyInert(std::string_view scene, const CorpusPass& pass) {
  for (const InertScene& entry : kDeliberatelyInert) {
    if (entry.scene == scene && (entry.onlyIn == nullptr || entry.onlyIn == &pass)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Corpus enumeration.
// ---------------------------------------------------------------------------

/// A directory holding part of the render corpus, plus the smallest number of
/// scenes it must contribute. The per-root minimum is what turns a dropped
/// `data` dependency into a loud failure: a single corpus-wide total would
/// still look healthy if one root vanished.
struct CorpusRoot {
  /// Path relative to the runfiles root.
  std::string_view path;
  size_t minimumScenes;
};

/// The repository root itself carries the checked-in logo SVGs, which ship in
/// the same `testdata` filegroup as the renderer corpus.
constexpr CorpusRoot kCorpusRoots[] = {
    {".", 3},
    {"donner/svg/renderer/testdata", 70},
    {"donner/svg/renderer/benchmarks/testdata", 1},
};

/// Directory of the hermetic test fonts, relative to the runfiles root. Text
/// scenes must resolve glyphs from checked-in font data: host font resolution
/// would make every glyph-derived counter in this file a property of the
/// machine that ran it.
constexpr std::string_view kFontsRunfilesPath = "third_party/resvg-test-suite/fonts";

struct CorpusScene {
  std::string name;
  std::string path;
};

/// Maximum canvas edge, in device pixels, applied to every corpus scene.
///
/// Some corpus documents declare enormous intrinsic sizes (one is 100000 px
/// wide), past the WebGPU guaranteed maximum 2D texture dimension, which would
/// make the gate depend on the adapter's limits. Clamping bounds the suite's
/// memory and wall clock and keeps it adapter-independent, but it is not free:
/// a document with a viewBox is scaled down, which changes viewport culling and
/// the device-derived curve-flatten tolerance, and a document sized in absolute
/// units without a viewBox is cropped instead (`size_too_large` is measured
/// cropped). The counters recorded here therefore describe each scene AT THIS
/// CLAMP, not at its natural size.
constexpr int kMaxCanvasEdge = 512;

/// Longest canvas edge, in device pixels, of the above-natural-size pass: the canvas of a 1024
/// CSS px view on a high-density display. It is a quarter of the WebGPU guaranteed texture
/// dimension, so the pass stays adapter-independent, and it bounds a frame target to 16 MiB. A
/// scene whose natural long edge is above half of it is measured at this edge, which for an
/// oversized scene such as `size_too_large` is above its corpus size but below its natural size.
constexpr int kMaxAboveNaturalCanvasEdge = 2048;

/// The corpus pass's canvas: each edge of the natural size, clamped to `kMaxCanvasEdge`.
Vector2i corpusCanvasSize(const Vector2i& natural) {
  return {std::clamp(natural.x, 1, kMaxCanvasEdge), std::clamp(natural.y, 1, kMaxCanvasEdge)};
}

/// The above-natural-size pass's canvas: the natural size scaled to twice its long edge, at most
/// `kMaxAboveNaturalCanvasEdge`, keeping its aspect ratio.
Vector2i aboveNaturalCanvasSize(const Vector2i& natural) {
  const int naturalLongEdge = std::max({natural.x, natural.y, 1});
  const int longEdge = std::min(naturalLongEdge * 2, kMaxAboveNaturalCanvasEdge);
  const auto scaled = [&](int edge) {
    return std::max(
        1, static_cast<int>(std::lround(static_cast<double>(edge) * longEdge / naturalLongEdge)));
  };
  return {scaled(natural.x), scaled(natural.y)};
}

/// The large-canvas pass's canvas: the natural size scaled to a `kMaxAboveNaturalCanvasEdge` long
/// edge, keeping its aspect ratio.
Vector2i largeCanvasSize(const Vector2i& natural) {
  const int naturalLongEdge = std::max({natural.x, natural.y, 1});
  const auto scaled = [&](int edge) {
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(edge) *
                                                    kMaxAboveNaturalCanvasEdge / naturalLongEdge)));
  };
  return {scaled(natural.x), scaled(natural.y)};
}

/// gtest test-case names must be alphanumeric, so fold everything else to '_'.
std::string sanitizeName(std::string_view stem) {
  std::string out(stem);
  for (char& ch : out) {
    const bool ok =
        (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    if (!ok) {
      ch = '_';
    }
  }
  return out;
}

std::vector<CorpusScene> scenesUnderRoot(std::string_view root) {
  std::vector<CorpusScene> scenes;
  std::error_code ec;
  std::filesystem::directory_iterator it(std::filesystem::path(root), ec);
  if (ec) {
    return scenes;
  }
  for (const std::filesystem::directory_entry& entry : it) {
    if (!entry.is_regular_file() || entry.path().extension() != ".svg") {
      continue;
    }
    scenes.push_back({sanitizeName(entry.path().stem().string()), entry.path().string()});
  }

  // `directory_iterator` order is unspecified; sort by path so the enumeration,
  // the per-scene dump, and the test registration order are reproducible.
  std::sort(scenes.begin(), scenes.end(),
            [](const CorpusScene& a, const CorpusScene& b) { return a.path < b.path; });
  return scenes;
}

std::vector<CorpusScene> enumerateCorpus() {
  std::vector<CorpusScene> scenes;
  for (const CorpusRoot& root : kCorpusRoots) {
    std::vector<CorpusScene> found = scenesUnderRoot(root.path);
    scenes.insert(scenes.end(), std::make_move_iterator(found.begin()),
                  std::make_move_iterator(found.end()));
  }
  return scenes;
}

const std::vector<CorpusScene>& corpus() {
  static const std::vector<CorpusScene> scenes = enumerateCorpus();
  return scenes;
}

/// Scene names in corpus order. Used as the test parameter, because gtest can
/// print a string and would otherwise dump the raw bytes of a struct parameter
/// into every failure header.
std::vector<std::string> corpusNames() {
  std::vector<std::string> names;
  names.reserve(corpus().size());
  for (const CorpusScene& scene : corpus()) {
    names.push_back(scene.name);
  }
  return names;
}

const CorpusScene* findScene(std::string_view name) {
  for (const CorpusScene& scene : corpus()) {
    if (scene.name == name) {
      return &scene;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Rendering.
// ---------------------------------------------------------------------------

std::string readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

/// Parse a corpus scene the way the rest of the renderer test suites do:
/// external references resolve through a `SandboxedFileResourceLoader` rooted
/// at the scene's own directory, embedded `@font-face` bytes are trusted test
/// data, and the generic font families point at the checked-in font set. All
/// three matter for the counters: without them the external-resource scenes
/// draw a fraction of their content and the text scenes fall back to whatever
/// the host has installed. The canvas is \p canvasSize applied to the scene's
/// natural size.
std::optional<SVGDocument> loadScene(const CorpusScene& scene,
                                     Vector2i (*canvasSize)(const Vector2i&) = corpusCanvasSize) {
  const std::string source = readFile(scene.path);
  if (source.empty()) {
    ADD_FAILURE() << "Corpus scene not readable: " << scene.path;
    return std::nullopt;
  }

  const std::filesystem::path scenePath(scene.path);
  SVGDocument::Settings settings;
  settings.resourceLoader = std::make_unique<SandboxedFileResourceLoader>(
      ResolveRunfilesResourceRootForTesting(scenePath.parent_path(), scenePath), scenePath);

  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed =
      parser::SVGParser::ParseSVG(source, sink, parser::SVGParser::Options(), std::move(settings));
  if (parsed.hasError()) {
    ADD_FAILURE() << "ParseSVG failed for " << scene.path << ": " << parsed.error().reason;
    return std::nullopt;
  }

  SVGDocument document = std::move(parsed.result());
  TrustDocumentFontFacesForTesting(document);

  const std::filesystem::path fontsDir =
      Runfiles::instance().Rlocation(std::string(kFontsRunfilesPath));
  if (std::filesystem::is_directory(fontsDir)) {
    RegisterFontsFromDirectoryForTesting(document, fontsDir);
  }

  const Vector2i canvas = canvasSize(document.canvasSize());
  document.setCanvasSize(canvas.x, canvas.y);
  return document;
}

/// Both frames of one scene, plus how much of the result is actually visible.
struct SceneMeasurement {
  FrameSample first;
  FrameSample second;
  /// Non-transparent pixels in the settled frame-one snapshot.
  ///
  /// This, not a counter, is the liveness signal. Draw calls looked like the
  /// obvious choice and are wrong: an `feImage` scene composites entirely
  /// inside the filter engine and issues zero `drawPath` calls while rendering
  /// a full image. Pixels are the one signal that means "this scene renders
  /// something" no matter which path composited it.
  size_t visiblePixels = 0;
  /// Filters of frame one that ran on a reduced-resolution raster.
  std::uint64_t filterReducedRasters = 0;
};

FrameSample sampleFrame(const RendererGeode& renderer) {
  const auto timings = renderer.lastFrameTimings();
  const geode::GeodeCounters& counters = timings.counters;
  return FrameSample{{counters.pathEncodes, counters.textureCreates, counters.bufferWrites,
                      counters.bindgroupCreates},
                     counters.drawCalls};
}

/// The device-shared texture pool a measurement starts from.
enum class PoolStart {
  /// Whatever the scenes before it left.
  AsLeft,
  /// Emptied, at its starting budget, so the measurement depends on no earlier scene.
  Empty,
};

/// Render @p document twice on one renderer and return both frames.
///
/// One renderer across both frames keeps the document's cache and residence
/// components live on frame two, which is the steady state an interactive host
/// sits in between edits.
SceneMeasurement measureFrames(const std::shared_ptr<geode::GeodeDevice>& device,
                               SVGDocument& document, PoolStart poolStart = PoolStart::AsLeft) {
  SceneMeasurement measurement;

  RendererGeode renderer(device);
  if (poolStart == PoolStart::Empty) {
    renderer.trimTexturePool(RendererGeodeTexturePoolTrim::All);
  }
  renderer.draw(document);
  // Sampled before the snapshot, like frame two, so the two frames are
  // measured the same way.
  measurement.first = sampleFrame(renderer);
  measurement.filterReducedRasters = renderer.resourceStats().filterReducedRasters;
  // Settle frame one through a readback so deferred GPU work completes before
  // the frame that gets measured, and use the pixels for the liveness check.
  measurement.visiblePixels = test::CountNonTransparentPixels(renderer.takeSnapshot());

  renderer.draw(document);
  // Deliberately sampled BEFORE a second snapshot; see the file header.
  measurement.second = sampleFrame(renderer);
  return measurement;
}

void printMeasurement(const char* label, const std::string& name,
                      const SceneMeasurement& measurement) {
  std::fprintf(stderr,
               "[%s] %-48s pathEncodes=%6" PRIu64 " textureCreates=%4" PRIu64
               " bufferWrites=%6" PRIu64 " bindgroupCreates=%6" PRIu64 " drawCalls=%6" PRIu64
               " frame1DrawCalls=%6" PRIu64 " visiblePixels=%8zu\n",
               label, name.c_str(), measurement.second.gated.pathEncodes,
               measurement.second.gated.textureCreates, measurement.second.gated.bufferWrites,
               measurement.second.gated.bindgroupCreates, measurement.second.drawCalls,
               measurement.first.drawCalls, measurement.visiblePixels);
}

/// One process-wide device shared by every scene. Creating a WebGPU device and
/// compiling the pipelines costs tens of seconds on a software adapter, so the
/// corpus would be unaffordable with a device per scene. It also matches how
/// embedders wire things up: the host owns the GPU context and renderers come
/// and go. `SceneCountersAreOrderIndependent` is the check that sharing the
/// device does not make a scene's counters depend on which scenes ran before
/// it.
std::shared_ptr<geode::GeodeDevice> sharedDevice() {
  static auto device = [] {
    return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
  }();
  return device;
}

/// Hold one measured scene to the steady state, or to its entry in \p pass's ledger.
void expectSteadyState(const std::string& name, const CorpusPass& pass,
                       const SceneMeasurement& measurement) {
  const FrameSample& sample = measurement.second;

  if (isDeliberatelyInert(name, pass)) {
    EXPECT_EQ(measurement.visiblePixels, 0u)
        << name << " is listed in kDeliberatelyInert for the " << pass.name
        << " pass but now renders visible pixels there. Fix the entry and measure the scene like "
        << "any other.";
  } else {
    EXPECT_GT(measurement.visiblePixels, 0u)
        << name << " rendered no visible pixels at " << pass.name
        << ", so the counter ceilings below prove nothing about it: an upper bound is trivially "
        << "satisfied by drawing nothing. Either the scene regressed to rendering blank output, "
        << "or it legitimately renders nothing and belongs in kDeliberatelyInert with a reason.";
  }
  EXPECT_EQ(measurement.filterReducedRasters, 0u)
      << name << " ran a filter on a reduced-resolution raster at " << pass.name
      << ", so its picture is not the full-resolution rendering this pass measures.";

  const KnownViolation* known = findKnownViolation(pass.ledger, name);
  const Observed ceilings = known != nullptr ? known->ceiling : Observed{};
  for (const CounterField& field : kCounterFields) {
    const uint64_t value = sample.gated.*field.member;
    const uint64_t target = kSteadyState.*field.member;
    const bool expectedToViolate = known != nullptr && (known->violated & field.bit) != 0;

    if (!expectedToViolate) {
      EXPECT_LE(value, target)
          << name << " broke the steady-state invariant at " << pass.name << " on " << field.name
          << " (" << field.rationale << "). If this is a known, accepted regression, add an entry "
          << "to that pass's ledger with the measured value and a reason; do not widen the shared "
          << "target.";
      continue;
    }

    const uint64_t ceiling = ceilings.*field.member;
    EXPECT_GT(value, target)
        << name << " now holds the steady-state invariant at " << pass.name << " on " << field.name
        << ", but that pass's ledger still lists it as violating. Delete that "
        << "counter from the entry (and the whole entry once it lists nothing) so the fix cannot "
        << "silently regress. Recorded reason: " << known->reason;
    EXPECT_LE(value, ceiling) << name << " regressed further at " << pass.name << " on "
                              << field.name
                              << " than its recorded known-violation ceiling. Recorded reason: "
                              << known->reason << "; tracking: " << known->tracking;

    // A ceiling only ratchets against the measurement it was taken from, so an
    // improvement that does not reach the target keeps passing against a stale,
    // looser bound. Say so rather than letting the slack accumulate silently.
    if (value > target && value * 2u <= ceiling) {
      std::fprintf(stderr,
                   "[counter-corpus] NOTICE %s improved at %.*s on %s (%" PRIu64
                   " vs recorded ceiling %" PRIu64 "); re-measure and tighten its ledger entry.\n",
                   name.c_str(), static_cast<int>(pass.name.size()), pass.name.data(), field.name,
                   value, ceiling);
    }
  }
}

using GeodeCounterCorpusTest = ::testing::TestWithParam<std::string>;

TEST_P(GeodeCounterCorpusTest, SecondFrameIsSteadyState) {
  const std::shared_ptr<geode::GeodeDevice> device = sharedDevice();
  ASSERT_THAT(device, testing::NotNull()) << "GeodeDevice::CreateHeadless failed";

  const std::string& name = GetParam();
  const CorpusScene* scene = findScene(name);
  ASSERT_NE(scene, nullptr) << "Corpus scene disappeared between enumeration and run: " << name;

  std::optional<SVGDocument> document = loadScene(*scene);
  ASSERT_THAT(document, testing::Optional(testing::_));

  const SceneMeasurement measurement = measureFrames(device, *document);
  printMeasurement("counter-corpus", name, measurement);
  expectSteadyState(name, kCorpusSizePass, measurement);
}

INSTANTIATE_TEST_SUITE_P(Corpus, GeodeCounterCorpusTest, ::testing::ValuesIn(corpusNames()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                           return info.param;
                         });

using GeodeCounterCorpusAboveNaturalSizeTest = ::testing::TestWithParam<std::string>;

/// The same gate at twice each scene's natural size. The pass starts each scene from an empty
/// texture pool, because what the pool keeps depends on texture size and on the working set of
/// the frames before, so a size-dependent miss shows the same way under any test filter or order.
TEST_P(GeodeCounterCorpusAboveNaturalSizeTest, SecondFrameIsSteadyState) {
  const std::shared_ptr<geode::GeodeDevice> device = sharedDevice();
  ASSERT_THAT(device, testing::NotNull()) << "GeodeDevice::CreateHeadless failed";

  const std::string& name = GetParam();
  const CorpusScene* scene = findScene(name);
  ASSERT_NE(scene, nullptr) << "Corpus scene disappeared between enumeration and run: " << name;

  std::optional<SVGDocument> document = loadScene(*scene, aboveNaturalCanvasSize);
  ASSERT_THAT(document, testing::Optional(testing::_));

  const SceneMeasurement measurement = measureFrames(device, *document, PoolStart::Empty);
  const Vector2i canvas = document->canvasSize();
  printMeasurement(
      ("counter-corpus-above-natural " + std::to_string(canvas.x) + "x" + std::to_string(canvas.y))
          .c_str(),
      name, measurement);
  expectSteadyState(name, kAboveNaturalSizePass, measurement);
}

INSTANTIATE_TEST_SUITE_P(Corpus, GeodeCounterCorpusAboveNaturalSizeTest,
                         ::testing::ValuesIn(corpusNames()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                           return info.param;
                         });

using GeodeCounterCorpusLargeCanvasTest = ::testing::TestWithParam<std::string>;

/// Every scene that is not listed as inert for this pass renders visible pixels on a 2048 px
/// canvas.
TEST_P(GeodeCounterCorpusLargeCanvasTest, RendersVisiblePixels) {
  const std::shared_ptr<geode::GeodeDevice> device = sharedDevice();
  ASSERT_THAT(device, testing::NotNull()) << "GeodeDevice::CreateHeadless failed";

  const std::string& name = GetParam();
  const CorpusScene* scene = findScene(name);
  ASSERT_NE(scene, nullptr) << "Corpus scene disappeared between enumeration and run: " << name;

  std::optional<SVGDocument> document = loadScene(*scene, largeCanvasSize);
  ASSERT_THAT(document, testing::Optional(testing::_));

  RendererGeode renderer(device);
  renderer.draw(*document);
  const size_t visiblePixels = test::CountNonTransparentPixels(renderer.takeSnapshot());
  const Vector2i canvas = document->canvasSize();
  std::fprintf(stderr, "[counter-corpus-large-canvas %dx%d] %-48s visiblePixels=%8zu\n", canvas.x,
               canvas.y, name.c_str(), visiblePixels);
  if (isDeliberatelyInert(name, kLargeCanvasPass)) {
    EXPECT_EQ(visiblePixels, 0u) << name << " is listed in kDeliberatelyInert for the "
                                 << kLargeCanvasPass.name
                                 << " pass but now renders visible pixels there. Delete the entry.";
  } else {
    EXPECT_GT(visiblePixels, 0u)
        << name << " rendered no visible pixels at " << kLargeCanvasPass.name
        << ": content that shows on a small canvas disappeared on a large one.";
  }
}

INSTANTIATE_TEST_SUITE_P(Corpus, GeodeCounterCorpusLargeCanvasTest,
                         ::testing::ValuesIn(corpusNames()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                           return info.param;
                         });

// ---------------------------------------------------------------------------
// Guards on the gate itself.
// ---------------------------------------------------------------------------

/// A missing `data` dependency would leave a root empty and its scenes
/// unregistered, which reads as a green suite. Fail instead, per root.
TEST(GeodeCounterCorpusGate, EveryCorpusRootIsPopulated) {
  for (const CorpusRoot& root : kCorpusRoots) {
    EXPECT_GE(scenesUnderRoot(root.path).size(), root.minimumScenes)
        << "Corpus root '" << root.path << "' contributed fewer scenes than expected. Its `data` "
        << "dependency is probably missing, which would silently shrink the gate.";
  }
}

/// The hermetic font set is what makes the text scenes' glyph-derived counters
/// a property of the code rather than of the machine.
TEST(GeodeCounterCorpusGate, HermeticFontsArePresent) {
  const std::filesystem::path fontsDir =
      Runfiles::instance().Rlocation(std::string(kFontsRunfilesPath));
  EXPECT_TRUE(std::filesystem::is_directory(fontsDir))
      << "Hermetic test fonts not found at '" << fontsDir
      << "'. Without them text scenes resolve fonts from the host and their recorded counters "
         "stop being reproducible.";
}

/// Two scenes folding to one sanitized name would collide as gtest parameter
/// names and silently drop one of them from the gate.
TEST(GeodeCounterCorpusGate, SceneNamesAreUnique) {
  std::vector<std::string> names = corpusNames();
  std::sort(names.begin(), names.end());
  const auto duplicate = std::adjacent_find(names.begin(), names.end());
  EXPECT_EQ(duplicate, names.end())
      << "Two corpus scenes share the sanitized name '"
      << (duplicate == names.end() ? std::string() : *duplicate)
      << "'. Rename one of the source files; gtest cannot register both.";
}

/// Every entry of \p ledger names a corpus scene, gates at least one counter, explains itself, and
/// has a ceiling that agrees with its mask.
void expectLedgerWellFormed(std::span<const KnownViolation> ledger) {
  const auto& scenes = corpus();
  for (const KnownViolation& entry : ledger) {
    const bool present = std::any_of(scenes.begin(), scenes.end(), [&](const CorpusScene& scene) {
      return scene.name == entry.scene;
    });
    EXPECT_TRUE(present) << "kKnownViolations lists '" << entry.scene
                         << "', which is not in the corpus. Delete the stale entry.";
    EXPECT_NE(entry.violated, 0u)
        << "kKnownViolations entry '" << entry.scene
        << "' claims no violated counter, so it gates nothing. Delete it.";
    EXPECT_FALSE(entry.reason.empty())
        << "kKnownViolations entry '" << entry.scene << "' needs a one-line reason.";
    EXPECT_FALSE(entry.tracking.empty())
        << "kKnownViolations entry '" << entry.scene << "' needs a tracking note.";

    for (const CounterField& field : kCounterFields) {
      const uint64_t ceiling = entry.ceiling.*field.member;
      const uint64_t target = kSteadyState.*field.member;
      if ((entry.violated & field.bit) != 0) {
        EXPECT_GT(ceiling, target)
            << "kKnownViolations entry '" << entry.scene << "' marks " << field.name
            << " as violated but records a ceiling at or below the shared target, so the "
            << "still-violating assertion could never hold. Fix the mask or the ceiling.";
      } else {
        EXPECT_LE(ceiling, target)
            << "kKnownViolations entry '" << entry.scene << "' records a " << field.name
            << " above the shared target without marking it violated, so the scene would fail "
            << "the shared assertion. Add the counter to the mask or correct the ceiling.";
      }
    }
  }
}

/// A stale or self-contradicting entry would sit in the table forever without
/// gating anything.
TEST(GeodeCounterCorpusGate, KnownViolationsAreWellFormed) {
  for (const CorpusPass* pass : kCorpusPasses) {
    SCOPED_TRACE(::testing::Message() << "ledger of the " << pass->name << " pass");
    expectLedgerWellFormed(pass->ledger);
  }
}

TEST(GeodeCounterCorpusGate, DeliberatelyInertScenesAreWellFormed) {
  const auto& scenes = corpus();
  for (const InertScene& entry : kDeliberatelyInert) {
    const bool present = std::any_of(scenes.begin(), scenes.end(), [&](const CorpusScene& scene) {
      return scene.name == entry.scene;
    });
    EXPECT_TRUE(present) << "kDeliberatelyInert lists '" << entry.scene
                         << "', which is not in the corpus. Delete the stale entry.";
    EXPECT_FALSE(entry.reason.empty())
        << "kDeliberatelyInert entry '" << entry.scene << "' needs a reason.";
    for (const CorpusPass* pass : kCorpusPasses) {
      if (!isDeliberatelyInert(entry.scene, *pass)) {
        continue;
      }
      EXPECT_EQ(findKnownViolation(pass->ledger, entry.scene), nullptr)
          << "'" << entry.scene << "' is listed as both inert and a known violator of the "
          << pass->name << " pass, which cannot both be true. Remove one entry.";
    }
  }
}

/// Every scene shares one device, so its texture pool and buffer pool carry
/// state from whichever scenes ran first. If that state leaked into the
/// measured counters, the recorded table would depend on test order, shard
/// assignment, and `--test_filter`. Render one scene, run unrelated scenes
/// through the same device, then render it again and require identical
/// counters.
TEST(GeodeCounterCorpusGate, SceneCountersAreOrderIndependent) {
  // The same device every scene uses, so this measures the real configuration
  // rather than a pristine one.
  const std::shared_ptr<geode::GeodeDevice> device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  // A pattern scene is the sentinel because it allocates a tile texture every
  // frame, so it is the most sensitive to texture-pool carryover.
  const CorpusScene* sentinel = findScene("geode_pattern_solid");
  ASSERT_NE(sentinel, nullptr) << "sentinel scene missing from the corpus";

  const auto renderSentinel = [&]() {
    std::optional<SVGDocument> document = loadScene(*sentinel);
    EXPECT_TRUE(document.has_value());
    return document.has_value() ? measureFrames(device, *document).second : FrameSample{};
  };

  const FrameSample first = renderSentinel();

  // Unrelated scenes with very different resource shapes: a many-path solid
  // fill, an isolated layer, and a filter graph.
  for (const char* name : {"lion", "image_data_url_opacity", "filter_fill_paint"}) {
    const CorpusScene* other = findScene(name);
    ASSERT_NE(other, nullptr) << "warm-up scene missing from the corpus: " << name;
    std::optional<SVGDocument> document = loadScene(*other);
    ASSERT_TRUE(document.has_value());
    (void)measureFrames(device, *document);
  }

  const FrameSample second = renderSentinel();
  std::fprintf(stderr,
               "[counter-corpus] order-independence sentinel: first drawCalls=%" PRIu64
               " bufferWrites=%" PRIu64 "; repeat drawCalls=%" PRIu64 " bufferWrites=%" PRIu64 "\n",
               first.drawCalls, first.gated.bufferWrites, second.drawCalls,
               second.gated.bufferWrites);

  EXPECT_TRUE(first == second)
      << "The sentinel scene's counters changed after other scenes ran on the same device, so "
      << "the recorded table depends on execution order. Every ceiling in this file would then "
      << "be a function of the shard and filter used to measure it.";
}

}  // namespace
}  // namespace donner::svg
