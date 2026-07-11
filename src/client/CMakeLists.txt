# Real client transport: libcurl-based, driving the Color client core.
add_library(color_http_client STATIC
  fault_http_transport.cc
  color_http_client.cc)
target_include_directories(color_http_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(color_http_client PUBLIC color_core CURL::libcurl)

add_executable(color_client color_client_main.cc)
target_link_libraries(color_client PRIVATE color_http_client)
