#!/bin/bash -e
cd "$(dirname "$0")/.."

directory="external/llvm_toolchain_llvm/lib"

# Loop through each .a file
for file in $directory/*.a; do
    echo "Processing $file ..."
    # Use nm tool to list symbols and append them to a file
    echo "Symbols from $file:" >> symbols.txt
    nm -C $file >> symbols.txt
    echo "-------------------" >> symbols.txt
done

echo "Saved to symbols.txt"
