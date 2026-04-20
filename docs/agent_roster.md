# Agent Roster {#AgentRoster}

\tableofcontents

Donner ships a roster of domain-expert **subagents** that live under
[`.claude/agents/`](https://github.com/jwmcglynn/donner/tree/main/.claude/agents). Each one is a Markdown file with a
YAML frontmatter block (`name`, `description`) followed by a prompt that gives
the agent its voice, its source-of-truth pointers, and its handoff rules.

When you work on Donner with an agent-capable tool (Claude Code, Codex, or any
client that understands the `.claude/agents/` convention), these files are
loaded automatically and made available as delegate targets. Instead of asking
one generic assistant every question, you can route a question to the bot that
owns the relevant area — BazelBot for a `BUILD.bazel` change, TextBot for a
HarfBuzz shaping bug, TinySkia Bot for a pixel diff in the default backend, and
so on.

This page is the map: what each bot owns, when to call it, and how they hand
work off to each other.

## Why a Roster? {#AgentRosterWhy}

A mid-size C++ project like Donner has enough distinct sub-disciplines (build,
rendering, parsing, text, CSS, security, spec conformance, perf, release) that
a single "generalist" agent tends to give shallow answers outside its comfort
zone. The roster solves that by:

- **Pinning source of truth.** Each bot's prompt lists the exact files it
  should grep before answering — no guessing about which flag lives where or
  which header defines a given macro.
- **Encoding handoff rules.** Each bot knows which questions aren't theirs and
  names the bot that *is* the right owner, so you (or another bot) can route
  quickly instead of speculating.
- **Giving each area a voice.** Review feedback from ReadabilityBot reads
  differently from DuckBot's big-picture brainstorming, which reads
  differently from SecurityBot's threat-modeling stance. The voices are
  deliberate — they cue you to what *kind* of feedback you're getting.

See [`AGENTS.md`](https://github.com/jwmcglynn/donner/blob/main/AGENTS.md) for the repo-wide coding and workflow rules
that every agent inherits.

## Quick Reference {#AgentRosterTable}

| Agent | Area | When to call |
|---|---|---|
| **BazelBot** | Bazel build system, custom rules, feature flags, presubmit, banned-patterns lint | Adding a target, debugging a `*_lint` failure, understanding a `--config=` flag, CMake-mirror questions |
| **CSSBot** | `donner::css` parser, selectors, cascade, `PropertyRegistry`, `StyleSystem` | Selector parsing bugs, specificity/inheritance questions, how presentation attributes interact with CSS in SVG2 |
| **DesignReviewBot** | Design docs under `docs/design_docs/` | Before a design moves from draft to implementing; periodic scope-drift checks during implementation |
| **DuckBot** | Big-picture brainstorming, Donner's innovation registry | You're stuck on *what* to build, not *how* — architectural reframes, "is there a cleverer way?" |
| **GeodeBot** | Geode GPU backend (WebGPU/Dawn, Slug, WGSL), `RendererGeode`, `--config=geode` | Geode architecture questions, `enable_dawn` gating, adding or editing shaders |
| **MiscBot** | Cross-cutting refactors, multi-PR initiatives | Planning a background project, breaking work into reviewable chunks; delegates to domain bots for depth |
| **ParserBot** | `donner::xml`, `donner::svg::parser`, `donner::css::parser`, fuzzer discipline, diagnostics | Parser bugs, fuzzer crashes, error-message quality, designing a new parser |
| **PerfBot** | Frame-budget discipline, profiling, allocation/hot-path analysis | Perf regressions, animation smoothness, "is this fast enough for 60/120fps?" |
| **ReadabilityBot** | Modern C++20 readability and safety review | Code review focused on idioms, modern C++, catching antipatterns, template discipline |
| **ReleaseBot** | Release checklist, versioning, `RELEASE_NOTES.md`, BCR publishing, build report | What's left before cutting a release, BCR publish flow, build-report issues |
| **SecurityBot** | Trust boundaries, input validation, fuzzing, resource limits, SVG-engine threat model | Security reviews, adversarial-input crash triage, DoS analysis, safe-input guarantees |
| **SpecBot** | SVG2 + dependent web standards (CSS, DOM, XML, Filter Effects, Unicode) | Edge-case spec questions, identifying UB, checking what browsers/resvg/librsvg/batik actually do |
| **TestBot** | GTest/GMock, diagnosable failures, custom matchers | Test-file reviews, "this failure message is useless" problems, promoting assertions into matchers |
| **TextBot** | Text rendering across the no-text default plus `--config=text` and `--config=text-full` | Any text bug, font matching, `@font-face`, WOFF2, shaping, cross-tier mismatches |
| **TinySkia Bot** | Vendored `tiny-skia-cpp` + `RendererTinySkia` (the default backend) | Pixel diffs in the default backend, SIMD parity, stroke/dash edge cases |

Each agent file in [`.claude/agents/`](https://github.com/jwmcglynn/donner/tree/main/.claude/agents) starts with the
same YAML frontmatter shape:

```markdown
---
name: BazelBot
description: Expert on Donner's Bazel build system — custom rules, feature
  flags, license/NOTICE pipeline, the CMake mirror, presubmit, and the
  banned-patterns lint. Use for questions about adding targets, debugging
  build failures, understanding config flags, or anything involving
  BUILD.bazel / MODULE.bazel / rules.bzl.
---
```

The `description` field is what's surfaced to the calling tool's router, so
it doubles as the "when to use me" hint.

## How Agents Are Invoked {#AgentRosterInvocation}

The exact invocation depends on the client:

- **Claude Code.** Agents under `.claude/agents/` are auto-discovered. A
  parent agent can delegate to one via its `Agent` tool by passing
  `subagent_type: "<AgentName>"` (for example `subagent_type: "BazelBot"`).
  You can also request one explicitly in conversation: "ask BazelBot why the
  `_lint` test is failing".
- **Codex.** Codex reads the same files and uses them as long-form system
  prompts when you hand off a task (for example, asking it to review a PR "as
  SecurityBot").
- **Any other client that follows the `.claude/agents/` convention.** The
  files are plain Markdown; nothing stops you from copy-pasting a bot's prompt
  into a different tool to get the same framing.

You don't need an agent-aware client to benefit from this roster, either. The
handoff map below is just as useful as a contributor cheat sheet for "who on
the team (or which docs) should I go read before touching area X".

## Source-of-Truth Discipline {#AgentRosterSoT}

The single most important thing each agent prompt does is name the files it
should read **first**, before speculating. For example, BazelBot's source of
truth includes:

- `MODULE.bazel` + `.bazelrc` for module deps and configs.
- `build_defs/rules.bzl` for the `donner_cc_library` / `_test` / `_binary`
  macros.
- `build_defs/check_banned_patterns.py` for the lint rules.
- `tools/presubmit.sh` for what CI actually runs.

If a bot ever gives you an answer that contradicts the code, your first move
is to re-point it at its source-of-truth list ("grep first, speculate never")
and ask again. A contradictory answer usually means the bot hallucinated
rather than that the file has moved.

## Handoff Map {#AgentRosterHandoff}

Agents cooperate. Every bot's prompt ends with a short list of questions it
does *not* own, each paired with the bot that does. A few representative
edges:

- **BazelBot → GeodeBot** for anything touching the `enable_dawn` flag.
- **BazelBot → ReleaseBot** for build-report layout and release-artifact
  builds.
- **TextBot → TinySkia Bot** when the bug is in glyph rasterization rather than shaping.
- **DuckBot → DesignReviewBot** when a big-picture proposal is ready to
  become a design doc.
- **DuckBot → PerfBot / SecurityBot** when a proposal needs a perf or
  safety sanity check before becoming real work.
- **Any bot → MiscBot** when a change turns out to be bigger than one PR and
  needs multi-PR sequencing.

The handoff rules are intentionally terse in each bot's file — they're meant
to be read quickly and routed on. If you find yourself routing a question
through three bots, that's usually a sign the work itself should be split.

## Reading a Bot's File {#AgentRosterAnatomy}

Every file in [`.claude/agents/`](https://github.com/jwmcglynn/donner/tree/main/.claude/agents) follows roughly the
same shape:

1. **Frontmatter** — `name` + `description`, used by the router.
2. **Opening identity line** — "You are BazelBot, the in-house expert on
   Donner's Bazel build system." This pins the persona for the LLM.
3. **Source of truth** — the explicit "grep these files before answering"
   list.
4. **Domain content** — tables of flags, descriptions of subsystems,
   conventions, worked examples. This is the bulk of the file.
5. **Common tasks** — "here's the five questions this bot gets asked most
   often, and here's the canonical answer shape."
6. **Handoff rules** — "don't answer questions about X; send them to YBot."
7. **Closing discipline notes** — "don't guess, don't paraphrase, always
   read the file."

If you want to see a representative example, open
[`.claude/agents/BazelBot.md`](https://github.com/jwmcglynn/donner/blob/main/.claude/agents/BazelBot.md). The
longer-form bots (TextBot, SecurityBot, PerfBot) follow the same structure but
with more worked examples.

## Tips for Using the Roster Effectively {#AgentRosterTips}

- **Route to one bot at a time, not all of them.** The whole point is focused
  expertise. Pick the single most relevant bot for the question; let it hand
  off if it needs to.
- **Read the bot's file once, so you know what to ask.** Each file is short
  enough (~100-200 lines) to skim. After one read you'll know whether a
  question is on-domain for that bot.
- **Trust the handoff rules.** If the bot says "that's the renderer bot's area", route
  there immediately rather than pushing the current bot outside its comfort
  zone.
- **Prefer the bot's source-of-truth files over the bot's paraphrase.** If
  you need to make a decision, open the file the bot points at — the bot is
  a fast index, not a replacement for reading code.
- **Use DuckBot for reframes, not execution.** DuckBot is the only bot whose
  job is explicitly *not* to write code; it's there to help you find the
  right question. Once you have it, hand off to the domain bot.

## Adding or Editing an Agent {#AgentRosterEditing}

If a new subsystem gets big enough to deserve its own bot — or an existing
bot is getting too broad — follow the pattern:

1. **Drop a new file under `.claude/agents/<Name>Bot.md`** with YAML
   frontmatter (`name`, `description`). The `description` must include both
   *what* the bot owns and a "use for …" clause.
2. **Pin source of truth.** Start the prompt with a "grep first, speculate
   never" section that lists the exact files the bot should read before
   answering. This is the single most load-bearing part of the prompt.
3. **Encode handoff rules.** End the prompt with a list of questions the bot
   should *not* answer, each pointing at the bot (or doc) that owns that
   question. Update neighboring bots' handoff rules to point at the new bot
   where appropriate.
4. **Match the house voice.** Each bot has its own tone; match the tone of a
   neighboring bot in the same discipline (for example, a new
   rendering-backend bot should read like TinySkia Bot or GeodeBot, not like
   DuckBot).
5. **Update this page.** Add a row to the quick-reference table and, if the
   bot is involved in any handoff you think contributors will hit, add it to
   the handoff map.

Removing or renaming a bot works in reverse: grep the other bot files for any
outgoing handoff to the old name and update them in the same PR.

See the repo-wide [`AGENTS.md`](https://github.com/jwmcglynn/donner/blob/main/AGENTS.md) for the coding, workflow,
and review conventions every bot inherits.
