# Getting started with Donner from CMake

This example is a standalone CMake project for users who want to add Donner to their own CMake
application from a Donner source checkout. It imports Donner with `add_subdirectory()`, links the
exported `donner` target, parses a small SVG document, queries the DOM, renders it, and exits
non-zero if any step fails.

Fonts are external package resources. With text enabled, deploy the generated fallback-font
directory beside the executable as `donner-assets`; linking the library alone does not copy data.
The example does this in its post-build step. Keep the complete directory, including its manifest,
with the executable when relocating or packaging it. A host with a different installation layout
can explicitly install a `PackagedResourceDirectoryProvider` instead. Resource lookup never uses
the working directory. No-text builds do not need this font data.

The important part is the same pattern an external project should use:

```cmake
set(DONNER_SOURCE_DIR "/path/to/donner" CACHE PATH "Path to Donner")
add_subdirectory("${DONNER_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/donner")

add_executable(my_app main.cc)
target_link_libraries(my_app PRIVATE donner)

if(DONNER_TEXT)
  get_target_property(font_assets donner_third_party_public-sans_public-sans DONNER_RESOURCE_DIRECTORY)
  add_custom_command(TARGET my_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${font_assets}"
            "$<TARGET_FILE_DIR:my_app>/donner-assets"
    VERBATIM)
endif()
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
