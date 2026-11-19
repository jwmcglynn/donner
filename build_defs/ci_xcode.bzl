"""Xcode identities shared by the remote platform and its Darwin transition."""

# The approved (version, build) pairs of tools/ci_macos_xcode.txt, in its preference order.
PINNED_XCODES = [
    ("27.1", "27A9275"),
    ("27.2", "27B52"),
]

def ci_xcode_identity(selected):
    """The approved identity for a selected remote Xcode version or build."""
    for version, build in PINNED_XCODES:
        parts = version.split(".")
        dotted = ".".join(parts + ["0"] * (3 - len(parts)))
        if selected in (version, dotted, build, dotted + "." + build):
            return "DONNER_CI_XCODE_IDENTITY=" + version + "-" + build
    fail("Unapproved remote Xcode: " + selected)
