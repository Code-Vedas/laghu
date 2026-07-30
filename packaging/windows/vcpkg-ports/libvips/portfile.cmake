# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO libvips/libvips
  REF v${VERSION}
  SHA512 6861bc7a65137817613448c2e5e44def7845e5537d68e43d245bf3b45eb0fad7ea297bc3864905ae4e33dbf11bc21ec6f76626ff92d15ee1aac6959768fbd256
  HEAD_REF master
)

vcpkg_add_to_path("${CURRENT_HOST_INSTALLED_DIR}/tools/glib/")
file(STRINGS "${SOURCE_PATH}/meson_options.txt" meson_option_lines)
set(meson_feature_options)
set(meson_boolean_options)
set(current_option_name "")
set(current_option_type "")

foreach(line IN LISTS meson_option_lines)
  string(STRIP "${line}" line)
  if(line MATCHES "^option\\('([^']+)'")
    set(current_option_name "${CMAKE_MATCH_1}")
    set(current_option_type "")
  elseif(NOT current_option_name STREQUAL "" AND line MATCHES "^type:[ ]*'([^']+)'")
    set(current_option_type "${CMAKE_MATCH_1}")
    if(current_option_type STREQUAL "feature")
      list(APPEND meson_feature_options "${current_option_name}")
    elseif(current_option_type STREQUAL "boolean")
      list(APPEND meson_boolean_options "${current_option_name}")
    endif()
  elseif(NOT current_option_name STREQUAL "" AND line STREQUAL ")")
    set(current_option_name "")
    set(current_option_type "")
  endif()
endforeach()

if(meson_feature_options STREQUAL "" AND meson_boolean_options STREQUAL "")
  message(FATAL_ERROR "Failed to parse libvips Meson options")
endif()

set(options)
foreach(option IN LISTS meson_feature_options)
  if("${option}" IN_LIST FEATURES)
    list(APPEND options -D${option}=enabled)
  else()
    list(APPEND options -D${option}=disabled)
  endif()
endforeach()
foreach(option IN LISTS meson_boolean_options)
  if("${option}" IN_LIST FEATURES)
    list(APPEND options -D${option}=true)
  else()
    list(APPEND options -D${option}=false)
  endif()
endforeach()

vcpkg_replace_string("${SOURCE_PATH}/meson.build" "subdir('fuzz')" "")
vcpkg_configure_meson(SOURCE_PATH "${SOURCE_PATH}" OPTIONS ${options})
vcpkg_install_meson()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
vcpkg_copy_tools(
  TOOL_NAMES vips vipsedit vipsheader vipsthumbnail
  AUTO_CLEAN
)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
