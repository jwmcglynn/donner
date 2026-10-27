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

if(DONNER_TEXT)
  get_target_property(_donner_font_assets donner_third_party_public-sans_public-sans
                      DONNER_RESOURCE_DIRECTORY)
  if(NOT _donner_font_assets)
    message(FATAL_ERROR "The Donner fallback font resource directory is unavailable")
  endif()
  add_custom_command(TARGET donner_cmake_consumer POST_BUILD
    COMMAND ${CMAKE_COMMAND} "-DSOURCE=${_donner_font_assets}"
            "-DDESTINATION=$<TARGET_FILE_DIR:donner_cmake_consumer>/donner-assets"
            -P "${DONNER_SOURCE_DIR}/tools/cmake/stage_consumer_resources.cmake"
    VERBATIM)
endif()

enable_testing()
add_test(NAME donner_cmake_consumer COMMAND donner_cmake_consumer)
if(DONNER_TEXT)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME donner_cmake_consumer_resources
    COMMAND ${Python3_EXECUTABLE} "${DONNER_SOURCE_DIR}/tools/cmake/consumer_resources_test.py"
            --binary "$<TARGET_FILE:donner_cmake_consumer>" --assets "${_donner_font_assets}"
            --cmake "${CMAKE_COMMAND}")
endif()
