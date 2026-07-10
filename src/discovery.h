// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// discovery.h — the client discovery file.
//
// The MCP endpoint's port (OS-assigned) and bearer token regenerate every REAPER launch, so any
// hand-copied client config (Inspector custom header, Claude Desktop mcp-remote args) goes stale on
// restart. To fix that, the extension writes {url, token, sessionId, pid, ...} to a stable path in
// REAPER's resource directory on startup and removes it on unload; clients read it instead of
// copying values by hand.
//
// The write/remove logic here is SDK-free (std streams + nlohmann/json) so it is unit-testable off a
// running REAPER. The extension supplies the path (via GetResourcePath()) and the live values.

#pragma once

#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace reaper_mcp {

// Atomically write the discovery JSON to `path` (write a temp file, then rename over the target so a
// reader never observes a half-written file). Returns false on any I/O failure.
inline bool writeDiscoveryFile(const std::string& path, const nlohmann::json& info) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) return false;
        os << info.dump(2) << "\n";
        if (!os.good()) return false;
    }
    std::remove(path.c_str());  // Windows rename() won't replace an existing file
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

inline void removeDiscoveryFile(const std::string& path) { std::remove(path.c_str()); }

}  // namespace reaper_mcp
