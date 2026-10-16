cmake_minimum_required(VERSION 3.24)
project(EdgeDesk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(EDGEDESK_WEB_MODE "embed" CACHE STRING "Frontend resource mode: embed or disk")
set_property(CACHE EDGEDESK_WEB_MODE PROPERTY STRINGS embed disk)
option(EDGEDESK_BUILD_SERVER "Compile libhv HTTP server" ON)
option(EDGEDESK_BUILD_TESTS "Build unit tests" ON)
option(EDGEDESK_BUILD_WEB "Build React assets with npm" ON)
option(EDGEDESK_STATIC_PIE "Link Linux release executable with musl static-pie" OFF)
set(EDGEDESK_WEB_ROOT "${CMAKE_SOURCE_DIR}/web/dist" CACHE PATH "Static asset directory (disk mode)")

if(NOT EDGEDESK_WEB_MODE STREQUAL "embed" AND NOT EDGEDESK_WEB_MODE STREQUAL "disk")
  message(FATAL_ERROR "EDGEDESK_WEB_MODE must be embed or disk")
endif()
if(EDGEDESK_WEB_MODE STREQUAL "embed")
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
    message(FATAL_ERROR "#embed mode requires GCC 15+; use -DEDGEDESK_WEB_MODE=disk for older compilers")
  endif()
endif()
if(EDGEDESK_STATIC_PIE)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "static-pie only defined for Linux targets")
  endif()
  add_link_options(-static-pie)
  add_compile_options(-fPIE)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(EDGEDESK_FRONTEND_DIR "${CMAKE_SOURCE_DIR}/web")
set(EDGEDESK_DIST_DIR "${EDGEDESK_FRONTEND_DIR}/dist")

if(EDGEDESK_BUILD_WEB)
  find_program(NPM_EXECUTABLE npm REQUIRED)
  add_custom_target(web_build
    COMMAND ${NPM_EXECUTABLE} install --no-audit --no-fund
    COMMAND ${NPM_EXECUTABLE} run build
    WORKING_DIRECTORY "${EDGEDESK_FRONTEND_DIR}"
    COMMENT "Build EdgeDesk frontend (React + Vite)"
    VERBATIM
  )
endif()

add_library(edgedesk_core STATIC
  src/app/config.cpp
  src/app/asset_provider.cpp
)
target_include_directories(edgedesk_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(edgedesk_core PUBLIC cxx_std_17)
target_compile_options(edgedesk_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(edgedesk_core PRIVATE EDGEDESK_DISK_DEFAULT_DIR="${EDGEDESK_WEB_ROOT}")

if(EDGEDESK_WEB_MODE STREQUAL "embed")
  set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
  set(GENERATED_EMBED_CPP "${GENERATED_DIR}/embedded_assets.cpp")
  file(MAKE_DIRECTORY "${GENERATED_DIR}")
  add_custom_target(generate_embedded_assets
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen_embed.py"
      --input "${EDGEDESK_DIST_DIR}" --output "${GENERATED_EMBED_CPP}"
    BYPRODUCTS "${GENERATED_EMBED_CPP}"
    COMMENT "Update embedded static asset index"
    VERBATIM
  )
  if(EDGEDESK_BUILD_WEB)
    add_dependencies(generate_embedded_assets web_build)
  endif()
  set_source_files_properties("${GENERATED_EMBED_CPP}" PROPERTIES GENERATED TRUE)
  target_sources(edgedesk_core PRIVATE "${GENERATED_EMBED_CPP}")
  add_dependencies(edgedesk_core generate_embedded_assets)
  target_compile_definitions(edgedesk_core PUBLIC EDGEDESK_EMBED_ASSETS=1)
else()
  target_compile_definitions(edgedesk_core PUBLIC EDGEDESK_EMBED_ASSETS=0)
endif()

if(EDGEDESK_BUILD_SERVER)
  include(cmake/Libhv.cmake)
  add_executable(edgedesk src/main.cpp src/app/http_server.cpp)
  target_link_libraries(edgedesk PRIVATE edgedesk_core hv_static)
  target_include_directories(edgedesk PRIVATE "${libhv_SOURCE_DIR}/http/server" "${libhv_SOURCE_DIR}/http")
  target_compile_features(edgedesk PRIVATE cxx_std_17)
  target_compile_options(edgedesk PRIVATE -Wall -Wextra -Wpedantic)
  set_target_properties(edgedesk PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  if(EDGEDESK_BUILD_WEB AND EDGEDESK_WEB_MODE STREQUAL "disk")
    add_dependencies(edgedesk web_build)
  endif()
endif()

if(EDGEDESK_BUILD_TESTS)
  enable_testing()
  add_executable(edgedesk_core_test tests/core_test.cpp)
  target_link_libraries(edgedesk_core_test PRIVATE edgedesk_core)
  add_test(NAME core_test COMMAND edgedesk_core_test)
  add_test(NAME embed_generator_test
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s "${CMAKE_SOURCE_DIR}/tools/tests" -p "test_*.py")
endif()
