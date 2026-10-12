export function createSurfaceProbeRegistry(answerMs) {
  const probed = new WeakMap();
  async function evaluate(workers, fn, boundMs = answerMs) {
    return Promise.all(workers.map((worker) => {
      let timer;
      const timedOut = new Promise((resolve) => {
        timer = setTimeout(() => resolve(null), boundMs);
      });
      return Promise.race([worker.evaluate(fn).catch(() => null), timedOut]).finally(() =>
        clearTimeout(timer)
      );
    }));
  }
  const workers = (page) => probed.get(page) ?? [];
  async function install(page, fn) {
    const current = page.workers();
    const answers = await evaluate(current, fn);
    const installed = current.filter((_, index) => answers[index] === true);
    probed.set(page, installed);
    return installed.length;
  }
  async function findOwner(page, fn, owns = (answer) => answer === true) {
    const current = workers(page);
    const answers = await evaluate(current, fn);
    const owners = current.filter((_, index) => owns(answers[index]));
    return owners.length === 1 ? owners[0] : null;
  }
  return { evaluate, install, workers, findOwner };
}
