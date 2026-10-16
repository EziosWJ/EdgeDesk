# AArch64 (ARM64) Linux cross toolchain, glibc based (Ubuntu/Debian cross packages).
#
# Two supported layouts:
#   * EDGEDESK_TOOLCHAIN_ROOT=/usr                 (apt-installed cross packages)
#   * EDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/arm64/root (debs extracted with dpkg -x)
#
# Usage:
#   cmake -S . -B out/build/arm64-disk -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
#     -DEDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/arm64/root \
#     -DEDGEDESK_WEB_MODE=disk -DEDGEDESK_BUILD_WEB=OFF
#
# Notes:
#   * this toolchain compiles disk resource mode; GCC 15+ is required for #embed,
#     so release-embed cross builds need a GCC 15 cross toolchain instead.
#   * run the produced binary with qemu-user-static when no board is available:
#     qemu-aarch64-static -L $HOME/opt/arm64/root out/build/arm64-disk/bin/edgedesk

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED EDGEDESK_TOOLCHAIN_ROOT)
  set(EDGEDESK_TOOLCHAIN_ROOT "$ENV{HOME}/opt/arm64/root" CACHE PATH "Cross toolchain root")
endif()

set(_edgedesk_cc_names aarch64-linux-gnu-gcc-13 aarch64-linux-gnu-gcc-14 aarch64-linux-gnu-gcc)
set(_edgedesk_cxx_names aarch64-linux-gnu-g++-13 aarch64-linux-gnu-g++-14 aarch64-linux-gnu-g++)
foreach(_name IN LISTS _edgedesk_cc_names)
  if(NOT CMAKE_C_COMPILER AND EXISTS "${EDGEDESK_TOOLCHAIN_ROOT}/usr/bin/${_name}")
    set(CMAKE_C_COMPILER "${EDGEDESK_TOOLCHAIN_ROOT}/usr/bin/${_name}")
  endif()
endforeach()
foreach(_name IN LISTS _edgedesk_cxx_names)
  if(NOT CMAKE_CXX_COMPILER AND EXISTS "${EDGEDESK_TOOLCHAIN_ROOT}/usr/bin/${_name}")
    set(CMAKE_CXX_COMPILER "${EDGEDESK_TOOLCHAIN_ROOT}/usr/bin/${_name}")
  endif()
endforeach()

if(NOT CMAKE_C_COMPILER OR NOT CMAKE_CXX_COMPILER)
  message(FATAL_ERROR "AArch64 cross compiler not found under ${EDGEDESK_TOOLCHAIN_ROOT}/usr/bin")
endif()

set(CMAKE_SYSROOT "${EDGEDESK_TOOLCHAIN_ROOT}")
set(CMAKE_FIND_ROOT_PATH "${EDGEDESK_TOOLCHAIN_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
