/* Version navigation uses only the verified inventory of this deployed site. */
(() => {
  "use strict";
  const script = document.querySelector("script[data-docs-current]");
  const select = document.getElementById("docs-version-select");
  if (!script || !select) return;
  const base = new URL(script.dataset.docsBase, location.origin);
  if (base.origin !== location.origin || !/^\/(?:[A-Za-z0-9_-]+\/)*$/.test(base.pathname)) return;
  const safePage = value => typeof value === "string" && value.length < 4096 &&
    !/[\\\s%?#:\x00-\x1f\x7f]/.test(value) && !value.startsWith("/") &&
    value.split("/").every(part => part && part !== "." && part !== "..");
  const hex = bytes => Array.from(new Uint8Array(bytes), byte => byte.toString(16).padStart(2, "0")).join("");
  const sha = text => crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)).then(hex);
  async function read(path, limit, expected) {
    const url = new URL(path, base);
    if (url.origin !== base.origin || !url.pathname.startsWith(base.pathname)) throw new Error("Invalid docs URL");
    const response = await fetch(url, {cache: "no-store", redirect: "error"});
    if (!response.ok || !response.body) throw new Error("Docs inventory unavailable");
    const reader = response.body.getReader();
    const parts = [];
    let size = 0;
    while (true) {
      const {done, value} = await reader.read();
      if (done) break;
      size += value.length;
      if (size > limit) { await reader.cancel(); throw new Error("Docs inventory too large"); }
      parts.push(value);
    }
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const part of parts) { bytes.set(part, offset); offset += part.length; }
    if (expected && hex(await crypto.subtle.digest("SHA-256", bytes)) !== expected) throw new Error("Docs inventory changed");
    return JSON.parse(new TextDecoder("utf-8", {fatal: true}).decode(bytes));
  }
  function notice(text) {
    const element = document.createElement("div");
    element.className = "docs-version-notice";
    element.setAttribute("role", "status");
    element.append(document.createTextNode(text));
    const close = document.createElement("button");
    close.type = "button";
    close.textContent = "Dismiss";
    close.addEventListener("click", () => element.remove());
    element.append(close);
    document.getElementById("top")?.after(element);
  }
  const parameters = new URLSearchParams(location.search);
  if (parameters.has("docs-missing")) {
    const missing = parameters.get("docs-missing");
    if (safePage(missing)) notice("This page is unavailable in this version: " + missing);
  } else if (parameters.has("docs-anchor-missing")) {
    notice("The selected version does not contain the section you were viewing.");
  }
  read("_docs/versions.json", 65536).then(manifest => {
    const keys = Object.keys(manifest.versions || {});
    if (manifest.schema !== 1 || keys.length < 2 || keys.length > 32 || !keys.includes("main") ||
        !keys.includes(manifest.stable) || keys.some(key => key !== "main" && !/^\d+\.\d+\.\d+$/.test(key))) return;
    if (keys.some(key => !/^[0-9a-f]{64}$/.test(manifest.versions[key]?.pages_sha256 || ""))) return;
    const available = new Set(keys);
    if (!available.has(script.dataset.docsCurrent)) return;
    for (const option of select.options) if (!available.has(option.value)) return;
    select.disabled = false;
    select.closest(".docs-version")?.classList.add("docs-version-ready");
    select.addEventListener("change", async () => {
      const target = select.value;
      if (!available.has(target)) return;
      select.disabled = true;
      let page = script.dataset.docsPage;
      const destination = new URL(target + "/index.html", base);
      try {
        if (!safePage(page)) throw new Error("Invalid page");
        const pages = await read(target + "/_docs/pages.json", 8 * 1024 * 1024, manifest.versions[target].pages_sha256);
        if (pages.schema !== 1 || !Object.hasOwn(pages.pages || {}, page)) {
          destination.searchParams.set("docs-missing", page);
        } else {
          const record = pages.pages[page];
          const anchorFile = "_docs/anchors/" + await sha(page) + ".json";
          if (record.anchors !== anchorFile || !/^[0-9a-f]{64}$/.test(record.sha256 || "")) throw new Error("Invalid anchor path");
          destination.pathname = base.pathname + target + "/" + page;
          if (location.hash) {
            const anchors = await read(target + "/" + anchorFile, 1024 * 1024, record.sha256);
            if (Array.isArray(anchors.anchors) && anchors.anchors.includes(decodeURIComponent(location.hash.slice(1)))) {
              destination.hash = location.hash;
            } else destination.searchParams.set("docs-anchor-missing", "1");
          }
        }
      } catch {
        destination.pathname = base.pathname + target + "/index.html";
        destination.searchParams.set("docs-missing", safePage(page) ? page : "index.html");
      }
      location.assign(destination.href);
    });
  }).catch(() => { /* The visible version-index links remain usable. */ });
})();
