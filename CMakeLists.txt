# Color — top-level build (CMake only).
#
# Builds the transport-agnostic Color core and the verification harness (fuzzy
# driver over a simulated lossy network). The real transports build behind the
# same core when their dependencies are present:
#   - color_client (src/client) builds if libcurl is found.
#   - color_server (src/server) builds if libevent + zlib are found; net_http and
#     abseil are pulled in automatically via FetchContent (needs network on the
#     first configure). Disable with -DCOLOR_BUILD_SERVER=OFF.
cmake_minimum_required(VERSION 3.19)
project(color CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# Transport-agnostic Color core: compiled static library.
add_library(color_core STATIC
  src/core/color_history.cc
  src/core/color_client.cc
  src/core/color_server.cc)
target_include_directories(color_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_compile_features(color_core PUBLIC cxx_std_17)

enable_testing()
add_subdirectory(src/wire)
add_subdirectory(verification)

# ---- Real transports (build only when their dependencies are available) ----

# Client: libcurl.
find_package(CURL)
if(CURL_FOUND)
  add_subdirectory(src/client)
else()
  message(STATUS "libcurl not found; skipping color_client. "
                 "Install libcurl dev headers to build the real client.")
endif()

# Server: net_http (Bazel-only) compiled via cmake/BuildNetHttp.cmake, with
# abseil fetched by FetchContent. libevent + zlib must be installed. Auto-on
# when libevent is available; override with -DCOLOR_BUILD_SERVER=ON/OFF.
option(COLOR_BUILD_SERVER "Build the net_http Color server" ON)
find_package(PkgConfig QUIET)
set(_have_libevent FALSE)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBEVENT QUIET IMPORTED_TARGET libevent libevent_pthreads)
  if(LIBEVENT_FOUND)
    set(_have_libevent TRUE)
  endif()
endif()

if(COLOR_BUILD_SERVER AND _have_libevent)
  find_package(ZLIB REQUIRED)
  find_package(Threads REQUIRED)

  include(FetchContent)
  set(ABSL_PROPAGATE_CXX_STD ON)
  set(ABSL_ENABLE_INSTALL OFF)
  FetchContent_Declare(
    absl
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG        20240722.0)
  FetchContent_MakeAvailable(absl)

  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildNetHttp.cmake)
  add_subdirectory(src/server)
else()
  message(STATUS "Skipping color_server "
                 "(COLOR_BUILD_SERVER=${COLOR_BUILD_SERVER}, libevent found=${_have_libevent}). "
                 "Install libevent-dev + zlib1g-dev to build it; see src/readme.md.")
endif()

# Runnable client/server programs (the demo). Each target builds only if its
# transport library above was configured. Added last so those targets exist.
add_subdirectory(demo/src)
