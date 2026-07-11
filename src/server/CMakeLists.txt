# Real server transport: Color as a net_http request interceptor.
#
# net_http_server (built by cmake/BuildNetHttp.cmake) publicly carries its
# include directory and the Abseil / libevent / zlib / pthreads dependencies.
add_library(color_interceptor STATIC color_interceptor.cc)
target_include_directories(color_interceptor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(color_interceptor PUBLIC color_core net_http_server)

add_executable(color_server color_server_main.cc)
target_include_directories(color_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(color_server PRIVATE color_interceptor net_http_server)
