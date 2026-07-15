add_library(laghu_core STATIC
  src/core.c
)

add_library(Laghu::Core ALIAS laghu_core)

target_include_directories(laghu_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(laghu_core PUBLIC c_std_11)
set_target_properties(laghu_core PROPERTIES
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
elseif(MSVC)
  target_compile_options(laghu_core PRIVATE /W4 /WX)
endif()

if(LAGHU_BUILD_TESTS)
  add_executable(laghu_core_test tests/core_test.c)
  target_link_libraries(laghu_core_test PRIVATE Laghu::Core)

  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_core_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  elseif(MSVC)
    target_compile_options(laghu_core_test PRIVATE /W4 /WX)
  endif()

  add_test(NAME laghu_core_test COMMAND laghu_core_test)
endif()

