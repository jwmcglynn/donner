import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import test from "node:test";
import { observeProcessExit, stopOwnedProcess } from "./owned-process-lifecycle.mjs";

function processStub() {
  return Object.assign(new EventEmitter(), { pid: 42, exitCode: null, signalCode: null });
}

test("a process error is observed without pretending the process exited", async () => {
  const child = processStub();
  const errors = [];
  const exited = observeProcessExit(child, (error) => errors.push(error));
  let finished = false;
  exited.then(() => {
    finished = true;
  });
  child.emit("error", new Error("kill refused"));
  await Promise.resolve();
  assert.equal(finished, false);
  assert.equal(errors.length, 1);
  child.exitCode = 0;
  child.emit("exit", 0);
  await exited;
  assert.equal(finished, true);
});

test("a failed graceful kill escalates and requires real exit", async () => {
  const child = processStub();
  const errors = [];
  const exited = observeProcessExit(child, (error) => errors.push(error));
  const signals = [];
  child.kill = (signal) => {
    signals.push(signal);
    if (signal === "SIGTERM") {
      child.emit("error", new Error("graceful termination refused"));
      return false;
    }
    child.signalCode = signal;
    child.emit("exit", null, signal);
    return true;
  };
  await stopOwnedProcess(child, exited, 1);
  assert.deepEqual(signals, ["SIGTERM", "SIGKILL"]);
  assert.equal(errors.length, 1);
});

test("missing exit confirmation fails cleanup instead of reporting success", async () => {
  const child = processStub();
  const exited = observeProcessExit(child, () => {});
  child.kill = () => true;
  await assert.rejects(stopOwnedProcess(child, exited, 1), /did not exit/);
});

test("spawn failures and already-exited processes require no kill", async () => {
  for (const values of [{ pid: undefined }, { exitCode: 0 }, { signalCode: "SIGTERM" }]) {
    const child = Object.assign(processStub(), values);
    child.kill = () => {
      throw new Error("unexpected kill");
    };
    await stopOwnedProcess(child, new Promise(() => {}), 1);
  }
});
