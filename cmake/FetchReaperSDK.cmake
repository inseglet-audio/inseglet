# FetchReaperSDK.cmake
#
# Fetches the REAPER C/C++ extension SDK and WDL headers, then exposes:
#   REAPER_SDK_FOUND          - TRUE when headers are available
#   REAPER_SDK_INCLUDE_DIRS   - include dirs for reaper_plugin.h / reaper_plugin_functions.h / WDL
#
# The SDK is header-only for our purposes (we import functions at runtime via GetFunc). Commits are
# pinned for reproducibility (verified to compile the extension against these on 2026-07-06).
#
# NOTE on the WDL relative include: sdk/reaper_plugin.h does `#include "../WDL/swell/swell.h"` on
# non-Windows, i.e. it expects WDL to live at <reaper_sdk>/WDL. FetchContent puts WDL in a separate
# _deps dir, so we materialize that relationship with a symlink (copy fallback on platforms without
# symlink support) after populate.

include(FetchContent)

# Pinned commits (justinfrankel/reaper-sdk @ ec60fb4, justinfrankel/WDL @ e7034b6 — 2026-07-06).
set(REAPER_MCP_SDK_TAG "ec60fb4c38e1f575e29e28bd01fcf50dbf1c0bc7" CACHE STRING "reaper-sdk commit")
set(REAPER_MCP_WDL_TAG "e7034b6d4ded49f3ae11ad3d7e6371bfa2f12a60" CACHE STRING "WDL commit")

FetchContent_Declare(
  reaper_sdk
  GIT_REPOSITORY https://github.com/justinfrankel/reaper-sdk.git
  GIT_TAG        ${REAPER_MCP_SDK_TAG}
)
FetchContent_Declare(
  wdl
  GIT_REPOSITORY https://github.com/justinfrankel/WDL.git
  GIT_TAG        ${REAPER_MCP_WDL_TAG}
)

# Populate without add_subdirectory (headers only).
FetchContent_GetProperties(reaper_sdk)
if(NOT reaper_sdk_POPULATED)
  FetchContent_Populate(reaper_sdk)
endif()
FetchContent_GetProperties(wdl)
if(NOT wdl_POPULATED)
  FetchContent_Populate(wdl)
endif()

set(REAPER_SDK_FOUND FALSE)
if(EXISTS "${reaper_sdk_SOURCE_DIR}/sdk/reaper_plugin.h")
  set(REAPER_SDK_FOUND TRUE)

  # Satisfy sdk/reaper_plugin.h's "../WDL/swell/swell.h" by linking <reaper_sdk>/WDL -> <wdl>/WDL.
  set(_wdl_link "${reaper_sdk_SOURCE_DIR}/WDL")
  if(NOT EXISTS "${_wdl_link}/swell/swell.h")
    file(CREATE_LINK "${wdl_SOURCE_DIR}/WDL" "${_wdl_link}" SYMBOLIC COPY_ON_ERROR)
  endif()
endif()

set(REAPER_SDK_INCLUDE_DIRS
  "${reaper_sdk_SOURCE_DIR}/sdk"
  "${reaper_sdk_SOURCE_DIR}/reaper-plugins"
  "${wdl_SOURCE_DIR}"
  CACHE INTERNAL "REAPER SDK include dirs")
message(STATUS "REAPER SDK found: ${REAPER_SDK_FOUND}  (${reaper_sdk_SOURCE_DIR})")
