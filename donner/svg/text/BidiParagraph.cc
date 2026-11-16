#include "donner/svg/text/BidiParagraph.h"

#include <SheenBidi/SBAlgorithm.h>
#include <SheenBidi/SBCodepoint.h>
#include <SheenBidi/SBLine.h>
#include <SheenBidi/SBParagraph.h>
#include <SheenBidi/SBScriptLocator.h>

#include <algorithm>
#include <array>
#include <iterator>

namespace donner::svg {
namespace {
using Algorithm = std::unique_ptr<const _SBAlgorithm, decltype(&SBAlgorithmRelease)>;
using Paragraph = std::unique_ptr<const _SBParagraph, decltype(&SBParagraphRelease)>;
using Line = std::unique_ptr<const _SBLine, decltype(&SBLineRelease)>;
using ScriptLocator = std::unique_ptr<_SBScriptLocator, decltype(&SBScriptLocatorRelease)>;

char32_t ValidScalar(char32_t cp) {
  return cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ? 0xfffd : cp;
}

bool ValidLineRun(const SBRun& run, size_t begin, size_t end) {
  return run.offset >= begin && run.offset <= end && run.length != 0 &&
         run.length <= end - run.offset && run.level <= 126;
}

bool AppendLineRuns(SBParagraphRef paragraph, uint32_t start, uint32_t end,
                    BidiResourceBudget& budget, std::vector<BidiParagraph::Run>& result) {
  const size_t pStart = SBParagraphGetOffset(paragraph);
  const size_t pEnd = pStart + SBParagraphGetLength(paragraph);
  const size_t begin = std::max<size_t>(start, pStart);
  const size_t limit = std::min<size_t>(end, pEnd);
  if (begin >= limit) {
    return true;
  }
  Line line(SBParagraphCreateLine(paragraph, begin, limit - begin), SBLineRelease);
  if (!line) {
    budget.rejected = true;
    return false;
  }
  const size_t count = SBLineGetRunCount(line.get());
  if (!budget.consume(budget.remaining.runs, count)) {
    return false;
  }
  const SBRun* runs = SBLineGetRunsPtr(line.get());
  for (size_t index = 0; index < count; ++index) {
    const auto& run = runs[index];
    if (!ValidLineRun(run, begin, limit)) {
      budget.rejected = true;
      return false;
    }
    result.push_back({static_cast<uint32_t>(run.offset),
                      static_cast<uint32_t>(run.offset + run.length), run.level});
  }
  return true;
}

/// Split [start, end) of \p scalars, all at \p level, at script boundaries in visual order, so
/// each run shapes with its own script, for example so Arabic keeps joining inside a
/// right-to-left override of mixed Latin and Arabic text.
bool AppendScriptRuns(const char32_t* scalars, uint32_t start, uint32_t end, uint8_t level,
                      BidiResourceBudget& budget, std::vector<BidiParagraph::Run>& result) {
  ScriptLocator locator(SBScriptLocatorCreate(), SBScriptLocatorRelease);
  if (!locator) {
    budget.rejected = true;
    return false;
  }
  const SBCodepointSequence line{SBStringEncodingUTF32, scalars + start, end - start};
  SBScriptLocatorLoadCodepoints(locator.get(), &line);
  const size_t first = result.size();
  while (SBScriptLocatorMoveNext(locator.get())) {
    const SBScriptAgent* agent = SBScriptLocatorGetAgent(locator.get());
    const uint32_t expected = result.size() == first ? start : result.back().end;
    if (agent->length == 0 || start + agent->offset != expected ||
        agent->offset + agent->length > end - start || !budget.consume(budget.remaining.runs, 1)) {
      budget.rejected = true;
      return false;
    }
    result.push_back({static_cast<uint32_t>(start + agent->offset),
                      static_cast<uint32_t>(start + agent->offset + agent->length), level});
  }
  if (result.size() == first || result.back().end != end) {
    budget.rejected = true;
    return false;
  }
  // Script runs arrive in logical order; a right-to-left run shows them right to left.
  if (level % 2 != 0) {
    std::reverse(result.begin() + static_cast<std::ptrdiff_t>(first), result.end());
  }
  return true;
}

/// Whether \p codepoint can open or close an embedding, override or isolate.
bool IsInjectableControl(char32_t codepoint) {
  return (codepoint >= bidi_control::kLre && codepoint <= bidi_control::kRlo) ||
         (codepoint >= bidi_control::kLri && codepoint <= bidi_control::kPdi);
}

/// Whether every control is an injectable control within \p scalarCount, in nondecreasing order.
bool ValidControls(std::span<const BidiParagraph::InjectedControl> controls, size_t scalarCount) {
  return std::all_of(controls.begin(), controls.end(),
                     [&](const auto& control) {
                       return control.before <= scalarCount &&
                              IsInjectableControl(control.codepoint);
                     }) &&
         std::is_sorted(controls.begin(), controls.end(),
                        [](const auto& a, const auto& b) { return a.before < b.before; });
}
}  // namespace

struct BidiParagraph::State {
  std::vector<char32_t> codepoints;
  uint8_t base = 0;
  /// Every scalar takes the explicit base level, as for `unicode-bidi: bidi-override` on the
  /// paragraph's own block container, so SheenBidi never resolves the paragraph.
  bool override = false;
  /// Analysis position of each scalar when controls were injected, in increasing order; empty
  /// when the analysis buffer is the scalars themselves.
  std::vector<uint32_t> scalarPositions;
  /// The scalars apart from injected controls, kept to split mixed-script override runs: a
  /// paragraph override that encloses injected controls resolves through an injected override
  /// control, whose uniform levels can mix scripts. Empty otherwise.
  std::vector<char32_t> overrideScalars;
  std::vector<uint8_t> levels;
  Algorithm algorithm{nullptr, SBAlgorithmRelease};
  std::vector<Paragraph> paragraphs;

  auto paragraphAfter(uint32_t index) const {
    return std::upper_bound(paragraphs.begin(), paragraphs.end(), index,
                            [](uint32_t value, const Paragraph& paragraph) {
                              return value < SBParagraphGetOffset(paragraph.get());
                            });
  }

  bool appendLines(uint32_t start, uint32_t end, BidiResourceBudget& budget,
                   std::vector<Run>& result) const {
    auto first = paragraphAfter(start);
    if (first != paragraphs.begin()) {
      --first;
    }
    for (auto it = first; it != paragraphs.end(); ++it) {
      if (SBParagraphGetOffset(it->get()) >= end) {
        break;
      }
      if (!AppendLineRuns(it->get(), start, end, budget, result)) {
        return false;
      }
    }
    return true;
  }

  /// An override keeps every scalar at the base level, but one level run can then mix scripts.
  bool appendOverrideRuns(uint32_t start, uint32_t end, BidiResourceBudget& budget,
                          std::vector<Run>& result) const {
    return AppendScriptRuns(codepoints.data(), start, end, base, budget, result);
  }

  /// Builds the analysis buffer with \p controls before their scalars. A paragraph override
  /// becomes a leading override control, so that nested controls still open their own levels.
  void buildInjectedBuffer(const std::vector<char32_t>& scalars,
                           std::span<const InjectedControl> controls, bool paragraphOverride) {
    codepoints.reserve(scalars.size() + controls.size() + 1);
    if (paragraphOverride) {
      codepoints.push_back(base % 2 != 0 ? bidi_control::kRlo : bidi_control::kLro);
      overrideScalars = scalars;
    }
    scalarPositions.reserve(scalars.size());
    auto control = controls.begin();
    for (uint32_t index = 0; index <= scalars.size(); ++index) {
      for (; control != controls.end() && control->before == index; ++control) {
        codepoints.push_back(control->codepoint);
      }
      if (index < scalars.size()) {
        scalarPositions.push_back(static_cast<uint32_t>(codepoints.size()));
        codepoints.push_back(scalars[index]);
      }
    }
  }

  /// Appends the visual runs of the nonempty scalar line [start, end).
  bool appendRuns(uint32_t start, uint32_t end, BidiResourceBudget& budget,
                  std::vector<Run>& result) const {
    if (!scalarPositions.empty()) {
      return appendInjectedLineRuns(start, end, budget, result);
    }
    if (override) {
      return appendOverrideRuns(start, end, budget, result);
    }
    if (!algorithm) {
      if (!budget.consume(budget.remaining.runs, 1)) {
        return false;
      }
      // Without reordering every scalar shares level zero on the left-to-right fast path.
      result.push_back({start, end, 0});
      return true;
    }
    return appendLines(start, end, budget, result);
  }

  uint32_t analysisPosition(uint32_t index) const {
    return scalarPositions.empty() ? index : scalarPositions[index];
  }

  /// Resolves the analysis-buffer line that holds scalars [start, end), including the controls
  /// injected before its first scalar, and maps its runs back to scalar ranges.
  bool appendInjectedLineRuns(uint32_t start, uint32_t end, BidiResourceBudget& budget,
                              std::vector<Run>& result) const {
    const uint32_t bufferStart = start == 0 ? 0 : scalarPositions[start - 1] + 1;
    const uint32_t bufferEnd = end == scalarPositions.size()
                                   ? static_cast<uint32_t>(codepoints.size())
                                   : scalarPositions[end];
    std::vector<Run> bufferRuns;
    if (!appendLines(bufferStart, bufferEnd, budget, bufferRuns)) {
      return false;
    }
    for (const Run& run : bufferRuns) {
      const auto scalarAt = [&](uint32_t position) {
        return static_cast<uint32_t>(
            std::lower_bound(scalarPositions.begin(), scalarPositions.end(), position) -
            scalarPositions.begin());
      };
      const uint32_t first = std::max(start, scalarAt(run.start));
      const uint32_t last = std::min(end, scalarAt(run.end));
      if (first >= last) {
        continue;  // A run of injected controls only.
      }
      if (!overrideScalars.empty()) {
        if (!AppendScriptRuns(overrideScalars.data(), first, last, run.level, budget, result)) {
          return false;
        }
      } else {
        result.push_back({first, last, run.level});
      }
    }
    return true;
  }

  std::optional<size_t> appendParagraph(size_t offset, BidiResourceBudget& budget) {
    if (offset != 0 && !budget.consume(budget.remaining.paragraphs, 1)) {
      return std::nullopt;
    }
    Paragraph paragraph(
        SBAlgorithmCreateParagraph(algorithm.get(), offset, codepoints.size() - offset,
                                   base == 2 ? SBLevelDefaultLTR : base),
        SBParagraphRelease);
    if (!paragraph) {
      budget.rejected = true;
      return std::nullopt;
    }
    const size_t length = SBParagraphGetLength(paragraph.get());
    if (SBParagraphGetOffset(paragraph.get()) != offset || length == 0 ||
        length > codepoints.size() - offset) {
      budget.rejected = true;
      return std::nullopt;
    }
    std::copy_n(SBParagraphGetLevelsPtr(paragraph.get()), length, levels.begin() + offset);
    paragraphs.push_back(std::move(paragraph));
    return offset + length;
  }

  bool initialize(BidiResourceBudget& budget, bool forceAnalysis) {
    levels.resize(codepoints.size(), override ? base : 0);
    const bool needsAnalysis =
        base != 0 || forceAnalysis ||
        std::any_of(codepoints.begin(), codepoints.end(), BidiParagraph::NeedsAnalysis);
    if (codepoints.empty()) {
      return true;
    }
    if (!budget.consume(budget.remaining.paragraphs, 1)) {
      return false;
    }
    if (!needsAnalysis || override) {
      return true;
    }
    const SBCodepointSequence source{SBStringEncodingUTF32, codepoints.data(), codepoints.size()};
    algorithm.reset(SBAlgorithmCreate(&source));
    if (!algorithm) {
      budget.rejected = true;
      return false;
    }
    for (size_t offset = 0; offset < codepoints.size();) {
      const auto next = appendParagraph(offset, budget);
      if (!next) {
        return false;
      }
      offset = *next;
    }
    return true;
  }
};

BidiParagraph::BidiParagraph(std::unique_ptr<State> state) : state_(std::move(state)) {}
BidiParagraph::~BidiParagraph() = default;
BidiParagraph::BidiParagraph(BidiParagraph&&) noexcept = default;
BidiParagraph& BidiParagraph::operator=(BidiParagraph&&) noexcept = default;

bool BidiParagraph::NeedsAnalysis(char32_t codepoint) {
  constexpr std::array<SBBidiType, 12> kAnalyzedTypes{
      SBBidiTypeR,   SBBidiTypeAL,  SBBidiTypeAN,  SBBidiTypeLRE, SBBidiTypeRLE, SBBidiTypeLRO,
      SBBidiTypeRLO, SBBidiTypePDF, SBBidiTypeLRI, SBBidiTypeRLI, SBBidiTypeFSI, SBBidiTypePDI};
  return std::find(kAnalyzedTypes.begin(), kAnalyzedTypes.end(),
                   SBCodepointGetBidiType(codepoint)) != kAnalyzedTypes.end();
}

bool BidiParagraph::IsNonspacing(char32_t cp) {
  return SBCodepointGetBidiType(cp) == SBBidiTypeNSM;
}

char32_t BidiParagraph::Mirror(char32_t cp) {
  const auto mirror = SBCodepointGetMirror(cp);
  return mirror ? mirror : cp;
}

std::optional<BidiParagraph> BidiParagraph::Analyze(std::vector<char32_t> codepoints,
                                                    BidiResourceBudget& budget, uint8_t base,
                                                    bool forceAnalysis, bool override,
                                                    std::span<const InjectedControl> controls) {
  if (base > 2 || (override && base == 2) || !ValidControls(controls, codepoints.size()) ||
      !budget.consume(budget.remaining.codepoints, codepoints.size()) ||
      !budget.consume(budget.remaining.codepoints, controls.size() + (override ? 1 : 0))) {
    budget.rejected = true;
    return std::nullopt;
  }
  std::transform(codepoints.begin(), codepoints.end(), codepoints.begin(), ValidScalar);
  auto state = std::make_unique<State>();
  state->base = base;
  if (controls.empty()) {
    state->codepoints = std::move(codepoints);
    state->override = override;
  } else {
    state->buildInjectedBuffer(codepoints, controls, override);
    forceAnalysis = true;
  }
  if (!state->initialize(budget, forceAnalysis)) {
    return std::nullopt;
  }
  return BidiParagraph(std::move(state));
}

uint8_t BidiParagraph::levelAt(uint32_t index) const {
  const uint32_t position = index < scalarCount() ? state_->analysisPosition(index) : index;
  return position < state_->levels.size() ? state_->levels[position] : 0;
}

uint8_t BidiParagraph::paragraphLevelAt(uint32_t index) const {
  if (state_->override) {
    return state_->base;
  }
  if (index < scalarCount()) {
    index = state_->analysisPosition(index);
  }
  const auto found = state_->paragraphAfter(index);
  return found == state_->paragraphs.begin() ? 0 : SBParagraphGetBaseLevel(std::prev(found)->get());
}

std::optional<std::vector<BidiParagraph::Run>> BidiParagraph::visualRuns(
    uint32_t start, uint32_t end, BidiResourceBudget& budget) const {
  if (budget.rejected || start > end || end > scalarCount()) {
    budget.rejected = true;
    return std::nullopt;
  }
  std::vector<Run> result;
  if (start != end && !state_->appendRuns(start, end, budget, result)) {
    return std::nullopt;
  }
  return result;
}

uint32_t BidiParagraph::scalarCount() const {
  return static_cast<uint32_t>(state_->scalarPositions.empty() ? state_->codepoints.size()
                                                               : state_->scalarPositions.size());
}

}  // namespace donner::svg
