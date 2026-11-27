// The optional package probes a worker export that actually suspends across a Promise.
(function () {
  "use strict";

  function hasJspi() {
    return typeof WebAssembly !== "undefined"
      && typeof WebAssembly.Suspending === "function"
      && typeof WebAssembly.promising === "function";
  }

  async function probeWorker() {
    if (!hasJspi() || !globalThis.crossOriginIsolated) return false;

    // (module
    //   (import "host" "wait" (func $wait (result i32)))
    //   (func (export "run") (result i32) call $wait))
    const module = new Uint8Array([
      0, 97, 115, 109, 1, 0, 0, 0,
      1, 5, 1, 96, 0, 1, 127,
      2, 13, 1, 4, 104, 111, 115, 116, 4, 119, 97, 105, 116, 0, 0,
      3, 2, 1, 0,
      7, 7, 1, 3, 114, 117, 110, 0, 1,
      10, 6, 1, 4, 0, 16, 0, 11,
    ]);
    // Match the editor's shared-memory worker requirements as well as its asynchronous call.
    const memory = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
    if (!(memory.buffer instanceof SharedArrayBuffer)) return false;
    const wait = new WebAssembly.Suspending(() => new Promise((resolve) => {
      setTimeout(() => resolve(7), 0);
    }));
    const { instance } = await WebAssembly.instantiate(module, { host: { wait } });
    const run = WebAssembly.promising(instance.exports.run);
    // A worker animation frame may be paused in a hidden document. The timer still exercises
    // a fresh JavaScript stack without making visibility part of the capability decision.
    return await new Promise((resolve, reject) => {
      setTimeout(() => {
        try {
          run().then((value) => resolve(value === 7), reject);
        } catch (error) {
          reject(error);
        }
      }, 0);
    });
  }

  if (typeof document === "undefined") {
    probeWorker().then(
      (supported) => globalThis.postMessage(supported),
      () => globalThis.postMessage(false),
    );
    return;
  }

  function showFailure(message) {
    document.getElementById("loading-screen").hidden = true;
    document.getElementById("capability-error-detail").textContent = message;
    document.getElementById("capability-error").hidden = false;
  }

  function showUnsupported() {
    showFailure(
      "This package requires browser-managed WebAssembly suspension. "
        + "Use the standard editor package for this browser.",
    );
  }

  const selector = document.currentScript;
  const probeUrl = selector.src;
  const canFallback = selector.hasAttribute("data-asyncify-fallback");
  function start(supported) {
    if (!supported && !canFallback) {
      showUnsupported();
      return;
    }
    const bootstrap = document.createElement("script");
    bootstrap.src = new URL("editor-bootstrap.js", probeUrl).href;
    if (!supported) bootstrap.dataset.runtime = "asyncify";
    bootstrap.addEventListener("error", () => {
      showFailure("Unable to load the editor bootstrap.");
    }, { once: true });
    document.body.appendChild(bootstrap);
  }

  if (!hasJspi() || !globalThis.crossOriginIsolated || typeof Worker !== "function") {
    start(false);
    return;
  }

  let worker;
  try {
    worker = new Worker(probeUrl);
  } catch {
    start(false);
    return;
  }
  let settled = false;
  const deadline = setTimeout(() => finish(false), 5000);
  function finish(supported) {
    if (settled) return;
    settled = true;
    clearTimeout(deadline);
    worker.terminate();
    start(supported);
  }
  worker.addEventListener("message", (event) => finish(event.data === true), { once: true });
  worker.addEventListener("error", () => finish(false), { once: true });
})();
