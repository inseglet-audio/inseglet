# FetchDeps.cmake
#
# Vendors the two runtime dependencies of the in-process MCP server via FetchContent:
#   - nlohmann/json  — JSON value type + (de)serialization for JSON-RPC / MCP payloads.
#   - cpp-httplib    — header-only HTTP/1.1 server with chunked (SSE) support for Streamable HTTP.
#
# Both are header-only for our use, so we only need their include directories. We expose:
#   REAPER_MCP_DEP_INCLUDE_DIRS  — add to target_include_directories.
#
# Pinned to exact tags for reproducibility.

include(FetchContent)

set(REAPER_MCP_JSON_TAG    "v3.12.0" CACHE STRING "nlohmann/json tag")
set(REAPER_MCP_HTTPLIB_TAG "v0.49.0" CACHE STRING "cpp-httplib tag")

# nlohmann/json — disable its own tests/install to keep configure fast.
set(JSON_BuildTests    OFF CACHE INTERNAL "")
set(JSON_Install       OFF CACHE INTERNAL "")
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        ${REAPER_MCP_JSON_TAG}
  GIT_SHALLOW    TRUE
)

# cpp-httplib — we consume the single header only (no OpenSSL/zlib link required for loopback HTTP).
FetchContent_Declare(
  cpp_httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        ${REAPER_MCP_HTTPLIB_TAG}
  GIT_SHALLOW    TRUE
)

FetchContent_GetProperties(nlohmann_json)
if(NOT nlohmann_json_POPULATED)
  FetchContent_Populate(nlohmann_json)
endif()
FetchContent_GetProperties(cpp_httplib)
if(NOT cpp_httplib_POPULATED)
  FetchContent_Populate(cpp_httplib)
endif()

set(REAPER_MCP_DEP_INCLUDE_DIRS
  "${nlohmann_json_SOURCE_DIR}/include"
  "${cpp_httplib_SOURCE_DIR}"
  CACHE INTERNAL "header-only dep include dirs")

message(STATUS "reaper_mcp deps: nlohmann/json ${REAPER_MCP_JSON_TAG}, cpp-httplib ${REAPER_MCP_HTTPLIB_TAG}")
