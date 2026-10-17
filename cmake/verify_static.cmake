# Fail the build when the produced executable still carries dynamic-linking
# metadata. Invoked from CMakeLists.txt as a POST_BUILD step:
#
#   cmake -DOBJDUMP=<objdump> -DBINARY=<exe> -P cmake/verify_static.cmake
#
# `objdump -p` lists INTERP/NEEDED for dynamically linked (PIE) executables and
# neither for static-pie. objdump comes from the target toolchain
# (CMAKE_OBJDUMP), so cross builds are checked with the matching binutils.

if(NOT DEFINED OBJDUMP OR NOT DEFINED BINARY)
  message(FATAL_ERROR "verify_static.cmake requires -DOBJDUMP=... -DBINARY=...")
endif()

execute_process(
  COMMAND "${OBJDUMP}" -p "${BINARY}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE info
  ERROR_VARIABLE error)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "verify_static: '${OBJDUMP} -p ${BINARY}' failed: ${error}")
endif()

if(info MATCHES "(^|\n)[ \t]*(INTERP|NEEDED)")
  message(FATAL_ERROR
    "verify_static: ${BINARY} is dynamically linked; release builds must be static-pie")
endif()
