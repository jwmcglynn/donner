// The editor's application pthread owns the transferred canvas. Firefox snapshots that canvas
// (page screenshots, printing, drawSnapshot) by running a task on the pthread and blocking the
// main thread until it answers, for up to 10 s. A synchronous call from the pthread to the main
// thread inside that window deadlocks both threads until the snapshot times out. So every call the
// pthread can make to the main thread during frames or events must be fire-and-forget.
//
// Scope: the proxy modes of the vendored GLFW JavaScript library, and the synchronous main-thread
// calls (MAIN_THREAD_EM_ASM*, emscripten_sync_run_in_main*, emscripten_proxy_sync*, and the
// synchronous Emscripten html5 calls) in donner/editor and donner/editor/gui, counted per enclosing
// function. Each remaining synchronous call is listed with the reason it cannot meet a snapshot;
// adding one, or making a listed asynchronous GLFW call synchronous again, fails here.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { basename, resolve } from "node:path";
import test from "node:test";

function requiredPaths(name) {
  const value = process.env[name];
  assert.ok(value, `${name} must be set by the Bazel test target`);
  return value.split(" ").filter(Boolean).map((path) => resolve(path));
}

const [glfwLibraryPath] = requiredPaths("DONNER_GLFW_JS_LIBRARY");
const glfwLibrary = readFileSync(glfwLibraryPath, "utf8");
const editorSources = requiredPaths("DONNER_EDITOR_SOURCES");

// GLFW calls the canvas owner makes during frames or events.
const kAsyncGlfwFunctions = [
  // ImGui's GLFW backend sets the cursor every frame while ImGui owns it.
  "emscripten_glfw3_window_set_standard_cursor",
  "emscripten_glfw3_window_set_custom_cursor",
  // Window title changes, from the editor's frame and from focus events.
  "emscripten_glfw3_context_set_title",
  // Copies from the source pane and the canvas, inside a frame.
  "emscripten_glfw3_context_set_clipboard_string",
  // A canvas mousedown focuses the window when it lost focus.
  "emscripten_glfw3_window_focus",
  // Window resize events size the GLFW window.
  "emscripten_glfw3_window_set_size",
];

// Synchronous GLFW calls that remain, and why none can run while a snapshot waits on the owner.
const kSyncGlfwFunctions = {
  emscripten_glfw3_context_init: "window system setup before the first frame",
  emscripten_glfw3_context_destroy: "teardown after the last frame",
  emscripten_glfw3_window_init: "window creation before the first frame; returns a status",
  emscripten_glfw3_window_on_created: "window creation before the first frame",
  emscripten_glfw3_window_destroy: "teardown after the last frame",
  emscripten_glfw3_context_make_canvas_resizable: "setup; returns a status",
  emscripten_glfw3_context_unmake_canvas_resizable: "teardown; returns a status",
  emscripten_glfw3_context_gl_init: "WebGL context creation; Geode presents through WebGPU",
  emscripten_glfw3_context_gl_bool_attribute: "WebGL context creation",
  emscripten_glfw3_context_gl_create_context: "WebGL context creation; returns a status",
  emscripten_glfw3_context_gl_make_context_current: "WebGL context creation; returns a status",
  emscripten_glfw3_context_is_extension_supported: "WebGL-only query",
  emscripten_glfw3_create_custom_cursor: "reads the caller's pixels; the editor's cursors use its own registry",
  emscripten_glfw3_destroy_custom_cursor: "custom GLFW cursors are not created in the browser build",
  emscripten_glfw3_window_get_position: "window setup only; the browser build never polls events",
  emscripten_glfw3_window_get_computed_opacity: "window creation only",
  emscripten_glfw3_window_set_opacity: "the editor never sets window opacity",
  emscripten_glfw3_window_get_computed_visibility: "window creation only",
  emscripten_glfw3_window_set_visibility: "window creation only; the editor never shows or hides it",
  emscripten_glfw3_context_is_any_element_focused: "called from the keyboard listener on the main thread",
  emscripten_glfw3_context_is_any_other_element_focused:
    "called from the keyboard listener on the main thread",
  emscripten_glfw3_context_get_fullscreen_window: "fullscreen changes; the editor never requests fullscreen",
  emscripten_glfw3_context_get_pointer_lock_window: "pointer-lock changes; the editor never locks the pointer",
  emscripten_glfw3_context_open_url: "the editor opens links through its own launcher",
  emscripten_glfw3_context_is_runtime_platform_apple: "never called by the editor build",
};

// Synchronous main-thread calls in the editor: file, enclosing function, call count and reason.
const kSyncEditorCalls = {
  "main.cc": {
    InstallGpuOwnerWaitProbeForTesting: [
      1,
      "query-string read during editor setup before the first frame",
    ],
  },
  "RotateCursorSet.cc": {
    RegisterBrowserCursor: [1, "cursor registration during editor setup; returns SVG cursor support"],
    DestroyBrowserCursorRegistry: [1, "setup and teardown; restores the page cursor first"],
  },
  "EditorShell.cc": {
    SampleThumbnailRendererCreationRequestForTesting: [1, "query-string read during editor setup"],
    SampleThumbnailRendererCreationDelayMsForTesting: [1, "query-string read during editor setup"],
    BrowserOverlayControlEnabledForTesting: [1, "query-string read during editor setup"],
    BrowserEyedropperControlEnabledForTesting: [
      1,
      "cached query-string read, first taken during editor setup",
    ],
  },
  "WholeAppWorkerBridge.cc": {
    Install: [2, "bridge installation and its mousedown listener, before the first frame"],
    InstallFrameDriver: [1, "frame driver selection before the first frame"],
  },
  "EditorWindow.cc": {
    EditorWindow: [2, "replaces ImGui's resize listener during window setup"],
  },
};

function glfwProxyModes() {
  const modes = {};
  for (const match of glfwLibrary.matchAll(/\b(emscripten_glfw3_\w+)__proxy:\s*'([a-z]+)'/g)) {
    modes[match[1]] = match[2];
  }
  return modes;
}

test("GLFW calls made during frames or events proxy to the main thread asynchronously", () => {
  const modes = glfwProxyModes();
  for (const name of kAsyncGlfwFunctions) {
    assert.equal(modes[name], "async", name);
  }
});

test("every remaining synchronous GLFW proxy is audited", () => {
  const sync = Object.entries(glfwProxyModes())
    .filter(([, mode]) => mode === "sync")
    .map(([name]) => name)
    .sort();
  assert.deepEqual(sync, Object.keys(kSyncGlfwFunctions).sort());
});

// Synchronous call tokens; matched without the "(" so a call split across lines still counts.
// Every Emscripten html5 or browser call that can proxy to the main thread starts with one of the
// prefixes below, so a new one fails closed. The exceptions run on the calling thread:
// emscripten_set_canvas_element_size resizes the transferred canvas on this thread.
const kSyncCall = new RegExp(
  "\\b(?:MAIN_THREAD_EM_ASM(?:_INT|_DOUBLE|_PTR)?|emscripten_sync_run_in_main\\w*"
    + "|emscripten_proxy_sync\\w*"
    + "|emscripten_(?:get|set|request|exit|enter|lock|unlock|vibrate|sample|html5_remove)\\w*)\\b",
  "g",
);
const kCallingThreadCalls = new Set([
  "emscripten_get_now",
  "emscripten_get_heap_size",
  "emscripten_set_main_loop_arg",
  "emscripten_set_canvas_element_size",
  "emscripten_exit_with_live_runtime",
]);
// A top-level function definition line: starts in column 0 and names the function before "(".
const kFunctionStart = /^(?![#/ ])[^;=]*?\b([A-Za-z_]\w*)\s*\([^;]*$/;

function syncCallCounts(source) {
  const counts = {};
  let enclosing = null;
  for (const line of source.split("\n")) {
    const start = kFunctionStart.exec(line);
    if (start) enclosing = start[1];
    // Strip line comments, but not "//" inside a string literal such as a URL.
    const code = line.replace(/(^|\s)\/\/.*$/, "");
    for (const [token] of code.matchAll(kSyncCall)) {
      if (!kCallingThreadCalls.has(token)) counts[enclosing] = (counts[enclosing] ?? 0) + 1;
    }
  }
  return counts;
}

test("every synchronous editor call to the main thread is audited", () => {
  const found = {};
  for (const path of editorSources) {
    const counts = syncCallCounts(readFileSync(path, "utf8"));
    if (Object.keys(counts).length > 0) found[basename(path)] = counts;
  }
  const expected = Object.fromEntries(
    Object.entries(kSyncEditorCalls).map(([file, calls]) => [
      file,
      Object.fromEntries(Object.entries(calls).map(([name, [count]]) => [name, count])),
    ]),
  );
  assert.deepEqual(found, expected);
});

test("the audited editor sources include the files with main-thread calls", () => {
  const names = editorSources.map((path) => basename(path));
  for (const file of Object.keys(kSyncEditorCalls)) assert.ok(names.includes(file), file);
});
