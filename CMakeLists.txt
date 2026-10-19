cmake_minimum_required(VERSION 3.24)
project(EdgeDesk VERSION 1.3.0 LANGUAGES C CXX)

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
option(EDGEDESK_STRIP_BINARY "Strip edgedesk in Release/MinSizeRel builds (-s)" ON)
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
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
set(EDGEDESK_FRONTEND_DIR "${CMAKE_SOURCE_DIR}/web")
set(EDGEDESK_DIST_DIR "${EDGEDESK_FRONTEND_DIR}/dist")

if(EDGEDESK_BUILD_WEB)
  find_program(NPM_EXECUTABLE npm REQUIRED)
  add_custom_target(web_build
    COMMAND ${NPM_EXECUTABLE} run build
    WORKING_DIRECTORY "${EDGEDESK_FRONTEND_DIR}"
    COMMENT "Build EdgeDesk frontend (React + Vite)"
    VERBATIM
  )
endif()

# Pinned Argon2id reference implementation. The library is intentionally built
# without its internal thread pool: EdgeDesk runs hashes through one bounded
# worker queue so embedded devices cannot spawn unbounded hash threads.
add_library(edgedesk_argon2 STATIC
  third_party/argon2/src/argon2.c
  third_party/argon2/src/core.c
  third_party/argon2/src/encoding.c
  third_party/argon2/src/ref.c
  third_party/argon2/src/thread.c
  third_party/argon2/src/blake2/blake2b.c
)
target_include_directories(edgedesk_argon2
  PUBLIC "${CMAKE_SOURCE_DIR}/third_party/argon2/include"
  PRIVATE "${CMAKE_SOURCE_DIR}/third_party/argon2/src"
          "${CMAKE_SOURCE_DIR}/third_party/argon2/src/blake2")
target_compile_definitions(edgedesk_argon2 PRIVATE ARGON2_NO_THREADS)
target_compile_options(edgedesk_argon2 PRIVATE -Wall -Wextra)

# SQLite amalgamation (public domain), pinned for the extension platform's
# metadata and KV store. Built without loadable extensions and with foreign
# keys enabled by default. This is the only database dependency in EdgeDesk.
add_library(edgedesk_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(edgedesk_sqlite PUBLIC "${CMAKE_SOURCE_DIR}/third_party/sqlite")
target_compile_definitions(edgedesk_sqlite PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_OMIT_LOAD_EXTENSION=1
  SQLITE_DQS=0
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_USE_URI=0
  SQLITE_ENABLE_JSON1=1)
target_compile_options(edgedesk_sqlite PRIVATE -w)
target_link_libraries(edgedesk_sqlite PUBLIC Threads::Threads)

# Library with all non-network modules: core, file, terminal, system, security,
# web assets, extension platform. Tests link this target without pulling in libhv.
add_library(edgedesk_core STATIC
  src/core/atomic_file.cpp
  src/core/config.cpp
  src/core/error.cpp
  src/core/fd.cpp
  src/core/types.cpp
  src/core/utf8.cpp
  src/core/util.cpp
  src/extensions/extension_runtime.cpp
  src/extensions/extension_store.cpp
  src/extensions/manifest.cpp
  src/extensions/package_manager.cpp
  src/extensions/semver.cpp
  src/extensions/zip_reader.cpp
  src/file/file_lock.cpp
  src/file/file_service.cpp
  src/file/file_types.cpp
  src/file/path_sandbox.cpp
  src/file/protected_paths.cpp
  src/security/audit.cpp
  src/security/auth.cpp
  src/security/auth_store.cpp
  src/security/password_hash.cpp
  src/security/rate_limiter.cpp
  src/security/settings_store.cpp
  src/system/cpu_metrics.cpp
  src/system/disk_metrics.cpp
  src/system/memory_metrics.cpp
  src/system/network_metrics.cpp
  src/system/system_metrics.cpp
  src/terminal/command_guard.cpp
  src/terminal/pty.cpp
  src/terminal/terminal_manager.cpp
  src/terminal/terminal_session.cpp
  src/web/web_root.cpp
)
target_include_directories(edgedesk_core
  PUBLIC "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/third_party")
target_compile_features(edgedesk_core PUBLIC cxx_std_17)
target_compile_options(edgedesk_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(edgedesk_core
  PRIVATE EDGEDESK_DISK_DEFAULT_DIR="${EDGEDESK_WEB_ROOT}"
  PUBLIC EDGEDESK_VERSION="${PROJECT_VERSION}"
  PUBLIC EDGEDESK_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
target_link_libraries(edgedesk_core PUBLIC Threads::Threads edgedesk_argon2 edgedesk_sqlite ZLIB::ZLIB)
if(UNIX AND NOT APPLE)
  # forkpty lives in libutil on glibc and in libc on musl.
  include(CheckLibraryExists)
  check_library_exists(util forkpty "" EDGEDESK_HAVE_LIBUTIL)
  if(EDGEDESK_HAVE_LIBUTIL)
    target_link_libraries(edgedesk_core PUBLIC util)
  endif()
endif()

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
  add_executable(edgedesk
    src/app/main.cpp
    src/app/application.cpp
    src/app/recovery.cpp
    src/http/http_util.cpp
    src/http/middleware.cpp
    src/http/server.cpp
    src/http/routes/auth_route.cpp
    src/http/routes/extension_route.cpp
    src/http/routes/file_route.cpp
    src/http/routes/health_route.cpp
    src/http/routes/settings_route.cpp
    src/http/routes/system_route.cpp
    src/http/routes/terminal_route.cpp
  )
  target_link_libraries(edgedesk PRIVATE edgedesk_core hv_static)
  # libhv v1.3.4 exports the static target but keeps several C API headers
  # in source subdirectories rather than an installed include tree.
  target_include_directories(edgedesk SYSTEM PRIVATE
    "${libhv_SOURCE_DIR}"
    "${libhv_SOURCE_DIR}/http/server" "${libhv_SOURCE_DIR}/http"
    "${libhv_SOURCE_DIR}/ssl" "${libhv_SOURCE_DIR}/event"
    "${libhv_SOURCE_DIR}/base" "${libhv_SOURCE_DIR}/util"
    "${libhv_SOURCE_DIR}/cpputil" "${libhv_SOURCE_DIR}/evpp")
  target_compile_features(edgedesk PRIVATE cxx_std_17)
  target_compile_options(edgedesk PRIVATE -Wall -Wextra -Wpedantic)
  if(EDGEDESK_STRIP_BINARY)
    # Link-time -s (equivalent to strip -s): release artifacts ship without
    # .symtab/.debug_* tables. Debug builds keep symbols for tests and crash
    # analysis; test executables are never stripped.
    target_link_options(edgedesk PRIVATE
      "$<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:-s>")
  endif()
  if(EDGEDESK_STATIC_PIE)
    # Guarantee that a static-pie request actually produced a static binary.
    add_custom_command(TARGET edgedesk POST_BUILD
      COMMAND "${CMAKE_COMMAND}"
        "-DOBJDUMP=${CMAKE_OBJDUMP}"
        "-DBINARY=$<TARGET_FILE:edgedesk>"
        -P "${CMAKE_SOURCE_DIR}/cmake/verify_static.cmake"
      COMMENT "Verify edgedesk is statically linked"
      VERBATIM)
  endif()
  set_target_properties(edgedesk PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  if(EDGEDESK_BUILD_WEB AND EDGEDESK_WEB_MODE STREQUAL "disk")
    add_dependencies(edgedesk web_build)
  endif()
endif()

if(EDGEDESK_BUILD_TESTS)
  enable_testing()
  set(EDGEDESK_TEST_TARGETS
    core_test:core/core_test.cpp
    utf8_test:core/utf8_test.cpp
    path_sandbox_test:file/path_sandbox_test.cpp
    file_service_test:file/file_service_test.cpp
    auth_store_test:security/auth_store_test.cpp
    password_hash_test:security/password_hash_test.cpp
    settings_store_test:security/settings_store_test.cpp
    command_guard_test:terminal/command_guard_test.cpp
    terminal_session_test:terminal/terminal_session_test.cpp
    system_metrics_test:system/system_metrics_test.cpp
    semver_test:extensions/semver_test.cpp
    manifest_test:extensions/manifest_test.cpp
    extension_store_test:extensions/extension_store_test.cpp
    extension_package_test:extensions/extension_package_test.cpp
    file_lock_test:file/file_lock_test.cpp
  )
  foreach(entry IN LISTS EDGEDESK_TEST_TARGETS)
    string(REPLACE ":" ";" pair "${entry}")
    list(GET pair 0 test_name)
    list(GET pair 1 test_source)
    add_executable(${test_name} "tests/${test_source}")
    target_include_directories(${test_name} PRIVATE "${CMAKE_SOURCE_DIR}/tests")
    target_link_libraries(${test_name} PRIVATE edgedesk_core)
    target_compile_features(${test_name} PRIVATE cxx_std_17)
    target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
  add_test(NAME embed_generator_test
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s "${CMAKE_SOURCE_DIR}/tools/tests" -p "test_*.py")
endif()
