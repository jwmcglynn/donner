#!/bin/bash

# Usage: wrap_hdoc.sh <hdoc-binary> <args...>

# Parses the --compilation-database parameter, copies the file and runs sed on it, replacing with $(pwd).
# For example: sed \"s#_EXEC_ROOT_#$(realpath $(pwd))#g\" %s > %s
# With the argument replaced, invokes hdoc-binary with the new args.

if [ $# -lt 1 ]; then
  echo "Usage: $0 <hdoc-binary> <args...>"
  exit 1
fi

hdoc_binary=$1
shift

compilation_database=""
new_args=()
previous_arg=""

for arg in "$@"
do
  if [ "$previous_arg" == "--compile-commands" ]; then
    compilation_database="$arg"
    previous_arg=""
  elif [ "$arg" == "--compile-commands" ]; then
    previous_arg="$arg"
  else
    new_args+=("$arg")
  fi
done

if [ -n "$compilation_database" ]; then
  modified_compilation_database=$(mktemp)
  sed "s#_EXEC_ROOT_#$(realpath $(pwd))#g" "$compilation_database" > "$modified_compilation_database"
  new_args+=("--compile-commands" "$modified_compilation_database")
fi

echo "Running $hdoc_binary ${new_args[@]}"
"$hdoc_binary" "${new_args[@]}"

if [ -n "$compilation_database" ]; then
  rm "$modified_compilation_database"
fi
