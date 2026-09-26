/// Executes the former native SlugEndpointTest probes against production WGSL in Chromium.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createServer } from "node:http";
import { createRequire } from "node:module";
import { resolve } from "node:path";
import test from "node:test";

function requiredPath(name) {
  const value = process.env[name];
  assert.ok(value, `${name} must be set by Bazel`);
  return resolve(value);
}

const sources = JSON.parse(readFileSync(requiredPath("DONNER_SLUG_ENDPOINT_WGSL"), "utf8"));
const playwrightPath = resolve(
  requiredPath("TEST_SRCDIR"),
  process.env.TEST_WORKSPACE || "_main",
  "donner/editor/wasm/tests/node_modules/@playwright/test",
);
const playwright = createRequire(import.meta.url)(playwrightPath);

const shaders = ["TypedFill", "Fill", "Gradient", "Mask"];
const cases = [
  ["RetainsSharedEndpointCrossings", "endpoint"],
  ["PreservesInteriorAndRejectsOutsideOrInvalidSamples", "boundary"],
  ["RetainsFlatTangentCrossings", "flat"],
  ["NonzeroRayEvents", "nonzero"],
  ["BoundedRayEvents", "bounded"],
  ["RaySelection", "selection"],
];

async function openWebGpuPage(context) {
  const browser = await playwright.chromium.launch({
    channel: "chromium",
    headless: true,
    args: ["--enable-unsafe-webgpu", "--use-gl=angle"],
  });
  const server = createServer((_request, response) => {
    response.writeHead(200, { "Content-Type": "text/html" });
    response.end("<!doctype html><title>Slug endpoint WebGPU execution</title>");
  });
  await new Promise((ready) => server.listen(0, "127.0.0.1", ready));
  const page = await browser.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}/`, { waitUntil: "domcontentloaded" });
  await page.evaluate(async () => {
    if (!navigator.gpu) throw new Error("pinned Chromium has no WebGPU service");
    const adapter = await navigator.gpu.requestAdapter();
    if (!adapter) throw new Error("pinned Chromium found no WebGPU adapter");
    window.slugEndpointDevice = await adapter.requestDevice();
  });
  context.after(async () => {
    await page.evaluate(() => window.slugEndpointDevice?.destroy());
    await browser.close();
    await new Promise((closed) => server.close(closed));
  });
  return page;
}

/// Runs one named legacy test case. All arithmetic inputs are rounded to f32 before upload.
async function runCase(page, shader, category) {
  return page.evaluate(async ({ shader, category, base }) => {
    const device = window.slugEndpointDevice;
    const failures = [];
    let probes = 0;
    const pipelines = new Map();
    const isFill = shader === "Fill";
    const isTyped = shader === "TypedFill";

    function nextDownPositive(value) {
      const data = new DataView(new ArrayBuffer(4));
      data.setFloat32(0, value, true);
      data.setUint32(0, data.getUint32(0, true) - 1, true);
      return data.getFloat32(0, true);
    }

    function orient(input, transpose, reverse) {
      if (reverse) {
        for (let i = 0; i < input.curves.length; i += 6) {
          [input.curves[i], input.curves[i + 4]] = [input.curves[i + 4], input.curves[i]];
          [input.curves[i + 1], input.curves[i + 5]] = [input.curves[i + 5], input.curves[i + 1]];
        }
      }
      if (transpose) {
        [input.sample[0], input.sample[1]] = [input.sample[1], input.sample[0]];
        for (let i = 0; i < input.curves.length; i += 2) {
          [input.curves[i], input.curves[i + 1]] = [input.curves[i + 1], input.curves[i]];
        }
      }
      return input;
    }

    function boundaryInput(probe, transpose, reverse) {
      const single = probe !== "Endpoint" && probe !== "FlatEndpoint";
      const input = {
        curves: [
          623.76,
          226.98,
          625.67,
          225.365,
          627.58,
          223.75,
          627.56,
          223.75,
          625.095,
          224.725,
          622.63,
          225.7,
        ],
        sample: [540, 223.75],
        curveCount: single ? 1 : 2,
      };
      if (single && probe !== "OwnedEndpoint") {
        input.curves = [627, 223, 627, 224, 627, 225];
      }
      if (probe === "FlatEndpoint" || probe === "FlatOwnedEndpoint") {
        input.curves = [627, 4, 627, 0, 627, 0, 627, 0, 627, 0, 627, 4];
        input.sample[1] = 0;
      }
      if (["NearLinear", "LargeQuadratic", "SmallQuadratic"].includes(probe)) {
        const scale = Math.fround(
          probe === "LargeQuadratic"
            ? 1e30
            : probe === "SmallQuadratic"
            ? 1e-25
            : 1e-6,
        );
        input.curves = [627, 0, 627, scale, 627, 4 * scale];
        input.sample[1] = 3 * scale;
      } else if (probe === "BelowStart") {
        input.sample[1] = nextDownPositive(223);
      } else if (probe === "AtMaximum") {
        input.sample[1] = 225;
      } else if (probe === "InvalidControl") {
        input.curves[3] = Infinity;
        input.sample[1] = 223;
      } else if (probe === "InvalidSample") {
        input.sample[1] = NaN;
      }
      return orient(input, transpose, reverse);
    }

    function rayInput(events, transpose, reverse) {
      const input = { curves: [], sample: [0, 0], curveCount: events.length };
      for (const [distance, direction, finite = true] of events) {
        const x = distance - 0.5;
        input.curves.push(x, -direction, 0.5, finite ? 0 : Infinity, x, direction);
      }
      return orient(input, transpose, reverse);
    }

    async function pipelineFor(transpose, weight, body) {
      const key = `${transpose}/${weight}/${body}`;
      if (pipelines.has(key)) return pipelines.get(key);
      const functionName = transpose ? "accumulateVert" : "accumulateHoriz";
      const code =
        `${base}\n@group(1) @binding(0) var endpointResult: texture_storage_2d<rgba8unorm, write>;
@group(1) @binding(1) var<storage, read> endpointSamples: array<vec2f>;
@compute @workgroup_size(1)
fn endpoint_coverage() {
${isFill ? "  var paint: PaintParams;\n" : ""}
  let ray = ${functionName}(${isFill ? "paint, " : ""}0u, endpointSamples[0], 2.0, true);
${
          body
          || `  textureStore(endpointResult, vec2i(0), vec4f(abs(ray.cov), ${
            weight ? "ray.wgt" : "0.0"
          }, 0.0, 1.0));`
        }
}\n`;
      const module = device.createShaderModule({ code });
      const info = await module.getCompilationInfo();
      const errors = info.messages.filter((item) => item.type === "error")
        .map((item) => `${item.lineNum}:${item.linePos} ${item.message}`);
      if (errors.length) {
        throw new Error(`Slug ${shader} probe failed WGSL compilation: ${errors.join("; ")}`);
      }
      const pipeline = await device.createComputePipelineAsync({
        layout: "auto",
        compute: { module, entryPoint: "endpoint_coverage" },
      });
      pipelines.set(key, pipeline);
      return pipeline;
    }

    function storage(bytes) {
      const buffer = device.createBuffer({
        size: bytes.byteLength,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
      });
      device.queue.writeBuffer(buffer, 0, bytes);
      return buffer;
    }

    async function dispatch(input, transpose, weight = false, body = "") {
      const pipeline = await pipelineFor(transpose, weight, body);
      const band = storage(new Uint32Array([0, input.curveCount, 0, 0, 0, 0, 0, 0]));
      const curves = storage(new Float32Array(input.curves));
      const indices = storage(
        new Uint32Array(Array.from(
          { length: input.curves.length / 6 },
          (_, index) => index,
        )),
      );
      const sample = storage(new Float32Array(input.sample));
      const output = device.createTexture({
        size: [1, 1, 1],
        format: "rgba8unorm",
        usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.COPY_SRC,
      });
      const readback = device.createBuffer({
        size: 256,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
      });
      const bandBinding = transpose ? (isTyped || isFill ? 8 : 5) : 1;
      const geometryEntries = [
        { binding: bandBinding, resource: { buffer: band } },
        { binding: bandBinding + 1, resource: { buffer: curves } },
      ];
      if (!isTyped) {
        geometryEntries.push({
          binding: isFill ? 10 : transpose ? 10 : 9,
          resource: { buffer: indices },
        });
      }
      const geometry = device.createBindGroup({
        layout: pipeline.getBindGroupLayout(0),
        entries: geometryEntries,
      });
      const result = device.createBindGroup({
        layout: pipeline.getBindGroupLayout(1),
        entries: [
          { binding: 0, resource: output.createView() },
          { binding: 1, resource: { buffer: sample } },
        ],
      });
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginComputePass();
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, geometry);
      pass.setBindGroup(1, result);
      pass.dispatchWorkgroups(1);
      pass.end();
      encoder.copyTextureToBuffer(
        { texture: output },
        { buffer: readback, bytesPerRow: 256, rowsPerImage: 1 },
        [1, 1, 1],
      );
      device.queue.submit([encoder.finish()]);
      await readback.mapAsync(GPUMapMode.READ);
      const actual = Array.from(new Uint8Array(readback.getMappedRange()).slice(0, 4));
      readback.unmap();
      for (const resource of [band, curves, indices, sample, output, readback]) resource.destroy();
      return actual;
    }

    async function expect(input, transpose, reverse, expected, label, weight = false, body = "") {
      const actual = await dispatch(input, transpose, weight, body);
      probes++;
      if (actual.some((byte, index) => byte !== expected[index])) {
        failures.push({ label, transpose, reverse, expected, actual });
      }
    }

    async function orientations(makeInput, expected, label, weight = false, body = "") {
      for (const transpose of [false, true]) {
        for (const reverse of [false, true]) {
          await expect(
            makeInput(transpose, reverse),
            transpose,
            reverse,
            expected,
            label,
            weight,
            body,
          );
        }
      }
    }

    async function events(label, list, coverage, weight = 0, body = "") {
      await orientations(
        (transpose, reverse) => rayInput(list, transpose, reverse),
        [coverage, weight, 0, 255],
        label,
        true,
        body,
      );
    }

    if (category === "endpoint") {
      await orientations(
        (transpose, reverse) => boundaryInput("SingleCrossing", transpose, reverse),
        [255, 0, 0, 255],
        "single-crossing control",
      );
      await orientations((transpose, reverse) => boundaryInput("Endpoint", transpose, reverse), [
        0,
        0,
        0,
        255,
      ], "shared endpoint cancellation");
    } else if (category === "boundary") {
      for (
        const probe of [
          "NearLinear",
          "LargeQuadratic",
          "SmallQuadratic",
          "BelowStart",
          "AtMaximum",
          "InvalidControl",
          "InvalidSample",
          "OwnedEndpoint",
          "FlatOwnedEndpoint",
        ]
      ) {
        const crosses = [
          "NearLinear",
          "LargeQuadratic",
          "SmallQuadratic",
          "OwnedEndpoint",
          "FlatOwnedEndpoint",
        ].includes(probe);
        await orientations((transpose, reverse) => boundaryInput(probe, transpose, reverse), [
          crosses ? 255 : 0,
          0,
          0,
          255,
        ], probe);
      }
    } else if (category === "flat") {
      await orientations(
        (transpose, reverse) => boundaryInput("FlatEndpoint", transpose, reverse),
        [0, 0, 0, 255],
        "flat tangent cancellation",
      );
    } else if (category === "nonzero") {
      await events("overlap", [[0.25, 1], [0.25, 1], [-0.25, -1]], 191, 128);
      await events("internal edge", [[1, 1], [0, 1]], 255);
      await events("hole", [[1, 1], [0.25, -1], [-0.25, 1]], 128, 128);
      await events("cancelling tie", [[0.25, 1], [0.25, -1]], 0);
      await events("pixel boundary", [[0.5, 1], [-0.5, -1]], 255);
      await events("inside sign change", [[1, 1], [0, -1], [0, -1]], 255);
      await events(
        "unordered crossings",
        [[0.25, 1], [-0.25, -1], [0.125, 1], [0.375, -1]],
        128,
        191,
      );
    } else if (category === "bounded") {
      const positive = Array.from({ length: 16 }, () => [0.25, 1]);
      const negative = Array.from({ length: 16 }, () => [-0.25, -1]);
      await events("capacity", [...positive, ...negative], 128, 128);
      await events("below capacity", [...positive, ...negative.slice(1)], 191, 128);
      await events("overflow", [[0.25, 1], ...positive, ...negative], 255, 128);
      await events(
        "overflow continues scan",
        [
          ...Array.from({ length: 17 }, () => [0.25, 1]),
          ...Array.from({ length: 17 }, () => [0.25, -1]),
        ],
        0,
        128,
      );
    } else if (category === "selection") {
      const fallbackPair = `var other = empty_ray();
other.cov = 0.25;
other.wgt = 1.0;
other.legacyCov = select(-0.25, 0.25, ray.winding > 0.0);
other.legacyWgt = 0.5;
let combined = fill_coverage(ray, other, 0u, 1u);
textureStore(endpointResult, vec2i(0), vec4f(combined, 0.0, 0.0, 1.0));`;
      const positive = Array.from({ length: 16 }, () => [0.25, 1]);
      const negative = Array.from({ length: 17 }, () => [0.25, -1]);
      await events("paired overflow", [...positive, ...negative], 128, 0, fallbackPair);
      await events("paired late root", [...positive, ...negative, [-0.25, 1]], 96, 0, fallbackPair);
      await events("paired nonfinite", [[0.25, -1], [0, 1, false]], 128, 0, fallbackPair);
      const pair = [[0.25, 1], [0.25, 1]];
      for (
        const [label, coverage, fillRule, antialias] of [
          ["evenodd", 128, 1, 1],
          ["binary evenodd", 0, 1, 0],
          ["binary nonzero", 255, 0, 0],
        ]
      ) {
        await events(
          label,
          pair,
          coverage,
          0,
          `let combined = fill_coverage(ray, ray, ${fillRule}u, ${antialias}u);\n`
            + "textureStore(endpointResult, vec2i(0), vec4f(combined, 0.0, 0.0, 1.0));",
        );
      }
    } else {
      throw new Error(`unknown Slug endpoint category ${category}`);
    }
    return { failures, probes };
  }, { shader, category, base: sources[shader] });
}

test("pinned Chromium executes all 24 migrated Slug endpoint cases", async (context) => {
  assert.deepEqual(Object.keys(sources).sort(), [...shaders].sort());
  assert.ok(shaders.every((shader) => sources[shader].length > 0));
  assert.equal(shaders.length * cases.length, 24);
  const page = await openWebGpuPage(context);
  for (const shader of shaders) {
    for (const [suffix, category] of cases) {
      await context.test(`SlugEndpointTest.${shader}${suffix}`, async () => {
        const result = await runCase(page, shader, category);
        assert.ok(result.probes >= 4, "the case dispatched no meaningful GPU probes");
        assert.deepEqual(result.failures, []);
      });
    }
  }
  await context.test("invalid appended probe produces compiler diagnostics", async () => {
    const diagnostics = await page.evaluate(async (source) => {
      const module = window.slugEndpointDevice.createShaderModule({
        code: `${source}\n@compute @workgroup_size(1) fn broken( -> nonsense {}`,
      });
      const info = await module.getCompilationInfo();
      return info.messages.filter((item) => item.type === "error")
        .map((item) => `${item.lineNum}:${item.linePos} ${item.message}`);
    }, sources.Fill);
    assert.ok(
      diagnostics.length > 0 && diagnostics.every((item) => item.length > 3),
      "the invalid WGSL control had no actionable diagnostic",
    );
  });
});
