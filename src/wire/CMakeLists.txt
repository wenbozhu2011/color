# JSON serialization for the failover checkpoint file and replay body.
# Shared by the client and server transports.
add_library(color_wire STATIC failover_json.cc)
target_include_directories(color_wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(color_wire PUBLIC color_core)
