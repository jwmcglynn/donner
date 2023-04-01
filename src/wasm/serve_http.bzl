def _serve_http_impl(ctx):
    dir = ctx.attr.dir.files.to_list()[0]
    simple_webserver = ctx.executable._simple_webserver

    script = ctx.actions.declare_file("serve_http.sh")

    # Create a shell script that runs the simple_webserver binary with the dir directory
    ctx.actions.write(script, "\n".join([
        "#!/bin/bash",
        "{} --dir \"$(dirname {})\"".format(simple_webserver.short_path, dir.short_path),
    ]), is_executable = True)

    # The datafile must be in the runfiles for the executable to see it.
    runfiles = ctx.runfiles(files = ctx.attr.dir.files.to_list())
    runfiles = runfiles.merge(ctx.attr._simple_webserver[DefaultInfo].default_runfiles)
    return [DefaultInfo(executable = script, runfiles = runfiles)]

serve_http = rule(
    implementation = _serve_http_impl,
    attrs = {
        "dir": attr.label(allow_files = True),
        "_simple_webserver": attr.label(
            default = "//src/wasm:simple_webserver",
            executable = True,
            cfg = "exec",
        ),
    },
    executable = True,
)
