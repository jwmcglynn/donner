const { execFileSync } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

function darwinProcessRows() {
  const output = execFileSync("/bin/ps", ["-axo", "pid=,ppid=,pgid=,rss=,lstart=,comm="], {
    encoding: "utf8",
    timeout: 1000,
    maxBuffer: 4 * 1024 * 1024,
  });
  return output.trim().split("\n").map((line) => {
    const match = line.match(
      /^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+([A-Za-z]{3}\s+[A-Za-z]{3}\s+\d{1,2}\s+\d{2}:\d{2}:\d{2}\s+\d{4})\s+(.+)$/,
    );
    if (!match) throw new Error("unreadable process identity");
    return {
      pid: Number(match[1]),
      ppid: Number(match[2]),
      pgid: Number(match[3]),
      rssBytes: Number(match[4]) * 1024,
      start: match[5],
      command: path.basename(match[6]),
    };
  });
}

exports.processRows = function processRows({
  platform = process.platform,
  procRoot = "/proc",
  now = () => performance.now(),
  measurementBudgetMs = 1000,
  maxSnapshotBytes = 4 * 1024 * 1024,
  maxEntries = 65536,
} = {}) {
  if (platform !== "linux") return darwinProcessRows();
  const started = now();
  let entries = 0;
  let bytes = 0;
  const rows = [];
  const checkBudget = () => {
    if (now() - started > measurementBudgetMs) {
      throw new Error("process measurement deadline exceeded");
    }
  };
  const read = (file) => {
    checkBudget();
    const text = fs.readFileSync(file, "utf8");
    if (Buffer.byteLength(text) > 65536) throw new Error("process file byte limit exceeded");
    checkBudget();
    return text;
  };
  const parse = (raw, name) => {
    const begin = raw.indexOf("(");
    const end = raw.lastIndexOf(")");
    const fields = raw.slice(end + 1).trim().split(/\s+/);
    if (
      begin < 1 || end <= begin || fields.length < 22
      || Number(raw.slice(0, begin).trim()) !== Number(name) || !/^\d+$/.test(fields[19])
    ) {
      throw new Error("unreadable process identity");
    }
    return { fields, command: path.basename(raw.slice(begin + 1, end)) };
  };
  const listing = fs.opendirSync(procRoot);
  try {
    for (let entry = listing.readSync(); entry !== null; entry = listing.readSync()) {
      checkBudget();
      if (++entries > maxEntries) throw new Error("process snapshot entry limit exceeded");
      const name = entry.name;
      if (!/^\d+$/.test(name)) continue;
      const directory = path.join(procRoot, name);
      try {
        const before = parse(read(path.join(directory, "stat")), name);
        const status = read(path.join(directory, "status"));
        const after = parse(read(path.join(directory, "stat")), name);
        if (before.fields[19] !== after.fields[19]) continue;
        const fields = after.fields;
        const rss = status.match(/^VmRSS:\s+(\d+)\s+kB$/m);
        if (
          !rss && !["Z", "X"].includes(fields[0]) && fields[2] !== "0"
          && fs.statSync(directory).uid === process.getuid()
        ) {
          throw new Error(
            `unreadable process RSS (pid=${Number(name)} state=${
              /^[A-Za-z]$/.test(fields[0]) ? fields[0] : "invalid"
            } flags=${Number(fields[6])} threads=${Number(fields[17])})`,
          );
        }
        const row = {
          pid: Number(name),
          ppid: Number(fields[1]),
          pgid: Number(fields[2]),
          start: fields[19],
          rssBytes: rss ? Number(rss[1]) * 1024 : 0,
          command: after.command,
        };
        if (
          ![row.pid, row.ppid, row.pgid, row.rssBytes].every((value) =>
            Number.isSafeInteger(value) && value >= 0
          ) || row.pid === 0
        ) {
          throw new Error("unreadable process identity");
        }
        bytes += Buffer.byteLength(JSON.stringify(row));
        if (bytes > maxSnapshotBytes) throw new Error("process snapshot byte limit exceeded");
        rows.push(row);
      } catch (error) {
        if (["ENOENT", "ESRCH"].includes(error.code)) continue;
        if (error.code === "EACCES" && fs.statSync(directory).uid !== process.getuid()) continue;
        throw error;
      }
    }
  } finally {
    listing.closeSync();
  }
  checkBudget();
  return rows;
};
