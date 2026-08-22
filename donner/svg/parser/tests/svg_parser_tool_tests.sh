#!/usr/bin/env bash

set -euo pipefail

readonly tool="${TEST_SRCDIR}/_main/donner/svg/parser/svg_parser_tool"
readonly bidi_control="$(printf '\342\200\256')"
readonly input="${TEST_TMPDIR}/safe.svg"
printf '<svg xmlns="http://www.w3.org/2000/svg"><g id="safe%sevil"/></svg>' \
  "${bidi_control}" >"${input}"

output="$("${tool}" "${input}" 2>&1)"
if [[ "${output}" != *'safe\u202eevil'* ]]; then
  echo "svg_parser_tool did not escape a bidi control in an element id" >&2
  exit 1
fi
if [[ "${output}" == *"${bidi_control}"* ]]; then
  echo "svg_parser_tool emitted a raw bidi control" >&2
  exit 1
fi
