cmake_minimum_required(VERSION 3.25)

set(SUPERNET_PLATFORM "" CACHE STRING "Device platform: kindlehf or kobov4")
set_property(CACHE SUPERNET_PLATFORM PROPERTY STRINGS kindlehf kobov4)
set(SUPERNET_SDK_ROOT "" CACHE PATH "Runtime SDK root on the device")
if(SUPERNET_PLATFORM STREQUAL "kindlehf")
  set(sdk_root "/mnt/us/.eink-sdk")
elseif(SUPERNET_PLATFORM STREQUAL "kobov4")
  set(sdk_root "/mnt/onboard/.eink-sdk")
else()
  message(FATAL_ERROR "SUPERNET_PLATFORM must be kindlehf or kobov4")
endif()
if(NOT SUPERNET_SDK_ROOT STREQUAL sdk_root)
  message(FATAL_ERROR "SUPERNET_SDK_ROOT must be ${sdk_root} for ${SUPERNET_PLATFORM}")
endif()

project(supernet VERSION 0.1.0 LANGUAGES C)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Supernet requires a Linux target; supply a device toolchain file")
endif()
include(GNUInstallDirs)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(supernet
  src/main.c
  src/control/client.c src/control/service.c src/control/status.c
  src/access/ssh.c src/access/adb.c src/access/wake.c
  src/network/interface.c src/network/wifi.c src/network/firewall.c
  src/platform/usb.c
  src/runtime/config.c src/runtime/files.c src/runtime/log.c
  src/runtime/process.c src/runtime/staging.c)
target_include_directories(supernet PRIVATE src)
target_compile_definitions(supernet PRIVATE
  _GNU_SOURCE SN_VERSION="${PROJECT_VERSION}"
  SN_PLATFORM="${SUPERNET_PLATFORM}" SN_SDK_ROOT="${SUPERNET_SDK_ROOT}")
if(SUPERNET_PLATFORM STREQUAL "kobov4")
  target_sources(supernet PRIVATE src/platform/kobo.c src/platform/kobo_lock.c)
  target_compile_definitions(supernet PRIVATE SN_KOBO)
else()
  target_sources(supernet PRIVATE src/platform/kindle.c src/platform/lipc.c)
  target_include_directories(supernet PRIVATE vendor/openlipc)
endif()
target_compile_options(supernet PRIVATE
  -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wstrict-prototypes)
target_link_libraries(supernet PRIVATE dl rt)
install(TARGETS supernet RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
