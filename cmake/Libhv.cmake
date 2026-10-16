include(FetchContent)

# libhv 1.3.4 pinned to the Git commit behind tag v1.3.4.
# For offline builds pass -DFETCHCONTENT_SOURCE_DIR_LIBHV=/path/to/libhv.
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(WITH_EVPP ON CACHE BOOL "" FORCE)
set(WITH_HTTP ON CACHE BOOL "" FORCE)
set(WITH_HTTP_SERVER ON CACHE BOOL "" FORCE)
set(WITH_HTTP_CLIENT OFF CACHE BOOL "" FORCE)
set(WITH_MQTT OFF CACHE BOOL "" FORCE)
set(WITH_REDIS OFF CACHE BOOL "" FORCE)
set(WITH_PROTOCOL OFF CACHE BOOL "" FORCE)
set(WITH_OPENSSL OFF CACHE BOOL "" FORCE)
set(WITH_MBEDTLS OFF CACHE BOOL "" FORCE)
set(WITH_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(WITH_KCP OFF CACHE BOOL "" FORCE)
set(WITH_IO_URING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libhv
  GIT_REPOSITORY https://github.com/ithewei/libhv.git
  GIT_TAG 71770e04becaa149e0ef8ffc4d3900c5466ddddb
  GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(libhv)
if(NOT TARGET hv_static)
  message(FATAL_ERROR "Expected libhv target hv_static not found")
endif()
# libhv sets CMAKE_CXX_STANDARD in its own CMakeLists; restore project's baseline.
set(CMAKE_CXX_STANDARD 17)
