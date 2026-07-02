# Donner CMake consumer example

This example is a standalone CMake project that links the exported `donner` target, parses a small
SVG document, queries the DOM, renders it, and exits non-zero if any step fails.

Generate Donner's CMake files before configuring the example:

```sh
python3 tools/cmake/gen_cmakelists.py
cmake -S examples/cmake_consumer -B build/cmake-consumer
cmake --build build/cmake-consumer --target donner_cmake_consumer
ctest --test-dir build/cmake-consumer --output-on-failure
```

From outside the repository, point the example at a Donner checkout:

```sh
cmake -S /path/to/donner/examples/cmake_consumer -B build/cmake-consumer \
  -DDONNER_SOURCE_DIR=/path/to/donner
cmake --build build/cmake-consumer --target donner_cmake_consumer
ctest --test-dir build/cmake-consumer --output-on-failure
```

The top-level Donner CMake build also includes this example when configured with
`-DDONNER_BUILD_EXAMPLES=ON`. CI uses that mode so the exported `donner` target is checked in the
same build tree as the generated libraries.
