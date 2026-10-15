cmake_minimum_required(VERSION 3.24)
project(donner_svg_suite LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5)
FetchContent_Declare(pixelmatch
  URL https://github.com/jwmcglynn/pixelmatch-cpp17/releases/download/v2.0.0/pixelmatch-cpp17-2.0.0.tar.gz
  URL_HASH SHA256=04f3e6aff96094447c147a5ac2d3dcbeb7716c09cc5d83089b2ab3c8a7514852
  SOURCE_SUBDIR suite_no_cmake)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/include.zip
  URL_HASH SHA256=b8cb0ef2dd7f57f18933997c9934bb1fa962594f701cd5a8d3c2c80541559372
  SOURCE_SUBDIR suite_no_cmake)
FetchContent_Declare(picosha2
  URL https://github.com/okdshin/PicoSHA2/archive/refs/tags/v1.0.1.tar.gz
  URL_HASH SHA256=9983136544234e573fe07cc1a22fdf978ad7979043e7be2e9082f6d4991ff8a8
  SOURCE_SUBDIR suite_no_cmake)
FetchContent_MakeAvailable(googletest pixelmatch json picosha2)
get_filename_component(SUITE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_library(suite_pixelmatch "${pixelmatch_SOURCE_DIR}/src/pixelmatch/pixelmatch.cc")
target_include_directories(suite_pixelmatch PUBLIC "${pixelmatch_SOURCE_DIR}/src")
add_library(suite_image ImageCompare.cc StbStandalone.cc)
target_include_directories(suite_image PUBLIC "${SUITE_ROOT}")
target_include_directories(suite_image SYSTEM PRIVATE "${SUITE_ROOT}/third_party")
target_link_libraries(suite_image PUBLIC suite_pixelmatch)
add_library(svg_suite StaticSuite.cc GtestSuite.cc FileFingerprint.cc TextEncoding.cc)
target_include_directories(svg_suite SYSTEM PRIVATE "${json_SOURCE_DIR}/include" "${picosha2_SOURCE_DIR}")
target_link_libraries(svg_suite PUBLIC suite_image GTest::gtest)
if(UNIX)
  add_library(suite_provenance Provenance.cc)
  target_include_directories(suite_provenance SYSTEM PRIVATE "${json_SOURCE_DIR}/include" "${picosha2_SOURCE_DIR}")
  target_link_libraries(suite_provenance PUBLIC svg_suite)
  add_library(suite_process ProcessRenderer.cc)
  target_link_libraries(suite_process PUBLIC svg_suite)
  target_include_directories(suite_process SYSTEM PRIVATE "${json_SOURCE_DIR}/include" "${picosha2_SOURCE_DIR}")
  add_executable(donner-svg-suite SuiteMain.cc)
  target_link_libraries(donner-svg-suite PRIVATE suite_process suite_provenance)
endif()
enable_testing()
add_executable(svg-suite-tests SvgSuite_tests.cc)
target_link_libraries(svg-suite-tests PRIVATE svg_suite GTest::gtest_main GTest::gmock)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-work")
add_test(NAME svg-suite-tests COMMAND svg-suite-tests)
set_tests_properties(svg-suite-tests PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/test-work")
if(UNIX)
  add_executable(process-renderer-tests ProcessRenderer_tests.cc)
  target_link_libraries(process-renderer-tests PRIVATE suite_process GTest::gmock)
  add_test(NAME process-renderer-tests COMMAND process-renderer-tests)
  set_tests_properties(process-renderer-tests PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/test-work")
endif()
# The source bundle includes the complete pinned upstream corpus and extension without Donner.
install(DIRECTORY "${SUITE_ROOT}/tools/svg_suite" DESTINATION tools)
install(DIRECTORY "${SUITE_ROOT}/third_party/resvg-test-suite" DESTINATION third_party)
install(DIRECTORY "${SUITE_ROOT}/donner/svg/renderer/tests/donner_svg2_corpus"
  DESTINATION donner/svg/renderer/tests)
install(FILES "${SUITE_ROOT}/third_party/stb/stb_image.h"
              "${SUITE_ROOT}/third_party/stb/stb_image_write.h"
              "${SUITE_ROOT}/third_party/stb/LICENSE" DESTINATION third_party/stb)
install(FILES "${SUITE_ROOT}/LICENSE" DESTINATION .)
if(UNIX)
  install(TARGETS donner-svg-suite RUNTIME DESTINATION bin)
endif()

add_executable(file-fingerprint-tests FileFingerprint_tests.cc)
target_link_libraries(file-fingerprint-tests PRIVATE svg_suite GTest::gtest_main GTest::gmock)
add_test(NAME file-fingerprint-tests COMMAND file-fingerprint-tests)
set_tests_properties(file-fingerprint-tests PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/test-work")

if(UNIX)
  add_executable(provenance-tests Provenance_tests.cc)
  target_include_directories(provenance-tests SYSTEM PRIVATE "${json_SOURCE_DIR}/include")
  target_link_libraries(provenance-tests PRIVATE suite_provenance GTest::gtest_main GTest::gmock)
  add_test(NAME provenance-tests COMMAND provenance-tests)
  set_tests_properties(provenance-tests PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/test-work")
endif()

if(UNIX)
  add_executable(gtest-suite-tests GtestSuite_tests.cc)
  target_link_libraries(gtest-suite-tests PRIVATE suite_provenance suite_process GTest::gmock)
  add_test(NAME gtest-suite-tests COMMAND gtest-suite-tests)
  set_tests_properties(gtest-suite-tests PROPERTIES ENVIRONMENT "TEST_TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/test-work")
endif()

install(FILES "${googletest_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME googletest-LICENSE)
install(FILES "${pixelmatch_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME pixelmatch-cpp17-LICENSE)
install(FILES "${pixelmatch_SOURCE_DIR}/third_party/pixelmatch/LICENSE"
  DESTINATION licenses RENAME pixelmatch-original-LICENSE)
install(FILES "${json_SOURCE_DIR}/LICENSE.MIT" DESTINATION licenses RENAME nlohmann-json-LICENSE)
install(FILES "${picosha2_SOURCE_DIR}/LICENSE" DESTINATION licenses RENAME picosha2-LICENSE)

install(FILES "${SUITE_ROOT}/docs/svg2_audit.md" "${SUITE_ROOT}/docs/svg2_audit_baseline.json"
              "${SUITE_ROOT}/docs/svg2_chapter_review.md"
  DESTINATION docs)
install(FILES "${SUITE_ROOT}/docs/design_docs/0057-donner_svg2_test_suite.md"
              "${SUITE_ROOT}/docs/design_docs/0026-svg_conformance_testing.md"
  DESTINATION docs/design_docs)

add_executable(text-encoding-tests TextEncoding_tests.cc)
target_link_libraries(text-encoding-tests PRIVATE svg_suite GTest::gtest_main)
add_test(NAME text-encoding-tests COMMAND text-encoding-tests)
