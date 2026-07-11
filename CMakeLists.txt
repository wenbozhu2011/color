# Color — top-level build (Phase I self-contained prototype).
#
# Builds the transport-agnostic Color core (header-only) and the verification
# harness (fuzzy client/server driver over a simulated lossy network). The real
# libcurl client and net_http server interceptor are added in later milestones
# behind the same core.
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
  core/color_history.cc
  core/color_client.cc
  core/color_server.cc)
target_include_directories(color_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_compile_features(color_core PUBLIC cxx_std_17)

enable_testing()
add_subdirectory(verification)
