import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import vm from "node:vm";

const hookUrl = new URL("../editor-abort-diagnostics.js", import.meta.url);
const hookSource = await readFile(hookUrl, "utf8");

/**
 * Runs the pre-js hook in a fresh context shaped like one thread of the editor module.
 * @param {{pthread: boolean, role?: string, workerScope?: boolean}} options The thread to model.
 */
function loadHook({ pthread, role, workerScope = true }) {
  const errors = [];
  const posted = [];
  const listeners = new Map();
  const context = {
    Module: {},
    ENVIRONMENT_IS_PTHREAD: pthread,
    err: (text) => errors.push(text),
    postMessage: (message) => posted.push(message),
    performance: { timeOrigin: 1000, now: () => 12.5 },
  };
  if (workerScope) {
    context.self = { addEventListener: (type, listener) => listeners.set(type, listener) };
  }
  context.globalThis = context;
  if (role !== undefined) context.__donnerThreadRole = role;
  vm.runInNewContext(hookSource, context, { filename: "editor-abort-diagnostics.js" });
  return { context, errors, posted, listeners };
}

test("the hook runs after a statement that has no semicolon", () => {
  // The runtime's thread detection, which immediately precedes --pre-js code, ends this way.
  const context = { Module: {}, ENVIRONMENT_IS_PTHREAD: false };
  context.globalThis = context;
  vm.runInNewContext(`var detected = "em-pthread"\n${hookSource}`, context);
  assert.equal(typeof context.Module.onAbort, "function");
});

test("a main-thread abort is logged with its stack and kept on the page", () => {
  const { context, errors, posted } = loadHook({ pthread: false });
  context.Module.onAbort("");
  assert.equal(errors.length, 1);
  assert.match(errors[0], /^\[donner-fatal\] Aborted\(\) thread=browser-main stack=Error\n/);
  assert.deepEqual(posted, []);
  assert.equal(context.__donnerAbortRecords.length, 1);
  assert.deepEqual(
    { ...context.__donnerAbortRecords[0], stack: undefined },
    { kind: "Aborted", thread: "browser-main", what: "", stack: undefined, atMs: 1012.5 },
  );
  assert.match(context.__donnerAbortRecords[0].stack, /editor-abort-diagnostics\.js/);
});

test("a pthread abort names the thread role and forwards its record to the page", () => {
  const { context, errors, posted, listeners } = loadHook({ pthread: true, role: "gpu-owner" });
  context.Module.onAbort("OOM");
  assert.equal(errors.length, 1);
  assert.match(errors[0], /^\[donner-fatal\] Aborted\(OOM\) thread=gpu-owner stack=/);
  assert.equal(context.__donnerAbortRecords, undefined);
  assert.equal(posted.length, 1);
  assert.deepEqual(
    { ...posted[0], args: undefined },
    { cmd: "callHandler", handler: "onAbort", args: undefined },
  );
  assert.equal(posted[0].args[1], "donner-forwarded-abort-record");
  assert.deepEqual(
    { ...posted[0].args[0], stack: undefined },
    { kind: "Aborted", thread: "gpu-owner", what: "OOM", stack: undefined, atMs: 1012.5 },
  );

  // The page's hook keeps a forwarded record without logging it again.
  const page = loadHook({ pthread: false });
  page.context.Module.onAbort(...posted[0].args);
  assert.deepEqual(page.errors, []);
  assert.equal(page.context.__donnerAbortRecords.length, 1);
  assert.equal(page.context.__donnerAbortRecords[0], posted[0].args[0]);
  assert.equal(listeners.has("error"), true);
});

test("an uncaught worker trap is reported once and an abort is left to the abort hook", () => {
  const { errors, posted, listeners } = loadHook({ pthread: true });
  const onError = listeners.get("error");
  class RuntimeError extends Error {}
  RuntimeError.prototype.name = "RuntimeError";
  const trap = new RuntimeError("access to a null reference");
  onError({ error: trap, message: "ignored" });
  onError({ error: { toString: () => "RuntimeError: Aborted(). Build with -sASSERTIONS" } });
  assert.equal(errors.length, 1);
  assert.match(errors[0], /^\[donner-fatal\] Uncaught worker error\(RuntimeError: access to a/);
  assert.match(errors[0], / thread=pthread stack=RuntimeError: access to a null reference\n/);
  assert.equal(posted.length, 1);
  assert.equal(posted[0].args[0].what, "RuntimeError: access to a null reference");
});

test("an error object passed as the abort reason is reported, not kept as a record", () => {
  const { context, errors } = loadHook({ pthread: false });
  context.Module.onAbort(new Error("ENOENT: missing module"));
  assert.equal(errors.length, 1);
  assert.match(errors[0], /^\[donner-fatal\] Aborted\(Error: ENOENT: missing module\) thread=/);
  assert.equal(context.__donnerAbortRecords[0].what, "Error: ENOENT: missing module");
});

test("a hook installed before this one still receives the abort reason", () => {
  const seen = [];
  const context = {
    Module: { onAbort: (what) => seen.push(what) },
    ENVIRONMENT_IS_PTHREAD: false,
    err: () => {},
    performance: { timeOrigin: 0, now: () => 0 },
  };
  context.globalThis = context;
  vm.runInNewContext(hookSource, context);
  context.Module.onAbort("OOM");
  context.Module.onAbort({ kind: "Aborted" }, "donner-forwarded-abort-record");
  assert.deepEqual(seen, ["OOM"]);
  assert.equal(context.__donnerAbortRecords.length, 2);
});

test("the hooks never throw and keep at most eight page records", () => {
  const { context, listeners } = loadHook({ pthread: false });
  for (let index = 0; index < 10; ++index) context.Module.onAbort(`reason ${index}`);
  assert.equal(context.__donnerAbortRecords.length, 8);

  // A console that throws loses neither a forwarded nor a kept record.
  const failing = loadHook({ pthread: true });
  failing.context.err = () => {
    throw new Error("console unavailable");
  };
  assert.doesNotThrow(() => failing.context.Module.onAbort(""));
  assert.doesNotThrow(() => failing.listeners.get("error")(null));
  assert.equal(failing.posted.length, 1);
  const failingPage = loadHook({ pthread: false });
  failingPage.context.err = failing.context.err;
  assert.doesNotThrow(() => failingPage.context.Module.onAbort("lost console"));
  assert.equal(failingPage.context.__donnerAbortRecords[0].what, "lost console");
  assert.equal(listeners.has("error"), false);

  // A pthread without a worker global scope, as Node hosts one, still loads and reports aborts.
  const nodeThread = loadHook({ pthread: true, workerScope: false });
  nodeThread.context.Module.onAbort("");
  assert.equal(nodeThread.posted.length, 1);
});
