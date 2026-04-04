#!/bin/sh

# Normalize PATH for Bazel so Codex-specific helper directories do not perturb
# Bazel's effective client environment and action cache keys.

sanitize_path() {
  old_ifs=$IFS
  IFS=:
  set -- $PATH
  IFS=$old_ifs

  sanitized_path=""
  for dir in "$@"; do
    case "$dir" in
      "$HOME/.codex/tmp/arg0"/*)
        continue
        ;;
      */@openai/codex*/vendor/*/path)
        continue
        ;;
    esac

    if [ -n "$sanitized_path" ]; then
      sanitized_path="${sanitized_path}:$dir"
    else
      sanitized_path="$dir"
    fi
  done

  printf '%s\n' "$sanitized_path"
}

PATH="$(sanitize_path)"
export PATH

exec bazel "$@"
