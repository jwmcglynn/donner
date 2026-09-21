/**
 * The browser half of Donner's GPU bridge.
 *
 * It owns every browser GPU object the runtime has asked for and hands the runtime nothing but
 * integer identifiers for them. An identifier is looked up in one table that also records what
 * kind of object it names, so a call that presents an identifier of the wrong kind is refused here
 * rather than reaching a browser method that would interpret it as something else. Identifiers are
 * minted by the runtime from a space it never reuses, so an identifier that outlived its object
 * finds nothing here instead of finding the object that took its place.
 *
 * Each Emscripten worker has its own copy of this state, and a browser GPU device belongs to the
 * context that obtained it. A worker that never obtained a device therefore holds no device and
 * reports that through `donner_gpu_owns_device`, which is what makes the runtime's ownership check
 * a real boundary rather than a convention.
 *
 * One check is deliberately not mirrored here: the runtime refuses to destroy a texture that is
 * some surface's frame, because the canvas owns that texture. Answering the same question on this
 * side would need an index from texture to surface kept up to date on every acquisition, paid for
 * on every texture destroyed, so a caller that asks for a frame texture to be destroyed is
 * believed. Nothing the runtime issues does.
 *
 * Structured descriptors arrive one item at a time, matching how recorded commands are replayed:
 * nothing here decodes a packed buffer, so there is no length, offset or arity arithmetic to get
 * wrong. Every numeric code below is fixed for the life of the protocol. Two things hold this list
 * to the one `BrowserWireCodes.h` assigns: `BrowserWireCodes_tests.cc` reads this file and compares
 * the `protocolCodes` array below against the C++ table, and `donner_gpu_check_protocol` compares
 * them again at runtime before a device is requested. The first is what fails a build; a C++ test
 * that did not read this file could not see a change here at all.
 */

var LibraryDonnerGpu = {
  $DonnerGpu__deps: ['$UTF8ToString', '$stringToUTF8', '$lengthBytesUTF8'],
  $DonnerGpu: {
    // Protocol codes, assigned by BrowserWireCodes.h rather than by the underlying values of the
    // C++ enumerators. donner_gpu_check_protocol compares the whole table below against the one
    // the C++ half builds from those same assignments before a device is requested, so a code that
    // means one thing there and another here stops the request instead of reaching a browser call.
    kSuccess: 1,
    kUnknownObject: 2,
    kWrongObjectKind: 3,
    kNotOwner: 4,
    kDeviceLost: 5,
    kFailed: 6,

    kRequestPending: 1,
    kRequestReady: 2,
    kRequestUnavailable: 3,
    kRequestFailed: 4,

    kMapPending: 1,
    kMapReady: 2,
    kMapDeviceLost: 3,
    kMapFailed: 4,

    kSurfaceSuccess: 1,
    kSurfaceOutdated: 2,
    kSurfaceLost: 3,
    kSurfaceDeviceLost: 4,
    kSurfaceTimeout: 5,

    // Texture usage bits and canvas alpha modes by name. These are the numbers the table below
    // sends for those values, and naming them keeps an entry point that reports a usage or answers
    // a question about an alpha mode from spelling out a bit pattern a second time. What holds a
    // name to its number is the browser-lane test that reads both out of this file: the runtime
    // comparison checks the table's contents, which a name used in the wrong place would still
    // satisfy.
    kUsageRenderAttachment: 1,
    kUsageTextureBinding: 2,
    kUsageCopySrc: 4,
    kUsageCopyDst: 8,
    kUsageStorageBinding: 16,

    kAlphaModeOpaque: 1,
    kAlphaModePremultiplied: 2,
    kAlphaModeInherit: 3,

    kBuffer: 1,
    kTexture: 2,
    kTextureView: 3,
    kSampler: 4,
    kBindGroupLayout: 5,
    kBindGroup: 6,
    kPipelineLayout: 7,
    kShaderModule: 8,
    kRenderPipeline: 9,
    kComputePipeline: 10,
    kSurface: 11,
    kBufferMapping: 12,

    device: null,
    queue: null,
    requestState: 1,  // Pending.
    requestError: '',
    requested: false,
    lost: false,
    lostReason: '',
    completedSerial: 0,
    encoder: null,
    recordingSerial: 0,
    recordedBuffers: null,  // Finished command buffers of the open submission, in order.
    pass: null,
    attachments: null,
    pending: null,  // Descriptor being built by a sequence of item calls.
    objects: null,  // Map from identifier to { kind, object }.
    mappings: null,  // Map from identifier to { buffer, offset, size, state, view }.

    // The protocol table, in the order BrowserWireCodes.cc builds it. This is the artifact the two
    // halves agree on: donner_gpu_check_protocol compares it element by element against the C++
    // table before a device is requested, so appending on one side only, renumbering, or inserting
    // an enumerator anywhere fails the comparison instead of quietly changing what a number means.
    protocolCodes: [
      // TextureFormat, TextureUsage, BufferUsage, ShaderStage, ColorWriteMask.
      1, 2, 3, 4,
      1, 2, 4, 8, 16,
      1, 2, 4, 8, 16, 32, 64,
      1, 2, 4,
      1, 2, 4, 8,
      // FilterMode, AddressMode, VertexFormat, VertexStepMode, IndexFormat.
      1, 2,
      1, 2,
      1, 2, 3,
      1, 2,
      1, 2,
      // PrimitiveTopology, CullMode, BlendFactor, BlendOperation, BindingType.
      1, 2,
      1, 2,
      1, 2, 3, 4, 5,
      1, 2,
      1, 2, 3, 4, 5, 6,
      // LoadOp, StoreOp, PresentMode, SurfaceAlphaMode.
      1, 2,
      1, 2,
      1, 2, 3,
      1, 2, 3,
      // BrowserObjectKind.
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
      // BridgeStatus, BrowserDeviceRequestState, MapSliceState, SurfaceStatus.
      1, 2, 3, 4, 5, 6,
      1, 2, 3, 4,
      1, 2, 3, 4,
      1, 2, 3, 4, 5,
    ],

    ensureTables: function() {
      if (DonnerGpu.objects === null) {
        DonnerGpu.objects = new Map();
        DonnerGpu.mappings = new Map();
      }
    },

    // Everything below belongs to one browser device. This state is per context - each Emscripten
    // worker has its own copy of this library - so a second device obtained here would share one
    // object map with the first while each C++ side numbers its identifiers from one. Handing the
    // device back clears all of it, so a later bridge starts from nothing rather than inheriting a
    // lost flag, a completed serial ahead of its own submissions, or another device's objects.
    resetDeviceState: function() {
      DonnerGpu.device = null;
      DonnerGpu.queue = null;
      DonnerGpu.requestState = DonnerGpu.kRequestPending;
      DonnerGpu.requestError = '';
      DonnerGpu.requested = false;
      DonnerGpu.lost = false;
      DonnerGpu.lostReason = '';
      DonnerGpu.completedSerial = 0;
      DonnerGpu.encoder = null;
      DonnerGpu.recordingSerial = 0;
      DonnerGpu.recordedBuffers = null;
      DonnerGpu.pass = null;
      DonnerGpu.attachments = null;
      DonnerGpu.pending = null;
      DonnerGpu.objects = null;
      DonnerGpu.mappings = null;
    },

    // Resolves the canvas a selector names, from whichever context is asking.
    //
    // A worker has no document. A canvas reaches one by being transferred, and Emscripten records
    // transferred canvases under the bare id, so that registry is what a worker looks in. The
    // document lookup stays for the main thread.
    resolveCanvas: function(selector) {
      var id = selector.charAt(0) === '#' ? selector.substring(1) : selector;
      if (typeof GL !== 'undefined' && GL.offscreenCanvases && GL.offscreenCanvases[id]) {
        var registered = GL.offscreenCanvases[id];
        return registered.offscreenCanvas || registered;
      }
      if (typeof document !== 'undefined' && document.querySelector) {
        return document.querySelector(selector);
      }
      return null;
    },

    // Refuses anything once the device is gone or was never this context's to use. Every entry
    // point that can be refused starts here, so a lost device cannot be driven further by any path.
    guard: function() {
      if (DonnerGpu.device === null) {
        return DonnerGpu.kNotOwner;
      }
      if (DonnerGpu.lost) {
        return DonnerGpu.kDeviceLost;
      }
      return DonnerGpu.kSuccess;
    },

    // The check releases use. A lost device still has to free what it holds - refusing here would
    // strand every object it owns for the life of the page - so loss is not a reason to refuse,
    // while a context that does not own the device still is.
    guardRelease: function() {
      return DonnerGpu.device === null ? DonnerGpu.kNotOwner : DonnerGpu.kSuccess;
    },

    // Returns the object `id` names if it is of `kind`, otherwise null. The caller turns null into
    // the refusal its own bookkeeping calls for.
    lookup: function(kind, id) {
      DonnerGpu.ensureTables();
      var entry = DonnerGpu.objects.get(id);
      if (entry === undefined || entry.kind !== kind) {
        return null;
      }
      return entry.object;
    },

    // The refusal `id` earns: unknown when nothing holds it, wrong-kind when something else does.
    refusalFor: function(kind, id) {
      DonnerGpu.ensureTables();
      var entry = DonnerGpu.objects.get(id);
      if (entry === undefined) {
        return DonnerGpu.kUnknownObject;
      }
      return entry.kind === kind ? DonnerGpu.kSuccess : DonnerGpu.kWrongObjectKind;
    },

    register: function(kind, id, object) {
      DonnerGpu.ensureTables();
      if (id === 0 || DonnerGpu.objects.has(id)) {
        return DonnerGpu.kFailed;
      }
      DonnerGpu.objects.set(id, { kind: kind, object: object });
      return DonnerGpu.kSuccess;
    },

    // Runs `build` and registers what it produces. A browser that refuses to create the object
    // throws, and that becomes a refusal rather than an exception crossing back into wasm.
    create: function(kind, id, build) {
      var status = DonnerGpu.guard();
      if (status !== DonnerGpu.kSuccess) {
        return status;
      }
      // Check the identifier before building: an object built for an identifier already in use
      // could not be registered and nothing would hold it afterwards.
      DonnerGpu.ensureTables();
      if (id === 0 || DonnerGpu.objects.has(id)) {
        return DonnerGpu.kFailed;
      }
      var object;
      try {
        object = build();
      } catch (e) {
        return DonnerGpu.kFailed;
      }
      if (!object) {
        return DonnerGpu.kFailed;
      }
      return DonnerGpu.register(kind, id, object);
    },

    // Runs `act`, turning a browser-side throw into a refusal.
    perform: function(act) {
      var status = DonnerGpu.guard();
      if (status !== DonnerGpu.kSuccess) {
        return status;
      }
      try {
        var result = act();
        return result === undefined ? DonnerGpu.kSuccess : result;
      } catch (e) {
        return DonnerGpu.kFailed;
      }
    },

    writeMessage: function(text, destination, capacity) {
      if (!text || capacity <= 0) {
        return 0;
      }
      var needed = lengthBytesUTF8(text);
      var written = needed < capacity ? needed : capacity - 1;
      stringToUTF8(text, destination, capacity);
      return written;
    },

    textureFormat: function(code) {
      switch (code) {
        case 1: return 'rgba8unorm';
        case 2: return 'bgra8unorm';
        case 3: return 'r8unorm';
        case 4: return 'rgba32float';
        default: return null;
      }
    },

    formatCode: function(name) {
      switch (name) {
        case 'rgba8unorm': return 1;
        case 'bgra8unorm': return 2;
        case 'r8unorm': return 3;
        case 'rgba32float': return 4;
        default: return 0;
      }
    },

    // Each mask below is translated bit by bit rather than passed through, so the protocol's bit
    // assignment stays independent of the browser's.
    textureUsage: function(bits) {
      var usage = 0;
      if (bits & DonnerGpu.kUsageRenderAttachment) usage |= GPUTextureUsage.RENDER_ATTACHMENT;
      if (bits & DonnerGpu.kUsageTextureBinding) usage |= GPUTextureUsage.TEXTURE_BINDING;
      if (bits & DonnerGpu.kUsageCopySrc) usage |= GPUTextureUsage.COPY_SRC;
      if (bits & DonnerGpu.kUsageCopyDst) usage |= GPUTextureUsage.COPY_DST;
      if (bits & DonnerGpu.kUsageStorageBinding) usage |= GPUTextureUsage.STORAGE_BINDING;
      return usage;
    },

    bufferUsage: function(bits) {
      var usage = 0;
      if (bits & 1) usage |= GPUBufferUsage.VERTEX;
      if (bits & 2) usage |= GPUBufferUsage.INDEX;
      if (bits & 4) usage |= GPUBufferUsage.UNIFORM;
      if (bits & 8) usage |= GPUBufferUsage.STORAGE;
      if (bits & 16) usage |= GPUBufferUsage.COPY_SRC;
      if (bits & 32) usage |= GPUBufferUsage.COPY_DST;
      if (bits & 64) usage |= GPUBufferUsage.MAP_READ;
      return usage;
    },

    shaderStage: function(bits) {
      var visibility = 0;
      if (bits & 1) visibility |= GPUShaderStage.VERTEX;
      if (bits & 2) visibility |= GPUShaderStage.FRAGMENT;
      if (bits & 4) visibility |= GPUShaderStage.COMPUTE;
      return visibility;
    },

    colorWriteMask: function(bits) {
      var mask = 0;
      if (bits & 1) mask |= GPUColorWrite.RED;
      if (bits & 2) mask |= GPUColorWrite.GREEN;
      if (bits & 4) mask |= GPUColorWrite.BLUE;
      if (bits & 8) mask |= GPUColorWrite.ALPHA;
      return mask;
    },

    // Every decoder below returns null for a code this protocol does not assign, and every caller
    // refuses on null. Returning a particular value for an unknown code would turn a disagreement
    // between the two halves into a silently different pipeline, sampler or pass rather than a
    // refusal, which is the one outcome this boundary must not produce.
    filterMode: function(code) {
      switch (code) {
        case 1: return 'nearest';
        case 2: return 'linear';
        default: return null;
      }
    },
    addressMode: function(code) {
      switch (code) {
        case 1: return 'clamp-to-edge';
        case 2: return 'repeat';
        default: return null;
      }
    },
    vertexFormat: function(code) {
      switch (code) {
        case 1: return 'float32x2';
        case 2: return 'float32x4';
        case 3: return 'uint32';
        default: return null;
      }
    },
    stepMode: function(code) {
      switch (code) {
        case 1: return 'vertex';
        case 2: return 'instance';
        default: return null;
      }
    },
    indexFormat: function(code) {
      switch (code) {
        case 1: return 'uint16';
        case 2: return 'uint32';
        default: return null;
      }
    },
    topology: function(code) {
      switch (code) {
        case 1: return 'triangle-list';
        case 2: return 'triangle-strip';
        default: return null;
      }
    },
    cullMode: function(code) {
      switch (code) {
        case 1: return 'none';
        case 2: return 'back';
        default: return null;
      }
    },
    blendFactor: function(code) {
      switch (code) {
        case 1: return 'zero';
        case 2: return 'one';
        case 3: return 'src-alpha';
        case 4: return 'one-minus-src-alpha';
        case 5: return 'one-minus-dst-alpha';
        default: return null;
      }
    },
    blendOperation: function(code) {
      switch (code) {
        case 1: return 'add';
        case 2: return 'max';
        default: return null;
      }
    },
    loadOp: function(code) {
      switch (code) {
        case 1: return 'clear';
        case 2: return 'load';
        default: return null;
      }
    },
    storeOp: function(code) {
      switch (code) {
        case 1: return 'store';
        case 2: return 'discard';
        default: return null;
      }
    },
    alphaMode: function(code) {
      switch (code) {
        case DonnerGpu.kAlphaModeOpaque: return 'opaque';
        case DonnerGpu.kAlphaModePremultiplied: return 'premultiplied';
        // A surface that asks the platform for its default gets the one a canvas actually has.
        case DonnerGpu.kAlphaModeInherit: return 'opaque';
        default: return null;
      }
    },

    // Stops naming the frame `surface` took from its canvas, if it has one. The canvas owns the
    // texture, so letting go of the identifier is all there is to do; the browser shows the canvas
    // on its own schedule either way.
    releaseFrame: function(surface) {
      if (surface.frame !== null) {
        DonnerGpu.objects.delete(surface.frame);
        surface.frame = null;
      }
    },

    // Gives up everything a surface holds: the frame its canvas is still waiting to take back,
    // and the context configuration naming this device. A canvas outlives the surface over it, so
    // leaving it configured would keep a device the caller has finished with attached to the page.
    releaseSurface: function(surface) {
      DonnerGpu.releaseFrame(surface);
      surface.context.unconfigure();
    },

    // True when every value decoded is one this protocol assigns; a null among them means the
    // other half named something this one has no meaning for, which is refused rather than guessed.
    allDecoded: function(values) {
      for (var i = 0; i < values.length; ++i) {
        if (values[i] === null) {
          return false;
        }
      }
      return true;
    },

    bindGroupLayoutEntry: function(item) {
      var entry = { binding: item.binding, visibility: DonnerGpu.shaderStage(item.visibility) };
      switch (item.type) {
        case 1: entry.buffer = { type: 'uniform' }; break;
        case 2: entry.buffer = { type: 'read-only-storage' }; break;
        case 3: entry.texture = { sampleType: 'float' }; break;
        case 4: entry.sampler = { type: 'filtering' }; break;
        case 5:
          var storageFormat = DonnerGpu.textureFormat(item.storageFormat);
          if (storageFormat === null) {
            return null;
          }
          entry.storageTexture = { access: 'write-only', format: storageFormat };
          break;
        case 6: entry.texture = { sampleType: 'unfilterable-float' }; break;
        default: return null;
      }
      return entry;
    },
  },

  // ----- Protocol agreement -------------------------------------------------

  donner_gpu_check_protocol__deps: ['$DonnerGpu'],
  donner_gpu_check_protocol: function(codes, count) {
    if (count !== DonnerGpu.protocolCodes.length) {
      DonnerGpu.requestError = 'GPU bridge protocol table has ' + DonnerGpu.protocolCodes.length +
          ' entries and the module was built against ' + count;
      DonnerGpu.requestState = DonnerGpu.kRequestFailed;
      return DonnerGpu.kFailed;
    }
    for (var i = 0; i < count; ++i) {
      var expected = DonnerGpu.protocolCodes[i];
      var actual = HEAPU32[(codes >> 2) + i];
      if (actual !== expected) {
        DonnerGpu.requestError = 'GPU bridge protocol entry ' + i + ' is ' + actual +
            ' and this library assigns ' + expected;
        DonnerGpu.requestState = DonnerGpu.kRequestFailed;
        return DonnerGpu.kFailed;
      }
    }
    return DonnerGpu.kSuccess;
  },

  // ----- Device acquisition -------------------------------------------------

  donner_gpu_begin_device_request__deps: ['$DonnerGpu'],
  donner_gpu_begin_device_request: function() {
    // One device per context. A second one would share this context's object map with the first
    // while each C++ side numbers identifiers from one, so the two would collide or operate on
    // each other's objects; refusing is the only answer that keeps either of them coherent.
    if (DonnerGpu.requested) {
      DonnerGpu.requestError = 'this context has already requested a GPU bridge device';
      DonnerGpu.requestState = DonnerGpu.kRequestFailed;
      return DonnerGpu.kFailed;
    }
    DonnerGpu.requested = true;
    DonnerGpu.ensureTables();
    if (typeof navigator === 'undefined' || !navigator.gpu) {
      DonnerGpu.requestState = DonnerGpu.kRequestUnavailable;
      return DonnerGpu.kSuccess;
    }
    DonnerGpu.requestState = DonnerGpu.kRequestPending;
    navigator.gpu.requestAdapter()
      .then(function(adapter) {
        if (!adapter) {
          throw new Error('no GPU adapter is available');
        }
        return adapter.requestDevice();
      })
      .then(function(device) {
        DonnerGpu.device = device;
        DonnerGpu.queue = device.queue;
        // Loss is permanent, and the runtime refuses everything once it is observed, so the only
        // thing to do here is record it where the next call will see it.
        device.lost.then(function(info) {
          DonnerGpu.lost = true;
          DonnerGpu.lostReason = String(info.reason) + ': ' + String(info.message);
        });
        DonnerGpu.requestState = DonnerGpu.kRequestReady;
      })
      .catch(function(e) {
        DonnerGpu.requestError = String(e && e.message ? e.message : e);
        DonnerGpu.requestState = DonnerGpu.kRequestFailed;
      });
    return DonnerGpu.kSuccess;
  },

  donner_gpu_release_device__deps: ['$DonnerGpu'],
  donner_gpu_release_device: function() {
    DonnerGpu.resetDeviceState();
  },

  donner_gpu_device_request_state__deps: ['$DonnerGpu'],
  donner_gpu_device_request_state: function() { return DonnerGpu.requestState; },

  donner_gpu_read_request_error__deps: ['$DonnerGpu'],
  donner_gpu_read_request_error: function(destination, capacity) {
    return DonnerGpu.writeMessage(DonnerGpu.requestError, destination, capacity);
  },

  donner_gpu_owns_device__deps: ['$DonnerGpu'],
  donner_gpu_owns_device: function() { return DonnerGpu.device !== null ? 1 : 0; },

  donner_gpu_is_device_lost__deps: ['$DonnerGpu'],
  donner_gpu_is_device_lost: function() { return DonnerGpu.lost ? 1 : 0; },

  donner_gpu_read_lost_reason__deps: ['$DonnerGpu'],
  donner_gpu_read_lost_reason: function(destination, capacity) {
    return DonnerGpu.writeMessage(DonnerGpu.lostReason, destination, capacity);
  },

  donner_gpu_completed_serial__deps: ['$DonnerGpu'],
  donner_gpu_completed_serial: function() { return DonnerGpu.completedSerial; },

  // ----- Resource creation --------------------------------------------------

  donner_gpu_create_buffer__deps: ['$DonnerGpu'],
  donner_gpu_create_buffer: function(id, byteSize, usageBits) {
    return DonnerGpu.create(DonnerGpu.kBuffer, id, function() {
      return DonnerGpu.device.createBuffer({
        size: byteSize,
        usage: DonnerGpu.bufferUsage(usageBits),
      });
    });
  },

  donner_gpu_create_texture__deps: ['$DonnerGpu'],
  donner_gpu_create_texture: function(id, width, height, formatCode, usageBits) {
    var format = DonnerGpu.textureFormat(formatCode);
    if (format === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.create(DonnerGpu.kTexture, id, function() {
      return DonnerGpu.device.createTexture({
        size: { width: width, height: height, depthOrArrayLayers: 1 },
        format: format,
        usage: DonnerGpu.textureUsage(usageBits),
      });
    });
  },

  donner_gpu_create_texture_view__deps: ['$DonnerGpu'],
  donner_gpu_create_texture_view: function(id, textureId) {
    var texture = DonnerGpu.lookup(DonnerGpu.kTexture, textureId);
    if (texture === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kTexture, textureId);
    }
    return DonnerGpu.create(DonnerGpu.kTextureView, id, function() {
      return texture.createView();
    });
  },

  donner_gpu_create_sampler__deps: ['$DonnerGpu'],
  donner_gpu_create_sampler: function(id, magFilterCode, minFilterCode, addressUCode,
                                      addressVCode) {
    var magFilter = DonnerGpu.filterMode(magFilterCode);
    var minFilter = DonnerGpu.filterMode(minFilterCode);
    var addressU = DonnerGpu.addressMode(addressUCode);
    var addressV = DonnerGpu.addressMode(addressVCode);
    if (!DonnerGpu.allDecoded([magFilter, minFilter, addressU, addressV])) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.create(DonnerGpu.kSampler, id, function() {
      return DonnerGpu.device.createSampler({
        magFilter: magFilter,
        minFilter: minFilter,
        addressModeU: addressU,
        addressModeV: addressV,
      });
    });
  },

  donner_gpu_create_shader_module__deps: ['$DonnerGpu'],
  donner_gpu_create_shader_module: function(id, wgsl, byteCount) {
    var code = UTF8ToString(wgsl, byteCount);
    return DonnerGpu.create(DonnerGpu.kShaderModule, id, function() {
      return DonnerGpu.device.createShaderModule({ code: code });
    });
  },

  donner_gpu_destroy_object__deps: ['$DonnerGpu'],
  donner_gpu_destroy_object: function(kindCode, id) {
    var released = DonnerGpu.guardRelease();
    if (released !== DonnerGpu.kSuccess) {
      return released;
    }
    DonnerGpu.ensureTables();
    var refusal = DonnerGpu.refusalFor(kindCode, id);
    if (refusal !== DonnerGpu.kSuccess) {
      return refusal;
    }
    var object = DonnerGpu.objects.get(id).object;
    DonnerGpu.objects.delete(id);
    DonnerGpu.mappings.delete(id);
    // A surface holds no GPU allocation of its own. Buffers and textures do, and the browser will
    // not release those until asked; the remaining kinds go when the last reference to them does.
    try {
      if (kindCode === DonnerGpu.kSurface) {
        DonnerGpu.releaseSurface(object);
      } else if (object && typeof object.destroy === 'function') {
        object.destroy();
      }
    } catch (e) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.kSuccess;
  },

  // ----- Bind group layouts, bind groups and pipeline layouts ---------------

  donner_gpu_bind_group_layout_begin__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_layout_begin: function() {
    var status = DonnerGpu.guard();
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    DonnerGpu.pending = { entries: [] };
    return DonnerGpu.kSuccess;
  },

  donner_gpu_bind_group_layout_entry__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_layout_entry: function(binding, visibilityBits, bindingTypeCode,
                                               storageTextureFormat) {
    if (DonnerGpu.pending === null) {
      return DonnerGpu.kFailed;
    }
    var entry = DonnerGpu.bindGroupLayoutEntry({
      binding: binding,
      visibility: visibilityBits,
      type: bindingTypeCode,
      storageFormat: storageTextureFormat,
    });
    if (entry === null) {
      return DonnerGpu.kFailed;
    }
    DonnerGpu.pending.entries.push(entry);
    return DonnerGpu.kSuccess;
  },

  donner_gpu_bind_group_layout_finish__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_layout_finish: function(id) {
    var pending = DonnerGpu.pending;
    DonnerGpu.pending = null;
    if (pending === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.create(DonnerGpu.kBindGroupLayout, id, function() {
      return DonnerGpu.device.createBindGroupLayout({ entries: pending.entries });
    });
  },

  donner_gpu_bind_group_begin__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_begin: function(layoutId) {
    var layout = DonnerGpu.lookup(DonnerGpu.kBindGroupLayout, layoutId);
    if (layout === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kBindGroupLayout, layoutId);
    }
    var status = DonnerGpu.guard();
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    DonnerGpu.pending = { layout: layout, entries: [] };
    return DonnerGpu.kSuccess;
  },

  donner_gpu_bind_group_entry__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_entry: function(binding, resourceKindCode, resourceId, offsetBytes,
                                        sizeBytes) {
    if (DonnerGpu.pending === null) {
      return DonnerGpu.kFailed;
    }
    var resource;
    if (resourceKindCode === 0) {
      var buffer = DonnerGpu.lookup(DonnerGpu.kBuffer, resourceId);
      if (buffer === null) {
        return DonnerGpu.refusalFor(DonnerGpu.kBuffer, resourceId);
      }
      resource = { buffer: buffer, offset: offsetBytes, size: sizeBytes };
    } else if (resourceKindCode === 1) {
      resource = DonnerGpu.lookup(DonnerGpu.kTextureView, resourceId);
      if (resource === null) {
        return DonnerGpu.refusalFor(DonnerGpu.kTextureView, resourceId);
      }
    } else if (resourceKindCode === 2) {
      resource = DonnerGpu.lookup(DonnerGpu.kSampler, resourceId);
      if (resource === null) {
        return DonnerGpu.refusalFor(DonnerGpu.kSampler, resourceId);
      }
    } else {
      return DonnerGpu.kFailed;
    }
    DonnerGpu.pending.entries.push({ binding: binding, resource: resource });
    return DonnerGpu.kSuccess;
  },

  donner_gpu_bind_group_finish__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_finish: function(id) {
    var pending = DonnerGpu.pending;
    DonnerGpu.pending = null;
    if (pending === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.create(DonnerGpu.kBindGroup, id, function() {
      return DonnerGpu.device.createBindGroup({
        layout: pending.layout,
        entries: pending.entries,
      });
    });
  },

  donner_gpu_pipeline_layout_begin__deps: ['$DonnerGpu'],
  donner_gpu_pipeline_layout_begin: function() {
    var status = DonnerGpu.guard();
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    DonnerGpu.pending = { layouts: [] };
    return DonnerGpu.kSuccess;
  },

  donner_gpu_pipeline_layout_group__deps: ['$DonnerGpu'],
  donner_gpu_pipeline_layout_group: function(bindGroupLayoutId) {
    if (DonnerGpu.pending === null) {
      return DonnerGpu.kFailed;
    }
    var layout = DonnerGpu.lookup(DonnerGpu.kBindGroupLayout, bindGroupLayoutId);
    if (layout === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kBindGroupLayout, bindGroupLayoutId);
    }
    DonnerGpu.pending.layouts.push(layout);
    return DonnerGpu.kSuccess;
  },

  donner_gpu_pipeline_layout_finish__deps: ['$DonnerGpu'],
  donner_gpu_pipeline_layout_finish: function(id) {
    var pending = DonnerGpu.pending;
    DonnerGpu.pending = null;
    if (pending === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.create(DonnerGpu.kPipelineLayout, id, function() {
      return DonnerGpu.device.createPipelineLayout({ bindGroupLayouts: pending.layouts });
    });
  },

  // ----- Pipelines ----------------------------------------------------------

  donner_gpu_render_pipeline_begin__deps: ['$DonnerGpu'],
  donner_gpu_render_pipeline_begin: function(layoutId, vertexModuleId, vertexEntryPoint,
                                             vertexEntryPointBytes, fragmentModuleId,
                                             fragmentEntryPoint, fragmentEntryPointBytes,
                                             topologyCode, cullModeCode) {
    var layout = DonnerGpu.lookup(DonnerGpu.kPipelineLayout, layoutId);
    if (layout === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kPipelineLayout, layoutId);
    }
    var vertexModule = DonnerGpu.lookup(DonnerGpu.kShaderModule, vertexModuleId);
    if (vertexModule === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kShaderModule, vertexModuleId);
    }
    var fragmentModule = DonnerGpu.lookup(DonnerGpu.kShaderModule, fragmentModuleId);
    if (fragmentModule === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kShaderModule, fragmentModuleId);
    }
    var topology = DonnerGpu.topology(topologyCode);
    var cullMode = DonnerGpu.cullMode(cullModeCode);
    if (!DonnerGpu.allDecoded([topology, cullMode])) {
      return DonnerGpu.kFailed;
    }
    var guarded = DonnerGpu.guard();
    if (guarded !== DonnerGpu.kSuccess) {
      return guarded;
    }
    DonnerGpu.pending = {
      layout: layout,
      vertexModule: vertexModule,
      vertexEntryPoint: UTF8ToString(vertexEntryPoint, vertexEntryPointBytes),
      fragmentModule: fragmentModule,
      fragmentEntryPoint: UTF8ToString(fragmentEntryPoint, fragmentEntryPointBytes),
      topology: topology,
      cullMode: cullMode,
      buffers: [],
      targets: [],
    };
    return DonnerGpu.kSuccess;
  },

  donner_gpu_render_pipeline_vertex_buffer__deps: ['$DonnerGpu'],
  donner_gpu_render_pipeline_vertex_buffer: function(strideBytes, stepModeCode) {
    var stepMode = DonnerGpu.stepMode(stepModeCode);
    if (DonnerGpu.pending === null || stepMode === null) {
      return DonnerGpu.kFailed;
    }
    DonnerGpu.pending.buffers.push({
      arrayStride: strideBytes,
      stepMode: stepMode,
      attributes: [],
    });
    return DonnerGpu.kSuccess;
  },

  donner_gpu_render_pipeline_vertex_attribute__deps: ['$DonnerGpu'],
  donner_gpu_render_pipeline_vertex_attribute: function(formatCode, offsetBytes, shaderLocation) {
    // An attribute belongs to the buffer most recently described; arriving before any buffer means
    // the two sides disagree about the shape being built, which is refused rather than guessed at.
    var format = DonnerGpu.vertexFormat(formatCode);
    if (DonnerGpu.pending === null || DonnerGpu.pending.buffers.length === 0 || format === null) {
      return DonnerGpu.kFailed;
    }
    DonnerGpu.pending.buffers[DonnerGpu.pending.buffers.length - 1].attributes.push({
      format: format,
      offset: offsetBytes,
      shaderLocation: shaderLocation,
    });
    return DonnerGpu.kSuccess;
  },

  donner_gpu_render_pipeline_color_target__deps: ['$DonnerGpu'],
  donner_gpu_render_pipeline_color_target: function(formatCode, blendEnabled, colorSrcFactor,
                                                    colorDstFactor, colorOperation,
                                                    alphaSrcFactor, alphaDstFactor,
                                                    alphaOperation, writeMaskBits) {
    if (DonnerGpu.pending === null) {
      return DonnerGpu.kFailed;
    }
    var format = DonnerGpu.textureFormat(formatCode);
    if (format === null) {
      return DonnerGpu.kFailed;
    }
    var target = { format: format, writeMask: DonnerGpu.colorWriteMask(writeMaskBits) };
    if (blendEnabled) {
      var colorSrc = DonnerGpu.blendFactor(colorSrcFactor);
      var colorDst = DonnerGpu.blendFactor(colorDstFactor);
      var colorOp = DonnerGpu.blendOperation(colorOperation);
      var alphaSrc = DonnerGpu.blendFactor(alphaSrcFactor);
      var alphaDst = DonnerGpu.blendFactor(alphaDstFactor);
      var alphaOp = DonnerGpu.blendOperation(alphaOperation);
      if (!DonnerGpu.allDecoded([colorSrc, colorDst, colorOp, alphaSrc, alphaDst, alphaOp])) {
        return DonnerGpu.kFailed;
      }
      target.blend = {
        color: { srcFactor: colorSrc, dstFactor: colorDst, operation: colorOp },
        alpha: { srcFactor: alphaSrc, dstFactor: alphaDst, operation: alphaOp },
      };
    }
    DonnerGpu.pending.targets.push(target);
    return DonnerGpu.kSuccess;
  },

  donner_gpu_render_pipeline_finish__deps: ['$DonnerGpu'],
  donner_gpu_render_pipeline_finish: function(id) {
    var pending = DonnerGpu.pending;
    DonnerGpu.pending = null;
    if (pending === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.create(DonnerGpu.kRenderPipeline, id, function() {
      return DonnerGpu.device.createRenderPipeline({
        layout: pending.layout,
        vertex: {
          module: pending.vertexModule,
          entryPoint: pending.vertexEntryPoint,
          buffers: pending.buffers,
        },
        fragment: {
          module: pending.fragmentModule,
          entryPoint: pending.fragmentEntryPoint,
          targets: pending.targets,
        },
        primitive: { topology: pending.topology, cullMode: pending.cullMode },
      });
    });
  },

  donner_gpu_create_compute_pipeline__deps: ['$DonnerGpu'],
  donner_gpu_create_compute_pipeline: function(id, layoutId, moduleId, entryPoint,
                                               entryPointBytes) {
    var layout = DonnerGpu.lookup(DonnerGpu.kPipelineLayout, layoutId);
    if (layout === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kPipelineLayout, layoutId);
    }
    var module = DonnerGpu.lookup(DonnerGpu.kShaderModule, moduleId);
    if (module === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kShaderModule, moduleId);
    }
    var name = UTF8ToString(entryPoint, entryPointBytes);
    return DonnerGpu.create(DonnerGpu.kComputePipeline, id, function() {
      return DonnerGpu.device.createComputePipeline({
        layout: layout,
        compute: { module: module, entryPoint: name },
      });
    });
  },

  // ----- Queue writes -------------------------------------------------------

  donner_gpu_write_buffer__deps: ['$DonnerGpu'],
  donner_gpu_write_buffer: function(bufferId, offsetBytes, data, byteCount) {
    var buffer = DonnerGpu.lookup(DonnerGpu.kBuffer, bufferId);
    if (buffer === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kBuffer, bufferId);
    }
    return DonnerGpu.perform(function() {
      // The browser copies during writeBuffer, so a view of the wasm heap is safe to hand over
      // even though the heap can move afterwards.
      DonnerGpu.queue.writeBuffer(buffer, offsetBytes,
                                  HEAPU8.subarray(data, data + byteCount));
    });
  },

  donner_gpu_write_texture__deps: ['$DonnerGpu'],
  donner_gpu_write_texture: function(textureId, data, byteCount, layoutOffsetBytes, bytesPerRow,
                                     rowsPerImage, destinationX, destinationY, width, height) {
    var texture = DonnerGpu.lookup(DonnerGpu.kTexture, textureId);
    if (texture === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kTexture, textureId);
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.queue.writeTexture(
        { texture: texture, origin: { x: destinationX, y: destinationY, z: 0 } },
        HEAPU8.subarray(data, data + byteCount),
        { offset: layoutOffsetBytes, bytesPerRow: bytesPerRow, rowsPerImage: rowsPerImage },
        { width: width, height: height, depthOrArrayLayers: 1 });
    });
  },

  // ----- Command recording --------------------------------------------------

  donner_gpu_begin_command_buffer__deps: ['$DonnerGpu'],
  donner_gpu_begin_command_buffer: function(submissionSerial, commandBufferIndex) {
    // A later buffer must continue the submission the first one opened, and the two halves must
    // agree on how many buffers have been finished under it; anything else means they disagree
    // about what is being recorded, so nothing is kept.
    if (commandBufferIndex !== 0 &&
        (DonnerGpu.recordingSerial !== submissionSerial || DonnerGpu.recordedBuffers === null ||
         DonnerGpu.recordedBuffers.length !== commandBufferIndex)) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      // A recording left open by a submission that was refused partway is dropped here rather
      // than continued, so nothing recorded before the refusal can reach the queue. The first
      // buffer of a submission starts the list empty, which drops whatever an earlier attempt
      // finished but never submitted - a refused submission keeps its serial, so the retry
      // arrives under the same one.
      DonnerGpu.pass = null;
      DonnerGpu.attachments = null;
      if (commandBufferIndex === 0) {
        DonnerGpu.recordedBuffers = [];
      }
      // Drop the old encoder before asking for a new one. If that ask throws, what is left behind
      // is nothing rather than the previous recording sitting under the new serial.
      DonnerGpu.encoder = null;
      DonnerGpu.recordingSerial = submissionSerial;
      DonnerGpu.encoder = DonnerGpu.device.createCommandEncoder();
    });
  },

  donner_gpu_begin_render_pass__deps: ['$DonnerGpu'],
  donner_gpu_begin_render_pass: function() {
    if (DonnerGpu.encoder === null) {
      return DonnerGpu.kFailed;
    }
    var status = DonnerGpu.guard();
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    DonnerGpu.attachments = [];
    return DonnerGpu.kSuccess;
  },

  donner_gpu_render_pass_attachment__deps: ['$DonnerGpu'],
  donner_gpu_render_pass_attachment: function(viewId, loadOpCode, storeOpCode, clearRed,
                                              clearGreen, clearBlue, clearAlpha) {
    if (DonnerGpu.attachments === null) {
      return DonnerGpu.kFailed;
    }
    var view = DonnerGpu.lookup(DonnerGpu.kTextureView, viewId);
    if (view === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kTextureView, viewId);
    }
    var loadOp = DonnerGpu.loadOp(loadOpCode);
    var storeOp = DonnerGpu.storeOp(storeOpCode);
    if (!DonnerGpu.allDecoded([loadOp, storeOp])) {
      return DonnerGpu.kFailed;
    }
    DonnerGpu.attachments.push({
      view: view,
      loadOp: loadOp,
      storeOp: storeOp,
      clearValue: { r: clearRed, g: clearGreen, b: clearBlue, a: clearAlpha },
    });
    return DonnerGpu.kSuccess;
  },

  donner_gpu_begin_render_pass_finish__deps: ['$DonnerGpu'],
  donner_gpu_begin_render_pass_finish: function() {
    var attachments = DonnerGpu.attachments;
    DonnerGpu.attachments = null;
    if (attachments === null || DonnerGpu.encoder === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.pass = DonnerGpu.encoder.beginRenderPass({ colorAttachments: attachments });
    });
  },

  donner_gpu_end_render_pass__deps: ['$DonnerGpu'],
  donner_gpu_end_render_pass: function() {
    if (DonnerGpu.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.pass.end();
      DonnerGpu.pass = null;
    });
  },

  donner_gpu_begin_compute_pass__deps: ['$DonnerGpu'],
  donner_gpu_begin_compute_pass: function() {
    if (DonnerGpu.encoder === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.pass = DonnerGpu.encoder.beginComputePass();
    });
  },

  donner_gpu_end_compute_pass__deps: ['$DonnerGpu'],
  donner_gpu_end_compute_pass: function() {
    if (DonnerGpu.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.pass.end();
      DonnerGpu.pass = null;
    });
  },

  donner_gpu_set_render_pipeline__deps: ['$DonnerGpu'],
  donner_gpu_set_render_pipeline: function(pipelineId) {
    var pipeline = DonnerGpu.lookup(DonnerGpu.kRenderPipeline, pipelineId);
    if (pipeline === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kRenderPipeline, pipelineId);
    }
    if (DonnerGpu.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() { DonnerGpu.pass.setPipeline(pipeline); });
  },

  donner_gpu_set_compute_pipeline__deps: ['$DonnerGpu'],
  donner_gpu_set_compute_pipeline: function(pipelineId) {
    var pipeline = DonnerGpu.lookup(DonnerGpu.kComputePipeline, pipelineId);
    if (pipeline === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kComputePipeline, pipelineId);
    }
    if (DonnerGpu.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() { DonnerGpu.pass.setPipeline(pipeline); });
  },

  donner_gpu_set_bind_group__deps: ['$DonnerGpu'],
  donner_gpu_set_bind_group: function(index, bindGroupId) {
    var bindGroup = DonnerGpu.lookup(DonnerGpu.kBindGroup, bindGroupId);
    if (bindGroup === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kBindGroup, bindGroupId);
    }
    if (DonnerGpu.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() { DonnerGpu.pass.setBindGroup(index, bindGroup); });
  },

  donner_gpu_set_vertex_buffer__deps: ['$DonnerGpu'],
  donner_gpu_set_vertex_buffer: function(slot, bufferId, offsetBytes) {
    var buffer = DonnerGpu.lookup(DonnerGpu.kBuffer, bufferId);
    if (buffer === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kBuffer, bufferId);
    }
    if (DonnerGpu.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.pass.setVertexBuffer(slot, buffer, offsetBytes);
    });
  },

  donner_gpu_set_index_buffer__deps: ['$DonnerGpu'],
  donner_gpu_set_index_buffer: function(bufferId, indexFormatCode, offsetBytes) {
    var buffer = DonnerGpu.lookup(DonnerGpu.kBuffer, bufferId);
    if (buffer === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kBuffer, bufferId);
    }
    var indexFormat = DonnerGpu.indexFormat(indexFormatCode);
    if (DonnerGpu.pass === null || indexFormat === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.pass.setIndexBuffer(buffer, indexFormat, offsetBytes);
    });
  },

  donner_gpu_set_scissor_rect__deps: ['$DonnerGpu'],
  donner_gpu_set_scissor_rect: function(x, y, width, height) {
    if (DonnerGpu.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() { DonnerGpu.pass.setScissorRect(x, y, width, height); });
  },

  donner_gpu_set_viewport__deps: ['$DonnerGpu'],
  donner_gpu_set_viewport: function(x, y, width, height, minDepth, maxDepth) {
    if (DonnerGpu.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.pass.setViewport(x, y, width, height, minDepth, maxDepth);
    });
  },

  donner_gpu_draw__deps: ['$DonnerGpu'],
  donner_gpu_draw: function(vertexCount, instanceCount, firstVertex, firstInstance) {
    if (DonnerGpu.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.pass.draw(vertexCount, instanceCount, firstVertex, firstInstance);
    });
  },

  donner_gpu_draw_indexed__deps: ['$DonnerGpu'],
  donner_gpu_draw_indexed: function(indexCount, instanceCount, firstIndex, baseVertex,
                                    firstInstance) {
    if (DonnerGpu.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.pass.drawIndexed(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
    });
  },

  donner_gpu_dispatch_workgroups__deps: ['$DonnerGpu'],
  donner_gpu_dispatch_workgroups: function(countX, countY, countZ) {
    if (DonnerGpu.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.pass.dispatchWorkgroups(countX, countY, countZ);
    });
  },

  donner_gpu_copy_texture_to_buffer__deps: ['$DonnerGpu'],
  donner_gpu_copy_texture_to_buffer: function(textureId, bufferId, layoutOffsetBytes, bytesPerRow,
                                              rowsPerImage, width, height) {
    var texture = DonnerGpu.lookup(DonnerGpu.kTexture, textureId);
    if (texture === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kTexture, textureId);
    }
    var buffer = DonnerGpu.lookup(DonnerGpu.kBuffer, bufferId);
    if (buffer === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kBuffer, bufferId);
    }
    if (DonnerGpu.encoder === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.encoder.copyTextureToBuffer(
        { texture: texture },
        { buffer: buffer, offset: layoutOffsetBytes, bytesPerRow: bytesPerRow,
          rowsPerImage: rowsPerImage },
        { width: width, height: height, depthOrArrayLayers: 1 });
    });
  },

  donner_gpu_copy_texture_to_texture__deps: ['$DonnerGpu'],
  donner_gpu_copy_texture_to_texture: function(sourceTextureId, destinationTextureId, sourceX,
                                               sourceY, destinationX, destinationY, width,
                                               height) {
    var source = DonnerGpu.lookup(DonnerGpu.kTexture, sourceTextureId);
    if (source === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kTexture, sourceTextureId);
    }
    var destination = DonnerGpu.lookup(DonnerGpu.kTexture, destinationTextureId);
    if (destination === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kTexture, destinationTextureId);
    }
    if (DonnerGpu.encoder === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      DonnerGpu.encoder.copyTextureToTexture(
        { texture: source, origin: { x: sourceX, y: sourceY, z: 0 } },
        { texture: destination, origin: { x: destinationX, y: destinationY, z: 0 } },
        { width: width, height: height, depthOrArrayLayers: 1 });
    });
  },

  donner_gpu_end_command_buffer__deps: ['$DonnerGpu'],
  donner_gpu_end_command_buffer: function(submissionSerial) {
    // The serial that opened the recording is the one that must close it. A mismatch means the two
    // halves disagree about which submission this encoder belongs to, so nothing is kept.
    if (DonnerGpu.encoder === null || DonnerGpu.recordingSerial !== submissionSerial) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      var commandBuffer = DonnerGpu.encoder.finish();
      DonnerGpu.encoder = null;
      DonnerGpu.recordedBuffers.push(commandBuffer);
    });
  },

  donner_gpu_submit_command_buffers__deps: ['$DonnerGpu'],
  donner_gpu_submit_command_buffers: function(submissionSerial) {
    // Every buffer of this submission must be finished and belong to this serial: an open encoder
    // or a serial that never opened one means the two halves disagree about what is being
    // submitted, and a submission with no buffers names no work to complete.
    if (DonnerGpu.encoder !== null || DonnerGpu.recordingSerial !== submissionSerial ||
        DonnerGpu.recordedBuffers === null || DonnerGpu.recordedBuffers.length === 0) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      // One queue submission for the whole list, in recording order, so the buffers execute in
      // that order and the submission completes once.
      var commandBuffers = DonnerGpu.recordedBuffers;
      DonnerGpu.recordedBuffers = [];
      DonnerGpu.recordingSerial = 0;
      DonnerGpu.queue.submit(commandBuffers);
      DonnerGpu.queue.onSubmittedWorkDone().then(function() {
        // Submissions complete in order, but the serial is recorded defensively as a maximum so a
        // completion observed out of order can never move the reported serial backwards.
        if (submissionSerial > DonnerGpu.completedSerial) {
          DonnerGpu.completedSerial = submissionSerial;
        }
      });
    });
  },

  // ----- Host mapping -------------------------------------------------------

  donner_gpu_map_buffer_async__deps: ['$DonnerGpu'],
  donner_gpu_map_buffer_async: function(mappingId, bufferId, offsetBytes, byteCount) {
    var buffer = DonnerGpu.lookup(DonnerGpu.kBuffer, bufferId);
    if (buffer === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kBuffer, bufferId);
    }
    var status = DonnerGpu.guard();
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    var record =
        { buffer: buffer, offset: offsetBytes, size: byteCount, state: DonnerGpu.kMapPending, view: null };
    DonnerGpu.ensureTables();
    var registered = DonnerGpu.register(DonnerGpu.kBufferMapping, mappingId, record);
    if (registered !== DonnerGpu.kSuccess) {
      return registered;
    }
    DonnerGpu.mappings.set(mappingId, record);
    try {
      buffer.mapAsync(GPUMapMode.READ, offsetBytes, byteCount)
        .then(function() {
          record.view = new Uint8Array(buffer.getMappedRange(offsetBytes, byteCount));
          record.state = DonnerGpu.kMapReady;
        })
        .catch(function() {
          record.state = DonnerGpu.lost ? DonnerGpu.kMapDeviceLost : DonnerGpu.kMapFailed;
        });
    } catch (e) {
      record.state = DonnerGpu.kMapFailed;
    }
    return DonnerGpu.kSuccess;
  },

  donner_gpu_mapping_state__deps: ['$DonnerGpu'],
  donner_gpu_mapping_state: function(mappingId) {
    DonnerGpu.ensureTables();
    // Loss outranks whatever the mapping last recorded: it can never complete afterwards, and
    // reporting it as still pending would leave the caller waiting out its whole budget.
    if (DonnerGpu.lost) {
      return DonnerGpu.kMapDeviceLost;
    }
    var record = DonnerGpu.mappings.get(mappingId);
    return record === undefined ? DonnerGpu.kMapFailed : record.state;
  },

  donner_gpu_copy_mapped_bytes__deps: ['$DonnerGpu'],
  donner_gpu_copy_mapped_bytes: function(mappingId, destination, byteCount) {
    DonnerGpu.ensureTables();
    var record = DonnerGpu.mappings.get(mappingId);
    if (record === undefined) {
      var refusal = DonnerGpu.refusalFor(DonnerGpu.kBufferMapping, mappingId);
      return refusal === DonnerGpu.kSuccess ? DonnerGpu.kFailed : refusal;
    }
    if (record.state !== DonnerGpu.kMapReady || record.view === null ||
        record.view.length < byteCount) {
      return DonnerGpu.kFailed;
    }
    HEAPU8.set(record.view.subarray(0, byteCount), destination);
    return DonnerGpu.kSuccess;
  },

  donner_gpu_unmap_buffer__deps: ['$DonnerGpu'],
  donner_gpu_unmap_buffer: function(mappingId) {
    var released = DonnerGpu.guardRelease();
    if (released !== DonnerGpu.kSuccess) {
      return released;
    }
    DonnerGpu.ensureTables();
    var refusal = DonnerGpu.refusalFor(DonnerGpu.kBufferMapping, mappingId);
    if (refusal !== DonnerGpu.kSuccess) {
      return refusal;
    }
    var record = DonnerGpu.mappings.get(mappingId);
    DonnerGpu.objects.delete(mappingId);
    DonnerGpu.mappings.delete(mappingId);
    if (record === undefined) {
      return DonnerGpu.kSuccess;
    }
    // The view aliases memory the browser is about to take back, so it is dropped before the
    // unmap rather than left reachable.
    record.view = null;
    try {
      record.buffer.unmap();
    } catch (e) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.kSuccess;
  },

  // ----- Presentation -------------------------------------------------------

  donner_gpu_create_surface__deps: ['$DonnerGpu'],
  donner_gpu_create_surface: function(id, canvasSelector, selectorBytes) {
    var selector = UTF8ToString(canvasSelector, selectorBytes);
    return DonnerGpu.create(DonnerGpu.kSurface, id, function() {
      var canvas = DonnerGpu.resolveCanvas(selector);
      if (!canvas) {
        return null;
      }
      var context = canvas.getContext('webgpu');
      if (!context) {
        return null;
      }
      return { canvas: canvas, context: context, frame: null };
    });
  },

  donner_gpu_surface_capabilities__deps: ['$DonnerGpu'],
  donner_gpu_surface_capabilities: function(surfaceId, preferredFormatCode, usageBits) {
    var surface = DonnerGpu.lookup(DonnerGpu.kSurface, surfaceId);
    if (surface === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kSurface, surfaceId);
    }
    return DonnerGpu.perform(function() {
      var preferred = navigator.gpu.getPreferredCanvasFormat();
      HEAPU32[preferredFormatCode >> 2] = DonnerGpu.formatCode(preferred);
      // A canvas texture is a render attachment and can be copied from; the rest of the usage
      // vocabulary does not apply to one.
      HEAPU32[usageBits >> 2] = DonnerGpu.kUsageRenderAttachment | DonnerGpu.kUsageCopySrc;
    });
  },

  donner_gpu_surface_supports_alpha_mode__deps: ['$DonnerGpu'],
  donner_gpu_surface_supports_alpha_mode: function(surfaceId, alphaModeCode, supported) {
    var surface = DonnerGpu.lookup(DonnerGpu.kSurface, surfaceId);
    if (surface === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kSurface, surfaceId);
    }
    return DonnerGpu.perform(function() {
      // What a canvas context can be configured with is exactly what this library has a canvas
      // value for, so the decoder that configure uses is what answers here. Reporting an alpha
      // mode configure would then refuse is the one answer this has to avoid.
      HEAPU32[supported >> 2] = DonnerGpu.alphaMode(alphaModeCode) === null ? 0 : 1;
    });
  },

  donner_gpu_configure_surface__deps: ['$DonnerGpu'],
  donner_gpu_configure_surface: function(surfaceId, formatCode, usageBits, width, height,
                                         alphaModeCode) {
    var surface = DonnerGpu.lookup(DonnerGpu.kSurface, surfaceId);
    if (surface === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kSurface, surfaceId);
    }
    var format = DonnerGpu.textureFormat(formatCode);
    var alphaMode = DonnerGpu.alphaMode(alphaModeCode);
    if (!DonnerGpu.allDecoded([format, alphaMode])) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(function() {
      surface.canvas.width = width;
      surface.canvas.height = height;
      surface.context.configure({
        device: DonnerGpu.device,
        format: format,
        usage: DonnerGpu.textureUsage(usageBits),
        alphaMode: alphaMode,
      });
      DonnerGpu.releaseFrame(surface);
    });
  },

  donner_gpu_acquire_current_texture__deps: ['$DonnerGpu'],
  donner_gpu_acquire_current_texture: function(surfaceId, textureId, surfaceStatusCode) {
    var surface = DonnerGpu.lookup(DonnerGpu.kSurface, surfaceId);
    if (surface === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kSurface, surfaceId);
    }
    var status = DonnerGpu.guard();
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    var texture;
    try {
      texture = surface.context.getCurrentTexture();
    } catch (e) {
      // A canvas whose context has gone away reports itself lost; a new surface is the recovery,
      // which is what Lost tells the caller.
      HEAPU32[surfaceStatusCode >> 2] = DonnerGpu.kSurfaceLost;
      return DonnerGpu.kSuccess;
    }
    if (!texture) {
      // No frame is available right now; retrying is the recovery, which is what Timeout says.
      HEAPU32[surfaceStatusCode >> 2] = DonnerGpu.kSurfaceTimeout;
      return DonnerGpu.kSuccess;
    }
    if (surface.frame !== null) {
      // A canvas holds one frame at a time, and the identifier naming it is still live. Taking a
      // second one would leave nothing able to name the first, so this is refused rather than
      // quietly replacing it; the runtime refuses it above here too.
      return DonnerGpu.kFailed;
    }
    var registered = DonnerGpu.register(DonnerGpu.kTexture, textureId, texture);
    if (registered !== DonnerGpu.kSuccess) {
      return registered;
    }
    surface.frame = textureId;
    HEAPU32[surfaceStatusCode >> 2] = DonnerGpu.kSurfaceSuccess;
    return DonnerGpu.kSuccess;
  },

  donner_gpu_abandon_current_texture__deps: ['$DonnerGpu'],
  donner_gpu_abandon_current_texture: function(surfaceId) {
    var released = DonnerGpu.guardRelease();
    if (released !== DonnerGpu.kSuccess) {
      return released;
    }
    var surface = DonnerGpu.lookup(DonnerGpu.kSurface, surfaceId);
    if (surface === null) {
      return DonnerGpu.refusalFor(DonnerGpu.kSurface, surfaceId);
    }
    DonnerGpu.releaseFrame(surface);
    return DonnerGpu.kSuccess;
  },
};

mergeInto(LibraryManager.library, LibraryDonnerGpu);
