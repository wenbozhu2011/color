# Real server transport: Color as a net_http request interceptor.
#
# net_http is a bazel project; there is no CMake package for it. To build here,
# configure with:
#   -DNET_HTTP_ROOT=/path/to/net_http           # dir that contains net_http/
#   -DNET_HTTP_INCLUDE_DIRS="/abseil/include;..."# extra includes (abseil, ...)
#   -DNET_HTTP_LIBS="/path/libnet_http.a;/path/libabsl_*.a;event;pthread"
# See src/readme.md for a worked example.

add_library(color_interceptor STATIC color_interceptor.cc)
target_include_directories(color_interceptor PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${NET_HTTP_ROOT}
  ${NET_HTTP_INCLUDE_DIRS})
target_link_libraries(color_interceptor PUBLIC color_core)

add_executable(color_server color_server_main.cc)
target_include_directories(color_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${NET_HTTP_ROOT}
  ${NET_HTTP_INCLUDE_DIRS})
target_link_libraries(color_server PRIVATE color_interceptor ${NET_HTTP_LIBS})
