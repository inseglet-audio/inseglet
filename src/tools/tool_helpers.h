// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tool_helpers.h — shared helpers for the tool translation units.
//
// Argument coercion (reqInt/optNum/...) is SDK-independent and used by both the native and the
// host/fallback path. The MediaTrack helpers compile only under REAPER_MCP_HAVE_SDK.
//
// Include this FIRST in a tool TU (it pulls reaper_api.h before any JSON/STL header, so SWELL's
// min/max macros are #undef'd before nlohmann/json is parsed — see reaper_api.h).

#pragma once

#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "../reaper_api.h"     // no-op unless REAPER_MCP_HAVE_SDK
#include "../tool_registry.h"

namespace reaper_mcp {

// ---- argument helpers (SDK-independent) ----
inline int reqInt(const Json& a, const char* k) {
    if (!a.contains(k) || !a[k].is_number())
        throw std::runtime_error(std::string("missing/invalid integer arg: ") + k);
    return a[k].get<int>();
}
inline std::string reqStr(const Json& a, const char* k) {
    if (!a.contains(k) || !a[k].is_string())
        throw std::runtime_error(std::string("missing/invalid string arg: ") + k);
    return a[k].get<std::string>();
}
inline double reqNum(const Json& a, const char* k) {
    if (!a.contains(k) || !a[k].is_number())
        throw std::runtime_error(std::string("missing/invalid number arg: ") + k);
    return a[k].get<double>();
}
inline int optInt(const Json& a, const char* k, int d) {
    return (a.contains(k) && a[k].is_number()) ? a[k].get<int>() : d;
}
inline double optNum(const Json& a, const char* k, double d) {
    return (a.contains(k) && a[k].is_number()) ? a[k].get<double>() : d;
}
inline bool optBool(const Json& a, const char* k, bool d) {
    return (a.contains(k) && a[k].is_boolean()) ? a[k].get<bool>() : d;
}
inline bool reqBool(const Json& a, const char* k) {
    if (!a.contains(k) || !a[k].is_boolean())
        throw std::runtime_error(std::string("missing/invalid boolean arg: ") + k);
    return a[k].get<bool>();
}
inline std::string optStr(const Json& a, const char* k, const std::string& d) {
    return (a.contains(k) && a[k].is_string()) ? a[k].get<std::string>() : d;
}
inline std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

#ifdef REAPER_MCP_HAVE_SDK
constexpr ReaProject* kCur = nullptr;  // REAPER treats a null ReaProject* as "current project"

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double g) { return g > 0.0 ? 20.0 * std::log10(g) : -150.0; }

inline std::string trackGuidStr(MediaTrack* t) {
    char buf[64] = {0};
    if (t) guidToString(GetTrackGUID(t), buf);
    return buf;
}
inline std::string trackNameStr(MediaTrack* t) {
    char buf[1024] = {0};
    if (t) GetSetMediaTrackInfo_String(t, "P_NAME", buf, false);
    return buf;
}
inline MediaTrack* requireTrack(int idx) {
    MediaTrack* t = GetTrack(kCur, idx);
    if (!t) throw std::runtime_error("track index out of range: " + std::to_string(idx));
    return t;
}
// Write a null-terminated string into a REAPER *_String setter (they take a non-const char*).
inline void setTrackString(MediaTrack* t, const char* parm, const std::string& v) {
    std::vector<char> nb(v.begin(), v.end());
    nb.push_back('\0');
    GetSetMediaTrackInfo_String(t, parm, nb.data(), true);
}

// Resolve a media item by track index + item index (both 0-based). Throws with a clear message.
inline MediaItem* requireItem(int trackIdx, int itemIdx) {
    MediaTrack* t = requireTrack(trackIdx);
    MediaItem* it = GetTrackMediaItem(t, itemIdx);
    if (!it) throw std::runtime_error("item index out of range on track " +
                                      std::to_string(trackIdx) + ": " + std::to_string(itemIdx));
    return it;
}
// Resolve a take on an item: takeIdx < 0 => the active take. Throws if the take doesn't exist.
inline MediaItem_Take* requireTake(MediaItem* it, int takeIdx) {
    MediaItem_Take* tk = (takeIdx < 0) ? GetActiveTake(it) : GetTake(it, takeIdx);
    if (!tk) throw std::runtime_error("no such take (index " + std::to_string(takeIdx) + ")");
    return tk;
}
inline std::string takeNameStr(MediaItem_Take* tk) {
    char buf[1024] = {0};
    if (tk) GetSetMediaItemTakeInfo_String(tk, "P_NAME", buf, false);
    return buf;
}
inline void setTakeString(MediaItem_Take* tk, const char* parm, const std::string& v) {
    std::vector<char> nb(v.begin(), v.end());
    nb.push_back('\0');
    GetSetMediaItemTakeInfo_String(tk, parm, nb.data(), true);
}
#endif  // REAPER_MCP_HAVE_SDK

}  // namespace reaper_mcp
