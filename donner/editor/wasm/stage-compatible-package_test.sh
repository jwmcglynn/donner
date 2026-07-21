#!/usr/bin/env bash
set -euo pipefail

script="$1"
fixture_root="${TEST_TMPDIR}/stage-compatible-package"
geode_dir="${fixture_root}/geode"
tiny_skia_dir="${fixture_root}/tiny-skia"
output_dir="${fixture_root}/output"
mkdir -p "${geode_dir}" "${tiny_skia_dir}"

for file in index.html editor-bootstrap.js editor.css enable-threads.js donner_icon.svg \
  worker-surface-selector.js; do
  printf 'shared-%s\n' "${file}" > "${geode_dir}/${file}"
  cp "${geode_dir}/${file}" "${tiny_skia_dir}/${file}"
done
for file in editor.js editor.wasm; do
  printf 'geode-%s\n' "${file}" > "${geode_dir}/${file}"
  printf 'tiny-%s\n' "${file}" > "${tiny_skia_dir}/${file}"
done
"${script}" "${geode_dir}" "${tiny_skia_dir}" "${output_dir}"
cmp "${geode_dir}/worker-surface-selector.js" "${output_dir}/worker-surface-selector.js"
test ! -e "${output_dir}/fonts"
