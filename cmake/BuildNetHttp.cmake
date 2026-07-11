# Builds net_http (a Bazel-only project) as a CMake static library.
#
# net_http ships no CMake build files, so we fetch the source with FetchContent
# and compile the handful of translation units required by the server API into
# `net_http_server`. The source list and link dependencies are derived from the
# Bazel BUILD graph
# (//net_http/server/public:http_server -> evhttp_server -> gzip_zlib,
#  net_logging, shared_files).
#
# The server-side interceptor API Color relies on is not yet on upstream
# google/net_http, so we pin the `server_interceptor` branch of the
# wenbozhu2011/net_http fork. The compiled source list is identical to upstream;
# only the interceptor hooks and the public server headers differ.
#
# Callers must have already defined: the Abseil `absl::*` targets, ZLIB::ZLIB,
# PkgConfig::LIBEVENT and Threads::Threads.

include(FetchContent)

FetchContent_Declare(
  net_http
  GIT_REPOSITORY https://github.com/wenbozhu2011/net_http.git
  # Tip of the `server_interceptor` branch (interceptor API support).
  GIT_TAG        c56de144656726c5966da4673bac5b307162feb0
)
# net_http has no top-level CMakeLists.txt, so MakeAvailable only populates the
# source tree (no add_subdirectory) and sets ${net_http_SOURCE_DIR}.
FetchContent_MakeAvailable(net_http)

add_library(net_http_server STATIC
  ${net_http_SOURCE_DIR}/net_http/public/header_names.cc
  ${net_http_SOURCE_DIR}/net_http/internal/net_logging.cc
  ${net_http_SOURCE_DIR}/net_http/compression/gzip_zlib.cc
  ${net_http_SOURCE_DIR}/net_http/server/internal/evhttp_request.cc
  ${net_http_SOURCE_DIR}/net_http/server/internal/evhttp_server.cc)

# Includes in the net_http sources are rooted at the repo top, e.g.
# #include "net_http/server/public/httpserver.h".
target_include_directories(net_http_server PUBLIC ${net_http_SOURCE_DIR})

target_link_libraries(net_http_server PUBLIC
  absl::base
  absl::config
  absl::core_headers
  absl::log_severity
  absl::memory
  absl::strings
  absl::synchronization
  absl::span
  absl::time
  ZLIB::ZLIB
  PkgConfig::LIBEVENT
  Threads::Threads)

# Upstream builds at C++14; the sources are forward-compatible and we compile
# them at C++17 to match modern Abseil.
target_compile_features(net_http_server PUBLIC cxx_std_17)
