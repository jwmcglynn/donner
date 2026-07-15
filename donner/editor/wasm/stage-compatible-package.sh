#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 GEODE_PACKAGE TINY_SKIA_PACKAGE OUTPUT_DIRECTORY" >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
geode_dir="$(cd "$1" && pwd -P)"
tiny_skia_dir="$(cd "$2" && pwd -P)"
output_dir="$3"

if [[ -e "$output_dir" ]]; then
  echo "error: output already exists: $output_dir" >&2
  exit 1
fi

shared_files=(index.html editor-bootstrap.js editor.css enable-threads.js donner_icon.svg)
backend_files=(editor.js editor.wasm)
for file in "${shared_files[@]}"; do
  [[ -f "${geode_dir}/${file}" ]] || { echo "error: missing Geode ${file}" >&2; exit 1; }
  [[ -f "${tiny_skia_dir}/${file}" ]] || {
    echo "error: missing TinySkia ${file}" >&2
    exit 1
  }
  cmp "${geode_dir}/${file}" "${tiny_skia_dir}/${file}"
done
for file in "${backend_files[@]}"; do
  [[ -f "${geode_dir}/${file}" ]] || { echo "error: missing Geode ${file}" >&2; exit 1; }
  [[ -f "${tiny_skia_dir}/${file}" ]] || {
    echo "error: missing TinySkia ${file}" >&2
    exit 1
  }
done

mkdir -p "${output_dir}/geode" "${output_dir}/tiny_skia"
for file in "${shared_files[@]}"; do
  cp "${geode_dir}/${file}" "${output_dir}/${file}"
done
cp "${script_dir}/backend-selector.js" "${output_dir}/backend-selector.js"
for file in "${backend_files[@]}"; do
  cp "${geode_dir}/${file}" "${output_dir}/geode/${file}"
  cp "${tiny_skia_dir}/${file}" "${output_dir}/tiny_skia/${file}"
done

actual_files="$(cd "$output_dir" && find . -type f | LC_ALL=C sort)"
expected_files="$(printf '%s\n' \
  ./backend-selector.js \
  ./donner_icon.svg \
  ./editor-bootstrap.js \
  ./editor.css \
  ./enable-threads.js \
  ./geode/editor.js \
  ./geode/editor.wasm \
  ./index.html \
  ./tiny_skia/editor.js \
  ./tiny_skia/editor.wasm)"
if [[ "$actual_files" != "$expected_files" ]]; then
  echo "error: staged package inventory mismatch" >&2
  diff <(printf '%s\n' "$expected_files") <(printf '%s\n' "$actual_files") >&2 || true
  exit 1
fi
