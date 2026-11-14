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
}  // namespace

struct BidiParagraph::State {
  std::vector<char32_t> codepoints;
  uint8_t base = 0;
  /// Every scalar takes the explicit base level, as for `unicode-bidi: bidi-override` on the
  /// paragraph's own block container, so SheenBidi never resolves the paragraph.
  bool override = false;
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
  /// Split it at script boundaries so each run shapes with its own script, for example so Arabic
  /// keeps joining inside a right-to-left override of mixed Latin and Arabic text.
  bool appendOverrideRuns(uint32_t start, uint32_t end, BidiResourceBudget& budget,
                          std::vector<Run>& result) const {
    ScriptLocator locator(SBScriptLocatorCreate(), SBScriptLocatorRelease);
    if (!locator) {
      budget.rejected = true;
      return false;
    }
    const SBCodepointSequence line{SBStringEncodingUTF32, codepoints.data() + start, end - start};
    SBScriptLocatorLoadCodepoints(locator.get(), &line);
    const size_t first = result.size();
    while (SBScriptLocatorMoveNext(locator.get())) {
      const SBScriptAgent* agent = SBScriptLocatorGetAgent(locator.get());
      if (agent->length == 0 || agent->offset + agent->length > end - start ||
          !budget.consume(budget.remaining.runs, 1)) {
        budget.rejected = true;
        return false;
      }
      result.push_back({static_cast<uint32_t>(start + agent->offset),
                        static_cast<uint32_t>(start + agent->offset + agent->length), base});
    }
    if (result.size() == first || result.back().end != end) {
      budget.rejected = true;
      return false;
    }
    // Script runs arrive in logical order; a right-to-left line shows them right to left.
    if (base % 2 != 0) {
      std::reverse(result.begin() + static_cast<std::ptrdiff_t>(first), result.end());
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
                                                    bool forceAnalysis, bool override) {
  if (base > 2 || (override && base == 2) ||
      !budget.consume(budget.remaining.codepoints, codepoints.size())) {
    budget.rejected = true;
    return std::nullopt;
  }
  std::transform(codepoints.begin(), codepoints.end(), codepoints.begin(), ValidScalar);
  auto state = std::make_unique<State>();
  state->codepoints = std::move(codepoints);
  state->base = base;
  state->override = override;
  if (!state->initialize(budget, forceAnalysis)) {
    return std::nullopt;
  }
  return BidiParagraph(std::move(state));
}

uint8_t BidiParagraph::levelAt(uint32_t index) const {
  return index < state_->levels.size() ? state_->levels[index] : 0;
}

uint8_t BidiParagraph::paragraphLevelAt(uint32_t index) const {
  if (state_->override) {
    return state_->base;
  }
  const auto found = state_->paragraphAfter(index);
  return found == state_->paragraphs.begin() ? 0 : SBParagraphGetBaseLevel(std::prev(found)->get());
}

std::optional<std::vector<BidiParagraph::Run>> BidiParagraph::visualRuns(
    uint32_t start, uint32_t end, BidiResourceBudget& budget) const {
  if (budget.rejected || start > end || end > state_->codepoints.size()) {
    budget.rejected = true;
    return std::nullopt;
  }
  std::vector<Run> result;
  if (start == end) {
    return result;
  }
  if (state_->override) {
    if (!state_->appendOverrideRuns(start, end, budget, result)) {
      return std::nullopt;
    }
    return result;
  }
  if (!state_->algorithm) {
    if (!budget.consume(budget.remaining.runs, 1)) {
      return std::nullopt;
    }
    // Without reordering every scalar shares level zero on the left-to-right fast path.
    result.push_back({start, end, 0});
    return result;
  }
  if (!state_->appendLines(start, end, budget, result)) {
    return std::nullopt;
  }
  return result;
}

}  // namespace donner::svg
