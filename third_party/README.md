# Third-Party Libraries

Third-party libraries are sometimes added as subtrees, and sometimes as bazel external repositories. If the library is small, it is added as a subtree.

## Subtrees

To list all subtrees, run:

```sh
git log | grep git-subtree-dir | tr -d ' ' | cut -d ":" -f2 | xargs -I {} bash -c 'if [ -d $(git rev-parse --show-toplevel)/{} ] ; then echo {}; fi'
```

The following subtrees have been added to the repo

```sh
git subtree add https://github.com/jwmcglynn/css-parsing-tests --prefix third_party/css-parsing-tests master --squash
git subtree add https://github.com/nothings/stb --prefix third_party/stb master --squash
git subtree add https://github.com/jwmcglynn/tiny-skia-cpp --prefix third_party/tiny-skia-cpp main --squash
```

## Updating

To update a subtree, run the `pull` command and pattern-match from an `add` command above:

```sh
git subtree pull https://github.com/jwmcglynn/css-parsing-tests --prefix third_party/css-parsing-tests master --squash
```

## Local modifications

`third_party/emscripten-glfw` carries Donner-specific changes to its JavaScript library
(`src/js/lib_emscripten_glfw3.js`) and to its C++ callers in `src/cpp/emscripten/glfw3/`
(`Clipboard.cpp`, `Context.cpp`, `Window.cpp`). Keep them when updating it:

- Window resize and teardown skip `canvas.width`/`canvas.height` writes once the canvas has been
  transferred to an `OffscreenCanvas`.
- The cursor, window title, clipboard, window focus and window size setters are proxied to the
  main thread asynchronously, so a pthread that owns the canvas never blocks on the main thread
  while a browser snapshots that canvas. The title and clipboard setters take a malloc-allocated
  copy, which the C++ callers make with `strdup` and the main thread frees.
  `//donner/editor/wasm/tests:main_thread_proxy_tests` lists the remaining synchronous calls.
- Keyboard events, which the listener receives on the main thread, are forwarded through Emscripten's
  system proxying queue to the thread that created the GLFW context, the same queue that carries
  mouse and focus events, so that thread applies all input in order. The main thread decides only
  whether to prevent the browser default.

`third_party/stb` carries Donner-specific changes. Keep them when updating it:

- `stb_image.h`: the PNG `IDAT` reader skips the copy for a zero-length chunk instead of calling
  `stbi__getn` with a length of 0.
- `stb_truetype.h`: GPOS pair kerning also follows Extension lookups (type 9) whose extension type
  is pair adjustment, which fonts such as Source Sans Pro use for all of their kerning. A 32-bit
  extension offset that leaves the GPOS table is skipped, and an Extension-wrapped PairPos
  subtable the reader cannot decode skips only that subtable instead of ending the walk.
  `//donner/svg/text:text_backend_simple_tests` covers these cases. Donner passes only trusted
  font bytes to stb_truetype.
