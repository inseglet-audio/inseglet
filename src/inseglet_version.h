// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// inseglet_version.h — the ONE in-source version string.
//
// History: v1.5.0 release prep found `serverInfo.version` hardcoded "0.1.0" since
// inception — a sync-site the bump procedure had never listed. The two in-source literals
// (mcp_server.cpp serverInfo + reaper_mcp.cpp discovery-file) and the intent-sidecar
// producer stamp were consolidated onto this constant, so the release bump's in-source
// half is exactly ONE line. The remaining sync sites are the CMake project(VERSION), this
// file, the release tag, and packaging/reapack/index.xml.

#pragma once

namespace reaper_mcp {
constexpr char kInsegletVersion[] = "1.7.0";
}  // namespace reaper_mcp
