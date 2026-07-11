# Demo executables: the runnable Color client and server.
#
# Each is built only when its transport library was configured (libcurl for the
# client; libevent/net_http for the server), so the demo follows whatever the
# top-level build enabled.

if(TARGET color_http_client)
  add_executable(color_client color_client_main.cc)
  target_link_libraries(color_client PRIVATE color_http_client)
else()
  message(STATUS "color_http_client not built; skipping color_client demo.")
endif()

if(TARGET color_interceptor)
  add_executable(color_server color_server_main.cc)
  target_link_libraries(color_server PRIVATE color_interceptor)
else()
  message(STATUS "color_interceptor not built; skipping color_server demo.")
endif()
