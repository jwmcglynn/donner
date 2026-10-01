const childProcess = require("node:child_process");
const originalSpawn = childProcess.spawn;
const originalFork = childProcess.fork;
// Every test child remains in the supervisor's dedicated process group.
childProcess.spawn = function(command, args, options) {
  if (!Array.isArray(args)) {
    options = args;
    args = [];
  }
  return originalSpawn.call(this, command, args, { ...options, detached: false });
};
childProcess.fork = function(modulePath, args, options) {
  if (!Array.isArray(args)) {
    options = args;
    args = [];
  }
  return originalFork.call(this, modulePath, args, { ...options, detached: false });
};
