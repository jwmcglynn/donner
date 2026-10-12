export function createSurfaceProbeRegistry(answerMs) {
  const pages = new WeakMap();
  const pending = new WeakMap();
  const watched = new WeakSet();
  const closed = new WeakSet();
  const lateDiscovery = new WeakMap();

  function watch(worker) {
    if (watched.has(worker)) return;
    watched.add(worker);
    if (typeof worker.once === "function") {
      worker.once("close", () => {
        closed.add(worker);
        lateDiscovery.delete(worker);
      });
    }
  }
  function snapshot(page) {
    const current = page.workers().filter((worker) => !closed.has(worker));
    let entries = pages.get(page);
    if (entries === undefined) {
      entries = new Map();
      pages.set(page, entries);
    }
    for (const worker of entries.keys()) {
      if (!current.includes(worker)) entries.delete(worker);
    }
    return { current, entries };
  }
  function workers(page) {
    const { current, entries } = snapshot(page);
    return current.filter((worker) => entries.get(worker)?.installed === true);
  }
  async function request(worker, fn, boundMs, onReply, discovery = false) {
    if (closed.has(worker)) return null;
    const saved = lateDiscovery.get(worker);
    if (discovery && saved?.has(fn)) {
      const value = saved.get(fn);
      saved.delete(fn);
      return value;
    }
    if (pending.has(worker)) return null;
    watch(worker);
    let expired = false;
    let timer;
    const reply = Promise.resolve().then(() => worker.evaluate(fn)).catch(() => null).then(
      (value) => {
        if (closed.has(worker)) return null;
        onReply?.(value);
        if (expired && discovery && value !== null) {
          let saved = lateDiscovery.get(worker);
          if (saved === undefined) {
            saved = new Map();
            lateDiscovery.set(worker, saved);
          }
          saved.set(fn, value);
        }
        return value;
      },
    ).finally(() => pending.delete(worker));
    pending.set(worker, reply);
    const timedOut = new Promise((resolve) => {
      timer = setTimeout(() => {
        expired = true;
        resolve(null);
      }, boundMs);
    });
    return Promise.race([reply, timedOut]).finally(() => clearTimeout(timer));
  }
  function evaluate(current, fn, boundMs = answerMs) {
    return Promise.all(current.map((worker) => request(worker, fn, boundMs)));
  }
  async function install(page, fn) {
    const { current, entries } = snapshot(page);
    await Promise.all(current.map((worker) => {
      let entry = entries.get(worker);
      if (entry === undefined) {
        entry = { installed: null };
        entries.set(worker, entry);
      }
      if (entry.installed !== null) return null;
      return request(worker, fn, answerMs, (value) => {
        if (entries.get(worker) === entry && page.workers().includes(worker)) {
          entry.installed = typeof value === "boolean" ? value : null;
        }
      });
    }));
    return workers(page).length;
  }
  async function findOwner(page, fn, owns = (answer) => answer === true) {
    const current = workers(page);
    const answers = await Promise.all(
      current.map((worker) => request(worker, fn, answerMs, undefined, true)),
    );
    const live = workers(page);
    const owners = current.filter((worker, index) => live.includes(worker) && owns(answers[index]));
    return owners.length === 1 ? owners[0] : null;
  }
  return { evaluate, install, workers, findOwner };
}
