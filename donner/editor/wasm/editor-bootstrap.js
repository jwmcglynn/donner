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
