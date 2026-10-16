# Generic x86_64 musl cross toolchain (musl.cc tarballs or equivalent).
#
# Usage:
#   cmake -S . -B out/build/x86_64-musl -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-linux-musl.cmake \
#     -DEDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/musl/x86_64-linux-musl-cross \
#     -DEDGEDESK_WEB_MODE=disk -DEDGEDESK_BUILD_WEB=OFF \
#     -DEDGEDESK_STATIC_PIE=ON
#
# Note: musl.cc currently ships GCC 11/12; `#embed` needs GCC 15+, so this
# toolchain builds disk resource mode. The musl release target is normally
# produced with a GCC 15 musl toolchain using the same options.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED EDGEDESK_TOOLCHAIN_ROOT)
  set(EDGEDESK_TOOLCHAIN_ROOT "$ENV{HOME}/opt/musl/x86_64-linux-musl-cross" CACHE PATH "musl cross toolchain root")
endif()

set(CMAKE_C_COMPILER "${EDGEDESK_TOOLCHAIN_ROOT}/bin/x86_64-linux-musl-gcc")
set(CMAKE_CXX_COMPILER "${EDGEDESK_TOOLCHAIN_ROOT}/bin/x86_64-linux-musl-g++")
set(CMAKE_SYSROOT "${EDGEDESK_TOOLCHAIN_ROOT}/x86_64-linux-musl")
set(CMAKE_FIND_ROOT_PATH "${EDGEDESK_TOOLCHAIN_ROOT}/x86_64-linux-musl")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
