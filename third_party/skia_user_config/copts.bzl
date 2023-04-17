DEFAULT_COPTS = select({
    "@donner//:debug_build": ["-DSK_DEBUG"],
    "//conditions:default": [],
})

DEFAULT_OBJC_COPTS = []
