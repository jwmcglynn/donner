// Provides cross-origin isolation on secure static hosts that cannot set response headers.

if (typeof window === "undefined") {
  self.addEventListener("install", () => self.skipWaiting());
  self.addEventListener("activate", (event) => event.waitUntil(self.clients.claim()));

  async function AddIsolationHeaders(request) {
    const response = await fetch(request);
    if (!response || response.status === 0) {
      return response;
    }

    const headers = new Headers(response.headers);
    headers.set("Cross-Origin-Opener-Policy", "same-origin");
    headers.set("Cross-Origin-Embedder-Policy", "require-corp");
    return new Response(response.body, {
      headers,
      status: response.status,
      statusText: response.statusText,
    });
  }

  self.addEventListener("fetch", (event) => {
    event.respondWith(AddIsolationHeaders(event.request));
  });
} else {
  const fallbackScriptUrl = document.currentScript?.src;
  (async function() {
    if (window.crossOriginIsolated === true) {
      return;
    }

    if (!window.isSecureContext || !("serviceWorker" in navigator) || !fallbackScriptUrl) {
      console.warn(
        "COOP/COEP service worker not available on this origin. Use HTTPS or http://localhost.",
      );
      return;
    }

    const registration = await navigator.serviceWorker
      .register(fallbackScriptUrl)
      .catch((error) => console.error("COOP/COEP service worker registration failed:", error));
    if (!registration) {
      return;
    }

    let reloading = false;
    const reload = () => {
      if (!reloading) {
        reloading = true;
        window.location.reload();
      }
    };
    if (navigator.serviceWorker.controller) {
      reload();
      return;
    }

    const watchWorker = (worker) => {
      if (!worker) {
        return;
      }
      if (worker.state === "activated") {
        reload();
        return;
      }
      worker.addEventListener("statechange", () => {
        if (worker.state === "activated") {
          reload();
        }
      });
    };

    if (registration.active) {
      reload();
      return;
    }
    watchWorker(registration.installing || registration.waiting);
    registration.addEventListener("updatefound", () => watchWorker(registration.installing));
  })();
}
