#!/usr/bin/env bash
# Run libFuzzer against a writable copy of the declared seed corpus.
#
# Seeds arrive as read-only runfiles and may share a basename, so they are copied into a scratch
# directory under numeric names before the fuzzer runs. Keeping this launcher in shell also keeps
# an interpreter out of the soak target's runfiles: a Python launcher resolves its runtime library
# through an rpath relative to its own location, which does not survive execution from a relocated
# content-addressed runfiles tree.
set -euo pipefail

binary="$1"
shift

scratch="$(mktemp -d "${TEST_TMPDIR:-${TMPDIR:-/tmp}}/fuzzer-soak.XXXXXX")"
trap 'rm -rf "${scratch}"' EXIT
corpus="${scratch}/corpus"
mkdir -p "${corpus}"

flags=()
index=0
for argument in "$@"; do
  case "${argument}" in
    -*) flags+=("${argument}") ;;
    *)
      cp -- "${argument}" "${corpus}/${index}"
      index=$((index + 1))
      ;;
  esac
done

artifacts="${TEST_UNDECLARED_OUTPUTS_DIR:-${scratch}}/findings"
mkdir -p "${artifacts}"

# Staging above runs under errexit, so a seed that cannot be copied fails the run rather than
# silently shrinking the corpus. The fuzzer itself must not be intercepted: its status is the
# launcher's, including 128+N when a signal kills it.
set +e
"${binary}" ${flags[@]+"${flags[@]}"} "-artifact_prefix=${artifacts}/" "${corpus}"
exit $?
