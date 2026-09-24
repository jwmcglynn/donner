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
 * context that obtained it. A worker holds at most one browser device, and several logical devices
 * over it: one per runtime device, such as a renderer and the snapshot capture context that reads
 * its textures back on the same thread. Every entry point names its logical device first, by a
 * handle the runtime mints from a space shared by every worker and never reuses. A logical device
 * keeps its own identifiers, host mappings, recording and completed serial, and shares the browser
 * device, its queue and its loss with the others; the first to ask begins the browser's request
 * and the rest join it, and none of them can refuse, overwrite or release another's state. A
 * handle this worker never opened owns nothing here, which is what makes the runtime's ownership
 * check a real boundary rather than a convention.
 *
 * A texture one logical device shares is registered on another as an alias of the same browser
 * texture. The share holds it: the producer releasing its identifier does not destroy a texture a
 * share still holds, and releasing the share destroys it then if the producer already has. Only the
 * logical device that made a share releases it, and a share names the browser device it was made
 * on. The runtime hands a release made on another thread to the one that made the share, so a
 * share is released here when its last holder goes. Shares also go with their browser device,
 * which destroys the textures they still hold, so a share whose owning thread has exited keeps its
 * texture only until then, and a later device finds nothing under its number.
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

    // The browser device this worker holds, and everything that belongs to it rather than to one
    // logical device over it: its queue, the request that obtained it, and its loss.
    device: null,
    queue: null,
    requestStarted: false,
    // Which request is current. A request that settles after the device it was for has been let
    // go must not install a device into state that has moved on.
    requestGeneration: 0,
    requestState: 1,  // Pending.
    requestError: '',
    lost: false,
    lostReason: '',
    // The handle of the logical device whose request obtained the browser device. Handles are
    // unique across workers and never reused, so this names the device across workers as well.
    deviceIdentity: 0,

    logical: null,  // Map from logical-device handle to its own state; see openLogical.
    // Map from share identifier to the texture it holds; see donner_gpu_share_texture.
    shares: null,
    nextShare: 1,

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
      if (DonnerGpu.logical === null) {
        DonnerGpu.logical = new Map();
        DonnerGpu.shares = new Map();
      }
    },

    // The state of the logical device `handle`, or null for one this worker never opened. A handle
    // from another worker finds nothing here, which is the ownership refusal.
    logicalFor: function(handle) {
      DonnerGpu.ensureTables();
      var record = DonnerGpu.logical.get(handle);
      return record === undefined ? null : record;
    },

    // Opens the logical device `handle` if it is not open yet, and returns its state. Handle zero
    // names no logical device and opens nothing.
    openLogical: function(handle) {
      var record = DonnerGpu.logicalFor(handle);
      if (record !== null || !(handle > 0)) {
        return record;
      }
      record = {
        handle: handle,
        requested: false,
        // Why this logical device's own request was refused, which is its alone: a protocol table
        // that disagrees with this library, or a second request on one handle.
        failure: '',
        objects: new Map(),  // Map from identifier to { kind, object, alias, frame, share }.
        mappings: new Map(),  // Map from identifier to { buffer, offset, size, state, view }.
        completedSerial: 0,
        encoder: null,
        recordingSerial: 0,
        recordedBuffers: null,  // Finished command buffers of the open submission, in order.
        pass: null,
        attachments: null,
        pending: null,  // Descriptor being built by a sequence of item calls.
      };
      DonnerGpu.logical.set(handle, record);
      return record;
    },

    // Closes the logical device `handle`. What it named goes with it, except a texture a share
    // still holds, which the share releases. Once no logical device is left, the browser device is
    // let go, so a later request starts from nothing rather than inheriting a loss or objects.
    closeLogical: function(handle) {
      var record = DonnerGpu.logicalFor(handle);
      if (record === null) {
        return;
      }
      record.objects.forEach(function(entry) {
        DonnerGpu.releaseProducerHold(entry);
      });
      DonnerGpu.logical.delete(handle);
      if (DonnerGpu.logical.size === 0) {
        DonnerGpu.releaseSharedDevice();
      }
    },

    // Asks the browser for this worker's device and installs it when it arrives, unless the device
    // it was asked for has been let go by then.
    requestBrowserDevice: function() {
      DonnerGpu.requestState = DonnerGpu.kRequestPending;
      var generation = DonnerGpu.requestGeneration;
      navigator.gpu.requestAdapter()
        .then(function(adapter) {
          if (!adapter) {
            throw new Error('no GPU adapter is available');
          }
          return adapter.requestDevice();
        })
        .then(function(device) {
          if (generation === DonnerGpu.requestGeneration) {
            DonnerGpu.installDevice(device);
          }
        })
        .catch(function(e) {
          if (generation === DonnerGpu.requestGeneration) {
            DonnerGpu.requestError = String(e && e.message ? e.message : e);
            DonnerGpu.requestState = DonnerGpu.kRequestFailed;
          }
        });
    },

    installDevice: function(device) {
      DonnerGpu.device = device;
      DonnerGpu.queue = device.queue;
      // Loss is permanent, and the runtime refuses everything once it is observed, so the only
      // thing to do here is record it where the next call will see it - unless the device has been
      // let go since, and the loss describes a device nothing here names any more.
      device.lost.then(function(info) {
        if (DonnerGpu.device === device) {
          DonnerGpu.lost = true;
          DonnerGpu.lostReason = String(info.reason) + ': ' + String(info.message);
        }
      });
      DonnerGpu.requestState = DonnerGpu.kRequestReady;
    },

    // Lets the browser device go once no logical device is left over it. The shares go with it: no
    // logical device is left to register one, and a share whose owning thread exited before its
    // holder let go was never released, so the textures they still hold are destroyed here rather
    // than kept for the life of the worker. A frame is the canvas's, and is left to it.
    releaseSharedDevice: function() {
      DonnerGpu.shares.forEach(function(held) {
        if (!held.canvasOwned) {
          try {
            held.texture.destroy();
          } catch (e) {
            // Nothing names the texture any more, so there is no caller to report a refusal to.
          }
        }
      });
      DonnerGpu.shares.clear();
      DonnerGpu.device = null;
      DonnerGpu.queue = null;
      DonnerGpu.requestStarted = false;
      DonnerGpu.requestGeneration += 1;
      DonnerGpu.requestState = DonnerGpu.kRequestPending;
      DonnerGpu.requestError = '';
      DonnerGpu.lost = false;
      DonnerGpu.lostReason = '';
      DonnerGpu.deviceIdentity = 0;
    },

    // Marks the share holding `entry`'s texture, if any, as the only holder left: the producer has
    // let go of its identifier, so releasing the share is what destroys the texture now.
    releaseProducerHold: function(entry) {
      if (entry.share) {
        var share = DonnerGpu.shares.get(entry.share);
        if (share !== undefined) {
          share.producerReleased = true;
        }
      }
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

    // Refuses anything once the device is gone or was never this logical device's to use. Every
    // entry point that can be refused starts here, so a lost device cannot be driven further by
    // any path.
    guard: function(record) {
      if (record === null || DonnerGpu.device === null) {
        return DonnerGpu.kNotOwner;
      }
      if (DonnerGpu.lost) {
        return DonnerGpu.kDeviceLost;
      }
      return DonnerGpu.kSuccess;
    },

    // The check releases use. A lost device still has to free what it holds - refusing here would
    // strand every object it owns for the life of the page - so loss is not a reason to refuse,
    // while a logical device this worker does not hold still is.
    guardRelease: function(record) {
      return record === null || DonnerGpu.device === null ? DonnerGpu.kNotOwner :
                                                            DonnerGpu.kSuccess;
    },

    // Returns the object `id` names in `record` if it is of `kind`, otherwise null. The caller
    // turns null into the refusal its own bookkeeping calls for.
    lookup: function(record, kind, id) {
      if (record === null) {
        return null;
      }
      var entry = record.objects.get(id);
      if (entry === undefined || entry.kind !== kind) {
        return null;
      }
      return entry.object;
    },

    // The refusal `id` earns in `record`: not-owner for a logical device this worker does not
    // hold, unknown when nothing holds the identifier, wrong-kind when something else does.
    refusalFor: function(record, kind, id) {
      if (record === null) {
        return DonnerGpu.kNotOwner;
      }
      var entry = record.objects.get(id);
      if (entry === undefined) {
        return DonnerGpu.kUnknownObject;
      }
      return entry.kind === kind ? DonnerGpu.kSuccess : DonnerGpu.kWrongObjectKind;
    },

    register: function(record, kind, id, object) {
      if (id === 0 || record.objects.has(id)) {
        return DonnerGpu.kFailed;
      }
      record.objects.set(id, { kind: kind, object: object, alias: false, frame: false, share: 0 });
      return DonnerGpu.kSuccess;
    },

    // Runs `build` and registers what it produces. A browser that refuses to create the object
    // throws, and that becomes a refusal rather than an exception crossing back into wasm.
    create: function(record, kind, id, build) {
      var status = DonnerGpu.guard(record);
      if (status !== DonnerGpu.kSuccess) {
        return status;
      }
      // Check the identifier before building: an object built for an identifier already in use
      // could not be registered and nothing would hold it afterwards.
      if (id === 0 || record.objects.has(id)) {
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
      return DonnerGpu.register(record, kind, id, object);
    },

    // Runs `act`, turning a browser-side throw into a refusal.
    perform: function(record, act) {
      var status = DonnerGpu.guard(record);
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
    // on its own schedule either way. A share of the frame keeps naming it until it is released,
    // and never destroys it.
    releaseFrame: function(record, surface) {
      if (surface.frame !== null) {
        var entry = record.objects.get(surface.frame);
        if (entry !== undefined) {
          DonnerGpu.releaseProducerHold(entry);
        }
        record.objects.delete(surface.frame);
        surface.frame = null;
      }
    },

    // Gives up everything a surface holds: the frame its canvas is still waiting to take back,
    // and the context configuration naming this device. A canvas outlives the surface over it, so
    // leaving it configured would keep a device the caller has finished with attached to the page.
    releaseSurface: function(record, surface) {
      DonnerGpu.releaseFrame(record, surface);
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
  donner_gpu_check_protocol: function(handle, codes, count) {
    // A disagreement refuses this logical device's request and nobody else's: another logical
    // device already running over the browser device goes on as it was.
    var record = DonnerGpu.openLogical(handle);
    if (record === null) {
      return DonnerGpu.kFailed;
    }
    if (count !== DonnerGpu.protocolCodes.length) {
      record.failure = 'GPU bridge protocol table has ' + DonnerGpu.protocolCodes.length +
          ' entries and the module was built against ' + count;
      return DonnerGpu.kFailed;
    }
    for (var i = 0; i < count; ++i) {
      var expected = DonnerGpu.protocolCodes[i];
      var actual = HEAPU32[(codes >> 2) + i];
      if (actual !== expected) {
        record.failure = 'GPU bridge protocol entry ' + i + ' is ' + actual +
            ' and this library assigns ' + expected;
        return DonnerGpu.kFailed;
      }
    }
    return DonnerGpu.kSuccess;
  },

  // ----- Device acquisition -------------------------------------------------

  donner_gpu_begin_device_request__deps: ['$DonnerGpu'],
  donner_gpu_begin_device_request: function(handle) {
    var record = DonnerGpu.openLogical(handle);
    if (record === null || record.failure !== '') {
      return DonnerGpu.kFailed;
    }
    if (record.requested) {
      // A second request on one handle would stand for a second device where the runtime holds
      // one, so it is refused, on this logical device alone.
      record.failure = 'this logical device has already requested its GPU bridge device';
      return DonnerGpu.kFailed;
    }
    record.requested = true;
    if (DonnerGpu.requestStarted) {
      // The worker already holds, or is obtaining, its browser device; this logical device runs
      // over the same one rather than asking the browser for another.
      return DonnerGpu.kSuccess;
    }
    DonnerGpu.requestStarted = true;
    DonnerGpu.deviceIdentity = handle;
    if (typeof navigator === 'undefined' || !navigator.gpu) {
      DonnerGpu.requestState = DonnerGpu.kRequestUnavailable;
      return DonnerGpu.kSuccess;
    }
    DonnerGpu.requestBrowserDevice();
    return DonnerGpu.kSuccess;
  },

  donner_gpu_release_device__deps: ['$DonnerGpu'],
  donner_gpu_release_device: function(handle) {
    DonnerGpu.closeLogical(handle);
  },

  donner_gpu_device_request_state__deps: ['$DonnerGpu'],
  donner_gpu_device_request_state: function(handle) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.failure !== '') {
      return DonnerGpu.kRequestFailed;
    }
    return DonnerGpu.requestState;
  },

  donner_gpu_read_request_error__deps: ['$DonnerGpu'],
  donner_gpu_read_request_error: function(handle, destination, capacity) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null) {
      return 0;
    }
    var reason = record.failure !== '' ? record.failure :
                                         DonnerGpu.requestState === DonnerGpu.kRequestFailed ?
                                         DonnerGpu.requestError : '';
    return DonnerGpu.writeMessage(reason, destination, capacity);
  },

  donner_gpu_owns_device__deps: ['$DonnerGpu'],
  donner_gpu_owns_device: function(handle) {
    return DonnerGpu.logicalFor(handle) !== null && DonnerGpu.device !== null ? 1 : 0;
  },

  donner_gpu_is_device_lost__deps: ['$DonnerGpu'],
  donner_gpu_is_device_lost: function(handle) {
    return DonnerGpu.logicalFor(handle) !== null && DonnerGpu.lost ? 1 : 0;
  },

  donner_gpu_read_lost_reason__deps: ['$DonnerGpu'],
  donner_gpu_read_lost_reason: function(handle, destination, capacity) {
    if (DonnerGpu.logicalFor(handle) === null) {
      return 0;
    }
    return DonnerGpu.writeMessage(DonnerGpu.lostReason, destination, capacity);
  },

  donner_gpu_completed_serial__deps: ['$DonnerGpu'],
  donner_gpu_completed_serial: function(handle) {
    var record = DonnerGpu.logicalFor(handle);
    return record === null ? 0 : record.completedSerial;
  },

  donner_gpu_device_identity__deps: ['$DonnerGpu'],
  donner_gpu_device_identity: function(handle) {
    // Zero until the device exists: an identity handed out for a request still in flight could
    // name a device the request never obtains.
    if (DonnerGpu.logicalFor(handle) === null || DonnerGpu.device === null) {
      return 0;
    }
    return DonnerGpu.deviceIdentity;
  },

  // ----- Resource creation --------------------------------------------------

  donner_gpu_create_buffer__deps: ['$DonnerGpu'],
  donner_gpu_create_buffer: function(handle, id, byteSize, usageBits) {
    return DonnerGpu.create(DonnerGpu.logicalFor(handle), DonnerGpu.kBuffer, id, function() {
      return DonnerGpu.device.createBuffer({
        size: byteSize,
        usage: DonnerGpu.bufferUsage(usageBits),
      });
    });
  },

  donner_gpu_create_texture__deps: ['$DonnerGpu'],
  donner_gpu_create_texture: function(handle, id, width, height, formatCode, usageBits) {
    var format = DonnerGpu.textureFormat(formatCode);
    if (format === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.create(DonnerGpu.logicalFor(handle), DonnerGpu.kTexture, id, function() {
      return DonnerGpu.device.createTexture({
        size: { width: width, height: height, depthOrArrayLayers: 1 },
        format: format,
        usage: DonnerGpu.textureUsage(usageBits),
      });
    });
  },

  donner_gpu_create_texture_view__deps: ['$DonnerGpu'],
  donner_gpu_create_texture_view: function(handle, id, textureId) {
    var record = DonnerGpu.logicalFor(handle);
    var texture = DonnerGpu.lookup(record, DonnerGpu.kTexture, textureId);
    if (texture === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kTexture, textureId);
    }
    return DonnerGpu.create(record, DonnerGpu.kTextureView, id, function() {
      return texture.createView();
    });
  },

  donner_gpu_create_sampler__deps: ['$DonnerGpu'],
  donner_gpu_create_sampler: function(handle, id, magFilterCode, minFilterCode, addressUCode,
                                      addressVCode) {
    var magFilter = DonnerGpu.filterMode(magFilterCode);
    var minFilter = DonnerGpu.filterMode(minFilterCode);
    var addressU = DonnerGpu.addressMode(addressUCode);
    var addressV = DonnerGpu.addressMode(addressVCode);
    if (!DonnerGpu.allDecoded([magFilter, minFilter, addressU, addressV])) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.create(DonnerGpu.logicalFor(handle), DonnerGpu.kSampler, id, function() {
      return DonnerGpu.device.createSampler({
        magFilter: magFilter,
        minFilter: minFilter,
        addressModeU: addressU,
        addressModeV: addressV,
      });
    });
  },

  donner_gpu_create_shader_module__deps: ['$DonnerGpu'],
  donner_gpu_create_shader_module: function(handle, id, wgsl, byteCount) {
    var code = UTF8ToString(wgsl, byteCount);
    return DonnerGpu.create(DonnerGpu.logicalFor(handle), DonnerGpu.kShaderModule, id, function() {
      return DonnerGpu.device.createShaderModule({ code: code });
    });
  },

  donner_gpu_destroy_object__deps: ['$DonnerGpu'],
  donner_gpu_destroy_object: function(handle, kindCode, id) {
    var record = DonnerGpu.logicalFor(handle);
    var released = DonnerGpu.guardRelease(record);
    if (released !== DonnerGpu.kSuccess) {
      return released;
    }
    var refusal = DonnerGpu.refusalFor(record, kindCode, id);
    if (refusal !== DonnerGpu.kSuccess) {
      return refusal;
    }
    var entry = record.objects.get(id);
    record.objects.delete(id);
    record.mappings.delete(id);
    // An alias names another logical device's texture, so only the identifier goes. A texture a
    // share still holds goes when the share is released, not now.
    if (entry.alias) {
      return DonnerGpu.kSuccess;
    }
    if (entry.share && DonnerGpu.shares.has(entry.share)) {
      DonnerGpu.releaseProducerHold(entry);
      return DonnerGpu.kSuccess;
    }
    // A surface holds no GPU allocation of its own. Buffers and textures do, and the browser will
    // not release those until asked; the remaining kinds go when the last reference to them does.
    try {
      if (kindCode === DonnerGpu.kSurface) {
        DonnerGpu.releaseSurface(record, entry.object);
      } else if (entry.object && typeof entry.object.destroy === 'function') {
        entry.object.destroy();
      }
    } catch (e) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.kSuccess;
  },

  // ----- Sharing between logical devices ------------------------------------

  donner_gpu_share_texture__deps: ['$DonnerGpu'],
  donner_gpu_share_texture: function(handle, textureId, shareOut) {
    var record = DonnerGpu.logicalFor(handle);
    var status = DonnerGpu.guard(record);
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    var refusal = DonnerGpu.refusalFor(record, DonnerGpu.kTexture, textureId);
    if (refusal !== DonnerGpu.kSuccess) {
      return refusal;
    }
    var entry = record.objects.get(textureId);
    // A registration is shared from the logical device that allocated its texture, and a texture
    // is shared once: the runtime keeps one share per texture for as long as the texture lives.
    if (entry.alias || entry.share) {
      return DonnerGpu.kFailed;
    }
    var share = DonnerGpu.nextShare++;
    DonnerGpu.shares.set(share, {
      texture: entry.object,
      // A frame belongs to the canvas that handed it out, so releasing its share never destroys it.
      canvasOwned: entry.frame,
      // The browser device the texture belongs to; a share is registered only on a logical device
      // over the same one.
      deviceIdentity: DonnerGpu.deviceIdentity,
      // The logical device that made the share, and the only one that may release it. Handles are
      // unique across workers, while share numbers are this worker's own.
      producer: handle,
      producerId: textureId,
      producerReleased: false,
    });
    entry.share = share;
    HEAPU32[shareOut >> 2] = share;
    return DonnerGpu.kSuccess;
  },

  donner_gpu_register_shared_texture__deps: ['$DonnerGpu'],
  donner_gpu_register_shared_texture: function(handle, id, share) {
    var record = DonnerGpu.logicalFor(handle);
    var status = DonnerGpu.guard(record);
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    var held = DonnerGpu.shares.get(share);
    // A share of another browser device names nothing here. The shares go with their device, so a
    // stale one is not listed at all; comparing the identity keeps that true for any share that is.
    if (held === undefined || held.deviceIdentity !== DonnerGpu.deviceIdentity) {
      return DonnerGpu.kUnknownObject;
    }
    var registered = DonnerGpu.register(record, DonnerGpu.kTexture, id, held.texture);
    if (registered === DonnerGpu.kSuccess) {
      record.objects.get(id).alias = true;
    }
    return registered;
  },

  donner_gpu_release_texture_share__deps: ['$DonnerGpu'],
  donner_gpu_release_texture_share: function(handle, share) {
    DonnerGpu.ensureTables();
    var held = DonnerGpu.shares.get(share);
    // A release names the logical device that made the share. Share numbers are this worker's own,
    // so a share of another worker can carry the same number; the handle cannot, which leaves this
    // worker's share alone when a release reaches the wrong worker.
    if (held === undefined || held.producer !== handle) {
      return;
    }
    DonnerGpu.shares.delete(share);
    if (!held.producerReleased) {
      // The producer still names the texture and destroys it itself when it lets go.
      var producer = DonnerGpu.logicalFor(held.producer);
      var entry = producer === null ? undefined : producer.objects.get(held.producerId);
      if (entry !== undefined && entry.share === share) {
        entry.share = 0;
      }
      return;
    }
    if (!held.canvasOwned) {
      try {
        held.texture.destroy();
      } catch (e) {
        // Nothing names the texture any more, so there is no caller to report a refusal to.
      }
    }
  },

  // ----- Bind group layouts, bind groups and pipeline layouts ---------------

  donner_gpu_bind_group_layout_begin__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_layout_begin: function(handle) {
    var record = DonnerGpu.logicalFor(handle);
    var status = DonnerGpu.guard(record);
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    record.pending = { entries: [] };
    return DonnerGpu.kSuccess;
  },

  donner_gpu_bind_group_layout_entry__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_layout_entry: function(handle, binding, visibilityBits, bindingTypeCode,
                                               storageTextureFormat) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pending === null) {
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
    record.pending.entries.push(entry);
    return DonnerGpu.kSuccess;
  },

  donner_gpu_bind_group_layout_finish__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_layout_finish: function(handle, id) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pending === null) {
      return DonnerGpu.kFailed;
    }
    var pending = record.pending;
    record.pending = null;
    return DonnerGpu.create(record, DonnerGpu.kBindGroupLayout, id, function() {
      return DonnerGpu.device.createBindGroupLayout({ entries: pending.entries });
    });
  },

  donner_gpu_bind_group_begin__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_begin: function(handle, layoutId) {
    var record = DonnerGpu.logicalFor(handle);
    var layout = DonnerGpu.lookup(record, DonnerGpu.kBindGroupLayout, layoutId);
    if (layout === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kBindGroupLayout, layoutId);
    }
    var status = DonnerGpu.guard(record);
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    record.pending = { layout: layout, entries: [] };
    return DonnerGpu.kSuccess;
  },

  donner_gpu_bind_group_entry__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_entry: function(handle, binding, resourceKindCode, resourceId, offsetBytes,
                                        sizeBytes) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pending === null) {
      return DonnerGpu.kFailed;
    }
    var resource;
    if (resourceKindCode === 0) {
      var buffer = DonnerGpu.lookup(record, DonnerGpu.kBuffer, resourceId);
      if (buffer === null) {
        return DonnerGpu.refusalFor(record, DonnerGpu.kBuffer, resourceId);
      }
      resource = { buffer: buffer, offset: offsetBytes, size: sizeBytes };
    } else if (resourceKindCode === 1) {
      resource = DonnerGpu.lookup(record, DonnerGpu.kTextureView, resourceId);
      if (resource === null) {
        return DonnerGpu.refusalFor(record, DonnerGpu.kTextureView, resourceId);
      }
    } else if (resourceKindCode === 2) {
      resource = DonnerGpu.lookup(record, DonnerGpu.kSampler, resourceId);
      if (resource === null) {
        return DonnerGpu.refusalFor(record, DonnerGpu.kSampler, resourceId);
      }
    } else {
      return DonnerGpu.kFailed;
    }
    record.pending.entries.push({ binding: binding, resource: resource });
    return DonnerGpu.kSuccess;
  },

  donner_gpu_bind_group_finish__deps: ['$DonnerGpu'],
  donner_gpu_bind_group_finish: function(handle, id) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pending === null) {
      return DonnerGpu.kFailed;
    }
    var pending = record.pending;
    record.pending = null;
    return DonnerGpu.create(record, DonnerGpu.kBindGroup, id, function() {
      return DonnerGpu.device.createBindGroup({
        layout: pending.layout,
        entries: pending.entries,
      });
    });
  },

  donner_gpu_pipeline_layout_begin__deps: ['$DonnerGpu'],
  donner_gpu_pipeline_layout_begin: function(handle) {
    var record = DonnerGpu.logicalFor(handle);
    var status = DonnerGpu.guard(record);
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    record.pending = { layouts: [] };
    return DonnerGpu.kSuccess;
  },

  donner_gpu_pipeline_layout_group__deps: ['$DonnerGpu'],
  donner_gpu_pipeline_layout_group: function(handle, bindGroupLayoutId) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pending === null) {
      return DonnerGpu.kFailed;
    }
    var layout = DonnerGpu.lookup(record, DonnerGpu.kBindGroupLayout, bindGroupLayoutId);
    if (layout === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kBindGroupLayout, bindGroupLayoutId);
    }
    record.pending.layouts.push(layout);
    return DonnerGpu.kSuccess;
  },

  donner_gpu_pipeline_layout_finish__deps: ['$DonnerGpu'],
  donner_gpu_pipeline_layout_finish: function(handle, id) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pending === null) {
      return DonnerGpu.kFailed;
    }
    var pending = record.pending;
    record.pending = null;
    return DonnerGpu.create(record, DonnerGpu.kPipelineLayout, id, function() {
      return DonnerGpu.device.createPipelineLayout({ bindGroupLayouts: pending.layouts });
    });
  },

  // ----- Pipelines ----------------------------------------------------------

  donner_gpu_render_pipeline_begin__deps: ['$DonnerGpu'],
  donner_gpu_render_pipeline_begin: function(handle, layoutId, vertexModuleId, vertexEntryPoint,
                                             vertexEntryPointBytes, fragmentModuleId,
                                             fragmentEntryPoint, fragmentEntryPointBytes,
                                             topologyCode, cullModeCode) {
    var record = DonnerGpu.logicalFor(handle);
    var layout = DonnerGpu.lookup(record, DonnerGpu.kPipelineLayout, layoutId);
    if (layout === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kPipelineLayout, layoutId);
    }
    var vertexModule = DonnerGpu.lookup(record, DonnerGpu.kShaderModule, vertexModuleId);
    if (vertexModule === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kShaderModule, vertexModuleId);
    }
    var fragmentModule = DonnerGpu.lookup(record, DonnerGpu.kShaderModule, fragmentModuleId);
    if (fragmentModule === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kShaderModule, fragmentModuleId);
    }
    var topology = DonnerGpu.topology(topologyCode);
    var cullMode = DonnerGpu.cullMode(cullModeCode);
    if (!DonnerGpu.allDecoded([topology, cullMode])) {
      return DonnerGpu.kFailed;
    }
    var guarded = DonnerGpu.guard(record);
    if (guarded !== DonnerGpu.kSuccess) {
      return guarded;
    }
    record.pending = {
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
  donner_gpu_render_pipeline_vertex_buffer: function(handle, strideBytes, stepModeCode) {
    var record = DonnerGpu.logicalFor(handle);
    var stepMode = DonnerGpu.stepMode(stepModeCode);
    if (record === null || record.pending === null || stepMode === null) {
      return DonnerGpu.kFailed;
    }
    record.pending.buffers.push({
      arrayStride: strideBytes,
      stepMode: stepMode,
      attributes: [],
    });
    return DonnerGpu.kSuccess;
  },

  donner_gpu_render_pipeline_vertex_attribute__deps: ['$DonnerGpu'],
  donner_gpu_render_pipeline_vertex_attribute: function(handle, formatCode, offsetBytes,
                                                        shaderLocation) {
    // An attribute belongs to the buffer most recently described; arriving before any buffer means
    // the two sides disagree about the shape being built, which is refused rather than guessed at.
    var record = DonnerGpu.logicalFor(handle);
    var format = DonnerGpu.vertexFormat(formatCode);
    if (record === null || record.pending === null || record.pending.buffers.length === 0 ||
        format === null) {
      return DonnerGpu.kFailed;
    }
    record.pending.buffers[record.pending.buffers.length - 1].attributes.push({
      format: format,
      offset: offsetBytes,
      shaderLocation: shaderLocation,
    });
    return DonnerGpu.kSuccess;
  },

  donner_gpu_render_pipeline_color_target__deps: ['$DonnerGpu'],
  donner_gpu_render_pipeline_color_target: function(handle, formatCode, blendEnabled,
                                                    colorSrcFactor, colorDstFactor,
                                                    colorOperation, alphaSrcFactor,
                                                    alphaDstFactor, alphaOperation,
                                                    writeMaskBits) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pending === null) {
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
    record.pending.targets.push(target);
    return DonnerGpu.kSuccess;
  },

  donner_gpu_render_pipeline_finish__deps: ['$DonnerGpu'],
  donner_gpu_render_pipeline_finish: function(handle, id) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pending === null) {
      return DonnerGpu.kFailed;
    }
    var pending = record.pending;
    record.pending = null;
    return DonnerGpu.create(record, DonnerGpu.kRenderPipeline, id, function() {
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
  donner_gpu_create_compute_pipeline: function(handle, id, layoutId, moduleId, entryPoint,
                                               entryPointBytes) {
    var record = DonnerGpu.logicalFor(handle);
    var layout = DonnerGpu.lookup(record, DonnerGpu.kPipelineLayout, layoutId);
    if (layout === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kPipelineLayout, layoutId);
    }
    var module = DonnerGpu.lookup(record, DonnerGpu.kShaderModule, moduleId);
    if (module === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kShaderModule, moduleId);
    }
    var name = UTF8ToString(entryPoint, entryPointBytes);
    return DonnerGpu.create(record, DonnerGpu.kComputePipeline, id, function() {
      return DonnerGpu.device.createComputePipeline({
        layout: layout,
        compute: { module: module, entryPoint: name },
      });
    });
  },

  // ----- Queue writes -------------------------------------------------------

  donner_gpu_write_buffer__deps: ['$DonnerGpu'],
  donner_gpu_write_buffer: function(handle, bufferId, offsetBytes, data, byteCount) {
    var record = DonnerGpu.logicalFor(handle);
    var buffer = DonnerGpu.lookup(record, DonnerGpu.kBuffer, bufferId);
    if (buffer === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kBuffer, bufferId);
    }
    return DonnerGpu.perform(record, function() {
      // The browser copies during writeBuffer, so a view of the wasm heap is safe to hand over
      // even though the heap can move afterwards.
      DonnerGpu.queue.writeBuffer(buffer, offsetBytes,
                                  HEAPU8.subarray(data, data + byteCount));
    });
  },

  donner_gpu_write_texture__deps: ['$DonnerGpu'],
  donner_gpu_write_texture: function(handle, textureId, data, byteCount, layoutOffsetBytes,
                                     bytesPerRow, rowsPerImage, destinationX, destinationY, width,
                                     height) {
    var record = DonnerGpu.logicalFor(handle);
    var texture = DonnerGpu.lookup(record, DonnerGpu.kTexture, textureId);
    if (texture === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kTexture, textureId);
    }
    return DonnerGpu.perform(record, function() {
      DonnerGpu.queue.writeTexture(
        { texture: texture, origin: { x: destinationX, y: destinationY, z: 0 } },
        HEAPU8.subarray(data, data + byteCount),
        { offset: layoutOffsetBytes, bytesPerRow: bytesPerRow, rowsPerImage: rowsPerImage },
        { width: width, height: height, depthOrArrayLayers: 1 });
    });
  },

  // ----- Command recording --------------------------------------------------

  donner_gpu_begin_command_buffer__deps: ['$DonnerGpu'],
  donner_gpu_begin_command_buffer: function(handle, submissionSerial, commandBufferIndex) {
    // A later buffer must continue the submission the first one opened, and the two halves must
    // agree on how many buffers have been finished under it; anything else means they disagree
    // about what is being recorded, so nothing is kept.
    var record = DonnerGpu.logicalFor(handle);
    if (record === null) {
      return DonnerGpu.kNotOwner;
    }
    if (commandBufferIndex !== 0 &&
        (record.recordingSerial !== submissionSerial || record.recordedBuffers === null ||
         record.recordedBuffers.length !== commandBufferIndex)) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      // A recording left open by a submission that was refused partway is dropped here rather
      // than continued, so nothing recorded before the refusal can reach the queue. The first
      // buffer of a submission starts the list empty, which drops whatever an earlier attempt
      // finished but never submitted - a refused submission keeps its serial, so the retry
      // arrives under the same one.
      record.pass = null;
      record.attachments = null;
      if (commandBufferIndex === 0) {
        record.recordedBuffers = [];
      }
      // Drop the old encoder before asking for a new one. If that ask throws, what is left behind
      // is nothing rather than the previous recording sitting under the new serial.
      record.encoder = null;
      record.recordingSerial = submissionSerial;
      record.encoder = DonnerGpu.device.createCommandEncoder();
    });
  },

  donner_gpu_begin_render_pass__deps: ['$DonnerGpu'],
  donner_gpu_begin_render_pass: function(handle) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.encoder === null) {
      return DonnerGpu.kFailed;
    }
    var status = DonnerGpu.guard(record);
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    record.attachments = [];
    return DonnerGpu.kSuccess;
  },

  donner_gpu_render_pass_attachment__deps: ['$DonnerGpu'],
  donner_gpu_render_pass_attachment: function(handle, viewId, loadOpCode, storeOpCode, clearRed,
                                              clearGreen, clearBlue, clearAlpha) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.attachments === null) {
      return DonnerGpu.kFailed;
    }
    var view = DonnerGpu.lookup(record, DonnerGpu.kTextureView, viewId);
    if (view === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kTextureView, viewId);
    }
    var loadOp = DonnerGpu.loadOp(loadOpCode);
    var storeOp = DonnerGpu.storeOp(storeOpCode);
    if (!DonnerGpu.allDecoded([loadOp, storeOp])) {
      return DonnerGpu.kFailed;
    }
    record.attachments.push({
      view: view,
      loadOp: loadOp,
      storeOp: storeOp,
      clearValue: { r: clearRed, g: clearGreen, b: clearBlue, a: clearAlpha },
    });
    return DonnerGpu.kSuccess;
  },

  donner_gpu_begin_render_pass_finish__deps: ['$DonnerGpu'],
  donner_gpu_begin_render_pass_finish: function(handle) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null) {
      return DonnerGpu.kFailed;
    }
    var attachments = record.attachments;
    record.attachments = null;
    if (attachments === null || record.encoder === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.pass = record.encoder.beginRenderPass({ colorAttachments: attachments });
    });
  },

  donner_gpu_end_render_pass__deps: ['$DonnerGpu'],
  donner_gpu_end_render_pass: function(handle) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.pass.end();
      record.pass = null;
    });
  },

  donner_gpu_begin_compute_pass__deps: ['$DonnerGpu'],
  donner_gpu_begin_compute_pass: function(handle) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.encoder === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.pass = record.encoder.beginComputePass();
    });
  },

  donner_gpu_end_compute_pass__deps: ['$DonnerGpu'],
  donner_gpu_end_compute_pass: function(handle) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.pass.end();
      record.pass = null;
    });
  },

  donner_gpu_set_render_pipeline__deps: ['$DonnerGpu'],
  donner_gpu_set_render_pipeline: function(handle, pipelineId) {
    var record = DonnerGpu.logicalFor(handle);
    var pipeline = DonnerGpu.lookup(record, DonnerGpu.kRenderPipeline, pipelineId);
    if (pipeline === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kRenderPipeline, pipelineId);
    }
    if (record.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() { record.pass.setPipeline(pipeline); });
  },

  donner_gpu_set_compute_pipeline__deps: ['$DonnerGpu'],
  donner_gpu_set_compute_pipeline: function(handle, pipelineId) {
    var record = DonnerGpu.logicalFor(handle);
    var pipeline = DonnerGpu.lookup(record, DonnerGpu.kComputePipeline, pipelineId);
    if (pipeline === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kComputePipeline, pipelineId);
    }
    if (record.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() { record.pass.setPipeline(pipeline); });
  },

  donner_gpu_set_bind_group__deps: ['$DonnerGpu'],
  donner_gpu_set_bind_group: function(handle, index, bindGroupId) {
    var record = DonnerGpu.logicalFor(handle);
    var bindGroup = DonnerGpu.lookup(record, DonnerGpu.kBindGroup, bindGroupId);
    if (bindGroup === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kBindGroup, bindGroupId);
    }
    if (record.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() { record.pass.setBindGroup(index, bindGroup); });
  },

  donner_gpu_set_vertex_buffer__deps: ['$DonnerGpu'],
  donner_gpu_set_vertex_buffer: function(handle, slot, bufferId, offsetBytes) {
    var record = DonnerGpu.logicalFor(handle);
    var buffer = DonnerGpu.lookup(record, DonnerGpu.kBuffer, bufferId);
    if (buffer === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kBuffer, bufferId);
    }
    if (record.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.pass.setVertexBuffer(slot, buffer, offsetBytes);
    });
  },

  donner_gpu_set_index_buffer__deps: ['$DonnerGpu'],
  donner_gpu_set_index_buffer: function(handle, bufferId, indexFormatCode, offsetBytes) {
    var record = DonnerGpu.logicalFor(handle);
    var buffer = DonnerGpu.lookup(record, DonnerGpu.kBuffer, bufferId);
    if (buffer === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kBuffer, bufferId);
    }
    var indexFormat = DonnerGpu.indexFormat(indexFormatCode);
    if (record.pass === null || indexFormat === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.pass.setIndexBuffer(buffer, indexFormat, offsetBytes);
    });
  },

  donner_gpu_set_scissor_rect__deps: ['$DonnerGpu'],
  donner_gpu_set_scissor_rect: function(handle, x, y, width, height) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.pass.setScissorRect(x, y, width, height);
    });
  },

  donner_gpu_set_viewport__deps: ['$DonnerGpu'],
  donner_gpu_set_viewport: function(handle, x, y, width, height, minDepth, maxDepth) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.pass.setViewport(x, y, width, height, minDepth, maxDepth);
    });
  },

  donner_gpu_draw__deps: ['$DonnerGpu'],
  donner_gpu_draw: function(handle, vertexCount, instanceCount, firstVertex, firstInstance) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.pass.draw(vertexCount, instanceCount, firstVertex, firstInstance);
    });
  },

  donner_gpu_draw_indexed__deps: ['$DonnerGpu'],
  donner_gpu_draw_indexed: function(handle, indexCount, instanceCount, firstIndex, baseVertex,
                                    firstInstance) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.pass.drawIndexed(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
    });
  },

  donner_gpu_dispatch_workgroups__deps: ['$DonnerGpu'],
  donner_gpu_dispatch_workgroups: function(handle, countX, countY, countZ) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.pass === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.pass.dispatchWorkgroups(countX, countY, countZ);
    });
  },

  donner_gpu_copy_texture_to_buffer__deps: ['$DonnerGpu'],
  donner_gpu_copy_texture_to_buffer: function(handle, textureId, bufferId, layoutOffsetBytes,
                                              bytesPerRow, rowsPerImage, width, height) {
    var record = DonnerGpu.logicalFor(handle);
    var texture = DonnerGpu.lookup(record, DonnerGpu.kTexture, textureId);
    if (texture === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kTexture, textureId);
    }
    var buffer = DonnerGpu.lookup(record, DonnerGpu.kBuffer, bufferId);
    if (buffer === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kBuffer, bufferId);
    }
    if (record.encoder === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.encoder.copyTextureToBuffer(
        { texture: texture },
        { buffer: buffer, offset: layoutOffsetBytes, bytesPerRow: bytesPerRow,
          rowsPerImage: rowsPerImage },
        { width: width, height: height, depthOrArrayLayers: 1 });
    });
  },

  donner_gpu_copy_texture_to_texture__deps: ['$DonnerGpu'],
  donner_gpu_copy_texture_to_texture: function(handle, sourceTextureId, destinationTextureId,
                                               sourceX, sourceY, destinationX, destinationY,
                                               width, height) {
    var record = DonnerGpu.logicalFor(handle);
    var source = DonnerGpu.lookup(record, DonnerGpu.kTexture, sourceTextureId);
    if (source === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kTexture, sourceTextureId);
    }
    var destination = DonnerGpu.lookup(record, DonnerGpu.kTexture, destinationTextureId);
    if (destination === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kTexture, destinationTextureId);
    }
    if (record.encoder === null) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      record.encoder.copyTextureToTexture(
        { texture: source, origin: { x: sourceX, y: sourceY, z: 0 } },
        { texture: destination, origin: { x: destinationX, y: destinationY, z: 0 } },
        { width: width, height: height, depthOrArrayLayers: 1 });
    });
  },

  donner_gpu_end_command_buffer__deps: ['$DonnerGpu'],
  donner_gpu_end_command_buffer: function(handle, submissionSerial) {
    // The serial that opened the recording is the one that must close it. A mismatch means the two
    // halves disagree about which submission this encoder belongs to, so nothing is kept.
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.encoder === null ||
        record.recordingSerial !== submissionSerial) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      var commandBuffer = record.encoder.finish();
      record.encoder = null;
      record.recordedBuffers.push(commandBuffer);
    });
  },

  donner_gpu_submit_command_buffers__deps: ['$DonnerGpu'],
  donner_gpu_submit_command_buffers: function(handle, submissionSerial) {
    // Every buffer of this submission must be finished and belong to this serial: an open encoder
    // or a serial that never opened one means the two halves disagree about what is being
    // submitted, and a submission with no buffers names no work to complete.
    var record = DonnerGpu.logicalFor(handle);
    if (record === null || record.encoder !== null ||
        record.recordingSerial !== submissionSerial || record.recordedBuffers === null ||
        record.recordedBuffers.length === 0) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      // One queue submission for the whole list, in recording order, so the buffers execute in
      // that order and the submission completes once. The queue is the browser device's one, so
      // work every logical device submits runs in the order it was submitted.
      var commandBuffers = record.recordedBuffers;
      record.recordedBuffers = [];
      record.recordingSerial = 0;
      DonnerGpu.queue.submit(commandBuffers);
      DonnerGpu.queue.onSubmittedWorkDone().then(function() {
        // Submissions complete in order, but the serial is recorded defensively as a maximum so a
        // completion observed out of order can never move the reported serial backwards. It is
        // this logical device's serial: each numbers its submissions on its own.
        if (submissionSerial > record.completedSerial) {
          record.completedSerial = submissionSerial;
        }
      });
    });
  },

  // ----- Host mapping -------------------------------------------------------

  donner_gpu_map_buffer_async__deps: ['$DonnerGpu'],
  donner_gpu_map_buffer_async: function(handle, mappingId, bufferId, offsetBytes, byteCount) {
    var record = DonnerGpu.logicalFor(handle);
    var buffer = DonnerGpu.lookup(record, DonnerGpu.kBuffer, bufferId);
    if (buffer === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kBuffer, bufferId);
    }
    var status = DonnerGpu.guard(record);
    if (status !== DonnerGpu.kSuccess) {
      return status;
    }
    var mapping = {
      buffer: buffer,
      offset: offsetBytes,
      size: byteCount,
      state: DonnerGpu.kMapPending,
      view: null,
    };
    var registered = DonnerGpu.register(record, DonnerGpu.kBufferMapping, mappingId, mapping);
    if (registered !== DonnerGpu.kSuccess) {
      return registered;
    }
    record.mappings.set(mappingId, mapping);
    try {
      buffer.mapAsync(GPUMapMode.READ, offsetBytes, byteCount)
        .then(function() {
          mapping.view = new Uint8Array(buffer.getMappedRange(offsetBytes, byteCount));
          mapping.state = DonnerGpu.kMapReady;
        })
        .catch(function() {
          mapping.state = DonnerGpu.lost ? DonnerGpu.kMapDeviceLost : DonnerGpu.kMapFailed;
        });
    } catch (e) {
      mapping.state = DonnerGpu.kMapFailed;
    }
    return DonnerGpu.kSuccess;
  },

  donner_gpu_mapping_state__deps: ['$DonnerGpu'],
  donner_gpu_mapping_state: function(handle, mappingId) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null) {
      return DonnerGpu.kMapFailed;
    }
    // Loss outranks whatever the mapping last recorded: it can never complete afterwards, and
    // reporting it as still pending would leave the caller waiting out its whole budget.
    if (DonnerGpu.lost) {
      return DonnerGpu.kMapDeviceLost;
    }
    var mapping = record.mappings.get(mappingId);
    return mapping === undefined ? DonnerGpu.kMapFailed : mapping.state;
  },

  donner_gpu_copy_mapped_bytes__deps: ['$DonnerGpu'],
  donner_gpu_copy_mapped_bytes: function(handle, mappingId, destination, byteCount) {
    var record = DonnerGpu.logicalFor(handle);
    if (record === null) {
      return DonnerGpu.kNotOwner;
    }
    var mapping = record.mappings.get(mappingId);
    if (mapping === undefined) {
      var refusal = DonnerGpu.refusalFor(record, DonnerGpu.kBufferMapping, mappingId);
      return refusal === DonnerGpu.kSuccess ? DonnerGpu.kFailed : refusal;
    }
    if (mapping.state !== DonnerGpu.kMapReady || mapping.view === null ||
        mapping.view.length < byteCount) {
      return DonnerGpu.kFailed;
    }
    HEAPU8.set(mapping.view.subarray(0, byteCount), destination);
    return DonnerGpu.kSuccess;
  },

  donner_gpu_unmap_buffer__deps: ['$DonnerGpu'],
  donner_gpu_unmap_buffer: function(handle, mappingId) {
    var record = DonnerGpu.logicalFor(handle);
    var released = DonnerGpu.guardRelease(record);
    if (released !== DonnerGpu.kSuccess) {
      return released;
    }
    var refusal = DonnerGpu.refusalFor(record, DonnerGpu.kBufferMapping, mappingId);
    if (refusal !== DonnerGpu.kSuccess) {
      return refusal;
    }
    var mapping = record.mappings.get(mappingId);
    record.objects.delete(mappingId);
    record.mappings.delete(mappingId);
    if (mapping === undefined) {
      return DonnerGpu.kSuccess;
    }
    // The view aliases memory the browser is about to take back, so it is dropped before the
    // unmap rather than left reachable.
    mapping.view = null;
    try {
      mapping.buffer.unmap();
    } catch (e) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.kSuccess;
  },

  // ----- Presentation -------------------------------------------------------

  donner_gpu_create_surface__deps: ['$DonnerGpu'],
  donner_gpu_create_surface: function(handle, id, canvasSelector, selectorBytes) {
    var selector = UTF8ToString(canvasSelector, selectorBytes);
    return DonnerGpu.create(DonnerGpu.logicalFor(handle), DonnerGpu.kSurface, id, function() {
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
  donner_gpu_surface_capabilities: function(handle, surfaceId, preferredFormatCode, usageBits) {
    var record = DonnerGpu.logicalFor(handle);
    var surface = DonnerGpu.lookup(record, DonnerGpu.kSurface, surfaceId);
    if (surface === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kSurface, surfaceId);
    }
    return DonnerGpu.perform(record, function() {
      var preferred = navigator.gpu.getPreferredCanvasFormat();
      HEAPU32[preferredFormatCode >> 2] = DonnerGpu.formatCode(preferred);
      // A canvas texture is a render attachment and can be copied from; the rest of the usage
      // vocabulary does not apply to one.
      HEAPU32[usageBits >> 2] = DonnerGpu.kUsageRenderAttachment | DonnerGpu.kUsageCopySrc;
    });
  },

  donner_gpu_surface_supports_alpha_mode__deps: ['$DonnerGpu'],
  donner_gpu_surface_supports_alpha_mode: function(handle, surfaceId, alphaModeCode, supported) {
    var record = DonnerGpu.logicalFor(handle);
    var surface = DonnerGpu.lookup(record, DonnerGpu.kSurface, surfaceId);
    if (surface === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kSurface, surfaceId);
    }
    return DonnerGpu.perform(record, function() {
      // What a canvas context can be configured with is exactly what this library has a canvas
      // value for, so the decoder that configure uses is what answers here. Reporting an alpha
      // mode configure would then refuse is the one answer this has to avoid.
      HEAPU32[supported >> 2] = DonnerGpu.alphaMode(alphaModeCode) === null ? 0 : 1;
    });
  },

  donner_gpu_configure_surface__deps: ['$DonnerGpu'],
  donner_gpu_configure_surface: function(handle, surfaceId, formatCode, usageBits, width, height,
                                         alphaModeCode) {
    var record = DonnerGpu.logicalFor(handle);
    var surface = DonnerGpu.lookup(record, DonnerGpu.kSurface, surfaceId);
    if (surface === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kSurface, surfaceId);
    }
    var format = DonnerGpu.textureFormat(formatCode);
    var alphaMode = DonnerGpu.alphaMode(alphaModeCode);
    if (!DonnerGpu.allDecoded([format, alphaMode])) {
      return DonnerGpu.kFailed;
    }
    return DonnerGpu.perform(record, function() {
      surface.canvas.width = width;
      surface.canvas.height = height;
      surface.context.configure({
        device: DonnerGpu.device,
        format: format,
        usage: DonnerGpu.textureUsage(usageBits),
        alphaMode: alphaMode,
      });
      DonnerGpu.releaseFrame(record, surface);
    });
  },

  donner_gpu_acquire_current_texture__deps: ['$DonnerGpu'],
  donner_gpu_acquire_current_texture: function(handle, surfaceId, textureId, surfaceStatusCode) {
    var record = DonnerGpu.logicalFor(handle);
    var surface = DonnerGpu.lookup(record, DonnerGpu.kSurface, surfaceId);
    if (surface === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kSurface, surfaceId);
    }
    var status = DonnerGpu.guard(record);
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
    var registered = DonnerGpu.register(record, DonnerGpu.kTexture, textureId, texture);
    if (registered !== DonnerGpu.kSuccess) {
      return registered;
    }
    record.objects.get(textureId).frame = true;
    surface.frame = textureId;
    HEAPU32[surfaceStatusCode >> 2] = DonnerGpu.kSurfaceSuccess;
    return DonnerGpu.kSuccess;
  },

  donner_gpu_abandon_current_texture__deps: ['$DonnerGpu'],
  donner_gpu_abandon_current_texture: function(handle, surfaceId) {
    var record = DonnerGpu.logicalFor(handle);
    var released = DonnerGpu.guardRelease(record);
    if (released !== DonnerGpu.kSuccess) {
      return released;
    }
    var surface = DonnerGpu.lookup(record, DonnerGpu.kSurface, surfaceId);
    if (surface === null) {
      return DonnerGpu.refusalFor(record, DonnerGpu.kSurface, surfaceId);
    }
    DonnerGpu.releaseFrame(record, surface);
    return DonnerGpu.kSuccess;
  },
};

mergeInto(LibraryManager.library, LibraryDonnerGpu);
