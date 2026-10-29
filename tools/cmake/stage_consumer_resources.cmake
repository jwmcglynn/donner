# Replace only this example's generated executable companion tree. A merge-copy leaves
# obsolete digest paths that the runtime correctly rejects as a mixed installation.
if(NOT IS_DIRECTORY "${SOURCE}" OR NOT EXISTS "${SOURCE}/manifest.json")
  message(FATAL_ERROR "A complete generated resource source directory is required")
endif()
get_filename_component(_destination_name "${DESTINATION}" NAME)
if(NOT _destination_name STREQUAL "donner-assets" OR "${SOURCE}" STREQUAL "${DESTINATION}")
  message(FATAL_ERROR "The destination must be the generated donner-assets companion directory")
endif()
file(REMOVE_RECURSE "${DESTINATION}")
file(COPY "${SOURCE}/" DESTINATION "${DESTINATION}")
