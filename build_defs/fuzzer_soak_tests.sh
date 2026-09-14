#!/usr/bin/env bash
# Exercise the real soak launcher end to end: seed isolation, flag forwarding, artifact routing,
# and signal exit propagation. The fuzzer is a stub script so the checks stay hermetic and fast.
set -uo pipefail

runner="${TEST_SRCDIR}/${TEST_WORKSPACE}/build_defs/fuzzer_soak_runner"
if [[ ! -x "${runner}" ]]; then
  echo "launcher not found at ${runner}" >&2
  exit 1
fi

failures=0
check() {
  if [[ "$2" != "$3" ]]; then
    echo "FAIL $1: expected [$3], got [$2]" >&2
    failures=$((failures + 1))
  fi
}

work="$(mktemp -d "${TEST_TMPDIR:-/tmp}/soak-tests.XXXXXX")"
trap 'rm -rf "${work}"' EXIT

# Seeds that share a basename must both reach the corpus, and must not be modified in place.
mkdir -p "${work}/a space" "${work}/b"
printf 'first' > "${work}/a space/seed"
printf 'second' > "${work}/b/seed"

cat > "${work}/stub" <<'STUB'
#!/usr/bin/env bash
corpus="${!#}"
for seed in "${corpus}"/*; do cat "${seed}"; echo; done | sort | tr '\n' ',' > "${corpus}/../listing"
echo "flags=$1 contents=$(cat "${corpus}/../listing")"
printf 'generated' > "${corpus}/new-finding"
for argument in "$@"; do
  case "${argument}" in
    -artifact_prefix=*) printf 'repro' > "${argument#-artifact_prefix=}crash" ;;
  esac
done
exit "${STUB_EXIT:-70}"
STUB
chmod +x "${work}/stub"

outputs="${work}/outputs"
mkdir -p "${outputs}"
observed="$(TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${work}/stub" -runs=128 \
  "${work}/a space/seed" "${work}/b/seed")"
check "exit code is forwarded" "$?" "70"
check "flags are forwarded" "${observed%% contents=*}" "flags=-runs=128"
check "both same-name seeds are copied" "${observed##* contents=}" "first,second,"
check "first seed is untouched" "$(cat "${work}/a space/seed")" "first"
check "second seed is untouched" "$(cat "${work}/b/seed")" "second"
check "seed directory gains nothing" "$(ls "${work}/a space")" "seed"
check "findings are preserved" "$(cat "${outputs}/findings/crash")" "repro"

# An empty corpus is legal, and a fuzzer killed by a signal must surface as 128+N.
rm -rf "${outputs}"
mkdir -p "${outputs}"
cat > "${work}/killer" <<'KILL'
#!/usr/bin/env bash
corpus="${!#}"
if [[ -n "$(ls -A "${corpus}")" ]]; then
  echo "expected an empty corpus" >&2
  exit 1
fi
kill -ABRT $$
KILL
chmod +x "${work}/killer"
TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${work}/killer" > /dev/null 2>&1
check "signal exit maps to 128+N" "$?" "134"

# A seed that cannot be staged must fail the run. The stub exits zero, so a launcher that ignored
# the copy failure would report success with a silently shortened corpus.
cat > "${work}/ok" <<'OK'
#!/usr/bin/env bash
exit 0
OK
chmod +x "${work}/ok"
TEST_UNDECLARED_OUTPUTS_DIR="${outputs}" "${runner}" "${work}/ok" "${work}/missing-seed" \
  > /dev/null 2>&1
check "unstageable seed fails the run" "$(($? != 0))" "1"

exit "${failures}"
