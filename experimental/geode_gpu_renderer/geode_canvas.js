const GeodeSegmentKind = {
  Line: 0,
  Quadratic: 1,
};

async function loadShaderSource() {
  const response = await fetch('./shaders/geode_eval.wgsl');
  if (!response.ok) {
    throw new Error(`Failed to load shader: ${response.status}`);
  }
  return response.text();
}

function encodeGeodeSegments(segments) {
  const strideBytes = 32;
  const buffer = new ArrayBuffer(segments.length * strideBytes);
  const floats = new Float32Array(buffer);
  const uints = new Uint32Array(buffer);

  segments.forEach((seg, index) => {
    const base = (index * strideBytes) / 4;
    floats[base + 0] = seg.p0[0];
    floats[base + 1] = seg.p0[1];
    floats[base + 2] = seg.p1[0];
    floats[base + 3] = seg.p1[1];
    floats[base + 4] = seg.p2[0];
    floats[base + 5] = seg.p2[1];
    uints[base + 6] = seg.kind;
    uints[base + 7] = 0;
  });

  return buffer;
}

function computeBounds(segments) {
  let minX = Number.POSITIVE_INFINITY;
  let minY = Number.POSITIVE_INFINITY;
  let maxX = Number.NEGATIVE_INFINITY;
  let maxY = Number.NEGATIVE_INFINITY;

  segments.forEach((seg) => {
    const points = [seg.p0, seg.p1, seg.p2];
    points.forEach(([x, y]) => {
      minX = Math.min(minX, x);
      minY = Math.min(minY, y);
      maxX = Math.max(maxX, x);
      maxY = Math.max(maxY, y);
    });
  });

  return { min: [minX, minY], max: [maxX, maxY] };
}

function pathToSegments(commands) {
  const segments = [];
  let current = [0, 0];
  let subpathStart = [0, 0];

  commands.forEach((cmd) => {
    switch (cmd.type) {
      case 'moveTo':
        current = [cmd.x, cmd.y];
        subpathStart = [cmd.x, cmd.y];
        break;
      case 'lineTo':
        segments.push({
          p0: current,
          p1: [cmd.x, cmd.y],
          p2: [cmd.x, cmd.y],
          kind: GeodeSegmentKind.Line,
        });
        current = [cmd.x, cmd.y];
        break;
      case 'quadraticCurveTo':
        segments.push({
          p0: current,
          p1: [cmd.cpx, cmd.cpy],
          p2: [cmd.x, cmd.y],
          kind: GeodeSegmentKind.Quadratic,
        });
        current = [cmd.x, cmd.y];
        break;
      case 'closePath':
        segments.push({
          p0: current,
          p1: subpathStart,
          p2: subpathStart,
          kind: GeodeSegmentKind.Line,
        });
        current = subpathStart;
        break;
      default:
        throw new Error(`Unsupported path command: ${cmd.type}`);
    }
  });

  return segments;
}

function createFrameUniforms(device, boundsMin, boundsMax, viewport, segmentCount) {
  const uniformData = new Float32Array(12);
  uniformData[0] = boundsMin[0];
  uniformData[1] = boundsMin[1];
  uniformData[2] = boundsMax[0];
  uniformData[3] = boundsMax[1];
  uniformData[4] = viewport[0];
  uniformData[5] = viewport[1];
  const uint = new Uint32Array(uniformData.buffer);
  uint[6] = segmentCount;

  const uniformBuffer = device.createBuffer({
    size: uniformData.byteLength,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    mappedAtCreation: true,
  });
  new Float32Array(uniformBuffer.getMappedRange()).set(uniformData);
  uniformBuffer.unmap();
  return uniformBuffer;
}

class WebGpuGeodeCanvas {
  constructor(canvas, device, context, format, pipeline) {
    this.canvas = canvas;
    this.device = device;
    this.context = context;
    this.format = format;
    this.pipeline = pipeline;
    this.commands = [];
  }

  beginPath() {
    this.commands = [];
  }

  moveTo(x, y) {
    this.commands.push({ type: 'moveTo', x, y });
  }

  lineTo(x, y) {
    this.commands.push({ type: 'lineTo', x, y });
  }

  quadraticCurveTo(cpx, cpy, x, y) {
    this.commands.push({ type: 'quadraticCurveTo', cpx, cpy, x, y });
  }

  closePath() {
    this.commands.push({ type: 'closePath' });
  }

  async fill() {
    if (this.commands.length === 0) {
      return;
    }

    const segments = pathToSegments(this.commands);
    const bounds = computeBounds(segments);
    const geodeBufferData = encodeGeodeSegments(segments);

    const geodeBuffer = this.device.createBuffer({
      size: geodeBufferData.byteLength,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
      mappedAtCreation: true,
    });
    new Uint8Array(geodeBuffer.getMappedRange()).set(new Uint8Array(geodeBufferData));
    geodeBuffer.unmap();

    const uniformBuffer = createFrameUniforms(
      this.device,
      bounds.min,
      bounds.max,
      [this.canvas.width, this.canvas.height],
      segments.length,
    );

    const bindGroup = this.device.createBindGroup({
      layout: this.pipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: geodeBuffer } },
        { binding: 1, resource: { buffer: uniformBuffer } },
      ],
    });

    const encoder = this.device.createCommandEncoder();
    const view = this.context.getCurrentTexture().createView();
    const pass = encoder.beginRenderPass({
      colorAttachments: [
        {
          view,
          clearValue: { r: 0.04, g: 0.04, b: 0.08, a: 1.0 },
          loadOp: 'clear',
          storeOp: 'store',
        },
      ],
    });

    pass.setPipeline(this.pipeline);
    pass.setBindGroup(0, bindGroup);
    pass.draw(6, 1, 0, 0);
    pass.end();

    this.device.queue.submit([encoder.finish()]);
  }
}

export async function createGeodeCanvas(canvas) {
  if (!navigator.gpu) {
    throw new Error('WebGPU not available in this environment.');
  }

  const context = canvas.getContext('webgpu');
  if (!context) {
    throw new Error('Canvas does not support a WebGPU context.');
  }
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) {
    throw new Error('No WebGPU adapter found for Geode renderer.');
  }

  const device = await adapter.requestDevice();
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: 'premultiplied' });

  const code = await loadShaderSource();
  const module = device.createShaderModule({ code });
  const pipeline = device.createRenderPipeline({
    layout: 'auto',
    vertex: { module, entryPoint: 'vs_main' },
    fragment: { module, entryPoint: 'fs_main', targets: [{ format }] },
    primitive: { topology: 'triangle-list' },
  });

  return new WebGpuGeodeCanvas(canvas, device, context, format, pipeline);
}
