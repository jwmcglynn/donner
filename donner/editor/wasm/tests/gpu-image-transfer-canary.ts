import { expect, type Page } from "@playwright/test";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { PNG } from "pngjs";

interface TransferSample {
  sequence: number;
  width: number;
  height: number;
  stage: string;
  renderToImageMs?: number;
  transferToImageBitmapMs?: number;
  relayMs?: number;
  importMs?: number;
  sourceReadbacks?: number;
  actual?: number[];
  expected?: number[];
  error?: string;
}

/** Exercise the platform transport separately from production renderer timing. */
export async function inspectGpuImageTransfer(
  page: Page,
  outputDirectory: string,
  completionProgress: boolean = false,
) {
  fs.mkdirSync(outputDirectory, { recursive: true });
  const saved = new Map<number, Record<string, unknown>>();
  await page.exposeFunction("__donnerSaveGpuTransferCanarySample", (sample: TransferSample) => {
    if (!Number.isInteger(sample.sequence) || sample.sequence < 0 || sample.sequence >= 15) {
      throw new Error("invalid GPU transfer sample sequence");
    }
    const large = sample.sequence >= 12;
    if (sample.width !== (large ? 2560 : 32) || sample.height !== (large ? 1800 : 24)) {
      throw new Error("invalid GPU transfer sample dimensions");
    }
    const { actual, expected, ...timing } = sample;
    const record: Record<string, unknown> = { ...timing, pixelChecked: false };
    for (const [name, data] of [["actual", actual], ["expected", expected]] as const) {
      if (data === undefined) continue;
      if (large || data.length !== 32 * 24 * 4) throw new Error("invalid canary pixel extent");
      const file = path.join(outputDirectory, `${sample.sequence}-${name}.png`);
      fs.writeFileSync(file, PNG.sync.write({ width: 32, height: 24, data: Buffer.from(data) }));
      record[`${name}Path`] = file;
    }
    saved.set(sample.sequence, record);
    fs.writeFileSync(
      path.join(outputDirectory, "samples.json"),
      JSON.stringify(Array.from(saved.values()), null, 2),
    );
  });
  const failure = await page.evaluate(async (completionProgress) => {
    const script = `
      let device, canvas, context, pipeline, readbacks = 0;
      let current = {};
      const clock = () => performance.timeOrigin + performance.now();
      const timed = (promise, stage) => {
        current.stage = stage;
        let timer;
        return Promise.race([
          promise,
          new Promise((_, reject) => timer = setTimeout(() => reject(new Error(stage + ' timed out')), 5000))
        ]).finally(() => clearTimeout(timer));
      };
      const waitForCompletion = (stage, progress) => {
        const done = device.queue.onSubmittedWorkDone();
        let interval;
        if (progress) interval = setInterval(() => {
          device.queue.submit([]);
          const key = stage === 'source completion' ? 'sourceProgressRequests' : 'receiverProgressRequests';
          current[key] = (current[key] || 0) + 1;
        }, 8);
        return timed(done, stage).finally(() => clearInterval(interval));
      };
      const initialize = async () => {
        if (device) return;
        const adapter = await timed(navigator.gpu.requestAdapter(), 'adapter');
        if (!adapter) throw new Error('GPU adapter unavailable');
        device = await timed(adapter.requestDevice(), 'device');
        const shader = device.createShaderModule({code: \`
          @vertex fn canaryVertex(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
            let vertices = array<vec2f, 3>(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));
            return vec4f(vertices[index], 0, 1);
          }
          @fragment fn canaryFragment() -> @location(0) vec4f { return vec4f(0.1, 0.2, 0.05, 0.25); }
        \`});
        pipeline = device.createRenderPipeline({layout: 'auto',
          vertex: {module: shader, entryPoint: 'canaryVertex'},
          fragment: {module: shader, entryPoint: 'canaryFragment', targets: [{format: 'rgba8unorm'}]}});
      };
      const draw = (texture, color, pattern) => {
        const encoder = device.createCommandEncoder();
        const pass = encoder.beginRenderPass({colorAttachments: [{view: texture.createView(),
          loadOp: 'clear', storeOp: 'store', clearValue: color}]});
        if (pattern) {
          pass.setPipeline(pipeline);
          pass.setScissorRect(3, 5, 11, 7);
          pass.draw(3);
        }
        pass.end();
        device.queue.submit([encoder.finish()]);
      };
      const read = async texture => {
        ++readbacks;
        const buffer = device.createBuffer({size: 256 * 24,
          usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ});
        try {
          const encoder = device.createCommandEncoder();
          encoder.copyTextureToBuffer({texture}, {buffer, bytesPerRow: 256, rowsPerImage: 24},
            {width: 32, height: 24});
          device.queue.submit([encoder.finish()]);
          await timed(buffer.mapAsync(GPUMapMode.READ), 'test-only readback');
          const padded = new Uint8Array(buffer.getMappedRange());
          const pixels = new Uint8Array(32 * 24 * 4);
          for (let row = 0; row < 24; ++row) pixels.set(padded.subarray(row * 256, row * 256 + 128), row * 128);
          buffer.unmap();
          return Array.from(pixels);
        } finally { buffer.destroy(); }
      };
      onmessage = async ({data}) => {
        current = {sequence: data.sequence, width: data.width, height: data.height, stage: 'start'};
        let actual, expected, bitmap;
        try {
          await initialize();
          device.pushErrorScope('validation');
          if (data.role === 'source') {
            if (!canvas) {
              canvas = new OffscreenCanvas(data.width, data.height);
              context = canvas.getContext('webgpu');
            }
            current.stage = 'source configure';
            canvas.width = data.width;
            canvas.height = data.height;
            context.configure({device, format: 'rgba8unorm', alphaMode: 'premultiplied'});
            current.stage = 'source render';
            const start = clock();
            draw(context.getCurrentTexture(), data.color, data.pattern);
            await waitForCompletion('source completion', data.completionProgress);
            const bitmapStart = clock();
            bitmap = canvas.transferToImageBitmap();
            current.transferToImageBitmapMs = clock() - bitmapStart;
            current.renderToImageMs = clock() - start;
            current.sourceReadbacks = readbacks;
          } else {
            bitmap = data.bitmap;
            const arrived = clock();
            if (bitmap.width !== data.width || bitmap.height !== data.height) throw new Error('image extent mismatch');
            current.relayMs = arrived - data.sent;
            const descriptor = {size: {width: data.width, height: data.height}, format: 'rgba8unorm',
              usage: GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC | GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING};
            actual = device.createTexture(descriptor);
            device.queue.copyExternalImageToTexture({source: bitmap},
              {texture: actual, premultipliedAlpha: true, colorSpace: 'srgb'},
              {width: data.width, height: data.height});
            bitmap.close();
            bitmap = undefined;
            await waitForCompletion('receiver completion', data.completionProgress);
            current.importMs = clock() - arrived;
            if (data.width === 32) {
              expected = device.createTexture(descriptor);
              draw(expected, data.color, data.pattern);
              current.actual = await read(actual);
              current.expected = await read(expected);
            }
          }
          const validation = await timed(device.popErrorScope(), 'validation');
          if (validation) throw new Error(validation.message);
          current.stage = 'complete';
          if (data.role === 'source') current.sent = clock();
          postMessage({...current, bitmap}, bitmap ? [bitmap] : []);
          bitmap = undefined;
        } catch (error) {
          postMessage({...current, error: String(error)});
        } finally {
          bitmap?.close();
          actual?.destroy();
          expected?.destroy();
        }
      };
    `;
    const url = URL.createObjectURL(new Blob([script], { type: "text/javascript" }));
    const workers: Worker[] = [];
    const save = (sample: TransferSample) =>
      (window as unknown as {
        __donnerSaveGpuTransferCanarySample: (sample: TransferSample) => Promise<void>;
      }).__donnerSaveGpuTransferCanarySample(sample);
    const ask = (worker: Worker, data: object, transfers: Transferable[] = []) =>
      new Promise<TransferSample & { bitmap?: ImageBitmap; sent?: number }>((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error("worker reply timed out")), 5500);
        worker.onerror = (event) => {
          clearTimeout(timer);
          reject(new Error(event.message));
        };
        worker.onmessage = ({ data: result }) => {
          clearTimeout(timer);
          resolve(result);
        };
        worker.postMessage(data, transfers);
      });
    let current: TransferSample = { sequence: 0, width: 32, height: 24, stage: "setup" };
    let heldBitmap: ImageBitmap | undefined;
    try {
      workers.push(new Worker(url));
      workers.push(new Worker(url));
      const colors = [
        { r: 0.8, g: 0.1, b: 0.1, a: 1 },
        { r: 0.1, g: 0.25, b: 0.05, a: 0.5 },
        { r: 0, g: 0, b: 0, a: 0 },
        { r: 0.125, g: 0.25, b: 0.5, a: 0.75 },
      ];
      for (let sequence = 0; sequence < 15; ++sequence) {
        const width = sequence >= 12 ? 2560 : 32;
        const height = sequence >= 12 ? 1800 : 24;
        const color = colors[sequence % colors.length];
        const pattern = sequence % 4 === 3;
        current = { sequence, width, height, stage: "source request" };
        await save(current);
        const source = await ask(workers[0], {
          role: "source",
          completionProgress,
          sequence,
          width,
          height,
          color,
          pattern,
        });
        heldBitmap = source.bitmap;
        if (source.sequence !== sequence) throw new Error("source sequence mismatch");
        const { bitmap, ...sourceTiming } = source;
        current = {
          ...current,
          ...sourceTiming,
          stage: source.error ? source.stage : "receiver request",
        };
        if (source.error) throw new Error(source.error);
        if (!bitmap) throw new Error("source produced no image");
        const receiving = ask(workers[1], {
          role: "receiver",
          completionProgress,
          sequence,
          width,
          height,
          color,
          pattern,
          bitmap,
          sent: source.sent,
        }, [bitmap]);
        heldBitmap = undefined;
        const [, receiver] = await Promise.all([save(current), receiving]);
        if (receiver.sequence !== sequence) throw new Error("receiver sequence mismatch");
        current = { ...current, ...receiver };
        await save(current);
        if (receiver.error) throw new Error(receiver.error);
      }
      return null;
    } catch (error) {
      current.error = String(error);
      await save(current);
      return current.error;
    } finally {
      heldBitmap?.close();
      for (const worker of workers) worker.terminate();
      URL.revokeObjectURL(url);
    }
  }, completionProgress);
  expect(failure, "GPU transfer must complete every bounded stage").toBeNull();
  const comparator = process.env.DONNER_BROWSER_GOLDEN_COMPARE;
  expect(comparator, "Bazel must provide the shared pixelmatch comparator").toBeTruthy();
  const comparatorEnvironment = { ...process.env };
  delete comparatorEnvironment.UPDATE_GOLDEN_IMAGES_DIR;
  delete comparatorEnvironment.DONNER_BROWSER_COMPARE_MODE;
  for (const sample of saved.values()) {
    if (!sample.actualPath || !sample.expectedPath) continue;
    execFileSync(path.resolve(comparator!), [], {
      env: {
        ...comparatorEnvironment,
        DONNER_ACTUAL_PNG: String(sample.actualPath),
        DONNER_GOLDEN_PNG: String(sample.expectedPath),
      },
      stdio: "inherit",
      timeout: 10000,
    });
    sample.pixelChecked = true;
  }
  const samples = Array.from(saved.values());
  fs.writeFileSync(path.join(outputDirectory, "samples.json"), JSON.stringify(samples, null, 2));
  expect(samples.filter((sample) => sample.pixelChecked)).toHaveLength(12);
  return samples;
}
