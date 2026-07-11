# Server library: Color as a net_http request interceptor.
# (The runnable server/demo lives in demo/src.)
#
# net_http_server (built by cmake/BuildNetHttp.cmake) publicly carries its
# include directory and the Abseil / libevent / zlib / pthreads dependencies.
add_library(color_interceptor STATIC color_interceptor.cc)
target_include_directories(color_interceptor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(color_interceptor PUBLIC color_core color_wire net_http_server)
