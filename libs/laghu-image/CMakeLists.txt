# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set(LAGHU_HAVE_VIPS OFF)

if(NOT LAGHU_WITH_VIPS MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "LAGHU_WITH_VIPS must be AUTO, ON, or OFF")
endif()

if(NOT LAGHU_WITH_VIPS STREQUAL "OFF")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(VIPS QUIET IMPORTED_TARGET vips>=8.15)
  endif()
  if(VIPS_FOUND)
    set(LAGHU_HAVE_VIPS ON)
    set(LAGHU_VIPS_TARGET PkgConfig::VIPS)
  else()
    find_path(VIPS_INCLUDE_DIR NAMES vips/vips.h)
    find_library(VIPS_LIBRARY NAMES vips libvips)
    if(VIPS_INCLUDE_DIR AND VIPS_LIBRARY)
      add_library(LaghuVips UNKNOWN IMPORTED)
      set_target_properties(LaghuVips PROPERTIES
        IMPORTED_LOCATION "${VIPS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${VIPS_INCLUDE_DIR}"
      )
      set(LAGHU_VIPS_TARGET LaghuVips)
      set(LAGHU_HAVE_VIPS ON)
    elseif(LAGHU_WITH_VIPS STREQUAL "ON")
      message(FATAL_ERROR "LAGHU_WITH_VIPS=ON requires libvips >= 8.15")
    endif()
  endif()
endif()

add_library(laghu_image STATIC src/image.c src/markup.c)
add_library(Laghu::Image ALIAS laghu_image)

target_include_directories(laghu_image
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(laghu_image PUBLIC c_std_11)
target_link_libraries(laghu_image PUBLIC Laghu::Core)
target_compile_definitions(laghu_image PUBLIC LAGHU_HAVE_VIPS=$<BOOL:${LAGHU_HAVE_VIPS}>)
set_target_properties(laghu_image PROPERTIES C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)

if(LAGHU_HAVE_VIPS)
  target_link_libraries(laghu_image PRIVATE ${LAGHU_VIPS_TARGET})
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(laghu_image PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(LAGHU_BUILD_TESTS)
  add_executable(laghu_image_test tests/image_test.c)
  target_link_libraries(laghu_image_test PRIVATE Laghu::Image)
  if(LAGHU_HAVE_VIPS)
    target_link_libraries(laghu_image_test PRIVATE ${LAGHU_VIPS_TARGET})
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(laghu_image_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME laghu_image_test COMMAND laghu_image_test)
endif()

if(LAGHU_BUILD_FUZZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LAGHU_BUILD_FUZZERS requires Clang")
  endif()
  add_executable(laghu_image_fuzz tests/image_fuzz.c)
  target_link_libraries(laghu_image_fuzz PRIVATE Laghu::Image)
  target_compile_options(laghu_image_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(laghu_image_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()

message(STATUS "Laghu libvips image backend: ${LAGHU_HAVE_VIPS}")
set(LAGHU_HAVE_VIPS ${LAGHU_HAVE_VIPS} PARENT_SCOPE)
