const canvas = document.getElementById("canvas");
const status = document.getElementById("status");
const capabilityError = document.getElementById("capability-error");
const capabilityErrorDetail = document.getElementById("capability-error-detail");

function ShowCapabilityError(message) {
  capabilityErrorDetail.textContent = message;
  capabilityError.hidden = false;
  canvas.hidden = true;
  status.hidden = true;
  console.error(message);
}

function InstallTouchPointerBridge(targetCanvas) {
  if (!window.PointerEvent) {
    return;
  }

  let activeTouchId = null;
  const dispatchMouse = (type, event, buttons) => {
    targetCanvas.dispatchEvent(
      new MouseEvent(type, {
        bubbles: true,
        cancelable: true,
        view: window,
        clientX: event.clientX,
        clientY: event.clientY,
        button: 0,
        buttons,
      }),
    );
  };

  targetCanvas.style.touchAction = "none";
  targetCanvas.addEventListener("pointerdown", (event) => {
    if (event.pointerType !== "touch" || activeTouchId !== null) {
      return;
    }
    activeTouchId = event.pointerId;
    event.preventDefault();
    targetCanvas.focus();
    targetCanvas.setPointerCapture?.(event.pointerId);
    dispatchMouse("mousedown", event, 1);
  });
  targetCanvas.addEventListener("pointermove", (event) => {
    if (event.pointerType !== "touch" || event.pointerId !== activeTouchId) {
      return;
    }
    event.preventDefault();
    dispatchMouse("mousemove", event, 1);
  });
  const finishTouch = (event) => {
    if (event.pointerType !== "touch" || event.pointerId !== activeTouchId) {
      return;
    }
    event.preventDefault();
    dispatchMouse("mouseup", event, 0);
    targetCanvas.releasePointerCapture?.(event.pointerId);
    activeTouchId = null;
  };
  targetCanvas.addEventListener("pointerup", finishTouch);
  targetCanvas.addEventListener("pointercancel", finishTouch);
}

InstallTouchPointerBridge(canvas);

window.__donnerCanStartWasm = typeof SharedArrayBuffer !== "undefined";
if (!window.__donnerCanStartWasm) {
  const reason = window.isSecureContext
    ? "This page is secure, but cross-origin isolation is not active."
    : "This page is not running in a secure context.";
  ShowCapabilityError(`${reason} SharedArrayBuffer and Wasm threads are unavailable.`);
}

var Module = {
  preRun: [],
  postRun: [],
  print: function(text) {
    console.log(text);
  },
  printErr: function(text) {
    console.error(text);
  },
  setStatus: function(text) {
    status.textContent = text || "";
  },
  monitorRunDependencies: function(left) {
    if (left > 0) {
      status.textContent = `Loading… (${left} remaining)`;
    }
  },
  onRuntimeInitialized: function() {
    status.style.display = "none";
    canvas.focus();
  },
  locateFile: function(path, prefix) {
    if (path.endsWith(".wasm")) {
      return prefix + "editor.wasm";
    }
    return prefix + path;
  },
  canvas: canvas,
  contextAttributes: {
    preserveDrawingBuffer: true,
  },
};

canvas.addEventListener("contextmenu", function(event) {
  event.preventDefault();
});
canvas.addEventListener("webglcontextlost", function(event) {
  alert("WebGL context lost. Reload the page.");
  event.preventDefault();
}, false);

window.__donnerBackendPromise
  .then((backend) => {
    if (!window.__donnerCanStartWasm) {
      return;
    }
    const loader = document.createElement("script");
    loader.async = true;
    loader.type = "text/javascript";
    loader.src = backend.base + "editor.js";
    loader.addEventListener("error", () => {
      ShowCapabilityError(`Unable to load the ${backend.name} renderer package.`);
    });
    document.body.appendChild(loader);
  })
  .catch((error) => {
    if (window.__donnerCanStartWasm) {
      ShowCapabilityError(String(error));
    }
  });
