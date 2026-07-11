# Color — top-level build.
#
# Builds the transport-agnostic Color core and the verification harness (fuzzy
# driver over a simulated lossy network). The real transports build behind the
# same core when their dependencies are present:
#   - color_client (src/client) builds if libcurl is found.
#   - color_server (src/server) builds if -DNET_HTTP_ROOT=... points at a
#     net_http source/build tree (abseil + libevent are transitive deps).
cmake_minimum_required(VERSION 3.16)
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

# Server: net_http (plus abseil + libevent, its transitive deps). net_http is a
# bazel project, so point NET_HTTP_ROOT at a tree that contains its headers
# (the directory holding net_http/) and provide the link libraries in
# NET_HTTP_LIBS (and any extra include dirs, e.g. abseil, in NET_HTTP_INCLUDE_DIRS).
if(DEFINED NET_HTTP_ROOT)
  add_subdirectory(src/server)
else()
  message(STATUS "NET_HTTP_ROOT not set; skipping color_server. "
                 "Set -DNET_HTTP_ROOT=/path/to/net_http (see src/readme.md).")
endif()
