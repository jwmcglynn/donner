#!/bin/bash -e
cd "${0%/*}"

bazel build -c opt //src/svg/xml:xml_tool //src/svg/renderer:renderer_tool

du -h ../bazel-bin/src/svg/xml/xml_tool
du -h ../bazel-bin/src/svg/renderer/renderer_tool
