async function SelectDonnerBackend(options) {
  const requestedBackend = options.requestedBackend || "auto";
  if (!["auto", "geode", "tiny_skia"].includes(requestedBackend)) {
    throw new Error(`Unknown renderer backend: ${requestedBackend}`);
  }

  if (requestedBackend !== "tiny_skia") {
    let adapter = null;
    if (options.requestWebGpuAdapter) {
      try {
        adapter = await options.requestWebGpuAdapter();
      } catch (error) {
        if (requestedBackend === "geode") {
          throw new Error(`Unable to request a WebGPU adapter: ${error}`);
        }
      }
    }

    if (adapter) {
      return { name: "geode", base: "geode/" };
    }
    if (requestedBackend === "geode") {
      throw new Error("A WebGPU adapter is required for the Geode renderer");
    }
  }

  if (!options.hasWebGl2()) {
    throw new Error("WebGL2 is required for the TinySkia renderer fallback");
  }
  return { name: "tiny_skia", base: "tiny_skia/" };
}

globalThis.SelectDonnerBackend = SelectDonnerBackend;

if (typeof window !== "undefined") {
  const requestedBackend = new URLSearchParams(window.location.search).get("renderer") || "auto";
  const requestWebGpuAdapter = navigator.gpu
    ? () => navigator.gpu.requestAdapter()
    : null;
  const hasWebGl2 = () => {
    const canvas = document.createElement("canvas");
    const context = canvas.getContext("webgl2");
    if (!context) {
      return false;
    }
    context.getExtension("WEBGL_lose_context")?.loseContext();
    return true;
  };

  window.__donnerBackendPromise = SelectDonnerBackend({
    requestedBackend,
    requestWebGpuAdapter,
    hasWebGl2,
  }).then((backend) => {
    window.__donnerBackend = backend.name;
    return backend;
  });
}
