# Generic x86_64 musl cross toolchain.
#
# Supported layouts (auto-detected):
#   * musl.cc            : <root>/bin/x86_64-linux-musl-gcc, sysroot <root>/x86_64-linux-musl
#   * Bootlin toolchains : <root>/bin/x86_64-linux-gcc,
#                          sysroot <root>/x86_64-buildroot-linux-musl/sysroot
#
# Usage:
#   cmake -S . -B out/build/x86_64-musl -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-linux-musl.cmake \
#     -DEDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/musl/x86_64-linux-musl-cross \
#     -DEDGEDESK_WEB_MODE=disk -DEDGEDESK_BUILD_WEB=OFF \
#     -DEDGEDESK_STATIC_PIE=ON
#
# Note: prebuilt musl toolchains currently ship GCC 11-13; `#embed` needs
# GCC 15+, so they build disk resource mode. The musl release target is normally
# produced with a GCC 15 musl toolchain using the same options.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED EDGEDESK_TOOLCHAIN_ROOT)
  set(EDGEDESK_TOOLCHAIN_ROOT "$ENV{HOME}/opt/musl/x86_64-linux-musl-cross" CACHE PATH "musl cross toolchain root")
endif()

set(_edgedesk_cc_names x86_64-linux-musl-gcc x86_64-linux-gcc x86_64-buildroot-linux-musl-gcc)
set(_edgedesk_cxx_names x86_64-linux-musl-g++ x86_64-linux-g++ x86_64-buildroot-linux-musl-g++)
foreach(_name IN LISTS _edgedesk_cc_names)
  if(NOT CMAKE_C_COMPILER AND EXISTS "${EDGEDESK_TOOLCHAIN_ROOT}/bin/${_name}")
    set(CMAKE_C_COMPILER "${EDGEDESK_TOOLCHAIN_ROOT}/bin/${_name}")
  endif()
endforeach()
foreach(_name IN LISTS _edgedesk_cxx_names)
  if(NOT CMAKE_CXX_COMPILER AND EXISTS "${EDGEDESK_TOOLCHAIN_ROOT}/bin/${_name}")
    set(CMAKE_CXX_COMPILER "${EDGEDESK_TOOLCHAIN_ROOT}/bin/${_name}")
  endif()
endforeach()

if(NOT CMAKE_C_COMPILER OR NOT CMAKE_CXX_COMPILER)
  message(FATAL_ERROR "x86_64 musl cross compiler not found under ${EDGEDESK_TOOLCHAIN_ROOT}/bin")
endif()

set(_edgedesk_sysroot_candidates
  "${EDGEDESK_TOOLCHAIN_ROOT}/x86_64-linux-musl"
  "${EDGEDESK_TOOLCHAIN_ROOT}/x86_64-buildroot-linux-musl/sysroot"
  "${EDGEDESK_TOOLCHAIN_ROOT}/sysroot")
foreach(_candidate IN LISTS _edgedesk_sysroot_candidates)
  if(NOT EDGEDESK_MUSL_SYSROOT AND IS_DIRECTORY "${_candidate}")
    set(EDGEDESK_MUSL_SYSROOT "${_candidate}")
  endif()
endforeach()
if(NOT EDGEDESK_MUSL_SYSROOT)
  message(FATAL_ERROR "musl sysroot not found under ${EDGEDESK_TOOLCHAIN_ROOT}")
endif()

set(CMAKE_SYSROOT "${EDGEDESK_MUSL_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${EDGEDESK_MUSL_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Prefer -pthread over legacy -lpthreads in cross toolchains.
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
