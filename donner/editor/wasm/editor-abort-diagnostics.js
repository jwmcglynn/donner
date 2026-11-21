// Records the thread, reason and JavaScript stack of a runtime abort, and of an uncaught worker
// error such as a Wasm trap. A release build reports a plain abort() only as "Aborted()".
//
// Linked with --pre-js, so every pthread worker installs its own hook and the runtime keeps it
// instead of a proxy. Records reach the page console and `__donnerAbortRecords`.
//
// A block, not a parenthesized function: the runtime emits this after a statement without a
// semicolon, which a leading parenthesis would call.
{
  const kPrefix = "[donner-fatal] ";
  const kMaxReasonChars = 1000;
  const kMaxStackChars = 4000;
  const kMaxRecords = 8;
  // Marks a record a worker forwarded; the runtime passes only the reason.
  const kForwardedRecord = "donner-forwarded-abort-record";
  const isPthread = typeof ENVIRONMENT_IS_PTHREAD != "undefined" && ENVIRONMENT_IS_PTHREAD;
  const previousOnAbort = Module["onAbort"];

  const threadRole = () => {
    const role = globalThis["__donnerThreadRole"];
    return typeof role == "string" ? role : isPthread ? "pthread" : "browser-main";
  };

  const keep = (record) => {
    if (!Array.isArray(globalThis["__donnerAbortRecords"])) globalThis["__donnerAbortRecords"] = [];
    const records = globalThis["__donnerAbortRecords"];
    if (records.length < kMaxRecords) records.push(record);
  };

  const report = (kind, what, stack) => {
    const record = {
      "kind": kind,
      "thread": threadRole(),
      "what": String(what).slice(0, kMaxReasonChars),
      "stack": String(stack || "").slice(0, kMaxStackChars),
      "atMs": performance.timeOrigin + performance.now(),
    };
    // Store or forward first, so a failing console cannot lose the record.
    if (isPthread) {
      // The runtime's handler-forwarding message: the page's hook receives the record.
      const args = [record, kForwardedRecord];
      postMessage({ "cmd": "callHandler", "handler": "onAbort", "args": args });
    } else {
      keep(record);
    }
    err(`${kPrefix}${kind}(${record.what}) thread=${record.thread} stack=${record.stack}`);
  };

  Module["onAbort"] = (what, forwarded) => {
    if (forwarded === kForwardedRecord) {
      try {
        keep(what);
      } catch {}
      return;
    }
    try {
      report("Aborted", what, new Error().stack);
    } catch {}
    try {
      if (typeof previousOnAbort == "function") previousOnAbort(what);
    } catch {}
  };

  // Node hosts pthreads without a worker global scope.
  if (isPthread && typeof self != "undefined" && typeof self.addEventListener == "function") {
    self.addEventListener("error", (event) => {
      try {
        const error = event.error;
        // `String(error)` keeps the error's name, such as RuntimeError, which a stack may omit.
        const message = String(error || event.message);
        // The abort hook already reported this thread's abort.
        if (!message.includes("Aborted(")) {
          report("Uncaught worker error", message, error && error.stack);
        }
      } catch {}
    });
  }
}
