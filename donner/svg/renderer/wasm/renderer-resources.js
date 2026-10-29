// The compiled registry is the authority; audit JSON is never a runtime input.
export async function prepareRendererResources(module, options = {}) {
  if (module._donner_prepare_resources === undefined) return null;
  if (typeof module._donner_prepare_resources !== "function") {
    throw new Error("Invalid renderer resource capability");
  }
  if (options.signal?.aborted) throw new Error("Renderer resources cancelled");
  const location = new URL(globalThis.location.href);
  const base = new URL(options.baseUrl ?? "./", location);
  if (base.origin !== location.origin) throw new Error("Renderer resources must be same-origin");
  await Promise.all([import("./catalog-fonts.js"), import("./packaged-resource-loader.js")]);
  if (options.signal?.aborted) throw new Error("Renderer resources cancelled");
  const Loader = globalThis.DonnerPackagedResourceLoader;
  const loader = new Loader({ baseUrl: base.href, manifest: Loader.compiledManifest(module) });
  const close = () => {
    options.signal?.removeEventListener("abort", close);
    loader.close();
  };
  options.signal?.addEventListener("abort", close, { once: true });
  try {
    await loader.install(module);
    if (options.signal?.aborted) throw new Error("Renderer resources cancelled");
    if (module._donner_prepare_resources() !== 1) {
      throw new Error(module.cwrap("donner_get_last_error", "string", [])());
    }
    return { close };
  } catch (error) {
    close();
    throw error;
  }
}
