# Experimental WASM Support

To see the demo:
```sh
bazel run //experimental/wasm:serve_http
```

This builds Donner for wasm and uses the browser's Canvas as the rendering backend, effectively running Donner in a browser. At the moment this is redundant, since browsers already implement support for SVGs, but eventually this may evolve into a UI for Donner and debugging tools.
