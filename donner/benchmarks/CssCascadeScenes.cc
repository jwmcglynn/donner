#include "donner/benchmarks/CssCascadeScenes.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <iterator>
#include <random>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/SVGStyleElement.h"
#include "donner/svg/internal/RenderingDiagnostics.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"

namespace donner::benchmarks {
namespace {

using Clock = std::chrono::steady_clock;
using svg::SVGDocument;
using svg::SVGElement;
using svg::components::StyleCascadeCounters;

double ElapsedMs(std::chrono::nanoseconds duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

/// Uniform value in `[0, bound)` by masked rejection sampling, which unlike
/// `std::uniform_int_distribution` gives the same sequence with every standard library.
int PickBelow(std::mt19937& engine, int bound) {
  if (bound <= 1) {
    return 0;
  }
  const auto range = static_cast<std::uint32_t>(bound);
  const std::uint32_t mask = (std::uint32_t{1} << std::bit_width(range - 1)) - 1;
  std::uint32_t value = 0;
  do {
    value = static_cast<std::uint32_t>(engine()) & mask;
  } while (value >= range);
  return static_cast<int>(value);
}

std::string SelectorFor(CascadeRuleKind kind, int rule) {
  static constexpr std::string_view kTypes[] = {"rect", "circle", "path",    "ellipse",
                                                "g",    "line",   "polygon", "polyline"};
  if (kind == CascadeRuleKind::Mixed) {
    const int slot = rule % 20;
    kind = slot < 14   ? CascadeRuleKind::Class
           : slot < 16 ? CascadeRuleKind::Type
           : slot < 19 ? CascadeRuleKind::Descendant
                       : CascadeRuleKind::Attribute;
  }
  const std::string index = std::to_string(rule);
  switch (kind) {
    case CascadeRuleKind::Class: return ".c" + index;
    case CascadeRuleKind::Descendant: return ".g" + std::to_string(rule % 97) + " .c" + index;
    case CascadeRuleKind::Type:
      return std::string(kTypes[rule % 8]) + (rule >= 8 ? ":nth-child(" + index + ")" : "");
    case CascadeRuleKind::Attribute: return "[data-k" + index + "]";
    case CascadeRuleKind::Mixed: break;
  }
  return {};
}

/// One source edit of a keystroke: replace \ref replacedLength bytes at \ref offset past the
/// first \ref needle that follows the keystroke's anchor.
struct KeystrokeStep {
  std::string_view needle;
  std::size_t offset = 0;
  std::size_t replacedLength = 0;
  std::string_view replacement;
};

/// A keystroke on the splash document and the edit that undoes it.
struct KeystrokeSpec {
  std::string_view anchor;
  KeystrokeStep forward;
  KeystrokeStep undo;
};

/// Indexed by \ref CascadeKeystroke.
constexpr KeystrokeSpec kKeystrokeSpecs[] = {
    {".cls-2 {", {"#fffff", 6, 1, "d"}, {"#fffff", 6, 1, "c"}},
    {".cls-2", {".cls-2", 6, 0, "x"}, {".cls-2", 6, 1, ""}},
    {".cls-2 {", {"}", 0, 1, ""}, {";", 1, 0, "}"}},
    {"class=\"cls-71", {"cls-71", 6, 0, "x"}, {"cls-71", 6, 1, ""}},
};
static_assert(static_cast<std::size_t>(CascadeKeystroke::ClassAttribute) + 1 ==
              std::size(kKeystrokeSpecs));

/// One repetition's timings and style work.
struct Repetition {
  double setupMs = 0;
  double frameMs = 0;
  double styleMs = 0;
  StyleCascadeCounters counters;
  std::size_t distinctComputedStyleRecords = 0;
};

/// Collects repetitions after the warmup and summarizes them.
class RepetitionLog {
public:
  explicit RepetitionLog(CascadeRepetitions repetitions, bool allowPhysicalVariation = false)
      : repetitions_(repetitions), allowPhysicalVariation_(allowPhysicalVariation) {}

  [[nodiscard]] int total() const { return repetitions_.warmup + repetitions_.measured; }

  void record(int repetition, const Repetition& sample) {
    if (!recorded(repetition)) {
      return;
    }
    const bool stableWork = allowPhysicalVariation_ ? sample.counters.sameLogicalWork(counters_)
                                                    : sample.counters == counters_;
    if (!setupMs_.empty() &&
        (!stableWork || sample.distinctComputedStyleRecords != distinctComputedStyleRecords_)) {
      stable_ = false;
    }
    counters_ = sample.counters;
    distinctComputedStyleRecords_ = sample.distinctComputedStyleRecords;
    setupMs_.push_back(sample.setupMs);
    frameMs_.push_back(sample.frameMs);
    styleMs_.push_back(sample.styleMs);
  }

  void recordRendererFrame(int repetition, double frameMs) {
    if (recorded(repetition)) {
      rendererFrameMs_.push_back(frameMs);
    }
  }

  [[nodiscard]] CascadeMeasurement summarize() const {
    CascadeMeasurement result;
    result.setup = SummarizeTimings(setupMs_);
    result.nullFrame = SummarizeTimings(frameMs_);
    result.style = SummarizeTimings(styleMs_);
    if (!rendererFrameMs_.empty()) {
      result.rendererFrame = SummarizeTimings(rendererFrameMs_);
    }
    result.counters = counters_;
    result.distinctComputedStyleRecords = distinctComputedStyleRecords_;
    result.countersStable = stable_;
    return result;
  }

private:
  [[nodiscard]] bool recorded(int repetition) const {
    if (repetition < repetitions_.warmup) {
      return false;
    }
    switch (repetitions_.recorded) {
      case RecordedRepetitions::All: return true;
      case RecordedRepetitions::Even: return repetition % 2 == 0;
      case RecordedRepetitions::Odd: return repetition % 2 != 0;
    }
    return false;
  }

  CascadeRepetitions repetitions_;
  bool allowPhysicalVariation_;
  std::vector<double> setupMs_;
  std::vector<double> frameMs_;
  std::vector<double> styleMs_;
  std::vector<double> rendererFrameMs_;
  StyleCascadeCounters counters_;
  std::size_t distinctComputedStyleRecords_ = 0;
  bool stable_ = true;
};

/// Whether every count of \p after is at least the same count of \p before.
bool CountersNotBefore(const StyleCascadeCounters& after, const StyleCascadeCounters& before) {
  return after.elementsCascaded >= before.elementsCascaded &&
         after.rulesTested >= before.rulesTested && after.rulesMatched >= before.rulesMatched &&
         after.declarationsApplied >= before.declarationsApplied &&
         after.fullClears >= before.fullClears && after.budgetRejections >= before.budgetRejections;
}

/// Time \p step and the frame drawn after it, and the style work done during both.
Repetition TimeStepAndFrame(SVGDocument& document, svg::RendererInterface& renderer,
                            const std::function<void()>& step) {
  const StyleCascadeSample before = CaptureStyleCascade(document);
  const auto stepStart = Clock::now();
  step();
  const auto frameStart = Clock::now();
  renderer.draw(document);
  const auto frameEnd = Clock::now();
  const StyleCascadeSample after = CaptureStyleCascade(document);
  // The counts only grow while the document keeps its registry; anything else is a harness defect.
  UTILS_RELEASE_ASSERT_MSG(
      CountersNotBefore(after.counters, before.counters) && after.styleTime >= before.styleTime,
      "Style cascade counters went backwards between two captures");
  return Repetition{
      .setupMs = ElapsedMs(frameStart - stepStart),
      .frameMs = ElapsedMs(frameEnd - frameStart),
      .styleMs = ElapsedMs(after.styleTime - before.styleTime),
      .counters = after.counters - before.counters,
      .distinctComputedStyleRecords = after.distinctComputedStyleRecords,
  };
}

/// Builds the step applied to a loaded document before each frame, or nullopt without a target.
using MakeStep = std::function<std::optional<std::function<void(int)>>(SVGDocument&)>;

/// Measure the frame after a repeated step on a loaded document, one document per renderer.
std::optional<CascadeMeasurement> MeasureFramesAfterStep(std::string_view source,
                                                         const MakeStep& makeStep,
                                                         CascadeRepetitions repetitions,
                                                         bool withRenderer, bool disableCache) {
  RepetitionLog log(repetitions);
  for (const bool useRenderer : {false, true}) {
    if (useRenderer && !withRenderer) {
      break;
    }
    std::optional<SVGDocument> document = ParseCascadeDocument(source);
    if (!document) {
      return std::nullopt;
    }
    if (disableCache) {
      svg::internal::DisableMatchedPropertiesCacheForTesting(*document);
    }
    std::unique_ptr<svg::RendererInterface> renderer;
    if (useRenderer) {
      renderer = std::make_unique<svg::Renderer>();
    } else {
      renderer = std::make_unique<NullRenderer>();
    }
    renderer->draw(*document);
    std::optional<std::function<void(int)>> step = makeStep(*document);
    if (!step) {
      return std::nullopt;
    }
    for (int repetition = 0; repetition < log.total(); ++repetition) {
      const Repetition sample =
          TimeStepAndFrame(*document, *renderer, [&] { (*step)(repetition); });
      if (useRenderer) {
        log.recordRendererFrame(repetition, sample.frameMs);
      } else {
        log.record(repetition, sample);
      }
    }
  }
  return log.summarize();
}

}  // namespace

std::string MakeGroupedRectsDocument(int elementCount, int ruleCount, CascadeRuleKind kind) {
  std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="1000">)";
  svg += "\n";
  if (ruleCount > 0) {
    svg += "<style>\n";
    for (int rule = 0; rule < ruleCount; ++rule) {
      svg += SelectorFor(kind, rule) + " { fill: #" +
             std::to_string(100000 + (rule * 7919) % 800000) +
             "; stroke-width: " + std::to_string(rule % 5) + "; }\n";
    }
    svg += "</style>\n";
  }
  std::mt19937 engine(1234);
  const int groupCount = (elementCount + 10) / 11;
  int made = 0;
  for (int group = 0; group < groupCount && made < elementCount; ++group) {
    svg += "<g class=\"g" + std::to_string(group % 97) + "\">";
    ++made;
    for (int i = 0; i < 10 && made < elementCount; ++i, ++made) {
      svg += "<rect id=\"e" + std::to_string(made) + "\" class=\"c" +
             std::to_string(PickBelow(engine, ruleCount)) + "\" x=\"" +
             std::to_string((made * 13) % 990) + "\" y=\"" + std::to_string((made * 7) % 990) +
             "\" width=\"8\" height=\"8\"/>";
    }
    svg += "</g>";
  }
  svg += "</svg>\n";
  return svg;
}

std::string MakeSyntheticDocument() {
  return MakeGroupedRectsDocument(2000, 200, CascadeRuleKind::Mixed);
}

std::string MakeRepeatedClassTokensDocument(int ruleCount, int tokenCount) {
  std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg"><style>)";
  for (int rule = 0; rule < ruleCount; ++rule) {
    svg += ".a{fill:red}";
  }
  svg += R"(</style><rect class=")";
  for (int token = 0; token < tokenCount; ++token) {
    svg += "a ";
  }
  return svg + R"(" width="1" height="1"/></svg>)";
}

CascadeEditTarget SplashEditTarget() {
  return {".cls-71", "cls-71", "cls-88"};
}

CascadeEditTarget SyntheticEditTarget() {
  return {"#e1000", "c3", "c7"};
}

CascadeEditTarget BootstrapIconsEditTarget() {
  return {"symbol > path", "a", "b"};
}

std::string MakeUseInstancesDocument(int instanceCount, int symbolChildren, int ruleCount) {
  std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="1000" height="1000">)";
  svg += "\n<style>\n";
  for (int rule = 0; rule < ruleCount; ++rule) {
    svg +=
        ".c" + std::to_string(rule) + " { fill: #" + std::to_string(100000 + rule * 13) + "; }\n";
  }
  svg += "</style>\n<defs><symbol id=\"sym\">\n";
  for (int i = 0; i < symbolChildren; ++i) {
    svg += "  <rect class=\"c" + std::to_string(i % std::max(ruleCount, 1)) + "\" x=\"" +
           std::to_string(i) + "\" y=\"0\" width=\"4\" height=\"4\"/>\n";
  }
  svg += "</symbol></defs>\n";
  for (int i = 0; i < instanceCount; ++i) {
    svg += "<use href=\"#sym\" x=\"" + std::to_string((i * 11) % 990) + "\" y=\"" +
           std::to_string((i * 17) % 990) + "\"/>\n";
  }
  svg += "</svg>\n";
  return svg;
}

void NullRenderer::draw(svg::SVGDocument& document) {
  svg::RendererDriver(*this, false, nullptr, nullptr, nullptr, scheduler_).draw(document);
}

void NullRenderer::draw(svg::SVGDocument& document, ParseWarningSink& warningSink) {
  svg::RendererDriver(*this, /*verbose=*/false, nullptr, nullptr, &warningSink, scheduler_)
      .draw(document);
}

std::optional<SVGDocument> ParseCascadeDocument(std::string_view source) {
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto result = svg::parser::SVGParser::ParseSVG(source, warnings);
  if (result.hasError()) {
    return std::nullopt;
  }
  return std::move(result.result());
}

StyleCascadeSample CaptureStyleCascade(const SVGDocument& document) {
  const svg::internal::RenderingDiagnostics diagnostics =
      svg::internal::CaptureRenderingDiagnostics(document, 0);
  return StyleCascadeSample{diagnostics.styleCascade, diagnostics.styleTime,
                            diagnostics.distinctComputedStyleRecords};
}

std::optional<CascadeEditor> CascadeEditor::Create(SVGDocument& document,
                                                   const CascadeEditTarget& target,
                                                   CascadeEdit edit) {
  std::optional<SVGElement> element = document.querySelector(target.selector);
  if (!element) {
    return std::nullopt;
  }
  std::optional<SVGElement> style;
  std::string original;
  if (edit == CascadeEdit::AppendRule) {
    style = document.querySelector("style");
    if (!style) {
      return std::nullopt;
    }
    original = std::string(std::string_view(style->cast<svg::SVGStyleElement>().textContent()));
  }
  return CascadeEditor(*element, std::move(style), target, edit, std::move(original));
}

CascadeEditor::CascadeEditor(SVGElement element, std::optional<SVGElement> style,
                             CascadeEditTarget target, CascadeEdit edit,
                             std::string originalStylesheet)
    : element_(std::move(element)),
      style_(std::move(style)),
      target_(std::move(target)),
      edit_(edit),
      originalStylesheet_(std::move(originalStylesheet)) {}

void CascadeEditor::apply(int repetition) {
  const bool odd = repetition % 2 != 0;
  switch (edit_) {
    case CascadeEdit::Class:
      element_.setAttribute("class", odd ? target_.classA : target_.classB);
      break;
    case CascadeEdit::Fill: element_.setAttribute("fill", odd ? "#123456" : "#654321"); break;
    case CascadeEdit::Style:
      element_.setAttribute("style", odd ? "fill:#123456" : "fill:#654321");
      break;
    case CascadeEdit::AppendRule:
      style_->cast<svg::SVGStyleElement>().setContents(
          odd ? originalStylesheet_ + "\n.zz-added { fill: #00ff00; }\n" : originalStylesheet_);
      break;
  }
}

std::optional<xml::XMLEditIntent> MakeCascadeKeystroke(const SVGDocument& document,
                                                       CascadeKeystroke keystroke, int repetition,
                                                       std::string& replacementStorage) {
  const KeystrokeSpec& spec = kKeystrokeSpecs[static_cast<std::size_t>(keystroke)];
  const KeystrokeStep& step = repetition % 2 == 0 ? spec.forward : spec.undo;
  // Fixed literals of the splash fixture, located after a fixed anchor in its current source.
  const std::string_view source = document.source();
  const std::size_t anchorAt = source.find(spec.anchor);
  const std::size_t needleAt =
      anchorAt == std::string_view::npos ? anchorAt : source.find(step.needle, anchorAt);
  if (needleAt == std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t at = needleAt + step.offset;
  replacementStorage = std::string(step.replacement);
  return xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(at), FileOffset::Offset(at + step.replacedLength)},
      .replacement = replacementStorage,
      .sourceVersion = document.sourceVersion(),
  };
}

std::ostream& operator<<(std::ostream& os, const CascadeMeasurement& measurement) {
  return os << "{counters=" << measurement.counters
            << ", countersStable=" << (measurement.countersStable ? "true" : "false")
            << ", samples=" << measurement.nullFrame.samples << "}";
}

TimingSummary SummarizeTimings(std::vector<double> samplesMs) {
  if (samplesMs.empty()) {
    return {};
  }
  std::sort(samplesMs.begin(), samplesMs.end());
  return TimingSummary{
      .medianMs = samplesMs[samplesMs.size() / 2],
      .minMs = samplesMs.front(),
      .maxMs = samplesMs.back(),
      .samples = samplesMs.size(),
  };
}

std::optional<CascadeMeasurement> MeasureCascadeLoad(std::string_view source,
                                                     CascadeRepetitions repetitions,
                                                     bool withRenderer, bool disableCache,
                                                     tasks::TaskScheduler* scheduler) {
  CascadeMeasurement::ScheduledWork scheduled;
  RepetitionLog log(repetitions, scheduler != nullptr);
  for (int repetition = 0; repetition < log.total(); ++repetition) {
    const auto parseStart = Clock::now();
    std::optional<SVGDocument> document = ParseCascadeDocument(source);
    const auto parseEnd = Clock::now();
    if (!document) {
      return std::nullopt;
    }
    if (disableCache) {
      svg::internal::DisableMatchedPropertiesCacheForTesting(*document);
    }
    NullRenderer renderer(scheduler);
    Repetition sample = TimeStepAndFrame(*document, renderer, [] {});
    sample.setupMs = ElapsedMs(parseEnd - parseStart);
    log.record(repetition, sample);
    if (scheduler) {
      const auto access = document->readAccess();
      const auto& metrics = access.registry().ctx().get<svg::components::StyleCascadeMetrics>();
      scheduled = {metrics.scheduledPasses, metrics.admittedStyleTasks,
                   metrics.abandonedStylePasses, metrics.replayedSelectorTraces,
                   metrics.reusedCanonicalHits};
    }
    if (withRenderer) {
      std::optional<SVGDocument> rendered = ParseCascadeDocument(source);
      if (!rendered) {
        return std::nullopt;
      }
      if (disableCache) {
        svg::internal::DisableMatchedPropertiesCacheForTesting(*rendered);
      }
      svg::Renderer defaultRenderer;
      log.recordRendererFrame(repetition,
                              TimeStepAndFrame(*rendered, defaultRenderer, [] {}).frameMs);
    }
  }
  auto result = log.summarize();
  result.scheduled = scheduled;
  return result;
}

std::optional<CascadeMeasurement> MeasureCascadeEdit(std::string_view source,
                                                     const CascadeEditTarget& target,
                                                     CascadeEdit edit,
                                                     CascadeRepetitions repetitions,
                                                     bool withRenderer, bool disableCache) {
  return MeasureFramesAfterStep(
      source,
      [&](SVGDocument& document) -> std::optional<std::function<void(int)>> {
        std::optional<CascadeEditor> editor = CascadeEditor::Create(document, target, edit);
        if (!editor) {
          return std::nullopt;
        }
        return [editor = *editor](int repetition) mutable { editor.apply(repetition); };
      },
      repetitions, withRenderer, disableCache);
}

std::optional<CascadeMeasurement> MeasureCascadeKeystroke(std::string_view source,
                                                          CascadeKeystroke keystroke,
                                                          CascadeRepetitions repetitions,
                                                          bool withRenderer) {
  bool missingTarget = false;
  std::optional<CascadeMeasurement> result = MeasureFramesAfterStep(
      source,
      [&](SVGDocument& document) -> std::optional<std::function<void(int)>> {
        return [&document, keystroke, &missingTarget](int repetition) {
          std::string replacement;
          const std::optional<xml::XMLEditIntent> intent =
              MakeCascadeKeystroke(document, keystroke, repetition, replacement);
          if (!intent || !document.applySourceEdit(*intent).applied) {
            missingTarget = true;
          }
        };
      },
      repetitions, withRenderer, false);
  if (missingTarget) {
    return std::nullopt;
  }
  return result;
}

}  // namespace donner::benchmarks
