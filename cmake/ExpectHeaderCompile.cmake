# SPDX-License-Identifier: AGPL-3.0-only
if(NOT DEFINED CXX OR NOT DEFINED HEADER OR NOT DEFINED INCLUDE_DIRECTORIES)
  message(FATAL_ERROR "Laghu header compile expectation requires CXX, HEADER, and INCLUDE_DIRECTORIES")
endif()
if(DEFINED CXXFLAGS)
  separate_arguments(cxxflags NATIVE_COMMAND "${CXXFLAGS}")
else()
  set(cxxflags)
endif()
set(include_flags)
foreach(include_directory IN LISTS INCLUDE_DIRECTORIES)
  list(APPEND include_flags "-I${include_directory}")
endforeach()
string(SHA256 header_id "${HEADER}")
set(source "${CMAKE_CURRENT_BINARY_DIR}/header-${header_id}.cpp")
file(WRITE "${source}" "#include <${HEADER}>\nint main() { return 0; }\n")
execute_process(
  COMMAND "${CXX}" -std=c++23 ${cxxflags} -pedantic-errors -fno-exceptions -fno-rtti ${include_flags}
    -c "${source}" -o "${CMAKE_CURRENT_BINARY_DIR}/header-${header_id}.o"
  RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Laghu header self-containment failed: header=${HEADER}\n${stdout}${stderr}")
endif()
