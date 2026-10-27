// Metadata is generated with the application and checked against the compiled resource registry.
(() => {
  "use strict";

  function canonicalManifest(manifest) {
    if (manifest?.schemaVersion !== 1 || !Array.isArray(manifest.assets)) {
      throw new Error("Invalid application resource manifest");
    }
    return JSON.stringify(
      manifest.assets.map((asset) => [
        asset.id,
        asset.sha256,
        asset.encodedSize,
        asset.decodedSize,
        asset.mediaType,
        asset.group,
        asset.path,
      ]).sort((a, b) => a[0].localeCompare(b[0])),
    );
  }

  function readManifest(module) {
    const pointer = module._DonnerPackagedResourceManifest();
    const heap = module.HEAPU8;
    if (!pointer || pointer >= heap.length) throw new Error("Missing compiled resource manifest");
    const limit = Math.min(heap.length, pointer + 128 * 1024);
    let end = pointer;
    while (end < limit && heap[end] !== 0) ++end;
    if (end === limit) throw new Error("Invalid compiled resource manifest");
    return JSON.parse(
      // Browser decoders require a non-shared snapshot of the bounded pthread heap range.
      new TextDecoder("utf-8", { fatal: true }).decode(heap.slice(pointer, end)),
    );
  }

  class PackagedResourceLoader {
    static compiledManifest(module) {
      return readManifest(module);
    }

    constructor({ baseUrl, manifest, brokerOptions = {} }) {
      canonicalManifest(manifest);
      this.manifest = manifest;
      this.pending = new Map();
      this.installing = new Map();
      this.installed = new Set();
      this.nextToken = 1;
      this.closed = false;
      this.module = null;
      this.broker = new globalThis.DonnerAssetBroker({
        ...brokerOptions,
        baseUrl,
        assets: manifest.assets,
        onResult: (result) => {
          const pending = this.pending.get(result.id);
          if (!pending || pending.token !== result.token || this.closed) return;
          this.pending.delete(result.id);
          if (result.error) {
            pending.reject(new Error(`Application resource unavailable: ${result.error}`));
          } else pending.resolve(result.bytes);
        },
      });
      const criticalAssets = new Map(
        manifest.assets.filter((asset) =>
          ["ui-font", "fallback-font", "chrome"].includes(asset.group)
        ).map((asset) => [asset.sha256, asset]),
      );
      this.critical = Promise.all(Array.from(criticalAssets.values(), async (asset) => ({
        asset,
        bytes: await this.load(asset.sha256),
      })));
      // Downloads run alongside compilation; retain failures until the runtime reaches its barrier.
      void this.critical.catch(() => {});
    }

    load(digest, explicitRetry = false) {
      if (this.closed) return Promise.reject(new Error("Application resources closed"));
      const prior = this.pending.get(digest);
      if (prior) return prior.promise;
      const token = this.nextToken++;
      let resolve;
      let reject;
      const promise = new Promise((yes, no) => {
        resolve = yes;
        reject = no;
      });
      this.pending.set(digest, { token, promise, resolve, reject });
      const accepted = explicitRetry
        ? this.broker.retry(digest, token)
        : this.broker.request(digest, token);
      if (!accepted) {
        this.pending.delete(digest);
        reject(new Error("Application resource request unavailable"));
      }
      return promise;
    }

    installBytes(module, digest, bytes) {
      if (this.closed) throw new Error("Application resources closed");
      const pointer = module._malloc(65 + bytes.byteLength);
      if (!pointer) throw new Error("Application resource memory exhausted");
      try {
        const heap = module.HEAPU8;
        for (let index = 0; index < 64; ++index) heap[pointer + index] = digest.charCodeAt(index);
        heap[pointer + 64] = 0;
        heap.set(bytes, pointer + 65);
        if (module._DonnerInstallPackagedResource(pointer, pointer + 65, bytes.byteLength) !== 1) {
          throw new Error("Application resource installation failed");
        }
        this.installed.add(digest);
      } finally {
        module._free(pointer);
      }
    }

    async install(module) {
      if (canonicalManifest(readManifest(module)) !== canonicalManifest(this.manifest)) {
        this.close();
        throw new Error("Application and resources belong to different packages");
      }
      const critical = this.critical;
      this.critical = null;
      const resources = await critical;
      try {
        for (const { asset, bytes } of resources) this.installBytes(module, asset.sha256, bytes);
        this.module = module;
      } finally {
        resources.length = 0;
      }
    }

    request(digest, explicitRetry = false) {
      const module = this.module;
      if (!module || this.closed) {
        return Promise.reject(new Error("Application resources not initialized"));
      }
      if (!this.broker.assets.has(digest)) {
        return Promise.reject(new Error("Unknown application resource"));
      }
      if (this.installed.has(digest)) return Promise.resolve();
      const prior = this.installing.get(digest);
      if (prior) return prior;
      const operation = this.load(digest, explicitRetry).then((bytes) => {
        this.installBytes(module, digest, bytes);
      }).finally(() => {
        if (this.installing.get(digest) === operation) this.installing.delete(digest);
      });
      this.installing.set(digest, operation);
      return operation;
    }

    close() {
      if (this.closed) return;
      this.closed = true;
      this.module = null;
      this.broker.close();
      for (const pending of this.pending.values()) {
        pending.reject(new Error("Application resources closed"));
      }
      this.pending.clear();
      this.installing.clear();
      this.installed.clear();
      this.critical = null;
    }
  }

  globalThis.DonnerPackagedResourceLoader = PackagedResourceLoader;
})();
