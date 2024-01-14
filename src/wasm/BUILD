load("@emsdk//emscripten_toolchain:wasm_rules.bzl", "wasm_cc_binary")
load(":web_package.bzl", "web_package")
load("//tools/http:serve_http.bzl", "serve_http")

DEFAULT_EMSCRIPTEN_LINKOPTS = [
    "--bind",  # Compiles the source code using the Embind bindings to connect C/C++ and JavaScript
]

WASM_LINKOPTS = [
    "-s WASM=1",  # Specify wasm output
] + DEFAULT_EMSCRIPTEN_LINKOPTS + select({
    ":debug_opts": [
        "-s DEMANGLE_SUPPORT=1",  # Enable C++ name demangling in stack traces
        "-s ASSERTIONS=2",  # Enable runtime checks
        "-s SAFE_HEAP=1",  # Enable runtime checks
        "-s STACK_OVERFLOW_CHECK=2",  # Enable runtime checks
    ],
    ":release_opts": [],
    "//conditions:default": [],
})

config_setting(
    name = "release_opts",
    values = {"compilation_mode": "opt"},
)

config_setting(
    name = "debug_opts",
    values = {"compilation_mode": "dbg"},
)

cc_binary(
    name = "debugger_bin",
    srcs = [
        "debugger_main.cc",
    ],
    copts = ["-frtti"],
    linkopts = WASM_LINKOPTS,
    # This target won't build successfully on its own because of missing emscripten
    # headers etc. Therefore, we hide it from wildcards.
    tags = ["manual"],
    deps = [
        "//src/svg/renderer",
        "//src/svg/renderer/wasm_canvas",
        "//src/svg/xml",
    ],
)

wasm_cc_binary(
    name = "debugger_wasm",
    cc_target = ":debugger_bin",
    outputs = [
        "debugger_bin.js",
        "debugger_bin.wasm",
    ],
    tags = ["wasm"],
)

web_package(
    name = "debugger_web_package",
    srcs = [
        "index.html",
    ],
    tags = ["wasm"],
    wasm_deps = [
        ":debugger_wasm",
    ],
)

serve_http(
    name = "serve_http",
    dir = ":debugger_web_package",
    tags = ["wasm"],
)
