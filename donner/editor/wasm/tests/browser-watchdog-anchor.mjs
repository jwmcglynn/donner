import { spawn } from "node:child_process";
const ownerPid = process.ppid;
const terminateGroup = () => {
  try {
    process.kill(-process.pid, "SIGKILL");
  } catch (error) {
    if (error.code !== "ESRCH") throw error;
  }
};
process.once("disconnect", terminateGroup);
process.once("SIGTERM", terminateGroup);
process.once("SIGINT", terminateGroup);
setInterval(() => {
  if (process.ppid !== ownerPid) terminateGroup();
}, 100);
process.once("message", ({ executable, args, preload }) => {
  const child = spawn(executable, args, {
    detached: false,
    stdio: "inherit",
    env: { ...process.env, NODE_OPTIONS: `--require=${preload}` },
  });
  child.once(
    "error",
    (error) => process.send?.({ kind: "driver-exit", code: 127, error: error.message }),
  );
  child.once("exit", (code, signal) => process.send?.({ kind: "driver-exit", code, signal }));
});
process.send?.({ kind: "anchor-ready" });
