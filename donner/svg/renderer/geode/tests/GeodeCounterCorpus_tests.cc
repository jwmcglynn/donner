/// @file
/// Corpus-wide steady-state counter gate for the Geode backend.
///
/// `GeodeCounters` documents what an unchanged second frame must cost: the
/// path-encode cache serves every path (`pathEncodes == 0`), render targets and
/// scratch textures come back from the pool (`textureCreates == 0`), resident
/// geometry is already on the GPU so nothing is re-uploaded (`bufferWrites`
/// drives to 0), and bind groups are reused rather than rebuilt per draw
/// (`bindgroupCreates` bounded by the number of distinct pipelines a frame
/// touches).
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
/// silently fall back off residency once it is fixed. Listed scenes are also
/// held to their recorded ceilings, so a listed scene cannot get worse either.
///
/// To refresh the numbers after an intentional change, run the suite and read
/// the `[counter-corpus]` line each scene prints; it carries every gated
/// counter.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Vector2.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeCounters.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

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
/// so the bound is the number of distinct render pipelines a single frame can
/// bind: solid fill, gradient fill, image blit, clip mask, and the two
/// checkerboard variants the editor draws behind the document. Rebuilding a
/// bind group per DRAW - which is what every entry above this bound is doing -
/// is the regression the bound exists to catch.
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
  uint64_t Observed::*member;
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
  /// Bitmask of `CounterBit`: the counters this scene must STILL break.
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

/// Scenes that fall off the steady state today. Shrinking this table is the
/// point of the gate; growing it needs a deliberate justification in review.
constexpr std::array<KnownViolation, 32> kKnownViolations = {{
    {
        /*scene=*/"Edzample_Anim3",
        /*violated=*/kBufferWrites,
        /*ceiling=*/{0, 0, 4, 4},
        /*reason=*/"group opacity layers rewrite their composite blit uniform every frame",
        /*tracking=*/
        "clears when the isolated-layer composite reuses a stable uniform allocation across "
        "frames",
    },
    {
        /*scene=*/"donner_icon",
        /*violated=*/kTextureCreates | kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{0, 4, 65, 25},
        /*reason=*/
        "gradient-painted paths under an opacity group re-upload geometry, allocate scratch "
        "textures, and rebuild bind groups per draw",
        /*tracking=*/
        "clears when gradient-painted paths and layer composites get the residency the "
        "solid-fill path already has",
    },
    {
        /*scene=*/"donner_splash",
        /*violated=*/kPathEncodes | kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{2, 0, 64, 32},
        /*reason=*/
        "filters, clip paths, and dozens of nested opacity groups re-encode and re-upload their "
        "layer geometry every frame",
        /*tracking=*/
        "clears when filter and clip-mask layers cache their geometry and bind groups across "
        "frames",
    },
    {
        /*scene=*/"feimage_external_svg",
        /*violated=*/kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{0, 0, 9, 9},
        /*reason=*/
        "the feImage filter graph rewrites its per-pass scratch buffers and bind groups every "
        "frame",
        /*tracking=*/
        "clears when filter passes keep their uniform allocations and bind groups across "
        "unchanged frames",
    },
    {
        /*scene=*/"filter_displacement_map",
        /*violated=*/kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{0, 0, 11, 10},
        /*reason=*/
        "filter passes rewrite their per-pass uniform scratch and rebuild a bind group per pass "
        "every frame",
        /*tracking=*/
        "clears when filter passes keep their uniform allocations and bind groups across "
        "unchanged frames",
    },
    {
        /*scene=*/"filter_drop_shadow",
        /*violated=*/kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{0, 0, 24, 24},
        /*reason=*/
        "three stacked drop-shadow filters rewrite per-pass uniform scratch and rebuild a bind "
        "group per pass every frame",
        /*tracking=*/
        "clears when filter passes keep their uniform allocations and bind groups across "
        "unchanged frames",
    },
    {
        /*scene=*/"filter_fill_paint",
        /*violated=*/kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{0, 0, 10, 10},
        /*reason=*/
        "filter passes rewrite their per-pass uniform scratch and rebuild a bind group per pass "
        "every frame",
        /*tracking=*/
        "clears when filter passes keep their uniform allocations and bind groups across "
        "unchanged frames",
    },
    {
        /*scene=*/"filter_spot_light",
        /*violated=*/kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{0, 0, 10, 10},
        /*reason=*/
        "filter passes rewrite their per-pass uniform scratch and rebuild a bind group per pass "
        "every frame",
        /*tracking=*/
        "clears when filter passes keep their uniform allocations and bind groups across "
        "unchanged frames",
    },
    {
        /*scene=*/"filter_stroke_paint",
        /*violated=*/kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{0, 0, 10, 10},
        /*reason=*/
        "filter passes rewrite their per-pass uniform scratch and rebuild a bind group per pass "
        "every frame",
        /*tracking=*/
        "clears when filter passes keep their uniform allocations and bind groups across "
        "unchanged frames",
    },
    {
        /*scene=*/"geode_pattern_checker",
        /*violated=*/kTextureCreates | kBufferWrites,
        /*ceiling=*/{0, 1, 9, 1},
        /*reason=*/
        "the pattern tile is re-rendered into a freshly allocated texture every frame and the "
        "tiled fill draws through the per-frame arena",
        /*tracking=*/
        "clears when a pattern tile is cached per paint server and its fill gains residency",
    },
    {
        /*scene=*/"geode_pattern_nonrect",
        /*violated=*/kTextureCreates | kBufferWrites,
        /*ceiling=*/{0, 1, 9, 1},
        /*reason=*/
        "the pattern tile is re-rendered into a freshly allocated texture every frame and the "
        "tiled fill draws through the per-frame arena",
        /*tracking=*/
        "clears when a pattern tile is cached per paint server and its fill gains residency",
    },
    {
        /*scene=*/"geode_pattern_offset",
        /*violated=*/kTextureCreates | kBufferWrites,
        /*ceiling=*/{0, 1, 9, 1},
        /*reason=*/
        "the pattern tile is re-rendered into a freshly allocated texture every frame and the "
        "tiled fill draws through the per-frame arena",
        /*tracking=*/
        "clears when a pattern tile is cached per paint server and its fill gains residency",
    },
    {
        /*scene=*/"geode_pattern_solid",
        /*violated=*/kTextureCreates | kBufferWrites,
        /*ceiling=*/{0, 1, 9, 1},
        /*reason=*/
        "the pattern tile is re-rendered into a freshly allocated texture every frame and the "
        "tiled fill draws through the per-frame arena",
        /*tracking=*/
        "clears when a pattern tile is cached per paint server and its fill gains residency",
    },
    {
        /*scene=*/"geode_text_decoration_underline",
        /*violated=*/kPathEncodes | kBufferWrites,
        /*ceiling=*/{5, 0, 45, 5},
        /*reason=*/
        "glyph and decoration outlines are re-encoded and re-uploaded every frame; placed text "
        "has no path cache or residency",
        /*tracking=*/
        "clears when placed glyph geometry gets the same per-entity cache and residency shapes "
        "have",
    },
    {
        /*scene=*/"geode_text_pattern_fill",
        /*violated=*/kPathEncodes | kTextureCreates | kBufferWrites,
        /*ceiling=*/{4, 1, 36, 4},
        /*reason=*/
        "glyph outlines re-encode every frame and the pattern tile behind them is re-rendered "
        "into a fresh texture",
        /*tracking=*/
        "clears when placed glyph geometry gains residency and pattern tiles are cached per "
        "paint server",
    },
    {
        /*scene=*/"geode_text_span_gradient_over_pattern",
        /*violated=*/kPathEncodes | kTextureCreates | kBufferWrites,
        /*ceiling=*/{3, 1, 27, 3},
        /*reason=*/
        "glyph outlines re-encode every frame and the pattern tile behind them is re-rendered "
        "into a fresh texture",
        /*tracking=*/
        "clears when placed glyph geometry gains residency and pattern tiles are cached per "
        "paint server",
    },
    {
        /*scene=*/"geode_text_span_gradient_over_pattern_stroke",
        /*violated=*/kPathEncodes | kTextureCreates | kBufferWrites,
        /*ceiling=*/{3, 1, 27, 3},
        /*reason=*/
        "glyph outlines re-encode every frame and the pattern tile behind them is re-rendered "
        "into a fresh texture",
        /*tracking=*/
        "clears when placed glyph geometry gains residency and pattern tiles are cached per "
        "paint server",
    },
    {
        /*scene=*/"image_data_url_opacity",
        /*violated=*/kTextureCreates | kBufferWrites,
        /*ceiling=*/{0, 1, 2, 2},
        /*reason=*/
        "the image quad allocates a texture and re-uploads its quad geometry every frame, and "
        "the opacity layer composites through a fresh blit",
        /*tracking=*/
        "clears when decoded images keep their texture and the image quad gains residency",
    },
    {
        /*scene=*/"image_data_url_pixelated",
        /*violated=*/kTextureCreates | kBufferWrites,
        /*ceiling=*/{0, 1, 1, 1},
        /*reason=*/
        "the image quad allocates a texture and re-uploads its quad geometry every frame",
        /*tracking=*/
        "clears when decoded images keep their texture and the image quad gains residency",
    },
    {
        /*scene=*/"linear_gradient_stroke",
        /*violated=*/kBufferWrites,
        /*ceiling=*/{0, 0, 9, 1},
        /*reason=*/
        "a gradient-painted stroke outline draws through the per-frame arena; only solid-painted "
        "geometry is resident",
        /*tracking=*/
        "clears when gradient-painted stroke outlines gain residency alongside gradient fills",
    },
    {
        /*scene=*/"marker",
        /*violated=*/kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{0, 0, 414, 46},
        /*reason=*/
        "every drawn marker instance re-uploads its geometry through the per-frame arena and "
        "builds its own bind group",
        /*tracking=*/"clears when marker instance geometry gains per-entity residency",
    },
    {
        /*scene=*/"marker_segments",
        /*violated=*/kTextureCreates | kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{0, 7, 203, 43},
        /*reason=*/
        "every drawn marker instance re-uploads its geometry and allocates clip scratch textures "
        "each frame",
        /*tracking=*/
        "clears when marker instance geometry gains per-entity residency and marker clips reuse "
        "pooled scratch",
    },
    {
        /*scene=*/"marker_spec_example",
        /*violated=*/kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{0, 0, 92, 20},
        /*reason=*/
        "every drawn marker instance re-uploads its geometry through the per-frame arena and "
        "builds its own bind group",
        /*tracking=*/"clears when marker instance geometry gains per-entity residency",
    },
    {
        /*scene=*/"nested_svg_aspectratio",
        /*violated=*/kPathEncodes | kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{208, 0, 2582, 290},
        /*reason=*/
        "nested viewports carrying text and use instances re-encode and re-upload the whole "
        "subtree every frame",
        /*tracking=*/
        "clears when nested-viewport subtrees, placed text, and use instances all keep their "
        "cached geometry",
    },
    {
        /*scene=*/"poker_chips",
        /*violated=*/kPathEncodes | kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{12, 0, 96, 20},
        /*reason=*/
        "use instances share one source path with differing stroke styles, so the per-entity "
        "stroke cache thrashes and re-encodes every instance",
        /*tracking=*/
        "clears when the stroke cache keys on the resolved stroke style rather than one slot per "
        "source entity",
    },
    {
        /*scene=*/"radial_gradient_stroke",
        /*violated=*/kBufferWrites,
        /*ceiling=*/{0, 0, 9, 1},
        /*reason=*/
        "a gradient-painted stroke outline draws through the per-frame arena; only solid-painted "
        "geometry is resident",
        /*tracking=*/
        "clears when gradient-painted stroke outlines gain residency alongside gradient fills",
    },
    {
        /*scene=*/"simple_text_demo",
        /*violated=*/kPathEncodes | kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{24, 0, 216, 24},
        /*reason=*/
        "glyph outlines are re-encoded and re-uploaded every frame; placed text has no path "
        "cache or residency",
        /*tracking=*/
        "clears when placed glyph geometry gets the same per-entity cache and residency shapes "
        "have",
    },
    {
        /*scene=*/"stroking_complex",
        /*violated=*/kPathEncodes | kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{8, 0, 16, 8},
        /*reason=*/
        "eight use instances share one source path at eight different stroke widths, so the "
        "per-entity stroke cache thrashes and re-encodes each instance",
        /*tracking=*/
        "clears when the stroke cache keys on the resolved stroke style rather than one slot per "
        "source entity",
    },
    {
        /*scene=*/"svg2_e_use_003",
        /*violated=*/kBufferWrites,
        /*ceiling=*/{0, 0, 9, 1},
        /*reason=*/
        "the stroke outline of a styled use instance draws through the per-frame arena instead "
        "of a resident buffer",
        /*tracking=*/
        "clears when use-instance stroke outlines gain the residency their fills already have",
    },
    {
        /*scene=*/"svg2_e_use_004",
        /*violated=*/kBufferWrites,
        /*ceiling=*/{0, 0, 9, 1},
        /*reason=*/
        "the stroke outline of a styled use instance draws through the per-frame arena instead "
        "of a resident buffer",
        /*tracking=*/
        "clears when use-instance stroke outlines gain the residency their fills already have",
    },
    {
        /*scene=*/"text_inline_size_wrap",
        /*violated=*/kPathEncodes | kBufferWrites | kBindgroupCreates,
        /*ceiling=*/{35, 0, 315, 35},
        /*reason=*/
        "every wrapped glyph outline is re-encoded and re-uploaded each frame; placed text has "
        "no path cache or residency",
        /*tracking=*/
        "clears when placed glyph geometry gets the same per-entity cache and residency shapes "
        "have",
    },
    {
        /*scene=*/"text_nested_baseline_shift_idempotency",
        /*violated=*/kPathEncodes | kBufferWrites,
        /*ceiling=*/{2, 0, 18, 2},
        /*reason=*/
        "glyph outlines are re-encoded and re-uploaded every frame; placed text has no path "
        "cache or residency",
        /*tracking=*/
        "clears when placed glyph geometry gets the same per-entity cache and residency shapes "
        "have",
    },
}};

const KnownViolation* findKnownViolation(std::string_view scene) {
  for (const KnownViolation& entry : kKnownViolations) {
    if (entry.scene == scene) {
      return &entry;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Corpus enumeration.
// ---------------------------------------------------------------------------

/// Directories, relative to the runfiles root, holding the render corpus. The
/// repository root itself carries the logo SVGs that ship in the same
/// `testdata` filegroup.
constexpr std::string_view kCorpusRoots[] = {
    ".",
    "donner/svg/renderer/testdata",
    "donner/svg/renderer/benchmarks/testdata",
};

/// Lower bound on the corpus size, asserted so a missing `data` dependency
/// fails loudly instead of turning the whole gate into a no-op.
constexpr size_t kMinimumCorpusSize = 75;

struct CorpusScene {
  std::string name;
  std::string path;
};

/// Maximum canvas edge, in device pixels, applied to every corpus scene.
///
/// Some corpus documents declare enormous intrinsic sizes (one is 100000 px
/// wide), well past the WebGPU guaranteed maximum 2D texture dimension, which
/// would make the gate depend on the adapter's limits. Clamping each axis keeps
/// every scene inside the guaranteed limits and bounds the suite's memory and
/// wall clock without changing which code paths a scene exercises.
constexpr int kMaxCanvasEdge = 512;

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

std::vector<CorpusScene> enumerateCorpus() {
  std::vector<CorpusScene> scenes;
  for (const std::string_view root : kCorpusRoots) {
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(root), ec);
    if (ec) {
      continue;
    }
    for (const std::filesystem::directory_entry& entry : it) {
      if (!entry.is_regular_file() || entry.path().extension() != ".svg") {
        continue;
      }
      scenes.push_back({sanitizeName(entry.path().stem().string()), entry.path().string()});
    }
  }

  // Stable order so the per-scene dump is reproducible run to run.
  std::sort(scenes.begin(), scenes.end(),
            [](const CorpusScene& a, const CorpusScene& b) { return a.name < b.name; });
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

void printObserved(const std::string& name, const Observed& observed) {
  std::fprintf(stderr,
               "[counter-corpus] %-48s pathEncodes=%6" PRIu64 " textureCreates=%4" PRIu64
               " bufferWrites=%6" PRIu64 " bindgroupCreates=%6" PRIu64 "\n",
               name.c_str(), observed.pathEncodes, observed.textureCreates, observed.bufferWrites,
               observed.bindgroupCreates);
}

class GeodeCounterCorpusTest : public ::testing::TestWithParam<std::string> {
protected:
  /// One process-wide device shared by every scene. Creating a WebGPU device
  /// and compiling the pipelines costs tens of seconds on a software adapter,
  /// so the corpus would be unaffordable with a device per scene. It also
  /// matches how embedders wire things up: the host owns the GPU context and
  /// renderers come and go.
  static std::shared_ptr<geode::GeodeDevice> sharedDevice() {
    static auto device = [] {
      return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    }();
    return device;
  }
};

TEST_P(GeodeCounterCorpusTest, SecondFrameIsSteadyState) {
  const std::shared_ptr<geode::GeodeDevice> device = sharedDevice();
  ASSERT_TRUE(device) << "GeodeDevice::CreateHeadless failed";

  const std::string& name = GetParam();
  const CorpusScene* scene = findScene(name);
  ASSERT_NE(scene, nullptr) << "Corpus scene disappeared between enumeration and run: " << name;
  const std::string source = readFile(scene->path);
  ASSERT_FALSE(source.empty()) << "Corpus scene not readable: " << scene->path;

  ParseWarningSink sink = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source, sink);
  ASSERT_FALSE(parsed.hasError()) << "ParseSVG failed for " << scene->path << ": "
                                  << parsed.error().reason;
  SVGDocument document = std::move(parsed.result());

  const Vector2i natural = document.canvasSize();
  document.setCanvasSize(std::clamp(natural.x, 1, kMaxCanvasEdge),
                         std::clamp(natural.y, 1, kMaxCanvasEdge));

  // One renderer across both frames so the document's cache and residence
  // components are live on frame two, which is exactly the steady state an
  // interactive host sits in between edits.
  RendererGeode renderer(device);
  renderer.draw(document);
  // Settle frame one through a readback so any deferred GPU work completes
  // before the frame that gets measured.
  (void)renderer.takeSnapshot();

  renderer.draw(document);
  // Deliberately sampled BEFORE a second snapshot: `takeSnapshot` allocates a
  // readback buffer and its own bind group, which is readback cost rather than
  // the per-frame render cost this gate is about.
  const auto timings = renderer.lastFrameTimings();
  const geode::GeodeCounters& counters = timings.counters;
  const Observed observed = {counters.pathEncodes, counters.textureCreates, counters.bufferWrites,
                             counters.bindgroupCreates};
  printObserved(name, observed);

  const KnownViolation* known = findKnownViolation(name);
  for (const CounterField& field : kCounterFields) {
    const uint64_t value = observed.*field.member;
    const uint64_t target = kSteadyState.*field.member;
    const bool expectedToViolate = known != nullptr && (known->violated & field.bit) != 0;

    if (!expectedToViolate) {
      EXPECT_LE(value, target)
          << name << " broke the steady-state invariant on " << field.name << " ("
          << field.rationale << "). If this is a known, accepted regression, add an entry to "
          << "kKnownViolations with the measured value and a reason; do not widen the shared "
          << "target.";
      continue;
    }

    EXPECT_GT(value, target)
        << name << " now holds the steady-state invariant on " << field.name
        << ", but kKnownViolations still lists it as violating. Delete that counter from the "
        << "entry (and the whole entry once it lists nothing) so the fix cannot silently "
        << "regress. Recorded reason: " << known->reason;
    EXPECT_LE(value, known->ceiling.*field.member)
        << name << " regressed further on " << field.name
        << " than its recorded known-violation ceiling. Recorded reason: " << known->reason
        << "; tracking: " << known->tracking;
  }
}

INSTANTIATE_TEST_SUITE_P(Corpus, GeodeCounterCorpusTest, ::testing::ValuesIn(corpusNames()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                           return info.param;
                         });

// ---------------------------------------------------------------------------
// Guards on the gate itself.
// ---------------------------------------------------------------------------

/// A missing `data` dependency would leave the corpus empty and every
/// parameterized case unregistered, which reads as a green suite. Fail instead.
TEST(GeodeCounterCorpusGate, CorpusIsPopulated) {
  EXPECT_GE(corpus().size(), kMinimumCorpusSize)
      << "Only " << corpus().size()
      << " corpus scenes were found. The suite's data dependency on the render testdata is "
         "probably missing, which would make the whole gate vacuous.";
}

/// A stale entry (renamed or deleted scene, or one that no longer claims any
/// counter) would sit in the table forever without gating anything.
TEST(GeodeCounterCorpusGate, KnownViolationsAreWellFormed) {
  for (const KnownViolation& entry : kKnownViolations) {
    const auto& scenes = corpus();
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
  }
}

}  // namespace
}  // namespace donner::svg
