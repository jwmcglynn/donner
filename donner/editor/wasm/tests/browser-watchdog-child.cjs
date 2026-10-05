const childProcess = require("node:child_process");
const originalSpawn = childProcess.spawn;
const originalFork = childProcess.fork;
const originalKill = process.kill;
const children = new Map();
const group = Number(process.env.DONNER_WATCHDOG_GROUP);
if (!Number.isSafeInteger(group) || group <= 0) throw new Error("missing watchdog group");
const { processRows: rows } = require("./browser-process-snapshot.cjs");
function track(child) {
  const identity = rows().find((row) => row.pid === child.pid);
  if (identity) {
    if (identity.pgid !== group) throw new Error("child escaped watchdog group");
    children.set(child.pid, identity.start);
    child.once("exit", () => children.delete(child.pid));
  }
  return child;
}
// Keep ownership in the anchored group. Playwright still addresses each child as a process group;
// translate those requests to its verified subtree instead of signaling an unrelated group id.
process.kill = function(pid, signal) {
  if (!(pid < 0)) return originalKill.call(this, pid, signal);
  if (!children.has(-pid)) {
    const error = new Error("unknown child group");
    error.code = "ESRCH";
    throw error;
  }
  const snapshot = rows();
  const root = snapshot.find((row) => row.pid === -pid);
  if (!root || root.start !== children.get(-pid)) {
    const error = new Error("child identity expired");
    error.code = "ESRCH";
    throw error;
  }
  const selected = new Set([root.pid]);
  const subtree = [root];
  for (let index = 0; index < subtree.length; ++index) {
    for (const row of snapshot) {
      if (row.ppid === subtree[index].pid && !selected.has(row.pid)) {
        selected.add(row.pid);
        subtree.push(row);
      }
    }
  }
  for (const member of subtree.reverse()) {
    const current = rows().find((row) => row.pid === member.pid);
    if (!current || current.start !== member.start) continue;
    if (current.pgid !== group) throw new Error("child no longer belongs to watchdog group");
    try {
      originalKill.call(this, member.pid, signal);
    } catch (error) {
      if (error.code !== "ESRCH") throw error;
    }
  }
  return true;
};
childProcess.spawn = function(command, args, options) {
  if (!Array.isArray(args)) {
    options = args;
    args = [];
  }
  return track(originalSpawn.call(this, command, args, { ...options, detached: false }));
};
childProcess.fork = function(modulePath, args, options) {
  if (!Array.isArray(args)) {
    options = args;
    args = [];
  }
  return track(originalFork.call(this, modulePath, args, { ...options, detached: false }));
};
