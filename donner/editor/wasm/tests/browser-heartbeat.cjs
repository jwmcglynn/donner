const fs = require("node:fs");
exports.APPLICATION_HEARTBEAT_LIMIT_MS = 5000;
exports.writeHeartbeat = function(file, value) {
  const temporary = `${file}.${process.pid}.pending`;
  fs.writeFileSync(temporary, JSON.stringify(value));
  fs.renameSync(temporary, file);
};
