# xenia-core: the loose .h/.cc files directly under src/xenia/ (emulator,
# kernel_state, etc.) that aren't part of any specific submodule.
add_library(xenia-core STATIC)
file(GLOB _core_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)
target_sources(xenia-core PRIVATE ${_core_sources})
target_link_libraries(xenia-core PUBLIC fmt xenia-base)
xe_target_defaults(xenia-core)

add_subdirectory(base)
add_subdirectory(cpu)
if(XE_TARGET_X86_64)
  add_subdirectory(cpu/backend/x64)
endif()
if(XE_TARGET_AARCH64)
  add_subdirectory(cpu/backend/a64)
endif()
add_subdirectory(apu)
add_subdirectory(apu/nop)
if(NOT XENIA_BUILD_UWP)  # SDL and keyboard modules: not in the Xbox app
  add_subdirectory(apu/sdl)
endif()
add_subdirectory(gpu)
if(NOT XENIA_BUILD_UWP)
  add_subdirectory(gpu/null)
endif()
if(NOT APPLE AND NOT XENIA_BUILD_UWP)
  add_subdirectory(gpu/vulkan)
endif()
if(NOT XENIA_BUILD_UWP)  # SDL and keyboard modules: not in the Xbox app
  add_subdirectory(helper/sdl)
endif()
add_subdirectory(hid)
add_subdirectory(hid/nop)
if(NOT XENIA_BUILD_UWP)  # SDL and keyboard modules: not in the Xbox app
  add_subdirectory(hid/sdl)
endif()
add_subdirectory(hid/portal)
if(XENIA_BUILD_UWP)  # the Xbox app reads controllers through XInput
  add_subdirectory(hid/xinput)
endif()
add_subdirectory(net)
add_subdirectory(kernel)
add_subdirectory(patcher)
add_subdirectory(ui)
if(NOT APPLE AND NOT XENIA_BUILD_UWP)
  add_subdirectory(ui/vulkan)
endif()
add_subdirectory(vfs)
add_subdirectory(debug/ui)
add_subdirectory(app/discord)
if(XENIA_BUILD_UWP)
  add_subdirectory(${PROJECT_SOURCE_DIR}/xenia-canary-uwp
                   ${CMAKE_BINARY_DIR}/xenia-canary-uwp)
else()
  add_subdirectory(app)
endif()


# Platform-conditional modules
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(apu/alsa)
endif()

if(WIN32)
  add_subdirectory(apu/xaudio2)
  add_subdirectory(debug/gdb)
  add_subdirectory(gpu/d3d12)
  add_subdirectory(ui/d3d12)
endif()
if(NOT XENIA_BUILD_UWP)  # SDL and keyboard modules: not in the Xbox app
  add_subdirectory(hid/keyboard)
endif()
