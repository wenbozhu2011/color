# Client transport library: libcurl-based, driving the Color client core.
# (The runnable client/demo lives in demo/src.)
add_library(color_http_client STATIC
  curl_transport.cc
  color_http_client.cc)
target_include_directories(color_http_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(color_http_client PUBLIC color_core color_wire CURL::libcurl)
