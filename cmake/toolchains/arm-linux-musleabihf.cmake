# Generic musl cross toolchain for 32-bit ARM (arm-linux-musleabihf).
#
# Usage:
#   cmake -S . -B out/build/arm32-musl -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-linux-musleabihf.cmake \
#     -DEDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/arm-linux-musleabihf-cross \
#     -DEDGEDESK_WEB_MODE=disk -DEDGEDESK_BUILD_WEB=OFF \
#     -DEDGEDESK_STATIC_PIE=ON

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED EDGEDESK_TOOLCHAIN_ROOT)
  set(EDGEDESK_TOOLCHAIN_ROOT "$ENV{HOME}/opt/arm-linux-musleabihf-cross" CACHE PATH "musl cross toolchain root")
endif()

set(CMAKE_C_COMPILER "${EDGEDESK_TOOLCHAIN_ROOT}/bin/arm-linux-musleabihf-gcc")
set(CMAKE_CXX_COMPILER "${EDGEDESK_TOOLCHAIN_ROOT}/bin/arm-linux-musleabihf-g++")
set(CMAKE_SYSROOT "${EDGEDESK_TOOLCHAIN_ROOT}/arm-linux-musleabihf")
set(CMAKE_FIND_ROOT_PATH "${EDGEDESK_TOOLCHAIN_ROOT}/arm-linux-musleabihf")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Prefer -pthread over legacy -lpthreads in cross toolchains.
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
