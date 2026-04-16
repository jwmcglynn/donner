// NOTE: This file creates a service worker that cross-origin-isolates the page
// when the host cannot set COOP/COEP headers directly, which is required for
// wasm pthreads.

/* Edited version of coi-serviceworker v0.1.6 - Guido Zuidhof, MIT licensed */
if (typeof window === "undefined") {
  self.addEventListener("install", () => self.skipWaiting());
  self.addEventListener("activate", event => event.waitUntil(self.clients.claim()));

  async function handleFetch(request) {
    if (request.cache === "only-if-cached" && request.mode !== "same-origin") {
      return;
    }

    if (request.mode === "no-cors") {
      request = new Request(request.url, {
        cache: request.cache,
        credentials: "omit",
        headers: request.headers,
        integrity: request.integrity,
        destination: request.destination,
        keepalive: request.keepalive,
        method: request.method,
        mode: request.mode,
        redirect: request.redirect,
        referrer: request.referrer,
        referrerPolicy: request.referrerPolicy,
        signal: request.signal,
      });
    }

    let response = await fetch(request).catch(error => console.error(error));
    if (!response || response.status === 0) {
      return response;
    }

    const headers = new Headers(response.headers);
    headers.set("Cross-Origin-Embedder-Policy", "credentialless");
    headers.set("Cross-Origin-Opener-Policy", "same-origin");

    return new Response(response.body, {
      status: response.status,
      statusText: response.statusText,
      headers,
    });
  }

  self.addEventListener("fetch", event => {
    event.respondWith(handleFetch(event.request));
  });
} else {
  (async function() {
    if (window.crossOriginIsolated !== false) {
      return;
    }

    if (!window.isSecureContext || !("serviceWorker" in navigator)) {
      console.warn(
          "COOP/COEP service worker not available on this origin. Use HTTPS or http://localhost.");
      return;
    }

    const registration = await navigator.serviceWorker
        .register(window.document.currentScript.src)
        .catch(error => console.error("COOP/COEP service worker registration failed:", error));
    if (!registration) {
      return;
    }

    registration.addEventListener("updatefound", () => {
      window.location.reload();
    });

    if (registration.active && !navigator.serviceWorker.controller) {
      window.location.reload();
    }
  })();
}
