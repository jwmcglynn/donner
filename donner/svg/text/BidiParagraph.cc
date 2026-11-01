#include "donner/svg/text/BidiParagraph.h"

#include <SheenBidi/SBAlgorithm.h>
#include <SheenBidi/SBCodepoint.h>
#include <SheenBidi/SBLine.h>
#include <SheenBidi/SBParagraph.h>

#include <algorithm>
#include <iterator>

namespace donner::svg {
namespace {
using Algorithm = std::unique_ptr<const _SBAlgorithm, decltype(&SBAlgorithmRelease)>;
using Paragraph = std::unique_ptr<const _SBParagraph, decltype(&SBParagraphRelease)>;
using Line = std::unique_ptr<const _SBLine, decltype(&SBLineRelease)>;
}  // namespace

struct BidiParagraph::State {
  std::vector<char32_t> codepoints;
  uint8_t base = 0;
  std::vector<uint8_t> levels;
  Algorithm algorithm{nullptr, SBAlgorithmRelease};
  std::vector<Paragraph> paragraphs;
};

BidiParagraph::BidiParagraph(std::unique_ptr<State> state) : state_(std::move(state)) {}
BidiParagraph::~BidiParagraph() = default;
BidiParagraph::BidiParagraph(BidiParagraph&&) noexcept = default;
BidiParagraph& BidiParagraph::operator=(BidiParagraph&&) noexcept = default;

bool BidiParagraph::NeedsAnalysis(char32_t codepoint) {
  const auto type = SBCodepointGetBidiType(codepoint);
  return type == SBBidiTypeR || type == SBBidiTypeAL || type == SBBidiTypeAN ||
         type == SBBidiTypeLRE || type == SBBidiTypeRLE || type == SBBidiTypeLRO ||
         type == SBBidiTypeRLO || type == SBBidiTypePDF || type == SBBidiTypeLRI ||
         type == SBBidiTypeRLI || type == SBBidiTypeFSI || type == SBBidiTypePDI ||
         type == SBBidiTypeB;
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
                                                    bool forceAnalysis) {
  if (base > 2 || !budget.consume(budget.remaining.codepoints, codepoints.size())) {
    budget.rejected = true;
    return std::nullopt;
  }
  for (auto& cp : codepoints) {
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
      cp = 0xfffd;
    }
  }
  auto state = std::make_unique<State>();
  state->codepoints = std::move(codepoints);
  state->base = base;
  state->levels.resize(state->codepoints.size(), 0);
  const bool needsAnalysis =
      base != 0 || forceAnalysis ||
      std::any_of(state->codepoints.begin(), state->codepoints.end(), NeedsAnalysis);
  if (!needsAnalysis || state->codepoints.empty()) {
    return BidiParagraph(std::move(state));
  }
  const SBCodepointSequence source{SBStringEncodingUTF32, state->codepoints.data(),
                                   state->codepoints.size()};
  state->algorithm.reset(SBAlgorithmCreate(&source));
  if (!state->algorithm) {
    budget.rejected = true;
    return std::nullopt;
  }
  size_t offset = 0;
  while (offset < state->codepoints.size()) {
    Paragraph paragraph(SBAlgorithmCreateParagraph(state->algorithm.get(), offset,
                                                   state->codepoints.size() - offset,
                                                   base == 2 ? SBLevelDefaultLTR : base),
                        SBParagraphRelease);
    if (!paragraph) {
      budget.rejected = true;
      return std::nullopt;
    }
    const size_t length = SBParagraphGetLength(paragraph.get());
    if (SBParagraphGetOffset(paragraph.get()) != offset || length == 0 ||
        length > state->codepoints.size() - offset ||
        !budget.consume(budget.remaining.paragraphs, 1)) {
      budget.rejected = true;
      return std::nullopt;
    }
    std::copy_n(SBParagraphGetLevelsPtr(paragraph.get()), length, state->levels.begin() + offset);
    offset += length;
    state->paragraphs.push_back(std::move(paragraph));
  }
  return BidiParagraph(std::move(state));
}

uint8_t BidiParagraph::levelAt(uint32_t index) const {
  return index < state_->levels.size() ? state_->levels[index] : 0;
}

uint8_t BidiParagraph::paragraphLevelAt(uint32_t index) const {
  const auto found = std::upper_bound(state_->paragraphs.begin(), state_->paragraphs.end(), index,
                                      [](uint32_t value, const Paragraph& paragraph) {
                                        return value < SBParagraphGetOffset(paragraph.get());
                                      });
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
  if (!state_->algorithm) {
    if (!budget.consume(budget.remaining.runs, 1)) {
      return std::nullopt;
    }
    result.push_back({start, end, 0});
    return result;
  }
  auto first = std::upper_bound(state_->paragraphs.begin(), state_->paragraphs.end(), start,
                                [](uint32_t value, const Paragraph& paragraph) {
                                  return value < SBParagraphGetOffset(paragraph.get());
                                });
  if (first != state_->paragraphs.begin()) {
    --first;
  }
  for (auto it = first; it != state_->paragraphs.end(); ++it) {
    const auto& paragraph = *it;
    const size_t pStart = SBParagraphGetOffset(paragraph.get());
    if (pStart >= end) {
      break;
    }
    const size_t pEnd = pStart + SBParagraphGetLength(paragraph.get());
    const size_t begin = std::max<size_t>(start, pStart);
    const size_t limit = std::min<size_t>(end, pEnd);
    if (begin >= limit) {
      continue;
    }
    Line line(SBParagraphCreateLine(paragraph.get(), begin, limit - begin), SBLineRelease);
    if (!line || !budget.consume(budget.remaining.runs, SBLineGetRunCount(line.get()))) {
      budget.rejected = true;
      return std::nullopt;
    }
    const SBRun* runs = SBLineGetRunsPtr(line.get());
    for (size_t index = 0; index < SBLineGetRunCount(line.get()); ++index) {
      const auto& run = runs[index];
      if (run.offset < begin || run.length == 0 || run.offset > limit ||
          run.length > limit - run.offset || run.level > 126) {
        budget.rejected = true;
        return std::nullopt;
      }
      result.push_back({static_cast<uint32_t>(run.offset),
                        static_cast<uint32_t>(run.offset + run.length), run.level});
    }
  }
  return result;
}

}  // namespace donner::svg
