import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createServer } from "node:http";
import { createRequire } from "node:module";
import { resolve } from "node:path";
import test from "node:test";

function requiredPath(name) {
  const value = process.env[name];
  assert.ok(value, `${name} must be set by the Bazel test target`);
  return resolve(value);
}

const manifest = JSON.parse(readFileSync(requiredPath("DONNER_WGSL_MANIFEST"), "utf8"));
const mathFixture = JSON.parse(readFileSync(requiredPath("DONNER_MATH_FIXTURE"), "utf8"));
const build = readFileSync(requiredPath("DONNER_SHADER_BUILD"), "utf8");
const playwrightPath = resolve(
  requiredPath("TEST_SRCDIR"),
  process.env.TEST_WORKSPACE || "_main",
  "donner/editor/wasm/tests/node_modules/@playwright/test",
);
const playwright = createRequire(import.meta.url)(playwrightPath);

function shippedTargets() {
  return [...build.matchAll(/name = "([a-z0-9_]+_artifact)"/g)]
    .map((match) => match[1])
    .filter((name) => !name.endsWith("_native_artifact") && !name.endsWith("_test_artifact"))
    .sort();
}

async function openWebGpuPage(context) {
  const browser = await playwright.chromium.launch({
    channel: "chromium",
    headless: true,
    args: ["--enable-unsafe-webgpu", "--use-gl=angle"],
  });
  context.after(() => browser.close());
  const server = createServer((_request, response) => {
    response.writeHead(200, { "Content-Type": "text/html" });
    response.end("<!doctype html><title>WGSL validation</title>");
  });
  await new Promise((resolveReady) => server.listen(0, "127.0.0.1", resolveReady));
  context.after(() => server.close());
  const page = await browser.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}/`, { waitUntil: "domcontentloaded" });
  return page;
}

test("pinned Chromium compiles every shipped WGSL projection", async (context) => {
  assert.deepEqual(
    manifest.map((item) => item.artifact).sort(),
    shippedTargets(),
    "the browser must consume every production WGSL artifact",
  );
  assert.ok(manifest.length > 0);
  assert.ok(manifest.every((item) => typeof item.wgsl === "string" && item.wgsl.length > 0));

  const page = await openWebGpuPage(context);
  const result = await page.evaluate(async (items) => {
    if (!navigator.gpu) {
      throw new Error("the pinned Chromium profile has no WebGPU service");
    }
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) {
      throw new Error("the pinned Chromium profile found no WebGPU adapter");
    }
    const device = await adapter.requestDevice();
    const failures = [];
    for (const item of items) {
      const module = device.createShaderModule({ code: item.wgsl });
      const info = await module.getCompilationInfo();
      const errors = info.messages
        .filter((message) => message.type === "error")
        .map((message) => `${message.lineNum}:${message.linePos} ${message.message}`);
      if (errors.length > 0) {
        failures.push({ artifact: item.artifact, errors });
      }
    }

    const invalid = device.createShaderModule({
      code: "fn broken( -> nonsense { this is not wgsl }",
    });
    const invalidInfo = await invalid.getCompilationInfo();
    const invalidRejected = invalidInfo.messages.some((message) => message.type === "error");

    const compute = items.find((item) => item.artifact === "color_space_convert_artifact");
    if (!compute) {
      throw new Error("the compute mismatch control has no production artifact");
    }
    const computeModule = device.createShaderModule({ code: compute.wgsl });
    device.pushErrorScope("validation");
    let mismatchedEntryRejected = false;
    try {
      await device.createComputePipelineAsync({
        layout: "auto",
        compute: { module: computeModule, entryPoint: "missing_entry_point" },
      });
    } catch {
      mismatchedEntryRejected = true;
    }
    const mismatchError = await device.popErrorScope();
    mismatchedEntryRejected = mismatchedEntryRejected || mismatchError !== null;
    device.destroy();
    return { failures, invalidRejected, mismatchedEntryRejected };
  }, manifest);

  assert.deepEqual(result.failures, []);
  assert.equal(result.invalidRejected, true, "the invalid-WGSL control was accepted");
  assert.equal(result.mismatchedEntryRejected, true, "the wrong-entry control was accepted");
});

test("pinned Chromium executes round-half-away WGSL against CPU values", async (context) => {
  assert.ok(mathFixture.wgsl.length > 0);
  assert.equal(mathFixture.values.length % 8, 0);
  assert.ok(mathFixture.values.length > 0 && mathFixture.values.length * 4 <= 256);
  const page = await openWebGpuPage(context);
  const texels = await page.evaluate(async ({ wgsl, values: inputValues }) => {
    if (!navigator.gpu) {
      throw new Error("the pinned Chromium profile has no WebGPU service");
    }
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) {
      throw new Error("the pinned Chromium profile found no WebGPU adapter");
    }
    const device = await adapter.requestDevice();
    const module = device.createShaderModule({ code: wgsl });
    const info = await module.getCompilationInfo();
    if (info.messages.some((message) => message.type === "error")) {
      throw new Error("the test-only math shader failed browser compilation");
    }
    const pipeline = await device.createComputePipelineAsync({
      layout: "auto",
      compute: { module, entryPoint: "cs_main" },
    });
    const values = new Float32Array(inputValues);
    const input = device.createBuffer({
      size: values.byteLength,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
    });
    device.queue.writeBuffer(input, 0, values);
    const output = device.createTexture({
      size: { width: values.length, height: 1, depthOrArrayLayers: 1 },
      format: "rgba8unorm",
      usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.COPY_SRC,
    });
    const readback = device.createBuffer({
      size: 256,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    const group = device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: output.createView() },
        { binding: 1, resource: { buffer: input } },
      ],
    });
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginComputePass();
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, group);
    pass.dispatchWorkgroups(values.length / 8);
    pass.end();
    encoder.copyTextureToBuffer(
      { texture: output },
      { buffer: readback, bytesPerRow: 256, rowsPerImage: 1 },
      { width: values.length, height: 1, depthOrArrayLayers: 1 },
    );
    device.queue.submit([encoder.finish()]);
    await readback.mapAsync(GPUMapMode.READ);
    const bytes = Array.from(new Uint8Array(readback.getMappedRange()).slice(0, values.length * 4));
    readback.unmap();
    device.destroy();
    return bytes;
  }, mathFixture);

  assert.equal(texels.length, mathFixture.values.length * 4);
  for (const [index, raw] of mathFixture.values.entries()) {
    const value = Math.fround(raw);
    const offset = index * 4;
    const roundHalfAway = Math.sign(value) * Math.round(Math.abs(value));
    const sign = value > 0 ? 1 : value < 0 ? -1 : 0;
    const straight = Math.min(1, Math.max(0, value));
    const linearized = ((straight + 0.055) / 1.055) ** 2.4;
    const pow = 0.25 * (linearized + straight ** 2.4 + (1 - straight) ** 2.4);
    assert.equal(texels[offset], roundHalfAway + 128, `round-half-away at input ${index}`);
    assert.equal(texels[offset + 1], sign * 100 + 100, `sign at input ${index}`);
    assert.equal(texels[offset + 2], Math.floor(value * 0.25) + 128, `floor at input ${index}`);
    assert.ok(Math.abs(texels[offset + 3] - pow * 255) <= 2, `pow at input ${index}`);
  }
  const negativeHalfIndex = mathFixture.values.indexOf(-0.5);
  assert.ok(negativeHalfIndex >= 0);
  assert.notEqual(texels[negativeHalfIndex * 4], 128, "half-to-even rounding escaped detection");
});
