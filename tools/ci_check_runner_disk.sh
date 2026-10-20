#!/usr/bin/env bash
# Fails a macOS CI job in seconds, with a clear error, when the runner is low on disk,
# instead of minutes to an hour later as a Bazel ENOSPC, a job timeout or a lost runner
# connection, which read like failures of the change under test.
#
# MIN_FREE_DISK_GIB is the free space, in GiB, required on every filesystem the job writes.
# The self-hosted jobs set no --output_user_root, and --nohome_rc --nosystem_rc skip any user or
# system rc, so Bazel writes its macOS default output root and the `build:ci` repository cache.
# On a GitHub-hosted runner every one of these paths is on the same data volume.
set -euo pipefail
trap 'echo "::error title=Disk space check failed::The runner disk space check failed unexpectedly (exit $?, line $LINENO). Fix the check or the runner before retrying."' ERR
case "${MIN_FREE_DISK_GIB:-}" in
  '' | *[!0-9]*)
    echo "::error title=Invalid disk space threshold::MIN_FREE_DISK_GIB must be a whole number of GiB, got '${MIN_FREE_DISK_GIB:-}'."
    exit 1
    ;;
esac
# df follows a symlink at any of these paths; a path that does not exist yet
# is measured at its nearest existing ancestor.
rows=""
for location in \
  "workspace=${GITHUB_WORKSPACE}" \
  "runner temp=${RUNNER_TEMP}" \
  "TMPDIR=${TMPDIR:-/tmp}" \
  "Bazel output root=/private/var/tmp/_bazel_${USER:-$(id -un)}" \
  "Bazel repository cache=${HOME}/.cache/bazel-repo"; do
  label="${location%%=*}"
  path="${location#*=}"
  while [[ ! -e "$path" && ! -L "$path" ]]; do
    path="$(dirname "$path")"
  done
  # Anchor on the capacity column: device names and mount points can
  # contain spaces.
  rows+="$(df -Pk "$path" | awk -v label="$label" 'NR == 2 {
    for (c = 2; c <= NF && $c !~ /^[0-9]+%$/; c++) {}
    if (c < 5 || c >= NF || $(c - 1) !~ /^[0-9]+$/) exit 1
    device = $1
    for (i = 2; i <= c - 4; i++) device = device " " $i
    mount = $(c + 1)
    for (i = c + 2; i <= NF; i++) mount = mount " " $i
    printf "%s\t%s\t%s\t%s", device, $(c - 1), mount, label
    found = 1
  }
  END { exit !found }')"$'\n'
done
filesystems="$(printf '%s' "$rows" | awk -F '\t' '
  !($1 in labels) { order[++n] = $1; avail[$1] = $2; mount[$1] = $3; labels[$1] = $4; next }
  { labels[$1] = labels[$1] ", " $4 }
  END { for (i = 1; i <= n; i++) printf "%s\t%s\t%s\n", mount[order[i]], avail[order[i]], labels[order[i]] }')"
count=0
failed=0
all_labels=""
while IFS=$'\t' read -r mount avail_kib labels; do
  count=$((count + 1))
  all_labels+="${all_labels:+, }${labels}"
  if ((avail_kib < MIN_FREE_DISK_GIB * 1048576)); then
    echo "::error title=Runner low on disk space::Runner is below ${MIN_FREE_DISK_GIB} GiB free on ${mount} (${labels}). Free space on the runner before retrying."
    failed=1
  fi
done <<<"$filesystems"
if ((failed)); then
  exit 1
fi
noun=filesystems
if ((count == 1)); then
  noun=filesystem
fi
echo "Disk space check passed: at least ${MIN_FREE_DISK_GIB} GiB free on each of ${count} ${noun} (${all_labels})."
