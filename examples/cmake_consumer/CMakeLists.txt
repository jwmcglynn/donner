cmake_minimum_required(VERSION 3.20)
project(donner_cmake_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(_donner_default_source_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(DONNER_SOURCE_DIR
    "${_donner_default_source_dir}"
    CACHE PATH "Path to a Donner source tree with generated CMakeLists.txt files")

if(NOT EXISTS "${DONNER_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Run python3 tools/cmake/gen_cmakelists.py in DONNER_SOURCE_DIR first")
endif()

add_subdirectory("${DONNER_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/donner")

add_executable(donner_cmake_consumer main.cc)
target_link_libraries(donner_cmake_consumer PRIVATE donner)

enable_testing()
add_test(NAME donner_cmake_consumer COMMAND donner_cmake_consumer)
