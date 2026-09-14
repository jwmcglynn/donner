# Keep CMake's pinned WOFF2 decoder identical to the Bazel source. FetchContent may run a patch
# step again after reconfiguration, so accept only a clean apply or an exact already-applied patch.
if(NOT DEFINED SOURCE_DIR OR NOT DEFINED PATCH_FILE OR NOT EXISTS "${PATCH_FILE}")
  message(FATAL_ERROR "WOFF2 patch source and patch file are required")
endif()
find_package(Git REQUIRED)
execute_process(
  COMMAND "${GIT_EXECUTABLE}" apply --check "${PATCH_FILE}"
  WORKING_DIRECTORY "${SOURCE_DIR}"
  RESULT_VARIABLE check_result
  OUTPUT_QUIET ERROR_QUIET
)
if(check_result EQUAL 0)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE apply_result
    ERROR_VARIABLE apply_error
  )
  if(NOT apply_result EQUAL 0)
    message(FATAL_ERROR "Could not apply the pinned WOFF2 patch: ${apply_error}")
  endif()
else()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE reverse_result
    OUTPUT_QUIET ERROR_QUIET
  )
  if(NOT reverse_result EQUAL 0)
    message(FATAL_ERROR "WOFF2 source matches neither the pinned original nor the patched source")
  endif()
endif()
