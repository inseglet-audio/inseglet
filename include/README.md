# include/

REAPER SDK and WDL headers are fetched here (or referenced from the CMake build cache) by
`cmake/FetchReaperSDK.cmake`. You do **not** commit them.

Required headers (from https://github.com/justinfrankel/reaper-sdk and .../WDL):

- `reaper_plugin.h` — `reaper_plugin_info_t`, `IReaperControlSurface`, `gaccel_register_t`,
  the `ReaperPluginEntry` entry point contract.
- `reaper_plugin_functions.h` — the importable native API (`REAPERAPI_LoadAPI`, `ShowConsoleMsg`,
  `GetTrack`, `InsertTrackAtIndex`, `SetMediaTrackInfo_Value`, `TrackFX_AddByName`,
  `GetSetProjectInfo[_String]`, `Main_OnCommand`, `Undo_BeginBlock2`/`EndBlock2`, `APIExists`, ...).
- `WDL/` — helper utilities (optional but conventional in REAPER extensions).

If you build offline, drop the three headers here and set `REAPER_MCP_HAVE_SDK` + the include dir
manually. Without them the scaffold still compiles for structural review via the shims in
`src/reaper_mcp.cpp`.
