# Generic musl cross toolchain for 32-bit ARM (arm-linux-musleabihf).
#
# Supported layouts (auto-detected):
#   * musl.cc            : <root>/bin/arm-linux-musleabihf-gcc,
#                          sysroot <root>/arm-linux-musleabihf
#   * Bootlin toolchains : <root>/bin/arm-linux-gcc,
#                          sysroot <root>/arm-buildroot-linux-musleabihf/sysroot
#
# Usage:
#   cmake -S . -B out/build/arm32-musl -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-linux-musleabihf.cmake \
#     -DEDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/musl/armv7-eabihf--musl--stable-2026.08-1 \
#     -DEDGEDESK_WEB_MODE=embed -DEDGEDESK_BUILD_WEB=OFF \
#     -DEDGEDESK_STATIC_PIE=ON
#
# Note: `#embed` needs GCC 15+; Bootlin stable-2026.08 ships GCC 15.3. The
# armv7 target has not yet been built or run in this repository.

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

set(_edgedesk_cc_names arm-linux-musleabihf-gcc arm-linux-gcc arm-buildroot-linux-musleabihf-gcc)
set(_edgedesk_cxx_names arm-linux-musleabihf-g++ arm-linux-g++ arm-buildroot-linux-musleabihf-g++)
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
  message(FATAL_ERROR "ARM musl cross compiler not found under ${EDGEDESK_TOOLCHAIN_ROOT}/bin")
endif()

set(_edgedesk_sysroot_candidates
  "${EDGEDESK_TOOLCHAIN_ROOT}/arm-linux-musleabihf"
  "${EDGEDESK_TOOLCHAIN_ROOT}/arm-buildroot-linux-musleabihf/sysroot"
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
