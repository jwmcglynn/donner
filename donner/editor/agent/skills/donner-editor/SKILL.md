---
name: donner-editor
description: Edit SVG artwork collaboratively in Donner SVG Editor through its local MCP connection, including click-anchored feedback and raster-to-vector iteration. Use when the user asks to work in Donner or points to the app's bundled AGENTS.md.
---

# Donner editor

Work on the live document the user sees. Its DOM, source projection, selection, render, and
undo history are one editing session.

## Connect and inspect

Read the bundle's `AGENTS.md` for executable and adapter locations. Discover the installed
version's launch options and MCP tools instead of assuming that a newer feature exists.
For the stdio adapter, use the user's available Python 3 runtime and pass `--socket` with the
endpoint reported by the app. The connection uses a private local socket; it does not require
a hosted account or an external service.

Read `get_editor_state` and identify the document before mutation. Read `get_svg_source` when
geometry, paint, references, or layering need inspection. Confirm the advertised capabilities
for replies, events, atomic batches, reference views, and agent activity; older versions may
provide only the basic tools.

Use `pick_at` for a document-space coordinate and `select_by_selector` to inspect a proposed
target. A transparent overlay or masked ancestor can be the hit target: interpret the local
artwork and neighboring shapes rather than assuming the topmost picked element is the defect.

## Edit together

Use the app's DOM tools for attributes and structure. Do not perform structural source-string
surgery or replace the live document with an independently reparsed copy. Group a logical edit
into one undoable batch when the server supports it.

Carry the server's session, document-generation, and revision guards. When scoped preconditions
are available, bind the affected elements and relevant transforms. A stale/conflict response
means reread and reconcile the changed scope; blindly refreshing the revision and replaying the
same plan can overwrite human work. Allow disjoint human work and defer edits to a shape that
is being dragged or drawn.

Use activity/presence tools when advertised to show the intended target and progress. Report
intent as pending until the edit has actually been applied and presented. Never move the human's
system cursor to simulate agent activity.

For vectorization, read [the vectorization workflow](references/vectorization.md). For regular
SVG editing, keep the user's existing composition and styling unless the requested change
requires otherwise.

## Feedback loop

Read open comments, including their coordinates, element anchors, and thread replies. Comments
can refer to an empty gap between shapes, not just the selected element. Look at the local render
before deciding which geometry should change.

Subscribe to feedback events when supported. Otherwise use the bounded `wait_for_comments` or
`wait_for_events` tool between work batches. MCP notifications are delivered by the server, but
whether an idle agent wakes depends on its host client; do not claim unattended monitoring when
that integration is unavailable.

Reply in the comment thread when the server exposes a reply tool. Explain the local change or
an ambiguity in concrete terms. Resolve a comment after its requested result is verified;
retain the thread history and leave unrelated or newly arrived feedback open.

## Verify and save

Check both the full composition and a close-up of changed edges. A matching SVG string is not
proof of a correct live edit: geometry properties and cached clip/mask consumers must update in
the running editor. Use a current canvas capture or inspect the visible app after mutation.

The `donner-svg` command-line tool uses Donner's TinySkia CPU renderer. The native editor uses
Geode GPU rendering. A correct CPU render does not establish that live geometry, clipping, or
cached pixels are correct in the editor. If source is correct but live output differs, isolate
an editor/renderer problem rather than distorting the SVG to compensate.

A resource-limit warning concerns rendering capacity, not necessarily artwork complexity.
Keep vector geometry intact while diagnosing surfaces, bounds, caching, or preview resolution.

Save through the editor and verify the reported file and dirty state. Hand back the SVG path,
a current render when useful, the comments addressed, and any remaining limitation. Keep
reference rasters and collaboration metadata out of the SVG export unless the user requested
embedded images or metadata.
