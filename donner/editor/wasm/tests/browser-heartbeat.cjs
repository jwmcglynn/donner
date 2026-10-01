const fs = require("node:fs");
exports.writeHeartbeat = function(file, value) {
  const temporary = `${file}.${process.pid}.pending`;
  fs.writeFileSync(temporary, JSON.stringify(value));
  fs.renameSync(temporary, file);
};
