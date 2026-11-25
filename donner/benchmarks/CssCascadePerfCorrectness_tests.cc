/// @file
/// Exact style cascade work for fixed documents and edits, as reported through the rendering
/// diagnostics. Serial counts are exact. Across worker schedules only logical work is invariant.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "donner/base/tasks/TaskScheduler.h"
#include "donner/base/tasks/TaskScope.h"
#include "donner/base/tests/RunfileGate.h"
#include "donner/base/tests/StallGuard.h"
#include "donner/benchmarks/CssCascadeScenes.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::benchmarks {
namespace {

using svg::components::StyleCascadeCounters;
using ::testing::AllOf;
using ::testing::Field;
using ::testing::Gt;
using ::testing::Lt;
using ::testing::Matcher;
using ::testing::Optional;

/// Documents the cases run against.
enum class Document { Splash, Icons, Tiger, Synthetic, UseInstances, Descendant };

/// Source of \p document, read or generated once per process.
const std::string& SourceOf(Document document) {
  static std::map<Document, std::string> sources;
  if (const auto it = sources.find(document); it != sources.end()) {
    return it->second;
  }
  const auto read = [](std::string_view path) {
    tests::RequiredRunfile file = tests::ReadRequiredRunfile(path);
    EXPECT_THAT(file.error, ::testing::IsEmpty());
    return std::move(file.contents);
  };
  std::string source;
  switch (document) {
    case Document::Splash: source = read(kSplashPath); break;
    case Document::Icons: source = read(kBootstrapIconsPath); break;
    case Document::Tiger: source = read(kTigerPath); break;
    case Document::Synthetic: source = MakeSyntheticDocument(); break;
    case Document::UseInstances: source = MakeUseInstancesDocument(200, 20, 20); break;
    case Document::Descendant:
      source = MakeGroupedRectsDocument(1000, 100, CascadeRuleKind::Descendant);
      break;
  }
  return sources.emplace(document, std::move(source)).first->second;
}

std::string WideRenderFixture() {
  std::string source = "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'>";
  for (int i = 0; i < 1050; ++i) {
    source += "<rect width='1' height='1'/>";
  }
  return source + "</svg>";
}

TEST(CascadeRendererOptions, SchedulingIsSelectedByTheCaller) {
  const auto source = WideRenderFixture();
  auto serial = ParseCascadeDocument(source);
  auto scheduled = ParseCascadeDocument(source);
  ASSERT_TRUE(serial && scheduled);
  svg::Renderer serialRenderer;
  serialRenderer.draw(*serial);
  tasks::TaskScheduler scheduler({.workerCount = 2});
  svg::Renderer parallelRenderer(svg::RendererOptions{.scheduler = &scheduler});
  parallelRenderer.draw(*scheduled);
  const auto& reference = serial->registry().ctx().get<svg::components::StyleCascadeMetrics>();
  const auto& actual = scheduled->registry().ctx().get<svg::components::StyleCascadeMetrics>();
  EXPECT_EQ(reference.scheduledPasses, 0u);
  EXPECT_GT(actual.scheduledPasses, 0u);
  EXPECT_GT(actual.replayedSelectorTraces, 1000u);
  // Stealing may change physical hits, misses, skipped parses and registry copies.
  EXPECT_TRUE(actual.counters.sameLogicalWork(reference.counters));
}

TEST(CascadeRendererOptions, WorkerDrawFallsBackOnlyForItsOwnScheduler) {
  tasks::TaskScheduler caller({.workerCount = 2});
  tasks::TaskScheduler other({.workerCount = 2});
  tasks::TaskScope scope(caller);
  const auto submitter = std::this_thread::get_id();
  for (auto* borrowed : {&caller, &other}) {
    std::optional<std::uint64_t> scheduledPasses;
    ASSERT_TRUE(scope.trySpawn([&] {
      EXPECT_NE(std::this_thread::get_id(), submitter);
      auto document = ParseCascadeDocument(WideRenderFixture());
      ASSERT_TRUE(document);
      svg::Renderer renderer(svg::RendererOptions{.scheduler = borrowed});
      renderer.draw(*document);
      const auto access = document->readAccess();
      const auto& metrics = access.registry().ctx().get<svg::components::StyleCascadeMetrics>();
      scheduledPasses = metrics.scheduledPasses;
    }));
    scope.wait();
    ASSERT_TRUE(scheduledPasses);
    EXPECT_EQ(*scheduledPasses, borrowed == &caller ? 0u : 1u);
  }
}

TEST(CascadeRendererOptions, SeededDrawLeavesOtherScopeQueued) {
  tasks::TaskScheduler scheduler({.workerCount = 0, .seed = 7123});
  auto document = ParseCascadeDocument(WideRenderFixture());
  ASSERT_TRUE(document);
  bool earlierRan = false;
  tasks::TaskScope earlier(scheduler);
  ASSERT_TRUE(earlier.trySpawn([&] { earlierRan = true; }));
  svg::Renderer renderer(svg::RendererOptions{.scheduler = &scheduler});
  renderer.draw(*document);
  EXPECT_FALSE(earlierRan);
  const auto& metrics = document->registry().ctx().get<svg::components::StyleCascadeMetrics>();
  EXPECT_EQ(metrics.scheduledPasses, 1u);
  earlier.wait();
  EXPECT_TRUE(earlierRan);
}

TEST(CascadeRendererOptions, AdoptedDrawLeavesOlderDequeWorkQueued) {
  tasks::TaskScheduler scheduler({.workerCount = 1});
  auto document = ParseCascadeDocument(WideRenderFixture());
  ASSERT_TRUE(document);
  std::promise<void> occupied;
  std::promise<void> release;
  auto entered = occupied.get_future();
  auto done = release.get_future();
  std::thread blocker([&] {
    tasks::TaskScope scope(scheduler);
    scope.spawn([&] {
      occupied.set_value();
      EXPECT_EQ(done.wait_for(tests::kHangGuard), std::future_status::ready);
    });
  });
  if (entered.wait_for(tests::kHangGuard) == std::future_status::ready) {
    auto adopted = scheduler.adopt();
    EXPECT_TRUE(adopted);
    if (adopted) {
      std::atomic<bool> earlierRan = false;
      tasks::TaskScope earlier(scheduler);
      EXPECT_TRUE(earlier.trySpawn([&] { earlierRan = true; }));
      svg::Renderer renderer(svg::RendererOptions{.scheduler = &scheduler});
      renderer.draw(*document);
      EXPECT_FALSE(earlierRan.load());
      const auto& metrics = document->registry().ctx().get<svg::components::StyleCascadeMetrics>();
      EXPECT_EQ(metrics.scheduledPasses, 1u);
      earlier.wait();
      EXPECT_TRUE(earlierRan.load());
    }
  } else {
    ADD_FAILURE() << "The scheduler worker did not enter the blocking fixture";
  }
  release.set_value();
  blocker.join();
}

/// Matches every count of \p expected, naming each mismatched field.
Matcher<StyleCascadeCounters> CountersAre(const StyleCascadeCounters& expected) {
  return AllOf(
      Field("elementsCascaded", &StyleCascadeCounters::elementsCascaded, expected.elementsCascaded),
      Field("rulesTested", &StyleCascadeCounters::rulesTested, expected.rulesTested),
      Field("rulesMatched", &StyleCascadeCounters::rulesMatched, expected.rulesMatched),
      Field("declarationsApplied", &StyleCascadeCounters::declarationsApplied,
            expected.declarationsApplied),
      Field("fullClears", &StyleCascadeCounters::fullClears, expected.fullClears),
      Field("budgetRejections", &StyleCascadeCounters::budgetRejections,
            expected.budgetRejections));
}

/// Matches a measurement whose repetitions all did \p expected work.
Matcher<std::optional<CascadeMeasurement>> StableWork(const StyleCascadeCounters& expected) {
  return Optional(AllOf(Field("countersStable", &CascadeMeasurement::countersStable, true),
                        Field("counters", &CascadeMeasurement::counters, CountersAre(expected))));
}

/// Two repetitions, so each case also checks that a repeated frame does the same work.
constexpr CascadeRepetitions kRepetitions{.warmup = 0, .measured = 2};
/// One repetition, for the generated scaling documents, each cascaded once per process.
constexpr CascadeRepetitions kOneRepetition{.warmup = 0, .measured = 1};
/// The frame after the appended rule, which the first repetition's restore leaves out.
constexpr CascadeRepetitions kAppendFrame = OneDirection(kRepetitions, RecordedRepetitions::Odd);
/// The frame after the forward keystroke, for a keystroke whose undo does different work.
constexpr CascadeRepetitions kForwardKeystroke =
    OneDirection(kRepetitions, RecordedRepetitions::Even);

struct CascadeCase {
  std::string name;
  std::function<std::optional<CascadeMeasurement>()> measure;
  StyleCascadeCounters expected;
};

std::ostream& operator<<(std::ostream& os, const CascadeCase& value) {
  return os << value.name;
}

CascadeCase Load(std::string name, Document document, StyleCascadeCounters expected) {
  return {std::move(name),
          [document] { return MeasureCascadeLoad(SourceOf(document), kRepetitions, false); },
          expected};
}

CascadeCase LoadUncached(std::string name, Document document, StyleCascadeCounters expected) {
  return {std::move(name),
          [document] { return MeasureCascadeLoad(SourceOf(document), kRepetitions, false, true); },
          expected};
}

CascadeCase Generated(std::string name, int elements, int rules, CascadeRuleKind kind,
                      StyleCascadeCounters expected) {
  return {std::move(name),
          [=] {
            return MeasureCascadeLoad(MakeGroupedRectsDocument(elements, rules, kind),
                                      kOneRepetition, false);
          },
          expected};
}

CascadeCase GeneratedUncached(std::string name, int elements, int rules, CascadeRuleKind kind,
                              StyleCascadeCounters expected) {
  return {std::move(name),
          [=] {
            return MeasureCascadeLoad(MakeGroupedRectsDocument(elements, rules, kind),
                                      kOneRepetition, false, true);
          },
          expected};
}

CascadeCase Edit(std::string name, Document document, CascadeEditTarget target, CascadeEdit edit,
                 StyleCascadeCounters expected, CascadeRepetitions repetitions = kRepetitions) {
  return {std::move(name),
          [=] { return MeasureCascadeEdit(SourceOf(document), target, edit, repetitions, false); },
          expected};
}

CascadeCase EditUncached(std::string name, Document document, CascadeEditTarget target,
                         CascadeEdit edit, StyleCascadeCounters expected) {
  return {std::move(name),
          [=] {
            return MeasureCascadeEdit(SourceOf(document), target, edit, kRepetitions, false, true);
          },
          expected};
}

CascadeCase Keystroke(std::string name, CascadeKeystroke keystroke, StyleCascadeCounters expected,
                      CascadeRepetitions repetitions = kRepetitions) {
  return {std::move(name),
          [=] {
            return MeasureCascadeKeystroke(SourceOf(Document::Splash), keystroke, repetitions,
                                           false);
          },
          expected};
}

class CssCascadeWorkTest : public ::testing::TestWithParam<CascadeCase> {};

TEST_P(CssCascadeWorkTest, MatchesRecordedWork) {
  EXPECT_THAT(GetParam().measure(), StableWork(GetParam().expected));
}

std::string CaseName(const ::testing::TestParamInfo<CascadeCase>& info) {
  return info.param.name;
}

INSTANTIATE_TEST_SUITE_P(
    FirstFrame, CssCascadeWorkTest,
    ::testing::Values(
        Load("Splash", Document::Splash, {622, 1384, 1382, 1386, 0, 0}),
        Load("BootstrapIcons", Document::Icons, {5137, 9293, 9291, 9291, 0, 0}),
        Load("Tiger", Document::Tiger, {484, 483, 481, 481, 0, 0}),
        Load("Synthetic2000x200", Document::Synthetic, {2003, 3543, 3254, 4506, 0, 0}),
        Load("UseInstances", Document::UseInstances, {4425, 8848, 8846, 8846, 0, 0}),
        Load("Descendant1000x100", Document::Descendant, {1003, 1913, 1010, 1018, 0, 0})),
    CaseName);

const CascadeEditTarget kSplashTarget = SplashEditTarget();
const CascadeEditTarget kSyntheticTarget = SyntheticEditTarget();
const CascadeEditTarget kIconsTarget = BootstrapIconsEditTarget();

// A frame after an edit that restyles the whole document discards every computed style once and
// cascades each styled entity once, so it does the work of the edited document's first frame.
constexpr StyleCascadeCounters kSplashRestyle{622, 1384, 1382, 1386, 1, 0};
constexpr StyleCascadeCounters kIconsRestyle{5137, 9293, 9291, 9291, 1, 0};

INSTANTIATE_TEST_SUITE_P(
    EditFrame, CssCascadeWorkTest,
    ::testing::Values(
        Edit("SplashClass", Document::Splash, kSplashTarget, CascadeEdit::Class, kSplashRestyle),
        Edit("SplashFill", Document::Splash, kSplashTarget, CascadeEdit::Fill, kSplashRestyle),
        Edit("SplashStyle", Document::Splash, kSplashTarget, CascadeEdit::Style, kSplashRestyle),
        // The appended rule's class is on no element, so it is no element's candidate.
        Edit("SplashAppendRule", Document::Splash, kSplashTarget, CascadeEdit::AppendRule,
             {622, 1384, 1382, 1386, 1, 0}, kAppendFrame),
        Edit("SyntheticClass", Document::Synthetic, kSyntheticTarget, CascadeEdit::Class,
             {2003, 3544, 3255, 4508, 1, 0}),
        Edit("SyntheticFill", Document::Synthetic, kSyntheticTarget, CascadeEdit::Fill,
             {2003, 3543, 3254, 4506, 1, 0}),
        Edit("SyntheticStyle", Document::Synthetic, kSyntheticTarget, CascadeEdit::Style,
             {2003, 3543, 3254, 4506, 1, 0}),
        Edit("SyntheticAppendRule", Document::Synthetic, kSyntheticTarget, CascadeEdit::AppendRule,
             {2003, 3543, 3254, 4506, 1, 0}, kAppendFrame),
        Edit("IconsClass", Document::Icons, kIconsTarget, CascadeEdit::Class, kIconsRestyle),
        Edit("IconsFill", Document::Icons, kIconsTarget, CascadeEdit::Fill, kIconsRestyle),
        Edit("IconsStyle", Document::Icons, kIconsTarget, CascadeEdit::Style, kIconsRestyle)),
    CaseName);

INSTANTIATE_TEST_SUITE_P(
    KeystrokeFrame, CssCascadeWorkTest,
    ::testing::Values(
        Keystroke("DeclarationValue", CascadeKeystroke::DeclarationValue, kSplashRestyle),
        Keystroke("SelectorLetterInserted", CascadeKeystroke::Selector,
                  {622, 1383, 1381, 1385, 1, 0}, kForwardKeystroke),
        // The unclosed rule absorbs the rules after it, so fewer rules are candidates.
        Keystroke("ClosingBraceDeleted", CascadeKeystroke::ClosingBrace,
                  {622, 1246, 1244, 1245, 1, 0}, kForwardKeystroke),
        Keystroke("ClassLetterInserted", CascadeKeystroke::ClassAttribute,
                  {622, 1383, 1381, 1385, 1, 0}, kForwardKeystroke)),
    CaseName);

INSTANTIATE_TEST_SUITE_P(
    Scaling, CssCascadeWorkTest,
    ::testing::Values(
        Generated("Mixed1000x0", 1000, 0, CascadeRuleKind::Mixed, {1002, 1002, 1000, 1000, 0, 0}),
        Generated("Mixed1000x10", 1000, 10, CascadeRuleKind::Mixed, {1003, 1913, 1911, 2820, 0, 0}),
        Generated("Mixed1000x100", 1000, 100, CascadeRuleKind::Mixed,
                  {1003, 1784, 1646, 2290, 0, 0}),
        Generated("Mixed1000x1000", 1000, 1000, CascadeRuleKind::Mixed,
                  {1003, 1769, 1638, 2274, 0, 0}),
        Generated("Mixed4000x0", 4000, 0, CascadeRuleKind::Mixed, {4002, 4002, 4000, 4000, 0, 0}),
        Generated("Mixed4000x10", 4000, 10, CascadeRuleKind::Mixed,
                  {4003, 7640, 7638, 11274, 0, 0}),
        Generated("Mixed4000x100", 4000, 100, CascadeRuleKind::Mixed,
                  {4003, 7106, 6546, 9090, 0, 0}),
        // Only candidates are charged to the 1,048,576-test rule-element budget, so this document,
        // which would test four million rules unindexed, styles completely.
        Generated("Mixed4000x1000", 4000, 1000, CascadeRuleKind::Mixed,
                  {4003, 7102, 6555, 9108, 0, 0}),
        Generated("Class1000x1000", 1000, 1000, CascadeRuleKind::Class,
                  {1003, 1913, 1911, 2820, 0, 0}),
        Generated("Descendant1000x1000", 1000, 1000, CascadeRuleKind::Descendant,
                  {1003, 1913, 1013, 1024, 0, 0}),
        // Every type rule is keyed on its type, so each rect tests the 125 `rect` rules.
        Generated("Type1000x1000", 1000, 1000, CascadeRuleKind::Type,
                  {1003, 126004, 2104, 3206, 0, 0}),
        Generated("Attribute1000x1000", 1000, 1000, CascadeRuleKind::Attribute,
                  {1003, 1004, 1002, 1002, 0, 0}),
        // One rect naming `.a` 20,000 times tests each of the 2,000 `.a` rules once.
        CascadeCase{"RepeatedClassTokens2000x20000",
                    [] {
                      return MeasureCascadeLoad(MakeRepeatedClassTokensDocument(2000, 20000),
                                                kOneRepetition, false);
                    },
                    {4, 2005, 2003, 2003, 0, 0}}),
    CaseName);

struct CacheWork {
  std::uint64_t hits;
  std::uint64_t misses;
  std::uint64_t uncacheable;
};

Matcher<std::optional<CascadeMeasurement>> StableCacheWork(CacheWork expected) {
  return Optional(
      AllOf(Field("countersStable", &CascadeMeasurement::countersStable, true),
            Field("distinctComputedStyleRecords", &CascadeMeasurement::distinctComputedStyleRecords,
                  expected.misses),
            Field("counters", &CascadeMeasurement::counters,
                  AllOf(Field("matchedPropertiesCacheHits",
                              &StyleCascadeCounters::matchedPropertiesCacheHits, expected.hits),
                        Field("matchedPropertiesCacheMisses",
                              &StyleCascadeCounters::matchedPropertiesCacheMisses, expected.misses),
                        Field("matchedPropertiesCacheUncacheable",
                              &StyleCascadeCounters::matchedPropertiesCacheUncacheable,
                              expected.uncacheable),
                        Field("declarationParsesSkipped",
                              &StyleCascadeCounters::declarationParsesSkipped, Gt(0u))))));
}

class CssCascadeCacheWorkTest : public ::testing::TestWithParam<CascadeCase> {};

TEST_P(CssCascadeCacheWorkTest, ReusesExpectedComputedStyleRecords) {
  EXPECT_THAT(GetParam().measure(),
              StableCacheWork(CacheWork{GetParam().expected.matchedPropertiesCacheHits,
                                        GetParam().expected.matchedPropertiesCacheMisses,
                                        GetParam().expected.matchedPropertiesCacheUncacheable}));
}

constexpr StyleCascadeCounters CacheCounters(std::uint64_t hits, std::uint64_t misses,
                                             std::uint64_t uncacheable = 0) {
  return StyleCascadeCounters{.matchedPropertiesCacheHits = hits,
                              .matchedPropertiesCacheMisses = misses,
                              .matchedPropertiesCacheUncacheable = uncacheable};
}

INSTANTIATE_TEST_SUITE_P(
    CacheWork, CssCascadeCacheWorkTest,
    // These counts describe the serial cache. With stealing, hits, misses, copies and parses
    // skipped reflect actual worker-local reuse. A fixed seed must reproduce them.
    // The splash includes stop/flood base values and
    // cascade priority that the earlier approximate opportunity grouping did not distinguish.
    ::testing::Values(Load("Splash", Document::Splash, CacheCounters(401, 221)),
                      Load("BootstrapIcons", Document::Icons, CacheCounters(5129, 8)),
                      Load("Tiger", Document::Tiger, CacheCounters(388, 96)),
                      Load("Synthetic2000x200", Document::Synthetic, CacheCounters(1854, 149)),
                      Load("UseInstances", Document::UseInstances, CacheCounters(4378, 47)),
                      Load("Descendant1000x100", Document::Descendant, CacheCounters(991, 12))),
    CaseName);

/// Matches a measurement whose repetitions all made \p copies whole-registry copies.
Matcher<std::optional<CascadeMeasurement>> StableRegistryCopies(std::uint64_t copies) {
  return Optional(
      AllOf(Field("countersStable", &CascadeMeasurement::countersStable, true),
            Field("counters", &CascadeMeasurement::counters,
                  Field("registryCopies", &StyleCascadeCounters::registryCopies, copies))));
}

class CssCascadeRegistryCopyTest : public ::testing::TestWithParam<CascadeCase> {};

TEST_P(CssCascadeRegistryCopyTest, CopiesLocalDeclarationsOncePerElement) {
  EXPECT_THAT(GetParam().measure(), StableRegistryCopies(GetParam().expected.registryCopies));
}

class CssCascadeCachedRegistryCopyTest : public ::testing::TestWithParam<CascadeCase> {};

TEST_P(CssCascadeCachedRegistryCopyTest, CacheHitsAvoidLocalDeclarationCopies) {
  EXPECT_THAT(GetParam().measure(),
              Optional(AllOf(Field("countersStable", &CascadeMeasurement::countersStable, true),
                             Field("counters", &CascadeMeasurement::counters,
                                   Field("registryCopies", &StyleCascadeCounters::registryCopies,
                                         Lt(GetParam().expected.registryCopies))))));
}

/// Expected work with only the registry copies set; the other counts are checked above.
constexpr StyleCascadeCounters RegistryCopies(std::uint64_t copies) {
  return StyleCascadeCounters{.registryCopies = copies};
}

// Building an element's computed style copies its local declarations once, and inheritance builds
// the style in place, so a frame makes one whole-registry copy per cascaded element that has local
// declarations (presentation attributes or a `style` attribute) and none for the others.
INSTANTIATE_TEST_SUITE_P(
    RegistryCopies, CssCascadeRegistryCopyTest,
    ::testing::Values(LoadUncached("Splash", Document::Splash, RegistryCopies(520)),
                      LoadUncached("BootstrapIcons", Document::Icons, RegistryCopies(3057)),
                      LoadUncached("Tiger", Document::Tiger, RegistryCopies(481)),
                      LoadUncached("UseInstances", Document::UseInstances, RegistryCopies(4021)),
                      GeneratedUncached("Mixed1000x0", 1000, 0, CascadeRuleKind::Mixed,
                                        RegistryCopies(910)),
                      EditUncached("SplashClass", Document::Splash, kSplashTarget,
                                   CascadeEdit::Class, RegistryCopies(520)),
                      EditUncached("IconsStyle", Document::Icons, kIconsTarget, CascadeEdit::Style,
                                   RegistryCopies(3057))),
    CaseName);

INSTANTIATE_TEST_SUITE_P(
    RegistryCopySavings, CssCascadeCachedRegistryCopyTest,
    ::testing::Values(Load("Splash", Document::Splash, RegistryCopies(520)),
                      Load("BootstrapIcons", Document::Icons, RegistryCopies(3057)),
                      Load("Tiger", Document::Tiger, RegistryCopies(481)),
                      Load("UseInstances", Document::UseInstances, RegistryCopies(4021))),
    CaseName);

/// Matches a measurement whose repetitions all did the ancestor-filter work of \p expected.
Matcher<std::optional<CascadeMeasurement>> StableAncestorFilterWork(
    const StyleCascadeCounters& expected) {
  return Optional(AllOf(
      Field("countersStable", &CascadeMeasurement::countersStable, true),
      Field(
          "counters", &CascadeMeasurement::counters,
          AllOf(Field("ancestorFilterRejections", &StyleCascadeCounters::ancestorFilterRejections,
                      expected.ancestorFilterRejections),
                Field("ancestorFilterFalsePositives",
                      &StyleCascadeCounters::ancestorFilterFalsePositives,
                      expected.ancestorFilterFalsePositives),
                Field("ancestorFilterSaturations", &StyleCascadeCounters::ancestorFilterSaturations,
                      expected.ancestorFilterSaturations),
                Field("ancestorFilterKeyLimits", &StyleCascadeCounters::ancestorFilterKeyLimits,
                      expected.ancestorFilterKeyLimits)))));
}

class CssCascadeAncestorFilterWorkTest : public ::testing::TestWithParam<CascadeCase> {};

TEST_P(CssCascadeAncestorFilterWorkTest, RejectsExpectedCandidates) {
  EXPECT_THAT(GetParam().measure(), StableAncestorFilterWork(GetParam().expected));
}

/// Expected work with only the filter's rejections and false positives set; it is never disabled
/// on these documents.
constexpr StyleCascadeCounters FilterWork(std::uint64_t rejections,
                                          std::uint64_t falsePositives = 0) {
  return StyleCascadeCounters{.ancestorFilterRejections = rejections,
                              .ancestorFilterFalsePositives = falsePositives};
}

// Only `.gK .cN` rules store ancestor keys (the class `gK`), so only they can be rejected; a
// candidate fails when its rect's group is another `gK`, and no such class collides with the keys
// of the rect's ancestors in the filter. The real documents have no rule with ancestor keys: the
// user agent rule `*:not(svg), *:not(foreignObject) > svg` has a first entry without one, so it is
// never rejected, and the root `<svg>` failing it is the one failed ancestor walk left.
INSTANTIATE_TEST_SUITE_P(
    AncestorFilterWork, CssCascadeAncestorFilterWorkTest,
    ::testing::Values(
        Load("Splash", Document::Splash, FilterWork(0)),
        Load("BootstrapIcons", Document::Icons, FilterWork(0)),
        Load("Tiger", Document::Tiger, FilterWork(0)),
        Load("UseInstances", Document::UseInstances, FilterWork(0)),
        Load("Synthetic2000x200", Document::Synthetic, FilterWork(287)),
        // 901 of the 902 candidates that fail after an ancestor walk without the filter.
        Load("Descendant1000x100", Document::Descendant, FilterWork(901)),
        Generated("Descendant1000x1000", 1000, 1000, CascadeRuleKind::Descendant, FilterWork(898)),
        Generated("Mixed4000x1000", 4000, 1000, CascadeRuleKind::Mixed, FilterWork(545)),
        Edit("SyntheticClass", Document::Synthetic, kSyntheticTarget, CascadeEdit::Class,
             FilterWork(287)),
        Edit("SyntheticFill", Document::Synthetic, kSyntheticTarget, CascadeEdit::Fill,
             FilterWork(287))),
    CaseName);

}  // namespace
}  // namespace donner::benchmarks
