load("@emsdk//emscripten_toolchain:wasm_rules.bzl", "wasm_cc_binary")
load(":web_package.bzl", "web_package")
load(":serve_http.bzl", "serve_http")

DEFAULT_EMSCRIPTEN_LINKOPTS = [
    "--bind",  # Compiles the source code using the Embind bindings to connect C/C++ and JavaScript
]

WASM_LINKOPTS = [
    "-s WASM=1",  # Specify wasm output
]

cc_binary(
    name = "hello_world",
    srcs = [
        "hello_world.cc",
    ],
    linkopts = DEFAULT_EMSCRIPTEN_LINKOPTS + WASM_LINKOPTS,
    # This target won't build successfully on its own because of missing emscripten
    # headers etc. Therefore, we hide it from wildcards.
    tags = ["manual"],
)

wasm_cc_binary(
    name = "hello_world_wasm",
    cc_target = ":hello_world",
    outputs = [
        "hello_world.js",
        "hello_world.wasm",
    ],
)

web_package(
    name = "hello_world_web",
    srcs = [
        "index.html",
    ],
    wasm_deps = [
        "hello_world_wasm",
    ],
)

py_binary(
    name = "simple_webserver",
    srcs = ["simple_webserver.py"],
)

serve_http(
    name = "serve_http",
    dir = ":hello_world_web",
)
