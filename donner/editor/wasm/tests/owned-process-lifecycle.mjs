/** Observe real process exit separately from errors that can leave the process alive. */
export function observeProcessExit(child, onError) {
  child.on("error", onError);
  return new Promise((resolve) => child.once("exit", resolve));
}

async function waitForExit(exited, timeoutMs) {
  let timer;
  try {
    return await Promise.race([
      exited.then(() => true),
      new Promise((resolve) => {
        timer = setTimeout(() => resolve(false), timeoutMs);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

/** Terminate only the supplied owned process, failing if its exit cannot be confirmed. */
export async function stopOwnedProcess(child, exited, graceMs = 5000) {
  if (!child.pid || child.exitCode !== null || child.signalCode !== null) return;
  child.kill("SIGTERM");
  if (await waitForExit(exited, graceMs)) return;
  child.kill("SIGKILL");
  if (!await waitForExit(exited, graceMs)) {
    throw new Error("owned process did not exit after SIGKILL");
  }
}
