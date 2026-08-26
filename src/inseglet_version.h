// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// inseglet_version.h — the ONE in-source version string.
//
// History: v1.5.0 release prep found `serverInfo.version` hardcoded "0.1.0" since
// inception — a sync-site the bump procedure had never listed. The two in-source literals
// (mcp_server.cpp serverInfo + reaper_mcp.cpp discovery-file) and the intent-sidecar
// producer stamp were consolidated onto this constant, so the release bump's in-source
// half is exactly ONE line. The OTHER sync sites are the CMake project(VERSION), the
// release tag, packaging/reapack/index.xml (BOTH the <version name> and the <source>
// download URL, which embeds the tag), packaging/reapack/reaper_mcp.ext's @version,
// docs/MANUAL.md's version header and its "written for" note, CITATION.cff, and
// README.md's tool count whenever the surface moves.
//
// Sweep the tree for the OUTGOING version string rather than working a list. This very
// comment said FOUR for four releases while the tree carried nine, which is how
// packaging/reapack/reaper_mcp.ext came to sit at @version 1.5.0 with a changelog
// claiming 186 tools. Two lookalikes must NOT be rewritten by such a sweep:
// ambisonic_meter.h's shelf f0 = 1681.9744509555319 and test_meter.cpp's Table 1
// RLB a1 = -1.99004745483398.

#pragma once

namespace reaper_mcp {
constexpr char kInsegletVersion[] = "1.14.0";
}  // namespace reaper_mcp
