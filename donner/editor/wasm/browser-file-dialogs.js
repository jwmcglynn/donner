// File handles stay on the browser thread; only bounded SVG text crosses into the editor.
(function(host) {
  const maximumBytes = 8 * 1024 * 1024;
  const types = [{ description: "SVG image", accept: { "image/svg+xml": [".svg"] } }];

  class BrowserFileDialogs {
    constructor(environment) {
      this.host = environment;
      this.sessions = new Map();
    }

    forget(id) {
      this.sessions.delete(id);
    }

    acceptOpen(id, accepted) {
      const session = this.sessions.get(id);
      if (!session?.pendingOpen) return;
      if (accepted) session.handle = session.pendingOpen.handle;
      session.pendingOpen = null;
    }

    async pickInputFile() {
      const input = this.host.document.createElement("input");
      input.type = "file";
      input.accept = ".svg,image/svg+xml";
      input.hidden = true;
      this.host.document.body.append(input);
      try {
        return await new Promise((resolve, reject) => {
          input.addEventListener("change", () => resolve(input.files?.[0] ?? null), { once: true });
          input.addEventListener("cancel", () => resolve(null), { once: true });
          try {
            input.click();
          } catch (error) {
            reject(error);
          }
        });
      } finally {
        input.remove();
      }
    }

    async open(session) {
      let handle = null;
      let file;
      if (typeof this.host.showOpenFilePicker === "function") {
        [handle] = await this.host.showOpenFilePicker({ multiple: false, types });
        file = await handle.getFile();
      } else {
        file = await this.pickInputFile();
      }
      if (!file) return { status: 1 };
      if (file.size > maximumBytes) throw new Error("SVG files must be no larger than 8 MiB.");
      const source = await file.text();
      this.validateSource(source);
      session.pendingOpen = { handle };
      session.target = null;
      return { status: 0, name: file.name, source };
    }

    async chooseSaveTarget(session, name, saveAs, replaceBacking) {
      let handle = saveAs ? null : session.handle;
      if (handle?.requestPermission) {
        const permission = await handle.requestPermission({ mode: "readwrite" });
        if (permission !== "granted") return { status: 1 };
      }
      if (!handle && typeof this.host.showSaveFilePicker === "function") {
        handle = await this.host.showSaveFilePicker({
          suggestedName: name || "untitled.svg",
          types,
        });
      }
      session.target = { handle, name: handle?.name || name || "untitled.svg", replaceBacking };
      return { status: 0, name: session.target.name };
    }

    validateSource(source) {
      if (new TextEncoder().encode(source).length > maximumBytes) {
        throw new Error("SVG files must be no larger than 8 MiB.");
      }
      if (source.includes("\0")) throw new Error("SVG text cannot contain null characters.");
    }

    download(source, name) {
      const url = this.host.URL.createObjectURL(new Blob([source], { type: "image/svg+xml" }));
      const link = this.host.document.createElement("a");
      link.href = url;
      link.download = name;
      link.hidden = true;
      this.host.document.body.append(link);
      try {
        link.click();
      } finally {
        link.remove();
        this.host.setTimeout(() => this.host.URL.revokeObjectURL(url), 1000);
      }
    }

    async write(session, source) {
      this.validateSource(source);
      const target = session.target;
      if (!target) throw new Error("Choose a destination before saving.");
      if (target.handle) {
        const writable = await target.handle.createWritable();
        try {
          await writable.write(source);
          await writable.close();
        } catch (error) {
          await writable.abort().catch(() => {});
          throw error;
        }
      } else {
        this.download(source, target.name);
      }
      if (target.replaceBacking) session.handle = target.handle;
      session.target = null;
      return { status: 0, name: target.name };
    }

    async run(id, operation, name, source, saveAs) {
      if (!this.sessions.has(id)) this.sessions.set(id, { handle: null, target: null });
      const session = this.sessions.get(id);
      try {
        if (operation === 0) return await this.open(session);
        if (operation === 1 || operation === 3) {
          return await this.chooseSaveTarget(session, name, saveAs, operation === 1);
        }
        if (operation === 2) return await this.write(session, source);
        throw new Error("Unsupported file operation.");
      } catch (error) {
        return error?.name === "AbortError"
          ? { status: 1 }
          : { status: 2, error: String(error?.message || error) };
      }
    }
  }

  host.DonnerBrowserFileDialogs = BrowserFileDialogs;
  host.__donnerFileDialogs = new BrowserFileDialogs(host);
})(globalThis);
