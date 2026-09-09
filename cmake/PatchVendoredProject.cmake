# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED SOURCE_DIR OR NOT DEFINED PROJECT_ID)
  message(FATAL_ERROR "Laghu vendored dependency patch requires SOURCE_DIR and PROJECT_ID")
endif()

set(project_cmake "${SOURCE_DIR}/CMakeLists.txt")
if(NOT EXISTS "${project_cmake}")
  message(FATAL_ERROR "Laghu vendored dependency patch failed: project=${PROJECT_ID} rule=cmake_missing")
endif()
file(READ "${project_cmake}" contents)
string(FIND "${contents}" "add_custom_target(check" check_offset)
if(check_offset EQUAL -1)
  message(FATAL_ERROR "Laghu vendored dependency patch failed: project=${PROJECT_ID} rule=check_target_missing")
endif()
string(REPLACE "add_custom_target(check"
  "add_custom_target(laghu_vendor_${PROJECT_ID}_check" patched_contents "${contents}")
file(WRITE "${project_cmake}" "${patched_contents}")
