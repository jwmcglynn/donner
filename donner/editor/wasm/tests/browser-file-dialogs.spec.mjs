import assert from "node:assert/strict";
import fs from "node:fs";
import { test } from "node:test";
import vm from "node:vm";

const script = fs.readFileSync(new URL("../browser-file-dialogs.js", import.meta.url), "utf8");
function fixture(overrides = {}) {
  const host = { TextEncoder, Blob, ...overrides };
  vm.runInNewContext(script, host);
  return host.__donnerFileDialogs;
}

test("native open reads only the file chosen by the user", async () => {
  let calls = 0;
  const api = fixture({
    showOpenFilePicker: async () => {
      ++calls;
      return [{ getFile: async () => ({ name: "art.svg", size: 6, text: async () => "<svg/>" }) }];
    },
  });
  const result = await api.run(1, 0, "", "", false);
  assert.equal(calls, 1);
  assert.equal(result.source, "<svg/>");
  assert.equal(result.name, "art.svg");
});

test("cancelled pickers preserve the previous file handle", async () => {
  const handle = {
    getFile: async () => ({ name: "old.svg", size: 6, text: async () => "<svg/>" }),
  };
  const api = fixture({ showOpenFilePicker: async () => [handle] });
  await api.run(1, 0, "", "", false);
  api.acceptOpen(1, true);
  api.host.showOpenFilePicker = async () => {
    throw { name: "AbortError" };
  };
  assert.equal((await api.run(1, 0, "", "", false)).status, 1);
  assert.equal(api.sessions.get(1).handle, handle);
});

test("oversized files are rejected before their contents are read", async () => {
  let read = false;
  const api = fixture({
    showOpenFilePicker: async () => [{
      getFile: async () => ({
        size: 8 * 1024 * 1024 + 1,
        text: async () => {
          read = true;
          return "";
        },
      }),
    }],
  });
  assert.equal((await api.run(1, 0, "", "", false)).status, 2);
  assert.equal(read, false);
});

test("save chooses a destination before receiving the final source", async () => {
  const events = [];
  const handle = {
    name: "new.svg",
    createWritable: async () => ({
      write: async (source) => events.push(source),
      close: async () => events.push("closed"),
    }),
  };
  const api = fixture({
    showSaveFilePicker: async () => {
      events.push("picker");
      return handle;
    },
  });
  assert.equal((await api.run(1, 1, "new.svg", "", true)).status, 0);
  assert.deepEqual(events, ["picker"]);
  assert.equal((await api.run(1, 2, "", "<svg id='latest'/>", false)).status, 0);
  assert.deepEqual(events, ["picker", "<svg id='latest'/>", "closed"]);
  assert.equal(api.sessions.get(1).handle, handle);
});

test("failed writes abort without replacing the previous backing handle", async () => {
  let aborted = false;
  const api = fixture({
    showSaveFilePicker: async () => ({
      name: "new.svg",
      createWritable: async () => ({
        write: async () => {
          throw new Error("disk full");
        },
        close: async () => {
          assert.fail("must not commit failed writes");
        },
        abort: async () => {
          aborted = true;
        },
      }),
    }),
  });
  await api.run(1, 1, "new.svg", "", true);
  const result = await api.run(1, 2, "", "<svg/>", false);
  assert.equal(result.status, 2);
  assert.match(result.error, /disk full/);
  assert.equal(aborted, true);
  assert.equal(api.sessions.get(1).handle, null);
});

test("rejected SVG open does not replace the document backing", async () => {
  const old = {};
  const api = fixture({
    showOpenFilePicker: async () => [{
      getFile: async () => ({
        name: "invalid.svg",
        size: 7,
        text: async () => "not svg",
      }),
    }],
  });
  api.sessions.set(1, { handle: old });
  await api.run(1, 0, "", "", false);
  api.acceptOpen(1, false);
  assert.equal(api.sessions.get(1).handle, old);
});

test("export destination does not replace the document backing", async () => {
  const old = {};
  const api = fixture({
    showSaveFilePicker: async () => ({
      name: "export.svg",
      createWritable: async () => ({
        write: async () => {},
        close: async () => {},
      }),
    }),
  });
  api.sessions.set(1, { handle: old });
  await api.run(1, 3, "export.svg", "", true);
  assert.equal((await api.run(1, 2, "", "<svg/>", false)).status, 0);
  assert.equal(api.sessions.get(1).handle, old);
});

test("download fallback reports a copy without replacing the original handle", async () => {
  let downloads = 0;
  const original = {};
  const api = fixture();
  api.download = () => {
    ++downloads;
  };
  api.sessions.set(1, { handle: original });
  await api.run(1, 1, "copy.svg", "", true);
  const result = await api.run(1, 2, "", "<svg/>", false);
  assert.equal(result.status, 3);
  assert.equal(downloads, 1);
  assert.equal(api.sessions.get(1).handle, original);
});
