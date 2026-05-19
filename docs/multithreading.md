# Multithreading {#Multithreading}

Donner's SVG DOM is backed by `DocumentState`, which owns the ECS registry,
mutation revision, mutation log, detached-node lifetime state, and the
reader/writer access coordinator.

`SVGDocument` defaults to `ThreadingMode::SingleThreaded`. That keeps the
existing low-overhead path for users that touch a document from one thread. In
this mode, access guards assert that the calling thread is the original owner
thread but do not acquire locks.

`ThreadingMode::ConcurrentDom` enables guarded multi-threaded access. Public DOM
facade methods acquire the access they need. Code that touches raw ECS state
through `SVGDocument::registry()` or `SVGElement::entityHandle()` must hold an
explicit access guard:

```cpp
document.withReadAccess([](DocumentReadAccess& access) {
  const Registry& registry = access.registry();
  // Read ECS state here.
});

element.withWriteAccess([](DocumentWriteAccess& access, EntityHandle handle) {
  handle.get_or_emplace<components::DirtyFlagsComponent>().mark(
      components::DirtyFlagsComponent::RenderInstance);
  access.bumpMutationRevision();
});
```

Raw ECS access is intentionally noisy in concurrent mode. Use
`unsafeRegistry()` or `unsafeEntityHandle()` only when the caller has a stronger
external guarantee and wants to bypass the guard assertion.

## Mutation Revisions

Public DOM mutators use `DocumentMutationBatch`, which acquires write access and
commits one mutation revision on scope exit. Nested mutators coalesce into the
outer batch, so one logical DOM operation records one revision even when it calls
several setters internally.

Plain `DocumentWriteAccess` does not bump the mutation revision by itself. It is
for raw ECS work and internal cache/computed-component updates. Raw user
mutations made through a write guard must call
`DocumentWriteAccess::bumpMutationRevision()`.

## Rendering

Async rendering treats the document as concurrent at the render-request
boundary. The worker acquires one write guard around compositor work and
snapshot capture. Backend drawing replays the captured render snapshot, so DOM
mutations made after snapshot capture take effect on a later frame.

## Validation

Useful focused checks:

```sh
bazel test --config=tsan //donner/svg/tests:svg_document_concurrency_tests
bazel test --config=tsan //donner/svg/renderer/tests:renderer_snapshot_tests
bazel test //donner/editor/tests:async_renderer_tests
```
