cmake_minimum_required(VERSION 3.20)
project(reaper_mcp VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# macOS: build a universal binary so one .dylib serves Intel + Apple Silicon REAPER.
if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

# MSVC's <cmath> only defines M_PI (and the other M_* constants) when _USE_MATH_DEFINES is set
# BEFORE the header is parsed. glibc (we compile as gnu++17 => _GNU_SOURCE) and Apple's libc++ expose
# them unconditionally, which is why the Ubuntu + macOS legs are green — so this is a strict no-op off
# Windows. Without it the Windows release-build leg fails to compile with `error C2065: 'M_PI'` in the
# spatial/ADM/decode sources. Defined project-wide on the command line so include order can never bite.
if(MSVC)
  add_compile_definitions(_USE_MATH_DEFINES)
endif()

# ---- Dependencies -----------------------------------------------------------------------------
include(cmake/FetchReaperSDK.cmake)   # REAPER SDK + WDL headers (real API path)
include(cmake/FetchDeps.cmake)        # nlohmann/json + cpp-httplib (transport)

# The real API path compiles only when the SDK headers are present. Force OFF to build a pure
# host/structural binary (e.g. CI without SWELL) using the shims in reaper_mcp.cpp.
option(REAPER_MCP_WITH_SDK "Compile the extension against the REAPER SDK (real API path)" ${REAPER_SDK_FOUND})

# ---- Sources shared by the extension and the host-side test core ------------------------------
# The transport (mcp_server) is SDK-independent; the tool bodies call the real API only under
# REAPER_MCP_HAVE_SDK and otherwise return representative structured output (so they can be
# protocol-tested off a running REAPER).
set(REAPER_MCP_LIB_SOURCES
  src/mcp_server.cpp
  src/tools/tools_core.cpp
  src/tools/tools_meta.cpp
  src/tools/tools_markers.cpp
  src/tools/tools_items.cpp
  src/tools/tools_fx.cpp
  src/tools/tools_routing.cpp
  src/tools/tools_envelopes.cpp
  src/tools/tools_timeline.cpp
  src/tools/tools_midi.cpp
  src/tools/tools_spatial.cpp
  src/tools/tools_analysis.cpp
  src/tools/tools_mix.cpp
  src/tools/tools_session.cpp
  src/tools/tools_actions.cpp
  src/resources/resources_project.cpp
  src/prompts/prompts_expert.cpp
)

# ---- The extension MODULE (REAPER dlopen/LoadLibrary's this) -----------------------------------
add_library(reaper_mcp MODULE
  src/reaper_mcp.cpp
  ${REAPER_MCP_LIB_SOURCES}
)
target_include_directories(reaper_mcp PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${REAPER_MCP_DEP_INCLUDE_DIRS}       # json + httplib
  ${REAPER_SDK_INCLUDE_DIRS}           # reaper_plugin.h / functions / WDL
)
target_link_libraries(reaper_mcp PRIVATE Threads::Threads)
# cpp-httplib's loopback sockets need winsock on Windows. MSVC already auto-links it via httplib.h's
# `#pragma comment(lib, "ws2_32.lib")` (guarded by _MSC_VER), so on the MSVC CI toolchain this is a
# redundant no-op — it's here purely so a non-MSVC Windows build (MinGW / clang) links too.
if(WIN32)
  target_link_libraries(reaper_mcp PRIVATE ws2_32)
endif()
if(REAPER_MCP_WITH_SDK AND REAPER_SDK_FOUND)
  target_compile_definitions(reaper_mcp PRIVATE REAPER_MCP_HAVE_SDK=1)
endif()

# REAPER expects the extension file name to start with "reaper_" and have no lib prefix.
set_target_properties(reaper_mcp PROPERTIES PREFIX "" OUTPUT_NAME "reaper_mcp")
if(WIN32)
  set_target_properties(reaper_mcp PROPERTIES SUFFIX ".dll")
elseif(APPLE)
  set_target_properties(reaper_mcp PROPERTIES SUFFIX ".dylib")
else()
  set_target_properties(reaper_mcp PROPERTIES SUFFIX ".so")
endif()

# ---- Host-side core (NO SDK) — the transport + registry + tools for off-REAPER protocol tests --
add_library(reaper_mcp_hostcore STATIC ${REAPER_MCP_LIB_SOURCES})
target_include_directories(reaper_mcp_hostcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${REAPER_MCP_DEP_INCLUDE_DIRS}
)
target_link_libraries(reaper_mcp_hostcore PUBLIC Threads::Threads)
# Intentionally no REAPER_MCP_HAVE_SDK: tool handlers exercise their SDK-free structured-output path.

# ---- Reference-doc generator (dev tool) -------------------------------------------------------
# Regenerates docs/REFERENCE.md from the LIVE registry so the doc never drifts from the surface.
#   one command:  cmake --build build --target reference-doc
# dump_reference links the host core (no SDK, no REAPER), enumerates the registry exactly as the
# host protocol test does, and writes JSON; generate_reference.py renders the markdown.
# EXCLUDE_FROM_ALL: a normal `cmake --build build` does NOT build it — only the reference-doc target
# (which DEPENDS on it) does.
add_executable(dump_reference EXCLUDE_FROM_ALL docs/gen/dump_reference.cpp)
target_link_libraries(dump_reference PRIVATE reaper_mcp_hostcore)  # PUBLIC includes: src/ + json

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_custom_target(reference-doc
    COMMAND dump_reference ${CMAKE_BINARY_DIR}/registry_dump.json
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/docs/gen/generate_reference.py
            ${CMAKE_BINARY_DIR}/registry_dump.json
            ${CMAKE_CURRENT_SOURCE_DIR}/docs/REFERENCE.md
    DEPENDS dump_reference
    COMMENT "Regenerating docs/REFERENCE.md from the live registry"
    VERBATIM USES_TERMINAL)
else()
  message(STATUS "Python3 not found — 'reference-doc' target disabled (dump_reference still builds)")
endif()

# ---- Install to the REAPER UserPlugins dir ----------------------------------------------------
if(WIN32)
  set(REAPER_USERPLUGINS "$ENV{APPDATA}/REAPER/UserPlugins")
elseif(APPLE)
  set(REAPER_USERPLUGINS "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
else()
  set(REAPER_USERPLUGINS "$ENV{HOME}/.config/REAPER/UserPlugins")
endif()
install(TARGETS reaper_mcp LIBRARY DESTINATION "${REAPER_USERPLUGINS}"
                            RUNTIME DESTINATION "${REAPER_USERPLUGINS}")

# ---- Install the bundled JSFX ------------------------------------------------------------------
# Ships to REAPER's Effects/MCP/ so the tools can add each by name (TrackFX_AddByName
# "JS: MCP/<file>"). JSFX is part of REAPER => zero third-party dependency.
#   mcp_mc_limiter   (Phase 6 c): multichannel true-peak limiter for mix.apply_style
#   mcp_ambi_convert (Batch S1):  ambisonic normalization / format converter for spatial.convert_format
#   mcp_ambi_mirror  (Batch S1):  ambisonic scene mirror for spatial.mirror_scene
if(WIN32)
  set(REAPER_EFFECTS "$ENV{APPDATA}/REAPER/Effects")
elseif(APPLE)
  set(REAPER_EFFECTS "$ENV{HOME}/Library/Application Support/REAPER/Effects")
else()
  set(REAPER_EFFECTS "$ENV{HOME}/.config/REAPER/Effects")
endif()
install(FILES src/jsfx/mcp_mc_limiter.jsfx
              src/jsfx/mcp_ambi_convert.jsfx
              src/jsfx/mcp_ambi_mirror.jsfx
        DESTINATION "${REAPER_EFFECTS}/MCP")

# ---- Tests ------------------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
