#!/usr/bin/env bash

set -euo pipefail

readonly tool="${TEST_SRCDIR}/_main/donner/base/xml/xml_tool"
readonly bidi_control="$(printf '\342\200\256')"
readonly input="${TEST_TMPDIR}/unsafe${bidi_control}evil.xml"

status=0
output="$("${tool}" "${input}" 2>&1)" || status=$?
if [[ ${status} -ne 2 ]]; then
  echo "xml_tool returned ${status} instead of the expected file-read failure" >&2
  exit 1
fi
if [[ "${output}" != *'unsafe\u202eevil.xml'* ]]; then
  echo "xml_tool did not escape the bidi control in an attacker-controlled filename" >&2
  exit 1
fi
if [[ "${output}" == *"${bidi_control}"* ]]; then
  echo "xml_tool emitted a raw bidi control from an attacker-controlled filename" >&2
  exit 1
fi
