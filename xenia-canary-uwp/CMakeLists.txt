# Game Vault's Xbox app on Xenia Edge: a slim start-up (gv_main.cc) in place of the Blades front end
add_executable(xenia-uwp WIN32
  App.cpp
  gv_main.cc
  gv_uwp_util.cpp
  gv_audio_helper.cc
  gv_import_stubs.cc
  WinRTKeyboard.cpp
  XeniaUWP.cpp
  surface_uwp.cpp
  window_uwp.cc
  windowed_app_context_uwp.cc
  file_picker_uwp.cc
  menu_item_uwp.cc
  uwp_file_redirect.cc
  NetGame.cpp
  Package.appxmanifest
)

target_include_directories(xenia-uwp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(xenia-uwp PRIVATE
  XBYAK_NO_OP_NAMES XBYAK_ENABLE_OMITTED_OPERAND
)
target_compile_options(xenia-uwp PRIVATE /bigobj)
target_link_libraries(xenia-uwp PRIVATE
  xenia-apu xenia-apu-nop xenia-apu-xaudio2
  xenia-base xenia-core xenia-cpu xenia-cpu-backend-x64
  xenia-gpu xenia-gpu-d3d12
  xenia-hid xenia-hid-nop xenia-hid-xinput
  xenia-kernel xenia-patcher xenia-ui xenia-ui-d3d12 xenia-vfs
  capstone fmt imgui libavcodec libavutil mspack snappy xxhash
  windowsapp runtimeobject
)

set_target_properties(xenia-uwp PROPERTIES
  OUTPUT_NAME xeniacanary
  VS_WINRT_COMPONENT FALSE
  VS_GLOBAL_RootNamespace xenia_canary_uwp
  VS_GLOBAL_MinimalCoreWin true
  VS_GLOBAL_DefaultLanguage en-US
  VS_GLOBAL_GenerateWindowsMetadata false
  VS_GLOBAL_AppxPackageSigningEnabled false
  VS_GLOBAL_AppxAutoIncrementPackageRevision false
  VS_GLOBAL_AppxBundle Never
)

# Relative deployment locations preserve the exact paths the Blades UI loads.
file(GLOB_RECURSE _uwp_assets CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Assets/*")
foreach(asset IN LISTS _uwp_assets)
  file(RELATIVE_PATH relative_asset "${CMAKE_CURRENT_SOURCE_DIR}" "${asset}")
  get_filename_component(asset_directory "${relative_asset}" DIRECTORY)
  set_source_files_properties("${asset}" PROPERTIES
    VS_DEPLOYMENT_CONTENT 1
    VS_DEPLOYMENT_LOCATION "${asset_directory}"
  )
endforeach()
target_sources(xenia-uwp PRIVATE ${_uwp_assets})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${_uwp_assets})

# Game Vault: libsmb2 (LGPL-2.1, loaded at run time by NetGame.cpp for games on a PC's shared folder or a NAS)
# and its licence go in the package root.
# dxil.dll (DirectX Shader Compiler, from the Windows SDK redistributables, as xemu ships it) signs the shaders
# Edge makes with Mesa spirv_to_dxil.
foreach(_gv_file smb2.dll libsmb2-LICENCE-LGPL-2.1.txt dxil.dll DirectXShaderCompiler-LICENSE-MS.txt
    DirectXShaderCompiler-LICENSE-LLVM.txt)
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/${_gv_file}" PROPERTIES
    VS_DEPLOYMENT_CONTENT 1
    VS_DEPLOYMENT_LOCATION "."
  )
  target_sources(xenia-uwp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${_gv_file}")
endforeach()

# Ship the license for the TLS dependency used by the UWP package.

# Package the embedded CA bundle with its license and checksum.


# Copies the package to release/Xenia-Netplay-<manifest version>.appx and signs
# it when XENIA_UWP_SIGN_SHA1 is set.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/Package.appxmanifest")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/Package.appxmanifest"
  _uwp_identity_version_lines
  REGEX "^[ \t]*Version=\"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+\"")
list(GET _uwp_identity_version_lines 0 _uwp_identity_version_line)
string(REGEX MATCH "Version=\"([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)\""
  _ "${_uwp_identity_version_line}")
set(XENIA_UWP_PACKAGE_VERSION "${CMAKE_MATCH_1}")
if(NOT XENIA_UWP_PACKAGE_VERSION)
  message(FATAL_ERROR "Package.appxmanifest has no Identity Version")
endif()
message(STATUS "UWP package version: ${XENIA_UWP_PACKAGE_VERSION}")

set(XENIA_UWP_SIGN_SHA1 "" CACHE STRING
  "SHA-1 thumbprint of the certificate that signs the release package; empty leaves it unsigned")
set(_uwp_appx
  "${CMAKE_CURRENT_BINARY_DIR}/AppPackages/xenia-uwp/xenia-uwp_${XENIA_UWP_PACKAGE_VERSION}_x64_Test/xenia-uwp_${XENIA_UWP_PACKAGE_VERSION}_x64.appx")
set(_uwp_release_dir "${CMAKE_BINARY_DIR}/release")
set(_uwp_release "${_uwp_release_dir}/Xenia-Netplay-${XENIA_UWP_PACKAGE_VERSION}.appx")
set(_uwp_release_commands
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_uwp_release_dir}"
  COMMAND ${CMAKE_COMMAND} -E copy "${_uwp_appx}" "${_uwp_release}")
if(XENIA_UWP_SIGN_SHA1)
  file(GLOB _uwp_sdk_bin_dirs "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64")
  list(SORT _uwp_sdk_bin_dirs ORDER DESCENDING)
  find_program(XENIA_UWP_SIGNTOOL signtool PATHS ${_uwp_sdk_bin_dirs})
  if(NOT XENIA_UWP_SIGNTOOL)
    message(FATAL_ERROR "signtool was not found for XENIA_UWP_SIGN_SHA1")
  endif()
  list(APPEND _uwp_release_commands
    COMMAND "${XENIA_UWP_SIGNTOOL}" sign /fd SHA256 /sha1 "${XENIA_UWP_SIGN_SHA1}" "${_uwp_release}")
endif()
add_custom_target(xenia-uwp-release
  ${_uwp_release_commands}
  COMMENT "Release package: ${_uwp_release}"
  VERBATIM)
add_dependencies(xenia-uwp-release xenia-uwp)
