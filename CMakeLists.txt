cmake_minimum_required(VERSION 3.20)
project(donner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

enable_testing()

# External dependency: EnTT
FetchContent_Declare(
  entt
  GIT_REPOSITORY https://github.com/skypjack/entt.git
  GIT_TAG        v3.13.2
)
FetchContent_MakeAvailable(entt)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
  rules_cc
  GIT_REPOSITORY https://github.com/bazelbuild/rules_cc.git
  GIT_TAG        0.1.1
)
FetchContent_MakeAvailable(rules_cc)

execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
  ${rules_cc_SOURCE_DIR} ${CMAKE_BINARY_DIR}/rules_cc
  RESULT_VARIABLE _ignored)
add_library(rules_cc_runfiles ${rules_cc_SOURCE_DIR}/cc/runfiles/runfiles.cc)
target_include_directories(rules_cc_runfiles PUBLIC ${CMAKE_BINARY_DIR})

add_subdirectory(donner/base)
add_subdirectory(third_party/frozen)
add_subdirectory(donner/css)
