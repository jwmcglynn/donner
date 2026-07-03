# Getting started with Donner from CMake

This example is a standalone CMake project for users who want to add Donner to their own CMake
application from a Donner source checkout. It imports Donner with `add_subdirectory()`, links the
exported `donner` target, parses a small SVG document, queries the DOM, renders it, and exits
non-zero if any step fails.

The important part is the same pattern an external project should use:

```cmake
set(DONNER_SOURCE_DIR "/path/to/donner" CACHE PATH "Path to Donner")
add_subdirectory("${DONNER_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/donner")

add_executable(my_app main.cc)
target_link_libraries(my_app PRIVATE donner)
```

Generate Donner's CMake files before configuring your project:

```sh
python3 tools/cmake/gen_cmakelists.py
cmake -S examples/cmake_consumer -B build/cmake-consumer
cmake --build build/cmake-consumer --target donner_cmake_consumer
ctest --test-dir build/cmake-consumer --output-on-failure
```

From another source tree, point the example at a Donner checkout:

```sh
cmake -S /path/to/donner/examples/cmake_consumer -B build/cmake-consumer \
  -DDONNER_SOURCE_DIR=/path/to/donner
cmake --build build/cmake-consumer --target donner_cmake_consumer
ctest --test-dir build/cmake-consumer --output-on-failure
```
