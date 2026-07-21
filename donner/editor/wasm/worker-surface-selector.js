function SelectDonnerWorkerSurfaceMode(options) {
  if (["direct-surface", "bitmap-bridge"].includes(options.requestedMode)) {
    return options.requestedMode;
  }
  // WebKit can expose WebGPU on an OffscreenCanvas while intermittently
  // presenting a blank transferred canvas. Retain its frames through an
  // ImageBitmap handoff. Gecko's ImageBitmap handoff invalidates the worker
  // WebGPU swapchain, so it stays on direct surfaces with a retained layout.
  if (
    options.browserVendor === "Apple Computer, Inc."
    && options.browserEngine === "AppleWebKit"
  ) {
    return "bitmap-bridge";
  }
  return "direct-surface";
}

function SelectDonnerWorkerSurfaceLayoutPolicy(options) {
  void options;
  // A WebGPU surface already owns a browser-managed swapchain. Alternating two
  // independent DOM canvases adds a second, unsynchronized presentation layer:
  // Gecko can briefly expose the newly promoted canvas before its current
  // texture is latched, or revisit the other canvas's older drag epoch.
  return "single-visible";
}

function CreateDonnerWorkerSurfaceLayoutPlan(layout, policy) {
  const visibleSlot = Math.max(0, Math.min(1, Number(layout.surfaceSlot)));
  const hasVisibleSurface = Boolean(layout.visible) && layout.width > 0 && layout.height > 0;
  const retainInactiveSurface = hasVisibleSurface && policy === "occluded-double-buffer";
  return [0, 1].map((slot) => {
    const accepted = hasVisibleSurface && slot === visibleSlot;
    return {
      accepted,
      compositorVisible: accepted || retainInactiveSurface,
      slot,
      zIndex: accepted ? 1 : 0,
    };
  });
}

function CreateDonnerWorkerSurfacePixelLayout(layout, devicePixelRatio, backingSize = {}) {
  const scale = Number.isFinite(Number(devicePixelRatio)) && Number(devicePixelRatio) > 0
    ? Number(devicePixelRatio)
    : 1;
  const left = Number(layout.left);
  const top = Number(layout.top);
  const width = Number(layout.width);
  const height = Number(layout.height);
  if (
    !Number.isFinite(left) || !Number.isFinite(top) || !Number.isFinite(width)
    || !Number.isFinite(height) || width <= 0 || height <= 0
  ) {
    return { ...layout };
  }

  // The document bitmap and the ImGui overlay are separate compositor layers. The renderer's
  // layout is the live viewport projection and therefore owns CSS geometry; backing dimensions
  // describe only the resolution of the accepted epoch. Reusing an older backing store during an
  // immediate pan or zoom intentionally lets the browser resample that complete epoch until the
  // worker replaces it, keeping document pixels and overlay chrome in one coordinate system.
  void backingSize;
  const normalizeDeviceCoordinate = (value) => {
    const nearest = Math.round(value);
    return Math.abs(value - nearest) <= 1e-7 ? nearest : value;
  };
  const floorDevice = (value) => Math.floor(normalizeDeviceCoordinate(value * scale));
  const ceilDevice = (value) => Math.ceil(normalizeDeviceCoordinate(value * scale));
  const inset = (name) => {
    const value = Number(layout[name]);
    return Number.isFinite(value) ? Math.max(0, value) : 0;
  };
  const rawRight = left + width;
  const rawBottom = top + height;
  const clipLeftInset = inset("clipLeft");
  const clipTopInset = inset("clipTop");
  const clipRightInset = inset("clipRight");
  const clipBottomInset = inset("clipBottom");
  const clipLeft = clipLeftInset > 0
    ? floorDevice(Math.min(rawRight, left + clipLeftInset)) / scale - left
    : 0;
  const clipTop = clipTopInset > 0
    ? floorDevice(Math.min(rawBottom, top + clipTopInset)) / scale - top
    : 0;
  const clipRight = clipRightInset > 0
    ? rawRight - ceilDevice(Math.max(left, rawRight - clipRightInset)) / scale
    : 0;
  const clipBottom = clipBottomInset > 0
    ? rawBottom - ceilDevice(Math.max(top, rawBottom - clipBottomInset)) / scale
    : 0;

  return {
    ...layout,
    left,
    top,
    width,
    height,
    clipLeft,
    clipTop,
    clipRight,
    clipBottom,
  };
}

function CreateDonnerBitmapPresentationQueue(drawBackBuffer, applyLayout) {
  let acceptedToken = null;
  let acceptedSlot = null;
  let pendingLayout = null;
  let retiredThrough = 0;
  const stagedTokens = [null, null];

  const closeBitmap = (bitmap) => bitmap?.close?.();
  const normalizedSlot = (slot) => Math.max(0, Math.min(stagedTokens.length - 1, Number(slot)));
  const maybeApplyLayout = () => {
    if (
      !pendingLayout
      || acceptedToken !== pendingLayout.frameToken
      || acceptedSlot !== pendingLayout.surfaceSlot
      || stagedTokens[acceptedSlot] !== acceptedToken
    ) {
      return;
    }
    applyLayout(pendingLayout);
    retiredThrough = Math.max(retiredThrough, acceptedToken);
    pendingLayout = null;
  };

  return {
    stage(token, slot, bitmap, width, height) {
      const frameToken = Number(token);
      const surfaceSlot = normalizedSlot(slot);
      if (!Number.isFinite(frameToken) || frameToken <= retiredThrough || !bitmap) {
        closeBitmap(bitmap);
        return;
      }
      drawBackBuffer(surfaceSlot, bitmap, width, height, frameToken);
      stagedTokens[surfaceSlot] = frameToken;
      maybeApplyLayout();
    },
    commit(token, slot) {
      const frameToken = Number(token);
      if (!Number.isFinite(frameToken) || frameToken <= retiredThrough) {
        return;
      }
      acceptedToken = frameToken;
      acceptedSlot = normalizedSlot(slot);
      retiredThrough = Math.max(retiredThrough, frameToken - 1);
      maybeApplyLayout();
    },
    discard(token) {
      const frameToken = Number(token);
      if (!Number.isFinite(frameToken)) {
        return;
      }
      if (acceptedToken === frameToken) {
        acceptedToken = null;
        acceptedSlot = null;
      }
      if (pendingLayout?.frameToken === frameToken) {
        pendingLayout = null;
      }
      for (let slot = 0; slot < stagedTokens.length; ++slot) {
        if (stagedTokens[slot] === frameToken) {
          stagedTokens[slot] = null;
        }
      }
      retiredThrough = Math.max(retiredThrough, frameToken);
    },
    updateLayout(layout) {
      if (!layout.visible) {
        pendingLayout = null;
        applyLayout(layout);
        return;
      }
      pendingLayout = {
        ...layout,
        frameToken: Number(layout.frameToken),
        surfaceSlot: normalizedSlot(layout.surfaceSlot),
      };
      maybeApplyLayout();
    },
  };
}

globalThis.SelectDonnerWorkerSurfaceMode = SelectDonnerWorkerSurfaceMode;
globalThis.SelectDonnerWorkerSurfaceLayoutPolicy = SelectDonnerWorkerSurfaceLayoutPolicy;
globalThis.CreateDonnerWorkerSurfaceLayoutPlan = CreateDonnerWorkerSurfaceLayoutPlan;
globalThis.CreateDonnerWorkerSurfacePixelLayout = CreateDonnerWorkerSurfacePixelLayout;
globalThis.CreateDonnerBitmapPresentationQueue = CreateDonnerBitmapPresentationQueue;

if (typeof window !== "undefined") {
  const requestedMode = new URLSearchParams(window.location.search).get("workerSurface");
  const browserEngine = /AppleWebKit\//.test(navigator.userAgent || "")
    ? "AppleWebKit"
    : /Gecko\//.test(navigator.userAgent || "")
    ? "Gecko"
    : "other";
  window.__donnerWorkerSurfaceMode = SelectDonnerWorkerSurfaceMode({
    requestedMode,
    browserVendor: navigator.vendor || "",
    browserEngine,
  });
  window.__donnerWorkerSurfaceLayoutPolicy = SelectDonnerWorkerSurfaceLayoutPolicy({
    browserEngine,
    workerSurfaceMode: window.__donnerWorkerSurfaceMode,
  });
  if (typeof document !== "undefined" && document.documentElement) {
    document.documentElement.dataset.donnerWorkerSurfaceLayout =
      window.__donnerWorkerSurfaceLayoutPolicy;
  }
}
