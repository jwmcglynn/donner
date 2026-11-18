#!/bin/sh
set -eu
actual="$(uname -s) $(uname -m)"
expected="$1 $2"
printf 'test worker: %s\n' "$actual"
test "$actual" = "$expected"
