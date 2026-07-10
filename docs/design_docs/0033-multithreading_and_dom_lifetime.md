# Design: Multithreading and DOM Lifetime

**Status:** Implemented
**Author:** GPT-5
**Created:** 2026-05-16

> **This design shipped.** It is summarized here; the living documentation is in
> the developer docs linked below. The full original design doc (ownership model,
> six-month premortem, alternatives considered, performance budgets) lives in git
> history at [`6ca78e12^:docs/design_docs/0033-dom_lifetime_and_concurrency.md`](https://github.com/jwmcglynn/donner/blob/6ca78e12/docs/design_docs/0033-dom_lifetime_and_concurrency.md).

## What this design was

Donner's public DOM API is shaped like a dynamic SVG engine — `SVGElement`
values can be copied, held, removed, and re-inserted — but internally an element
was just an `entt` entity in a shared registry, and rendering and DOM mutation
both touched that same live registry. This design treated DOM lifetime and
thread-safety as one ownership problem and introduced:

- **Document-owned lifetime.** The document root owns attached nodes; public DOM
  handles retain detached nodes while user code can still reach them; detached
  subtrees are collected once unreachable. Removed nodes stay valid while held,
  matching browser semantics.
- **Scoped document access.** An access coordinator with read/write guards
  (`DocumentReadAccess` / `DocumentWriteAccess`) and an opt-in
  `ThreadingMode::ConcurrentDom`, so the public API is safe to call across
  threads — concurrent writes serialize into one document order; concurrent
  reads run together.
- **Immutable render snapshots.** Rendering runs from a snapshot that owns its
  replay payload, so mutations made while a backend draws appear on the next
  frame instead of racing the current one.
- **A single tree-mutation path** shared by parser, editor, scripting, and C++
  DOM callers.

It shipped via [PR #596](https://github.com/jwmcglynn/donner/pull/596) and its
follow-ups (#612, #615, …).

## Developer documentation it spawned

- [`docs/multithreading.md`](../multithreading.md) — running Donner across threads.
- [`docs/svg_dom_threading_and_lifetime.md`](../svg_dom_threading_and_lifetime.md) —
  the public SVG DOM threading & lifetime contract.
- [`docs/dom_element_lifetime.md`](../dom_element_lifetime.md) — element handle lifetime.
