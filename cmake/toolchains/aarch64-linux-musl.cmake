# Generic musl cross toolchain (aarch64-linux-musl, e.g. musl.cc tarballs).
#
# Usage:
#   cmake -S . -B out/build/arm64-musl -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-musl.cmake \
#     -DEDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/aarch64-linux-musl-cross \
#     -DEDGEDESK_WEB_MODE=disk -DEDGEDESK_BUILD_WEB=OFF \
#     -DEDGEDESK_STATIC_PIE=ON
#
# The cross toolchain must provide bin/aarch64-linux-musl-gcc(-g++). #embed
# requires GCC 15+; with older musl toolchains only disk resource mode builds.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED EDGEDESK_TOOLCHAIN_ROOT)
  set(EDGEDESK_TOOLCHAIN_ROOT "$ENV{HOME}/opt/aarch64-linux-musl-cross" CACHE PATH "musl cross toolchain root")
endif()

set(CMAKE_C_COMPILER "${EDGEDESK_TOOLCHAIN_ROOT}/bin/aarch64-linux-musl-gcc")
set(CMAKE_CXX_COMPILER "${EDGEDESK_TOOLCHAIN_ROOT}/bin/aarch64-linux-musl-g++")
set(CMAKE_SYSROOT "${EDGEDESK_TOOLCHAIN_ROOT}/aarch64-linux-musl")
set(CMAKE_FIND_ROOT_PATH "${EDGEDESK_TOOLCHAIN_ROOT}/aarch64-linux-musl")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
