# SVG DOM Threading And Lifetime {#SvgDomThreadingAndLifetime}

\tableofcontents

This page describes the public behavior of Donner's SVG DOM when applications use multiple threads
or keep handles to elements that are removed from the document tree.

## Default Mode

`SVGDocument` starts in single-threaded mode. This is the lowest-overhead mode and is the right
choice when one thread owns parsing, DOM updates, and rendering.

In single-threaded mode, do not call APIs on the same document from multiple threads at the same
time. If an application needs worker threads to inspect or mutate the same document, opt into
concurrent DOM mode before sharing the document.

## Concurrent DOM Mode

Enable concurrent DOM access with:

```cpp
document.setThreadingMode(ThreadingMode::ConcurrentDom);
```

After concurrent DOM mode is enabled, the public DOM APIs on `SVGDocument` and `SVGElement` may be
used from multiple threads. Reads may run concurrently. Writes are coordinated so the document stays
consistent.

Rendering is frame-based. A render observes a stable view of the document for that frame. If another
thread changes the DOM while rendering is in progress, the change is not mixed into the frame that
is already being drawn; it is visible to a later render.

## Batching Access

Individual DOM calls are safe in concurrent DOM mode and are convenient for occasional work:

```cpp
if (std::optional<SVGElement> rect = document.querySelector("#status")) {
  rect->setAttribute("fill", "green");
}
```

For repeated reads or writes, prefer the batching APIs. Batching keeps one access scope for the
whole operation instead of entering and leaving document access for every individual DOM call, so it
is more efficient for traversal, selector-heavy code, and groups of mutations.

Use \ref donner::svg::SVGDocument::withReadAccess "SVGDocument::withReadAccess()" for repeated
reads:

```cpp
document.withReadAccess([&](auto&) {
  for (std::optional<SVGElement> child = document.svgElement().firstChild(); child;
       child = child->nextSibling()) {
    std::cout << child->tagName() << "\n";
  }
});
```

Use \ref donner::svg::SVGDocument::withWriteAccess "SVGDocument::withWriteAccess()" to group
mutations:

```cpp
document.withWriteAccess([&](SVGDocumentMutation& mutation) {
  std::optional<SVGElement> rect = document.querySelector("#status");
  if (!rect) {
    return;
  }

  mutation.setAttribute(*rect, "fill", "green");
  mutation.setAttribute(*rect, "stroke", "black");
  mutation.setAttribute(*rect, "stroke-width", "2");
});
```

Batching is optional in single-threaded mode, but it is still useful when a large edit should be
treated as one logical document update.

## Removed Element Lifetime

`SVGElement` is a lightweight value handle. It can be copied, stored, and passed between functions.

Removing an element detaches it from the document tree. Existing `SVGElement` handles to that
element, and to its children, remain valid while user code still holds them:

```cpp
SVGElement badge = *document.querySelector("#badge");
badge.remove();

// The removed element is still available through the handle.
badge.setAttribute("opacity", "0.25");

// It can be inserted back into the document later.
document.svgElement().appendChild(badge);
```

A removed element is no longer found by normal document traversal or selectors until it is inserted
back into the tree. Donner reclaims the removed subtree after user code no longer holds public
handles to it and after in-progress rendering no longer needs it. Applications do not need to call a
manual dispose or free function for removed elements.

<div class="section_buttons">

| Previous                     |                                      Next |
| :--------------------------- | ----------------------------------------: |
| [Donner API](DonnerAPI.html) | [System architecture](SystemArchitecture.html) |

</div>
