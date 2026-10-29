const fs = require("node:fs");
exports.APPLICATION_HEARTBEAT_LIMIT_MS = 5000;
exports.writeHeartbeat = function(file, value) {
  const temporary = `${file}.${process.pid}.pending`;
  fs.writeFileSync(temporary, JSON.stringify(value));
  fs.renameSync(temporary, file);
};

/** Marker file that exists while an application session owns the heartbeat. */
exports.applicationActiveMarker = (file) => `${file}.app-active`;

/**
 * Keep the heartbeat fresh while no application session owns it. The first call records the
 * launch phase. Later calls only refresh the file's modification time, which is the liveness the
 * watchdog reads outside an active session, and never rewrite its record: the test worker may
 * replace that record at any moment (a session's teardown, or the next case ending it), and a
 * rewrite from a stale read would restore an older phase.
 * @param {string} file Heartbeat file.
 */
exports.refreshIdleHeartbeat = function(file) {
  if (fs.existsSync(exports.applicationActiveMarker(file))) return;
  const now = new Date();
  try {
    fs.utimesSync(file, now, now);
  } catch (error) {
    if (error.code !== "ENOENT") throw error;
    exports.writeHeartbeat(file, { atMs: now.getTime(), phase: "launch" });
  }
};
