# Generic musl cross toolchain for AArch64 (aarch64-linux-musl).
#
# Supported layouts (auto-detected):
#   * musl.cc            : <root>/bin/aarch64-linux-musl-gcc, sysroot <root>/aarch64-linux-musl
#   * Bootlin toolchains : <root>/bin/aarch64-linux-gcc,
#                          sysroot <root>/aarch64-buildroot-linux-musl/sysroot
#
# Usage:
#   cmake -S . -B out/build/arm64-musl -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-musl.cmake \
#     -DEDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/musl/aarch64-linux-musl-cross \
#     -DEDGEDESK_WEB_MODE=disk -DEDGEDESK_BUILD_WEB=OFF \
#     -DEDGEDESK_STATIC_PIE=ON

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED EDGEDESK_TOOLCHAIN_ROOT)
  set(EDGEDESK_TOOLCHAIN_ROOT "$ENV{HOME}/opt/musl/aarch64-linux-musl-cross" CACHE PATH "musl cross toolchain root")
endif()

set(_edgedesk_cc_names aarch64-linux-musl-gcc aarch64-linux-gcc aarch64-buildroot-linux-musl-gcc)
set(_edgedesk_cxx_names aarch64-linux-musl-g++ aarch64-linux-g++ aarch64-buildroot-linux-musl-g++)
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
  message(FATAL_ERROR "AArch64 musl cross compiler not found under ${EDGEDESK_TOOLCHAIN_ROOT}/bin")
endif()

set(_edgedesk_sysroot_candidates
  "${EDGEDESK_TOOLCHAIN_ROOT}/aarch64-linux-musl"
  "${EDGEDESK_TOOLCHAIN_ROOT}/aarch64-buildroot-linux-musl/sysroot"
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
