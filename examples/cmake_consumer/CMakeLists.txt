cmake_minimum_required(VERSION 3.20)
project(donner_cmake_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET donner)
  set(DONNER_SOURCE_DIR
      "${CMAKE_CURRENT_LIST_DIR}/../.."
      CACHE PATH "Path to a Donner source tree with generated CMakeLists.txt files")
  if(NOT EXISTS "${DONNER_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Run python3 tools/cmake/gen_cmakelists.py in DONNER_SOURCE_DIR first")
  endif()
  add_subdirectory("${DONNER_SOURCE_DIR}" donner)
endif()

add_executable(donner_cmake_consumer main.cc)
target_link_libraries(donner_cmake_consumer PRIVATE donner)

enable_testing()
add_test(NAME donner_cmake_consumer COMMAND donner_cmake_consumer)
