// Catalog identities come from the compiled application, never a fetched manifest or document.
// The browser owns transport and hashing; the application owns font decoding and rendering.
(() => {
  "use strict";

  const maximumAssetBytes = 2 * 1024 * 1024;
  const maximumCacheBytes = 4 * 1024 * 1024;
  const cacheName = "donner-catalog-fonts-v1";
  const hashPattern = /^[a-f0-9]{64}$/;

  class CatalogFailure extends Error {
    constructor(reason, retryable = false) {
      super(reason);
      this.retryable = retryable;
    }
  }

  function throwIfCancelled(signal) {
    if (signal?.aborted) throw new CatalogFailure("font-cache-timeout");
  }

  function abortable(promise, signal) {
    if (!signal) return promise;
    if (signal.aborted) {
      void Promise.resolve(promise).catch(() => {});
      return Promise.reject(new CatalogFailure("font-cache-timeout"));
    }
    return new Promise((resolve, reject) => {
      const abort = () => {
        signal.removeEventListener("abort", abort);
        reject(new CatalogFailure("font-cache-timeout"));
      };
      signal.addEventListener("abort", abort, { once: true });
      Promise.resolve(promise).then((value) => {
        signal.removeEventListener("abort", abort);
        resolve(value);
      }, (error) => {
        signal.removeEventListener("abort", abort);
        reject(error);
      });
    });
  }

  async function readBounded(response, limit, signal) {
    if (signal?.aborted) {
      void response.body?.cancel().catch(() => {});
      throw new CatalogFailure("font-cache-timeout");
    }
    const declared = response.headers.get("content-length");
    if (declared !== null && (!/^\d+$/.test(declared) || Number(declared) > limit)) {
      void response.body?.cancel().catch(() => {});
      throw new CatalogFailure("font-size");
    }
    if (!response.body) throw new CatalogFailure("font-empty");
    const reader = response.body.getReader();
    const abort = () => {
      void reader.cancel().catch(() => {});
    };
    signal?.addEventListener("abort", abort, { once: true });
    // Preallocate once from the compiled bound, avoiding accumulated chunks and geometric growth.
    const bytes = new Uint8Array(limit);
    let used = 0;
    try {
      for (;;) {
        const { done, value } = await abortable(reader.read(), signal);
        throwIfCancelled(signal);
        if (done) break;
        if (value.byteLength > limit - used) throw new CatalogFailure("font-size");
        bytes.set(value, used);
        used += value.byteLength;
      }
    } catch (error) {
      void reader.cancel().catch(() => {});
      throw error;
    } finally {
      signal?.removeEventListener("abort", abort);
      reader.releaseLock();
    }
    return bytes.subarray(0, used);
  }

  class CatalogFontBroker {
    constructor(
      {
        baseUrl,
        assets,
        onResult,
        fetchImpl = globalThis.fetch.bind(globalThis),
        storage = globalThis.caches,
        cryptoImpl = globalThis.crypto,
        lockManager = globalThis.navigator?.locks,
        now = () => Date.now(),
        timers = globalThis,
        enabled = true,
      },
    ) {
      this.base = new URL(baseUrl);
      if (
        !/^https?:$/.test(this.base.protocol) || this.base.username || this.base.password
        || this.base.search || this.base.hash || !this.base.pathname.endsWith("/")
      ) {
        throw new CatalogFailure("font-base");
      }
      if (!Array.isArray(assets) || assets.length > 12) throw new CatalogFailure("font-manifest");
      this.assets = new Map();
      let totalBytes = 0;
      for (const asset of assets) {
        if (
          !hashPattern.test(asset.id) || asset.path !== `fonts/${asset.id}.woff2`
          || !Number.isSafeInteger(asset.encodedBytes) || asset.encodedBytes < 48
          || asset.encodedBytes > maximumAssetBytes
          || !Number.isSafeInteger(asset.decodedBytes) || asset.decodedBytes < 12
          || asset.decodedBytes > maximumAssetBytes || this.assets.has(asset.id)
        ) {
          throw new CatalogFailure("font-manifest");
        }
        totalBytes += asset.encodedBytes;
        this.assets.set(asset.id, Object.freeze({ ...asset }));
      }
      if (totalBytes > maximumCacheBytes) throw new CatalogFailure("font-manifest");
      this.fetch = fetchImpl;
      this.storage = storage;
      this.crypto = cryptoImpl;
      this.locks = lockManager;
      this.onResult = onResult;
      this.now = now;
      this.timers = timers;
      this.requests = new Map();
      this.queue = [];
      this.running = 0;
      this.closed = false;
      this.enabled = enabled;
      this.cacheWork = Promise.resolve();
      this.cacheWritePending = false;
      this.cachePersistence = null;
      this.persistenceDisabled = false;
    }

    request(id, token, priority = 0) {
      if (this.closed || !this.assets.has(id) || !Number.isSafeInteger(token) || token <= 0) {
        return false;
      }
      const existing = this.requests.get(id);
      if (existing) {
        // A fresh store token after eviction may refill identical verified content. Pending and
        // failed requests still require their original token or the explicit retry policy.
        if (existing.state === "ready" && existing.token !== token) {
          this.requests.delete(id);
        } else {
          if (existing.token !== token) return false;
          existing.priority = Math.min(existing.priority, priority);
          return true;
        }
      }
      const request = { id, token, priority, state: "queued", failedAt: 0, controller: null };
      this.requests.set(id, request);
      this.queue.push(request);
      this.pump();
      return true;
    }

    retry(id, token) {
      const prior = this.requests.get(id);
      if (!prior || prior.state !== "failed" || this.now() - prior.failedAt < 30000) return false;
      this.requests.delete(id);
      return this.request(id, token, prior.priority);
    }

    cancel(id, token) {
      const request = this.requests.get(id);
      if (!request || request.token !== token) return;
      this.requests.delete(id);
      this.queue = this.queue.filter((item) => item !== request);
      request.controller?.abort();
      request.cancelBackoff?.();
    }

    close() {
      this.closed = true;
      this.cachePersistence?.controller.abort();
      for (const request of this.requests.values()) {
        request.controller?.abort();
        request.cancelBackoff?.();
      }
      this.requests.clear();
      this.queue = [];
    }

    current(request) {
      return !this.closed && this.requests.get(request.id) === request;
    }

    start() {
      this.enabled = true;
      this.pump();
    }

    pump() {
      this.queue.sort((a, b) => a.priority - b.priority);
      while (!this.closed && this.enabled && this.running < 2 && this.queue.length) {
        const request = this.queue.shift();
        if (!this.current(request)) continue;
        request.state = "fetching";
        ++this.running;
        void this.load(request).finally(() => {
          --this.running;
          this.pump();
        });
      }
    }

    async verify(bytes, asset) {
      if (
        bytes.byteLength !== asset.encodedBytes || bytes[0] !== 0x77 || bytes[1] !== 0x4f
        || bytes[2] !== 0x46 || bytes[3] !== 0x32
        || new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(16)
          !== asset.decodedBytes
      ) {
        throw new CatalogFailure("font-content");
      }
      let digest;
      try {
        digest = new Uint8Array(await this.crypto.subtle.digest("SHA-256", bytes));
      } catch {
        throw new CatalogFailure("font-verification");
      }
      const hash = Array.from(digest, (byte) => byte.toString(16).padStart(2, "0")).join("");
      if (hash !== asset.id) throw new CatalogFailure("font-integrity");
      return bytes;
    }

    cacheKey(id) {
      return new URL(`/.donner-font-cache/v1/${id}`, this.base.origin).href;
    }

    async readCache(asset, signal) {
      try {
        const cache = await abortable(this.storage?.open(cacheName), signal);
        throwIfCancelled(signal);
        if (this.closed) return null;
        const key = this.cacheKey(asset.id);
        const response = await abortable(cache?.match(key), signal);
        throwIfCancelled(signal);
        if (!response || this.closed) return null;
        try {
          const bytes = await this.verify(
            await readBounded(response, asset.encodedBytes, signal),
            asset,
          );
          throwIfCancelled(signal);
          if (this.closed) return null;
          this.persist(asset, bytes);
          return bytes;
        } catch {
          throwIfCancelled(signal);
          if (!this.closed) await cache.delete(key);
        }
      } catch {
        // Storage denial and corrupt cache entries are cache misses, never trusted font bytes.
      }
      return null;
    }

    async cached(asset) {
      const controller = new AbortController();
      let timer;
      const timeout = new Promise((resolve) => {
        timer = this.timers.setTimeout(() => {
          controller.abort();
          resolve(null);
        }, 250);
      });
      try {
        return await Promise.race([this.readCache(asset, controller.signal), timeout]);
      } finally {
        this.timers.clearTimeout(timer);
        controller.abort();
      }
    }

    persist(asset, bytes) {
      // Skip persistence when another tab owns the namespace; font delivery never waits for it.
      if (
        !this.locks || !this.storage || this.closed || this.persistenceDisabled
        || this.cacheWritePending
      ) return;
      const work = { asset, bytes, controller: new AbortController() };
      this.cachePersistence = work;
      this.cacheWritePending = true;
      this.cacheWork = this.runPersistence(work);
    }

    async runPersistence(work) {
      const signal = work.controller.signal;
      const abort = () => {
        work.bytes = null;
        // An issued Cache API mutation cannot be aborted. Do not admit another write behind it.
        this.persistenceDisabled = true;
      };
      signal.addEventListener("abort", abort, { once: true });
      const deadline = this.timers.setTimeout(() => work.controller.abort(), 250);
      try {
        await abortable(
          this.locks.request(cacheName, { ifAvailable: true }, (lock) => {
            if (!lock) return;
            return this.persistLocked(work);
          }),
          signal,
        );
      } catch {
        // Persistence is best effort, including denied storage and cancelled work.
      } finally {
        this.timers.clearTimeout(deadline);
        signal.removeEventListener("abort", abort);
        work.bytes = null;
        this.cachePersistence = null;
        this.cacheWritePending = false;
      }
    }

    async persistLocked(work) {
      const signal = work.controller.signal;
      throwIfCancelled(signal);
      const cache = await abortable(this.storage.open(cacheName), signal);
      throwIfCancelled(signal);
      if (!cache) return;
      await this.trimCache(cache, work.asset, signal);
      throwIfCancelled(signal);
      const response = new Response(work.bytes, {
        headers: {
          "content-type": "font/woff2",
          "content-length": String(work.bytes.byteLength),
          "x-donner-used": String(this.now()),
        },
      });
      work.bytes = null;
      // Keep the cross-tab lock until an issued mutation settles, even if cacheWork times out.
      await cache.put(this.cacheKey(work.asset.id), response);
      throwIfCancelled(signal);
    }

    async deleteCached(cache, key, signal) {
      throwIfCancelled(signal);
      await cache.delete(key);
      throwIfCancelled(signal);
    }

    async trimCache(cache, incoming, signal) {
      const keys = await abortable(cache.keys(), signal);
      throwIfCancelled(signal);
      const retained = [];
      let total = incoming.encodedBytes;
      const incomingKey = this.cacheKey(incoming.id);
      for (const key of keys) {
        throwIfCancelled(signal);
        if ((typeof key === "string" ? key : key.url) === incomingKey) continue;
        const response = await abortable(cache.match(key), signal);
        throwIfCancelled(signal);
        const used = Number(response?.headers.get("x-donner-used"));
        const declared = Number(response?.headers.get("content-length"));
        if (
          !response || !Number.isFinite(used) || !Number.isSafeInteger(declared)
          || declared < 48 || declared > maximumAssetBytes || retained.length >= 128
        ) {
          await this.deleteCached(cache, key, signal);
          continue;
        }
        // Reserve the incoming body before writing; cancellation cannot create over-budget storage.
        try {
          const body = await readBounded(response, declared, signal);
          throwIfCancelled(signal);
          if (body.byteLength !== declared) throw new CatalogFailure("font-size");
          retained.push({ key, used, size: body.byteLength });
          total += body.byteLength;
        } catch {
          throwIfCancelled(signal);
          await this.deleteCached(cache, key, signal);
        }
      }
      retained.sort((a, b) => a.used - b.used);
      for (const entry of retained) {
        if (total <= maximumCacheBytes) break;
        await this.deleteCached(cache, entry.key, signal);
        total -= entry.size;
      }
    }

    async attempt(request, asset) {
      const controller = new AbortController();
      request.controller = controller;
      const deadline = this.timers.setTimeout(() => controller.abort(), 15000);
      try {
        const url = new URL(asset.path, this.base);
        const response = await this.fetch(url.href, {
          signal: controller.signal,
          credentials: "same-origin",
          redirect: "error",
          referrerPolicy: "no-referrer",
        });
        if (response.redirected || (response.url && response.url !== url.href)) {
          await response.body?.cancel();
          throw new CatalogFailure("font-origin");
        }
        if (!response.ok) {
          await response.body?.cancel();
          throw new CatalogFailure(
            "font-http",
            response.status === 408 || response.status === 429
              || response.status >= 500,
          );
        }
        const mime = response.headers.get("content-type")?.split(";", 1)[0].trim().toLowerCase();
        if (
          mime !== "font/woff2" && mime !== "application/font-woff2"
          && mime !== "application/octet-stream"
        ) {
          await response.body?.cancel();
          throw new CatalogFailure("font-type");
        }
        return await this.verify(await readBounded(response, asset.encodedBytes), asset);
      } catch (error) {
        if (error instanceof CatalogFailure) throw error;
        throw new CatalogFailure(controller.signal.aborted ? "font-timeout" : "font-network", true);
      } finally {
        this.timers.clearTimeout(deadline);
        request.controller = null;
      }
    }

    backoff(request) {
      return new Promise((resolve) => {
        const finish = () => {
          request.cancelBackoff = null;
          resolve();
        };
        const timer = this.timers.setTimeout(finish, 1000);
        request.cancelBackoff = () => {
          this.timers.clearTimeout(timer);
          finish();
        };
      });
    }

    async load(request) {
      const asset = this.assets.get(request.id);
      try {
        let bytes = await this.cached(asset);
        if (!this.current(request)) return;
        if (!bytes) {
          for (let attempt = 0; attempt < 2; ++attempt) {
            try {
              bytes = await this.attempt(request, asset);
              break;
            } catch (error) {
              if (!this.current(request)) return;
              if (!error.retryable || attempt !== 0) throw error;
              await this.backoff(request);
              if (!this.current(request)) return;
            }
          }
          this.persist(asset, bytes);
        }
        if (!this.current(request)) return;
        request.state = "ready";
        this.onResult({ id: request.id, token: request.token, bytes });
      } catch (error) {
        if (!this.current(request)) return;
        request.state = "failed";
        request.failedAt = this.now();
        this.onResult({
          id: request.id,
          token: request.token,
          error: error instanceof CatalogFailure ? error.message : "font-network",
        });
      }
    }
  }

  globalThis.DonnerCatalogFontBroker = CatalogFontBroker;
})();
