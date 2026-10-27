add_library(xenia-hid-portal STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/portal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/portal.cc
)

if(WIN32 AND NOT XENIA_BUILD_UWP)  # no USB portal (libusb) in the Xbox app
  target_sources(xenia-hid-portal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware_portal.h
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware_portal.cc
  )
  target_link_libraries(xenia-hid-portal PUBLIC libusb)
endif()
xe_target_defaults(xenia-hid-portal)
