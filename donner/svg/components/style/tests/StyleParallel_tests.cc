#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "donner/base/tasks/TaskScheduler.h"
#include "donner/base/tests/StallGuard.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/style/MatchedPropertiesCache.h"
#include "donner/svg/components/style/ParallelStylePass.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererUtils.h"

namespace donner::svg::components {
namespace {

std::string SiblingScene(std::size_t count) {
  std::string text = R"(<svg xmlns="http://www.w3.org/2000/svg">
    <style>.leaf { stroke: blue; fill: red }</style><g id="parent" font-size="19">)";
  for (std::size_t i = 0; i < count; ++i) {
    text += "<rect class='leaf' id='r" + std::to_string(i) + "'/>";
  }
  return text + "</g></svg>";
}

struct Frame {
  SVGDocument doc;
  StyleCascadeCounters counters;
  std::uint64_t scheduled;
  std::uint64_t traces;
  std::uint64_t admitted;
  std::uint64_t declined;
  std::vector<ParseDiagnostic> warnings;
};

Frame FirstFrame(const std::string& source, tasks::TaskSchedulerOptions options,
                 bool useAncestorFilter = true, std::size_t rangeStackHeadroom = 0) {
  ParseWarningSink warnings;
  auto parse = parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_FALSE(parse.hasError());
  auto doc = std::move(parse).result();
  tasks::TaskScheduler scheduler(options);
  auto& registry = doc.registry();
  if (rangeStackHeadroom != 0) {
    auto& hooks = registry.ctx().emplace<StylePassPhasesForTesting>();
    hooks.minimumElements = 1;
    hooks.rangeStackHeadroom = rangeStackHeadroom;
  }
  if (!useAncestorFilter) {
    registry.ctx().emplace<UnfilteredCascadeForTesting>();
  }
  StyleSystem().computeAllStyles(registry, warnings, StyleSystem::FullRestyle::DiscardAndRecompute,
                                 &scheduler);
  const auto& metrics = registry.ctx().get<StyleCascadeMetrics>();
  return {std::move(doc),
          metrics.counters,
          metrics.scheduledPasses,
          metrics.replayedSelectorTraces,
          metrics.admittedStyleTasks,
          metrics.declinedStyleTasks,
          warnings.warnings()};
}

void ExpectSame(const Frame& serial, const Frame& scheduled) {
  const Registry& lhs = serial.doc.registry();
  const Registry& rhs = scheduled.doc.registry();
  EXPECT_TRUE(serial.counters.sameLogicalWork(scheduled.counters));
  EXPECT_EQ(serial.warnings, scheduled.warnings);
  EXPECT_EQ(lhs.view<const ComputedStyleComponent>().size(),
            rhs.view<const ComputedStyleComponent>().size());
  for (auto entity : lhs.view<const ComputedStyleComponent>()) {
    const auto& first = lhs.get<ComputedStyleComponent>(entity);
    const auto* second = rhs.try_get<ComputedStyleComponent>(entity);
    ASSERT_TRUE(first.style && second && second->style);
    EXPECT_TRUE(first.properties()->exactlyEquals(*second->properties()));
    EXPECT_EQ(first.resourceOpacities()->stopOpacity, second->resourceOpacities()->stopOpacity);
    EXPECT_EQ(first.style->resourceOpacities, second->style->resourceOpacities);
    EXPECT_EQ(first.externalFill, second->externalFill);
    EXPECT_EQ(first.externalStroke, second->externalStroke);
  }
}

TEST(StyleParallel, WideSiblingsMatchSerialAtEachWidth) {
  const auto source = SiblingScene(1200);
  const Frame serial = FirstFrame(source, {.workerCount = 0});
  EXPECT_EQ(serial.scheduled, 0u);
  for (std::size_t width : {1, 2, 4, 8}) {
    const auto result = FirstFrame(source, {.workerCount = width});
    EXPECT_EQ(result.scheduled, 1u);
    EXPECT_GT(result.traces, 1000u);
    ExpectSame(serial, result);
  }
}

TEST(StyleParallel, RetainedSharingKeepsLaterFamilyAdmissionIdenticalAcrossWidths) {
  auto source = SiblingScene(1200);
  source.replace(source.find("fill: red"), std::string_view("fill: red").size(),
                 "fill: red; stroke-dasharray: 1, 2, 3, 4");
  struct Result {
    std::size_t bytes;
    std::size_t refusals;
    bool admitted;
    Frame frame;
  };
  const auto run = [&](std::size_t limit, tasks::TaskSchedulerOptions options) {
    ParseWarningSink warnings;
    auto parsed = parser::SVGParser::ParseSVG(source, warnings);
    EXPECT_FALSE(parsed.hasError());
    auto doc = std::move(parsed).result();
    auto& registry = doc.registry();
    DocumentResourceFamilyBudget::Limits limits;
    limits.computedStyleBytes = limit;
    auto family = std::make_shared<DocumentResourceFamilyBudget>(limits);
    auto& logical = registry.ctx().emplace<StyleResourceBudget>(family);
    tasks::TaskScheduler scheduler(options);
    StyleSystem().computeAllStyles(registry, warnings,
                                   StyleSystem::FullRestyle::DiscardAndRecompute, &scheduler);
    const auto& metrics = registry.ctx().get<StyleCascadeMetrics>();
    EXPECT_EQ(family->stylePreparationBytes(), 0u);
    EXPECT_FALSE(logical.rejected());
    const auto representative =
        doc.querySelector("#r0")->entityHandle().get<ComputedStyleComponent>().style;
    EXPECT_EQ(representative,
              doc.querySelector("#r1199")->entityHandle().get<ComputedStyleComponent>().style);
    const bool admitted = family->reserve(DocumentResourceFamilyBudget::Kind::ComputedStyle, 1);
    return Result{
        logical.complexPropertyBytes(),
        family->securityStats().rejectedReservations,
        admitted,
        {std::move(doc), metrics.counters, metrics.scheduledPasses, metrics.replayedSelectorTraces,
         metrics.admittedStyleTasks, metrics.declinedStyleTasks, warnings.warnings()}};
  };

  const auto roomy = run(1024 * 1024, {.workerCount = 0});
  ASSERT_GT(roomy.bytes, 0u);
  for (std::size_t limit : {roomy.bytes, roomy.bytes + 1}) {
    const auto serial = run(limit, {.workerCount = 0});
    EXPECT_EQ(serial.admitted, limit > roomy.bytes);
    for (const tasks::TaskSchedulerOptions& options :
         {tasks::TaskSchedulerOptions{.workerCount = 1},
          {.workerCount = 2},
          {.workerCount = 4},
          {.workerCount = 8},
          {.workerCount = 0, .seed = 7123}}) {
      const auto scheduled = run(limit, options);
      EXPECT_EQ(scheduled.frame.scheduled, 1u);
      EXPECT_GT(scheduled.frame.traces, 1000u);
      EXPECT_EQ(scheduled.bytes, serial.bytes);
      EXPECT_EQ(scheduled.refusals, serial.refusals);
      EXPECT_EQ(scheduled.admitted, serial.admitted);
      ExpectSame(serial.frame, scheduled.frame);
    }
  }
}

TEST(StyleParallel, ConcurrentDocumentsLeaveFamilyHeadroomAvailableDuringPreparation) {
  using Kind = DocumentResourceFamilyBudget::Kind;
  DocumentResourceFamilyBudget::Limits limits;
  limits.maximumTotalRetainedBytes = 1;
  const auto family = std::make_shared<DocumentResourceFamilyBudget>(limits);
  const std::string source = SiblingScene(12);
  std::array<SVGDocument, 2> documents;
  std::array<ParseWarningSink, 2> warnings;
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t waiting = 0;
  bool release = false;
  std::atomic<bool> ownerTimedOut = false;

  for (std::size_t i = 0; i < documents.size(); ++i) {
    auto parsed = parser::SVGParser::ParseSVG(source, warnings[i]);
    ASSERT_FALSE(parsed.hasError());
    auto& document = documents[i];
    document = std::move(parsed).result();
    auto& registry = document.registry();
    std::ignore = registry.ctx().emplace<StyleResourceBudget>(family);
    auto& hooks = registry.ctx().emplace<StylePassPhasesForTesting>();
    hooks.minimumElements = 1;
    // Root preparation already reserved scratch on each owner. Gate the owners before either
    // submits tasks so the fixture never needs one scheduler worker to block another.
    hooks.beforeRegion = [&] {
      std::unique_lock lock(mutex);
      ++waiting;
      changed.notify_all();
      if (!changed.wait_for(lock, donner::tests::kHangGuard, [&] { return release; })) {
        ownerTimedOut.store(true, std::memory_order_relaxed);
      }
    };
    document.setThreadingMode(ThreadingMode::ConcurrentDom);
  }

  tasks::TaskScheduler scheduler({.workerCount = 2});
  std::array<std::thread, 2> owners;
  for (std::size_t i = 0; i < owners.size(); ++i) {
    owners[i] = std::thread([&, i] {
      auto access = documents[i].writeAccess();
      StyleSystem().computeAllStyles(access.registry(), warnings[i],
                                     StyleSystem::FullRestyle::ComputeMissing, &scheduler);
    });
  }

  bool overlapped = false;
  bool admitted = false;
  DocumentResourceFamilyBudget::SecurityStats during;
  std::size_t preparationBytes = 0;
  {
    std::unique_lock lock(mutex);
    overlapped =
        changed.wait_for(lock, donner::tests::kHangGuard, [&] { return waiting == owners.size(); });
    if (overlapped) {
      preparationBytes = family->stylePreparationBytes();
      admitted = family->reserve(Kind::ParsedPayload, limits.maximumTotalRetainedBytes);
      during = family->securityStats();
    }
    release = true;
    changed.notify_all();
  }
  for (auto& owner : owners) {
    owner.join();
  }

  ASSERT_TRUE(overlapped);
  EXPECT_FALSE(ownerTimedOut.load(std::memory_order_relaxed));
  EXPECT_GE(preparationBytes, owners.size() * StyleSystem::PreparedBatch::workerReservationBytes());
  EXPECT_TRUE(admitted);
  EXPECT_EQ(during.totalRetainedBytes, limits.maximumTotalRetainedBytes);
  EXPECT_FALSE(during.rejected);
  EXPECT_EQ(during.rejectedReservations, 0u);
  EXPECT_EQ(family->stylePreparationBytes(), 0u);
  EXPECT_EQ(family->securityStats().rejectedReservations, 0u);
  for (const auto& document : documents) {
    const auto access = document.readAccess();
    const auto& metrics = access.registry().ctx().get<StyleCascadeMetrics>();
    EXPECT_EQ(metrics.scheduledPasses, 1u);
    EXPECT_EQ(metrics.abandonedStylePasses, 0u);
    EXPECT_GT(metrics.replayedSelectorTraces, 0u);
  }
}

std::string EvictionScene() {
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg">)";
  // Equal rule positions and specificity share a placement bucket, while the stylesheet
  // revisions and declarations distinguish these keys in the exact lookup.
  for (int i = 0; i < 12; ++i) {
    const std::string n = std::to_string(i);
    source += "<style>.k" + n + " { stroke-dasharray: " + n + " 2 3 4; }</style>";
  }
  source += "<style>.warn1 { display: wrong; } .warn2 { stroke-width: wrong; }</style><g>";
  for (int i = 0; i < 12; ++i) {
    const std::string n = std::to_string(i);
    source += "<rect class='k" + n + "' id='a" + n + "'/><rect class='k" + n + "'/>";
  }
  return source +
         "<circle id='first-warning' class='warn1'/>"
         "<ellipse id='second-warning' class='warn2'/></g></svg>";
}

struct EvictionPass {
  StyleCascadeCounters work;
  std::vector<ParseDiagnostic> warnings;
  std::size_t warningAttempts;
  std::size_t bytes;
  std::size_t evictions;
  std::size_t scheduled;
  std::size_t abandoned;
  bool rejected;
};

struct EvictionResult {
  Frame frame;
  std::array<EvictionPass, 3> passes;
  bool siblingAdmitted;
  bool followingAdmitted;
  std::size_t refusals;
};

EvictionResult RunEvictionPasses(const std::string& source, std::size_t limit,
                                 tasks::TaskSchedulerOptions options) {
  using Kind = DocumentResourceFamilyBudget::Kind;
  DocumentResourceFamilyBudget::Limits limits;
  limits.computedStyleBytes = limit;
  const auto family = std::make_shared<DocumentResourceFamilyBudget>(limits);
  ParseWarningSink parseWarnings;
  auto parsed = parser::SVGParser::ParseSVG(source, parseWarnings);
  EXPECT_FALSE(parsed.hasError());
  SVGDocument document = std::move(parsed).result();
  auto& registry = document.registry();
  auto& budget = registry.ctx().emplace<StyleResourceBudget>(family);
  auto& hooks = registry.ctx().emplace<StylePassPhasesForTesting>();
  hooks.minimumElements = 1;
  hooks.grain = 2;
  auto& cache = registry.ctx().emplace<MatchedPropertiesCache>();
  const auto& metrics = registry.ctx().emplace<StyleCascadeMetrics>();
  tasks::TaskScheduler scheduler(options);
  StyleSystem styleSystem;
  std::vector<EntityHandle> restyle;
  for (int i = 0; i < 12; ++i) {
    restyle.push_back(document.querySelector("#a" + std::to_string(i))->entityHandle());
  }
  restyle.push_back(document.querySelector("#first-warning")->entityHandle());
  restyle.push_back(document.querySelector("#second-warning")->entityHandle());

  std::array<EvictionPass, 3> passes;
  for (auto& pass : passes) {
    for (EntityHandle handle : restyle) {
      styleSystem.invalidateComputed(handle);
    }
    const auto work = metrics.counters;
    const auto epoch = cache.evictionEpoch();
    const auto scheduled = metrics.scheduledPasses;
    const auto abandoned = metrics.abandonedStylePasses;
    ParseWarningSink warnings;
    styleSystem.computeAllStyles(registry, warnings, StyleSystem::FullRestyle::ComputeMissing,
                                 &scheduler);
    pass = {metrics.counters - work,
            warnings.warnings(),
            warnings.warningAttempts(),
            budget.complexPropertyBytes(),
            cache.evictionEpoch() - epoch,
            metrics.scheduledPasses - scheduled,
            metrics.abandonedStylePasses - abandoned,
            budget.rejected()};
    EXPECT_EQ(family->stylePreparationBytes(), 0u);
  }

  const std::size_t siblingBytes = family->availableBytes(Kind::ComputedStyle);
  EXPECT_GT(siblingBytes, 0u);
  const bool siblingAdmitted = family->reserve(Kind::ComputedStyle, siblingBytes);
  const bool followingAdmitted = family->reserve(Kind::ComputedStyle, 1);
  const auto refusals = family->securityStats().rejectedReservations;
  Frame frame{std::move(document),        metrics.counters,
              metrics.scheduledPasses,    metrics.replayedSelectorTraces,
              metrics.admittedStyleTasks, metrics.declinedStyleTasks,
              passes.back().warnings};
  return {std::move(frame), std::move(passes), siblingAdmitted, followingAdmitted, refusals};
}

TEST(StyleParallel, EvictionPreservesWarmRestylesAndTheNextFamilyAdmission) {
  const std::string source = EvictionScene();
  const auto roomy = RunEvictionPasses(source, 1024 * 1024, {.workerCount = 0});
  const auto peak = std::max({roomy.passes[0].bytes, roomy.passes[1].bytes, roomy.passes[2].bytes});
  ASSERT_GT(peak, 0u);
  const auto limit = peak + 4 * sizeof(Lengthd);
  const auto serial = RunEvictionPasses(source, limit, {.workerCount = 0});
  EXPECT_GT(serial.passes[0].evictions, 0u);
  for (const auto& pass : serial.passes) {
    EXPECT_GT(pass.warningAttempts, 1u);
    EXPECT_FALSE(pass.rejected);
  }
  EXPECT_TRUE(serial.siblingAdmitted);
  EXPECT_FALSE(serial.followingAdmitted);
  EXPECT_EQ(serial.refusals, 1u);

  for (const auto& options :
       {tasks::TaskSchedulerOptions{.workerCount = 2}, {.workerCount = 0, .seed = 1717}}) {
    const auto scheduled = RunEvictionPasses(source, limit, options);
    EXPECT_GT(scheduled.frame.traces, 0u);
    EXPECT_EQ(scheduled.siblingAdmitted, serial.siblingAdmitted);
    EXPECT_EQ(scheduled.followingAdmitted, serial.followingAdmitted);
    EXPECT_EQ(scheduled.refusals, serial.refusals);
    for (std::size_t i = 0; i < serial.passes.size(); ++i) {
      SCOPED_TRACE(i);
      const auto& expected = serial.passes[i];
      const auto& actual = scheduled.passes[i];
      EXPECT_EQ(actual.scheduled, 1u);
      EXPECT_EQ(actual.abandoned, 0u);
      EXPECT_TRUE(actual.work.sameLogicalWork(expected.work));
      EXPECT_EQ(actual.warnings, expected.warnings);
      EXPECT_EQ(actual.warningAttempts, expected.warningAttempts);
      EXPECT_EQ(actual.bytes, expected.bytes);
      EXPECT_EQ(actual.evictions, expected.evictions);
      EXPECT_EQ(actual.rejected, expected.rejected);
    }
    ExpectSame(serial.frame, scheduled.frame);
  }
}

TEST(StyleParallel, AncestorFilterChargesFollowTheOwnerVisitOrder) {
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg">
    <style>.missing .leaf { fill: green } .present .leaf { stroke: blue }</style>)";
  for (int group = 0; group < 20; ++group) {
    source += "<g class='present";
    if (group % 2 == 0) {
      for (int key = 0; key < 70; ++key) {
        source += " c" + std::to_string(key);
      }
    }
    source += "'>";
    for (int element = 0; element < 60; ++element) {
      source += "<rect class='leaf'/>";
    }
    source += "</g>";
  }
  source += "</svg>";

  for (bool useFilter : {false, true}) {
    const auto serial = FirstFrame(source, {.workerCount = 0}, useFilter);
    EXPECT_EQ(serial.counters.ancestorFilterRejections != 0, useFilter);
    EXPECT_EQ(serial.counters.ancestorFilterKeyLimits != 0, useFilter);
    for (const tasks::TaskSchedulerOptions& options :
         {tasks::TaskSchedulerOptions{.workerCount = 1},
          {.workerCount = 2},
          {.workerCount = 4},
          {.workerCount = 8},
          {.workerCount = 0, .seed = 7123}}) {
      const auto result = FirstFrame(source, options, useFilter);
      EXPECT_EQ(result.scheduled, 1u);
      EXPECT_GT(result.traces, 1000u);
      ExpectSame(serial, result);
    }
  }
}

TEST(StyleParallel, SelectivePassCreatesMissingAncestorsInSerialStorageOrder) {
  const auto source = SiblingScene(1200);
  const auto run = [&](std::size_t width) {
    Frame frame = FirstFrame(source, {.workerCount = 0});
    Registry& registry = frame.doc.registry();
    const auto root = frame.doc.svgElement().entityHandle().entity();
    const auto rootBefore = registry.get<ComputedStyleComponent>(root).style;
    const auto parent = frame.doc.querySelector("#parent")->entityHandle().entity();
    registry.remove<ComputedStyleComponent>(parent);
    registry.clear<DirtyFlagsComponent>();
    const auto& group = registry.get<donner::components::TreeComponent>(parent);
    for (Entity child = group.firstChild(); child != entt::null;
         child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
      registry.remove<ComputedStyleComponent>(child);
      registry.get_or_emplace<DirtyFlagsComponent>(child).mark(DirtyFlagsComponent::Style);
    }
    auto& state = registry.ctx().emplace<RenderTreeState>();
    state.hasBeenBuilt = true;
    state.needsFullStyleRecompute = false;
    ParseWarningSink warnings;
    tasks::TaskScheduler scheduler({.workerCount = width});
    StyleSystem().computeAllStyles(registry, warnings,
                                   StyleSystem::FullRestyle::DiscardAndRecompute, &scheduler);
    const auto& metrics = registry.ctx().get<StyleCascadeMetrics>();
    EXPECT_EQ(registry.get<ComputedStyleComponent>(root).style, rootBefore);
    frame.counters = metrics.counters;
    frame.scheduled = metrics.scheduledPasses;
    frame.traces = metrics.replayedSelectorTraces;
    frame.warnings = warnings.warnings();
    return frame;
  };
  const Frame serial = run(0);
  const Frame parallel = run(4);
  EXPECT_EQ(parallel.scheduled, 1u);
  EXPECT_GT(parallel.traces, 1000u);
  ExpectSame(serial, parallel);
  auto storageOrder = [](const Frame& frame) {
    const auto slots = frame.doc.registry().view<const ComputedStyleComponent>();
    return std::vector<Entity>(slots.begin(), slots.end());
  };
  EXPECT_EQ(storageOrder(serial), storageOrder(parallel));
}

TEST(StyleParallel, OffscreenChildrenFollowMainAndDetachedNodesReachOwnerCommit) {
  auto source = SiblingScene(1050);
  source.insert(source.rfind("</svg>"),
                "<g id='outside'><g id='nested'><rect id='leaf'/></g></g>"
                "<g id='detached'><rect id='unreachable'/></g>");
  ParseWarningSink warnings;
  auto parse = parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_FALSE(parse.hasError());
  auto doc = std::move(parse).result();
  auto& registry = doc.registry();
  const auto root = doc.svgElement().entityHandle().entity();
  const auto outside = doc.querySelector("#outside")->entityHandle().entity();
  const auto leaf = doc.querySelector("#leaf")->entityHandle().entity();
  const auto last = doc.querySelector("#r1049")->entityHandle().entity();
  const auto unreachable = doc.querySelector("#unreachable")->entityHandle().entity();
  const auto detached = doc.querySelector("#detached")->entityHandle().entity();
  registry.get<donner::components::TreeComponent>(detached).remove(registry);
  registry.get_or_emplace<ComputedShadowTreeComponent>(root).branches.push_back(
      {.branchType = ShadowBranchType::OffscreenMask,
       .lightTarget = outside,
       .shadowEntities = {outside}});
  bool startedOffscreen = false;
  bool committing = false;
  auto& hooks = registry.ctx().emplace<StylePassPhasesForTesting>();
  hooks.beforeOffscreen = [&] {
    startedOffscreen = true;
    EXPECT_TRUE(registry.get<ComputedStyleComponent>(last).style);
    EXPECT_FALSE(registry.get<ComputedStyleComponent>(outside).style);
    EXPECT_FALSE(registry.get<ComputedStyleComponent>(leaf).style);
  };
  hooks.beforeCommit = [&] {
    committing = true;
    EXPECT_TRUE(startedOffscreen);
    EXPECT_TRUE(registry.get<ComputedStyleComponent>(leaf).style);
    EXPECT_FALSE(registry.get<ComputedStyleComponent>(unreachable).style);
  };
  tasks::TaskSchedulerOptions options;
  options.seed = 9;
  // The main task still owns the only record when it offers the offscreen phase.
  options.maxTasksPerCreator = 1;
  tasks::TaskScheduler scheduler(options);
  StyleSystem().computeAllStyles(registry, warnings, StyleSystem::FullRestyle::DiscardAndRecompute,
                                 &scheduler);
  const auto& metrics = registry.ctx().get<StyleCascadeMetrics>();
  EXPECT_EQ(metrics.admittedStyleTasks, 1u);
  EXPECT_GT(metrics.declinedStyleTasks, 0u);
  EXPECT_TRUE(startedOffscreen);
  EXPECT_TRUE(committing);
  EXPECT_TRUE(registry.get<ComputedStyleComponent>(unreachable).style);
}

TEST(StyleParallel, IneligibleDocumentsFallBackBeforeEnteringTheScope) {
  for (bool external : {false, true}) {
    ParseWarningSink warnings;
    auto parsed = parser::SVGParser::ParseSVG(SiblingScene(1050), warnings);
    ASSERT_FALSE(parsed.hasError());
    auto doc = std::move(parsed).result();
    auto& registry = doc.registry();
    if (external) {
      registry.ctx().emplace<ExternalUseStyleContext>(
          entt::null, std::make_shared<const ComputedStyle>(), ResolvedPaintServer(),
          ResolvedPaintServer(), nullptr);
    } else {
      std::ignore = registry.emplace<AnimatedValuesComponent>(registry.create());
    }
    tasks::TaskScheduler scheduler({.workerCount = 2});
    bool entered = false;
    registry.ctx().emplace<StylePassPhasesForTesting>().beforeRegion = [&] { entered = true; };
    StyleSystem().computeAllStyles(registry, warnings,
                                   StyleSystem::FullRestyle::DiscardAndRecompute, &scheduler);
    EXPECT_FALSE(entered);
    EXPECT_EQ(registry.ctx().get<StyleCascadeMetrics>().scheduledPasses, 0u);
    EXPECT_TRUE(doc.querySelector("#r1049")->entityHandle().get<ComputedStyleComponent>().style);
  }
}

TEST(StyleParallel, LatchedWorkRefusalDeclinesDispatchWithoutChangingPools) {
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(SiblingScene(1050), warnings);
  ASSERT_FALSE(parsed.hasError());
  auto doc = std::move(parsed).result();
  auto& registry = doc.registry();
  StyleResourceBudget::Limits limits;
  limits.ruleElementMatches = 0;
  auto& budget = registry.ctx().emplace<StyleResourceBudget>(limits);
  ASSERT_FALSE(budget.reserveRuleElementMatch());
  tasks::TaskScheduler scheduler({.workerCount = 2});
  EXPECT_FALSE(TryParallelStylePass(registry, warnings, &scheduler, StylePassSelection::Missing));
  EXPECT_TRUE(budget.rejected());
  EXPECT_TRUE(std::as_const(registry).view<const ComputedStyleComponent>().empty());
}

TEST(StyleParallel, LatchedFamilyRefusalDeclinesBeforeDispatch) {
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(SiblingScene(1050), warnings);
  ASSERT_FALSE(parsed.hasError());
  auto doc = std::move(parsed).result();
  auto& registry = doc.registry();
  DocumentResourceFamilyBudget::Limits limits;
  limits.computedStyleBytes = 0;
  auto family = std::make_shared<DocumentResourceFamilyBudget>(limits);
  auto& logical = registry.ctx().emplace<StyleResourceBudget>(family);
  ASSERT_FALSE(family->reserve(DocumentResourceFamilyBudget::Kind::ComputedStyle, 1));
  ASSERT_FALSE(logical.rejected());
  tasks::TaskScheduler scheduler({.workerCount = 2});
  EXPECT_FALSE(TryParallelStylePass(registry, warnings, &scheduler, StylePassSelection::Missing));
  EXPECT_EQ(family->securityStats().rejectedReservations, 1u);
  EXPECT_EQ(family->stylePreparationBytes(), 0u);
  EXPECT_TRUE(std::as_const(registry).view<const ComputedStyleComponent>().empty());
}

TEST(StyleParallel, FirstReservationRefusalReplaysSeriallyWithAnUnpublishedRoot) {
  const auto source = SiblingScene(1050);
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_FALSE(parsed.hasError());
  auto doc = std::move(parsed).result();
  auto& registry = doc.registry();
  DocumentResourceFamilyBudget::Limits limits;
  limits.stylePreparationBytes = 0;
  auto family = std::make_shared<DocumentResourceFamilyBudget>(limits);
  auto& logical = registry.ctx().emplace<StyleResourceBudget>(family);
  tasks::TaskScheduler scheduler({.workerCount = 2});
  StyleSystem().computeAllStyles(registry, warnings, StyleSystem::FullRestyle::DiscardAndRecompute,
                                 &scheduler);
  const auto& metrics = registry.ctx().get<StyleCascadeMetrics>();
  EXPECT_FALSE(logical.rejected());
  EXPECT_EQ(family->securityStats().rejectedReservations, 0u);
  EXPECT_EQ(family->stylePreparationBytes(), 0u);
  EXPECT_EQ(metrics.scheduledPasses, 0u);
  EXPECT_GT(metrics.abandonedStylePasses, 0u);
  const Frame actual{std::move(doc),
                     metrics.counters,
                     metrics.scheduledPasses,
                     metrics.replayedSelectorTraces,
                     metrics.admittedStyleTasks,
                     metrics.declinedStyleTasks,
                     warnings.warnings()};
  ExpectSame(FirstFrame(source, {.workerCount = 0}), actual);
}

TEST(StyleParallel, UseShadowTreesStayStableAcrossTheWorkerRegion) {
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256">
    <style>symbol > rect { fill: currentColor }</style><defs><symbol id="tile">)";
  for (int i = 0; i < 16; ++i) {
    source += "<rect width='1' height='1'/>";
  }
  source += "</symbol></defs><g color='red'>";
  for (int i = 0; i < 96; ++i) {
    source += "<use href='#tile'/>";
  }
  source += "</g></svg>";
  const auto run = [&](std::size_t width) {
    ParseWarningSink warnings;
    auto parsed = parser::SVGParser::ParseSVG(source, warnings);
    EXPECT_FALSE(parsed.hasError());
    auto doc = std::move(parsed).result();
    auto& registry = doc.registry();
    using Counts = std::vector<std::pair<entt::id_type, std::size_t>>;
    const auto count = [&] {
      Counts counts;
      for (auto [id, pool] : std::as_const(registry).storage()) {
        counts.emplace_back(id, pool.size());
      }
      const auto* entities = std::as_const(registry).storage<Entity>();
      return std::tuple{entities->size(), entities->free_list(), counts};
    };
    auto before = count();
    std::size_t verified = 0;
    auto& hooks = registry.ctx().emplace<StylePassPhasesForTesting>();
    hooks.beforeRegion = [&] { before = count(); };
    hooks.beforeCommit = [&] {
      EXPECT_EQ(before, count());
      ++verified;
    };
    tasks::TaskScheduler scheduler({.workerCount = width});
    RendererUtils::prepareDocumentForRendering(doc, false, warnings, &scheduler);
    EXPECT_GT(std::as_const(registry).view<const ShadowEntityComponent>().size(), 1000u);
    if (width > 0) {
      EXPECT_GT(verified, 0u);
    }
    registry.ctx().erase<StylePassPhasesForTesting>();
    const auto& metrics = registry.ctx().get<StyleCascadeMetrics>();
    return Frame{std::move(doc),
                 metrics.counters,
                 metrics.scheduledPasses,
                 metrics.replayedSelectorTraces,
                 metrics.admittedStyleTasks,
                 metrics.declinedStyleTasks,
                 warnings.warnings()};
  };
  const auto serial = run(0);
  const auto parallel = run(4);
  EXPECT_GT(parallel.admitted, 1u);
  EXPECT_GT(parallel.traces, 1000u);
  ExpectSame(serial, parallel);
}

TEST(StyleParallel, DelayedParentPublishesBeforeChildAdmission) {
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(SiblingScene(1050), warnings);
  ASSERT_FALSE(parsed.hasError());
  auto doc = std::move(parsed).result();
  auto& registry = doc.registry();
  const Entity parent = doc.querySelector("#parent")->entityHandle().entity();
  std::mutex mutex;
  std::condition_variable changed;
  bool waiting = false, release = false, premature = false;
  std::atomic<bool> published = false;
  std::atomic<bool> admitted = false;
  bool workerTimedOut = false;
  auto& hooks = registry.ctx().emplace<StylePassPhasesForTesting>();
  hooks.beforeNode = [&](Entity entity) {
    if (entity == parent) {
      std::unique_lock lock(mutex);
      waiting = true;
      changed.notify_all();
      workerTimedOut = !changed.wait_for(lock, donner::tests::kHangGuard, [&] { return release; });
    }
  };
  hooks.published = [&](Entity entity) {
    if (entity == parent) {
      published.store(true, std::memory_order_release);
    }
  };
  hooks.allowChildAdmission = [&](Entity entity) {
    if (entity != parent) {
      return true;
    }
    if (published.load(std::memory_order_acquire)) {
      admitted.store(true, std::memory_order_release);
      return true;
    }
    std::lock_guard lock(mutex);
    premature = true;
    changed.notify_all();
    return false;
  };
  tasks::TaskScheduler scheduler({.workerCount = 4});
  doc.setThreadingMode(ThreadingMode::ConcurrentDom);
  std::thread owner([&] {
    auto access = doc.writeAccess();
    StyleSystem().computeAllStyles(access.registry(), warnings,
                                   StyleSystem::FullRestyle::DiscardAndRecompute, &scheduler);
  });
  bool observed = false;
  {
    std::unique_lock lock(mutex);
    observed =
        changed.wait_for(lock, donner::tests::kHangGuard, [&] { return waiting || premature; });
    EXPECT_FALSE(admitted.load(std::memory_order_acquire));
    release = true;
    changed.notify_all();
  }
  owner.join();
  EXPECT_TRUE(observed && waiting);
  EXPECT_FALSE(workerTimedOut);
  EXPECT_FALSE(premature);
  EXPECT_TRUE(admitted.load(std::memory_order_acquire));
}

TEST(StyleParallel, OptionalAncestorFilterIsExclusiveToEachExecutingContext) {
  struct Counts {
    std::atomic<unsigned> created{0}, used{0}, rejected{0}, wrongThread{0};
  } counts;
  class Filter final : public CascadeAncestorFilter {
  public:
    Filter(Counts& counts, const RuleIndex* author)
        : counts_(counts), author_(author), owner_(std::this_thread::get_id()) {
      ++counts_.created;
    }
    bool rejects(Entity, const RuleIndex& index, std::uint32_t position) override {
      if (!used_) {
        ++counts_.used;
        used_ = true;
      }
      if (owner_ != std::this_thread::get_id()) {
        ++counts_.wrongThread;
      }
      if (&index == author_ && position == 0) {
        ++counts_.rejected;
        return true;
      }
      return false;
    }

  private:
    Counts& counts_;
    const RuleIndex* author_;
    const std::thread::id owner_;
    bool used_ = false;
  };
  auto source = SiblingScene(1050);
  source.insert(source.find("<style>") + 7, ".missing .leaf { fill: purple } ");
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  ASSERT_FALSE(parsed.hasError());
  auto doc = std::move(parsed).result();
  auto& registry = doc.registry();
  const auto* author =
      &doc.querySelector("style")->entityHandle().get<StylesheetComponent>().ruleIndex;
  auto& provider = registry.ctx().emplace<CascadeAncestorFilters>();
  provider.maximumBytesPerWorker = sizeof(Filter);
  provider.create = [&](const Registry&, StyleCascadeCounters&) {
    return std::make_unique<Filter>(counts, author);
  };
  tasks::TaskScheduler scheduler({.workerCount = 2});
  StyleSystem().computeAllStyles(registry, warnings, StyleSystem::FullRestyle::DiscardAndRecompute,
                                 &scheduler);
  const auto& metrics = registry.ctx().get<StyleCascadeMetrics>();
  EXPECT_EQ(metrics.scheduledPasses, 1u);
  EXPECT_GE(counts.used.load(), 2u);
  EXPECT_EQ(counts.wrongThread.load(), 0u);
  EXPECT_GT(counts.rejected.load(), 0u);
  EXPECT_TRUE(
      metrics.counters.sameLogicalWork(FirstFrame(source, {.workerCount = 0}, false).counters));
}

TEST(StyleParallel, AncestorFilterScratchIsReservedBeforeConstruction) {
  ParseWarningSink parseWarnings;
  auto parsed = parser::SVGParser::ParseSVG(SiblingScene(4), parseWarnings);
  ASSERT_FALSE(parsed.hasError());
  auto doc = std::move(parsed).result();
  auto& registry = doc.registry();
  DocumentResourceFamilyBudget::Limits limits;
  limits.stylePreparationBytes = StyleSystem::PreparedBatch::workerReservationBytes();
  auto family = std::make_shared<DocumentResourceFamilyBudget>(limits);
  std::ignore = registry.ctx().emplace<StyleResourceBudget>(family);
  ParseWarningSink warnings;
  const auto parent = doc.querySelector("#parent")->entityHandle();
  StyleSystem().computeStyle(parent, warnings);
  const auto child = doc.querySelector("#r0")->entityHandle();
  std::ignore = child.get_or_emplace<ComputedStyleComponent>();
  unsigned factories = 0;
  auto& provider = registry.ctx().emplace<CascadeAncestorFilters>();
  provider.maximumBytesPerWorker = 1;
  provider.create = [&](const Registry&,
                        StyleCascadeCounters&) -> std::unique_ptr<CascadeAncestorFilter> {
    ++factories;
    return nullptr;
  };
  const std::array requests{child.entity()};
  StyleSystem::PreparedBatch batch(registry, requests, 1);
  EXPECT_EQ(batch.bindWorker(), nullptr);
  EXPECT_EQ(factories, 0u);
  EXPECT_EQ(family->stylePreparationBytes(), 0u);
  EXPECT_EQ(family->securityStats().rejectedReservations, 0u);
  EXPECT_EQ(batch.commit(requests, warnings, false), 0u);
  EXPECT_EQ(factories, 1u);
  EXPECT_TRUE(child.get<ComputedStyleComponent>().style);
}

TEST(StyleParallel, InsufficientRangeHeadroomDiscardsPreparationAndReplaysSerially) {
  const auto source = SiblingScene(12);
  const auto serial = FirstFrame(source, {.workerCount = 0});
  const auto candidate =
      FirstFrame(source, {.workerCount = 2}, true, std::numeric_limits<std::size_t>::max());
  EXPECT_EQ(candidate.scheduled, 1u);
  EXPECT_EQ(candidate.doc.registry().ctx().get<StyleCascadeMetrics>().abandonedStylePasses, 1u);
  EXPECT_EQ(candidate.traces, 0u);
  ExpectSame(serial, candidate);
}

TEST(StyleParallel, FourNestedSelectorFunctionsStayEligibleForScheduling) {
  auto source = SiblingScene(1050);
  source.insert(source.find("</style>"), ".leaf:is(:is(:is(:is(.leaf)))) { fill: blue; }");
  const auto serial = FirstFrame(source, {.workerCount = 0});
  const auto candidate = FirstFrame(source, {.workerCount = 2, .workerStackBytes = 128 * 1024});
  EXPECT_EQ(candidate.scheduled, 1u);
  EXPECT_GT(candidate.traces, 0u);
  ExpectSame(serial, candidate);
}

TEST(StyleParallel, DeepSelectorAndDeclarationUseTheUnchangedSerialCascade) {
  for (const std::string_view extra : {
           ".leaf:is(:is(:is(:is(:is(.leaf))))) { fill: blue; }",
           ".leaf { fill: f(f(f(f(f(blue))))); }",
       }) {
    SCOPED_TRACE(extra);
    std::string source = SiblingScene(1050);
    source.insert(source.find("</style>"), extra);
    const auto serial = FirstFrame(source, {.workerCount = 0});
    const auto candidate = FirstFrame(source, {.workerCount = 2});
    EXPECT_EQ(candidate.scheduled, 0u);
    EXPECT_EQ(candidate.traces, 0u);
    ExpectSame(serial, candidate);
  }
}

TEST(StyleParallel, HeldLocalComponentValuesAreCheckedBeforeCopying) {
  for (const bool inlineStyle : {true, false}) {
    const std::string_view attributes = inlineStyle ? " style='stop-color:f(f(f(f(f(blue)))));'"
                                                    : " stop-color='f(f(f(f(f(blue)))))'";
    SCOPED_TRACE(attributes);
    auto source = SiblingScene(1050);
    const auto target = source.find("id='r0'");
    ASSERT_NE(target, std::string::npos);
    source.insert(target + std::string_view("id='r0'").size(), attributes);
    const auto serial = FirstFrame(source, {.workerCount = 0});
    const auto candidate = FirstFrame(source, {.workerCount = 2});
    EXPECT_EQ(candidate.scheduled, 1u);
    // The presentation attribute is parsed before the pass. The inline declaration still owns
    // its component tree when a speculative worker reaches it.
    const auto& metrics = candidate.doc.registry().ctx().get<StyleCascadeMetrics>();
    EXPECT_EQ(metrics.abandonedStylePasses, inlineStyle ? 1u : 0u);
    EXPECT_EQ(candidate.traces == 0u, inlineStyle);
    ExpectSame(serial, candidate);
  }
}

TEST(StyleParallel, SmallPassStaysSerial) {
  const auto source = SiblingScene(12);
  const auto frame = FirstFrame(source, {.workerCount = 4});
  EXPECT_EQ(frame.scheduled, 0u);
  EXPECT_EQ(frame.traces, 0u);
}

TEST(StyleParallel, SeededWorkQueuesWithZeroPhysicalWorkers) {
  const auto source = SiblingScene(1200);
  tasks::TaskSchedulerOptions options;
  options.workerCount = 0;
  options.seed = 7123;
  const auto first = FirstFrame(source, options);
  const auto repeat = FirstFrame(source, options);
  EXPECT_EQ(first.scheduled, 1u);
  EXPECT_GT(first.traces, 1000u);
  EXPECT_GT(first.admitted, 1u);
  EXPECT_EQ(first.admitted, repeat.admitted);
  // A repeated seed reproduces physical cache and registry-copy work as well.
  EXPECT_EQ(first.counters, repeat.counters);
  ExpectSame(FirstFrame(source, {.workerCount = 0}), first);
  ExpectSame(first, repeat);
}

TEST(StyleParallel, DeclinedSpawnsCompleteOnTheExplicitStack) {
  const auto source = SiblingScene(1200);
  tasks::TaskSchedulerOptions options;
  options.workerCount = 0;
  options.seed = 19;
  options.maxTasksPerCreator = 1;
  options.maxExternalTasks = 1;
  const auto result = FirstFrame(source, options);
  EXPECT_GT(result.declined, 0u);
  EXPECT_EQ(result.scheduled, 1u);
  EXPECT_GT(result.traces, 1000u);
  ExpectSame(FirstFrame(source, {.workerCount = 0}), result);
}

}  // namespace
}  // namespace donner::svg::components
