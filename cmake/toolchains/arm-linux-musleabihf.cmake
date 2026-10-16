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

# Resolution order: -DEDGEDESK_TOOLCHAIN_ROOT, then the environment variable of
# the same name (CMake's compiler-test sub-projects do not inherit -D cache
# variables, but they do inherit the environment), then the default location.
if(NOT DEFINED EDGEDESK_TOOLCHAIN_ROOT)
  if(DEFINED ENV{EDGEDESK_TOOLCHAIN_ROOT} AND NOT "$ENV{EDGEDESK_TOOLCHAIN_ROOT}" STREQUAL "")
    set(EDGEDESK_TOOLCHAIN_ROOT "$ENV{EDGEDESK_TOOLCHAIN_ROOT}" CACHE PATH "Cross toolchain root")
  else()
    set(EDGEDESK_TOOLCHAIN_ROOT "$ENV{HOME}/opt/arm-linux-musleabihf-cross" CACHE PATH "Cross toolchain root")
  endif()
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
