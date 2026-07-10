// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// style_palette.h — the named mix/master style palette: in-box stock FX by default, with a detected
// third-party plug-in preferred when present.
//
// mix.apply_style inserts a NAMED FX chain on a track/bus. Each style is an ordered chain of steps; each
// step names an in-box (zero-dependency) stock FX (ReaEQ / ReaComp / ReaXcomp / ReaLimit) AND a
// preferred-first list of third-party plug-ins to use INSTEAD when one is detected on this machine. The
// palette + the resolver (which picks preferred-if-installed else in-box, and fills the limiter ceiling
// from the target deliverable's true-peak) are SDK-free and unit-tested (test_style_palette.cpp); the
// tool (tools_mix.cpp) only inserts the resolved FX and sets the params it can discover by name.
//
// "Immersive-aware": the chain respects the bed layout — the limiter ceiling is sized to the
// target spec's true-peak, and dynamics steps are marked lfeExempt so the tool wires them AROUND the LFE
// channel (a bed's LFE is never squashed with the mains). Applying per-bed rather than summing to stereo
// is the tool's job (it inserts on the given bus); this header carries the intent flags.
//
// SDK-free + JSON-only (pulls composite_support.h for Json). Include like the other composite headers.

#pragma once

#include "composite_support.h"  // Json; reaper_api.h before <json> (SWELL min/max)

#include <string>
#include <vector>

namespace reaper_mcp {
namespace style {

// One parameter the style wants set on a step's FX. `key` is a case-insensitive name substring the tool
// discovers on the instantiated plug-in; `value` is raw plug-in units unless `normalized`. A param with
// fromSpecTruePeak gets its value replaced by the resolved dBTP ceiling at resolve time (the limiter).
struct ParamIntent {
    std::string key;
    double value = 0.0;
    bool normalized = false;
    bool fromSpecTruePeak = false;
    std::string unit;
    std::string note;
};

struct FxStep {
    std::string role;                    // "eq" | "comp" | "multiband" | "limiter"
    std::string inboxFx;                 // guaranteed in-box stock FX (always available)
    std::vector<std::string> preferred;  // detected-suite preferences, first installed wins
    std::vector<ParamIntent> params;
    bool isLimiter = false;
    bool lfeExempt = false;              // exclude the LFE channel when the target is a bed
    std::string note;
};

struct Style {
    std::string name;
    std::string description;
    bool immersiveAware = false;
    std::string defaultSpec;             // spec whose TP sizes the limiter ceiling when none is passed
    std::vector<FxStep> chain;
};

// ---- the palette -------------------------------------------------------------------------------

inline const std::vector<Style>& styles() {
    static const std::vector<Style> table = {
        // pop-bright — presence + air, gentle glue, brickwall.
        {"pop-bright", "Bright, forward pop master: air + presence EQ, gentle glue comp, brickwall limit.",
         false, "streaming-stereo", {
            {"eq", "ReaEQ", {"FabFilter Pro-Q", "Pro-Q 3"},
             {{"", 0, false, false, "", "high-shelf air ~+2 dB @ 12 kHz, presence ~+1.5 dB @ 3-4 kHz (best-effort by name)"}},
             false, false, "brightening EQ"},
            {"comp", "ReaComp", {"FabFilter Pro-C", "Waves SSL Comp"},
             {{"thresh", -18.0, false, false, "dB", ""}, {"ratio", 2.0, false, false, ":1", ""},
              {"attack", 15.0, false, false, "ms", ""}, {"release", 120.0, false, false, "ms", ""}},
             false, true, "glue compression"},
            {"limiter", "ReaLimit", {"FabFilter Pro-L", "Pro-L 2"},
             {{"ceiling", -1.0, false, true, "dBTP", "ceiling = target spec true-peak"}},
             true, true, "brickwall limiter"},
         }},
        // cinema-warm — warm low-mids, transparent multiband, conservative limit.
        {"cinema-warm", "Warm, filmic master: low-mid warmth, transparent multiband control, safe limit.",
         false, "cinema-theatrical", {
            {"eq", "ReaEQ", {"FabFilter Pro-Q"},
             {{"", 0, false, false, "", "low-mid warmth ~+1.5 dB @ 200 Hz, gentle air rolloff (best-effort)"}},
             false, false, "warm EQ"},
            {"multiband", "ReaXcomp", {"FabFilter Pro-MB", "Waves C6"},
             {}, false, true, "transparent multiband glue"},
            {"limiter", "ReaLimit", {"FabFilter Pro-L"},
             {{"ceiling", -3.0, false, true, "dBTP", "ceiling = target spec true-peak"}},
             true, true, "safe limiter"},
         }},
        // dialog-clear — HPF + presence, leveling comp, gentle limit (speech).
        {"dialog-clear", "Clear dialog/VO: HPF + presence lift, leveling compression, gentle limit.",
         false, "podcast", {
            {"eq", "ReaEQ", {"FabFilter Pro-Q"},
             {{"", 0, false, false, "", "HPF ~80 Hz, presence ~+2 dB @ 2-4 kHz, de-ess dip ~6-8 kHz (best-effort)"}},
             false, false, "intelligibility EQ"},
            {"comp", "ReaComp", {"FabFilter Pro-C", "Waves Renaissance Vox"},
             {{"thresh", -22.0, false, false, "dB", ""}, {"ratio", 3.0, false, false, ":1", ""},
              {"attack", 5.0, false, false, "ms", ""}, {"release", 80.0, false, false, "ms", ""}},
             false, true, "dialog leveling"},
            {"limiter", "ReaLimit", {"FabFilter Pro-L"},
             {{"ceiling", -1.0, false, true, "dBTP", "ceiling = target spec true-peak"}},
             true, true, "gentle limiter"},
         }},
        // immersive-master-atmos — immersive-aware master: broad EQ, transparent multiband, LFE-safe limit.
        {"immersive-master-atmos",
         "Immersive-aware Atmos/bed master: broad EQ, transparent multiband, limiter sized to the spec "
         "true-peak, LFE excluded from the dynamics so it is never squashed with the mains.",
         true, "atmos-music", {
            {"eq", "ReaEQ", {"FabFilter Pro-Q"},
             {{"", 0, false, false, "", "broad corrective EQ only (best-effort by name)"}},
             false, true, "broad EQ (mains)"},
            {"multiband", "ReaXcomp", {"FabFilter Pro-MB"},
             {}, false, true, "transparent multiband, LFE-exempt"},
            {"limiter", "ReaLimit", {"FabFilter Pro-L"},
             {{"ceiling", -1.0, false, true, "dBTP", "ceiling = target spec true-peak (atmos-music −1 dBTP)"}},
             true, true, "true-peak limiter, LFE-exempt"},
         }},
        // warm-master — general warm glue.
        {"warm-master", "General warm master: subtle EQ, glue comp, brickwall.",
         false, "streaming-stereo", {
            {"eq", "ReaEQ", {"FabFilter Pro-Q"}, {}, false, false, "subtle EQ"},
            {"comp", "ReaComp", {"FabFilter Pro-C"},
             {{"thresh", -16.0, false, false, "dB", ""}, {"ratio", 2.0, false, false, ":1", ""},
              {"attack", 20.0, false, false, "ms", ""}, {"release", 150.0, false, false, "ms", ""}},
             false, true, "glue comp"},
            {"limiter", "ReaLimit", {"FabFilter Pro-L"},
             {{"ceiling", -1.0, false, true, "dBTP", ""}}, true, true, "brickwall"},
         }},
        // bright-master — general bright master, EQ + limiter only.
        {"bright-master", "General bright master: air/presence EQ then brickwall (no comp).",
         false, "streaming-stereo", {
            {"eq", "ReaEQ", {"FabFilter Pro-Q"}, {}, false, false, "air/presence EQ"},
            {"limiter", "ReaLimit", {"FabFilter Pro-L"},
             {{"ceiling", -1.0, false, true, "dBTP", ""}}, true, true, "brickwall"},
         }},
    };
    return table;
}

inline const Style* findStyle(const std::string& nameRaw) {
    std::string n;
    for (char c : nameRaw) n.push_back((char)std::tolower((unsigned char)c));
    for (const auto& s : styles()) if (s.name == n) return &s;
    return nullptr;
}

inline std::string knownStyles() {
    std::string out;
    for (const auto& s : styles()) { if (!out.empty()) out += ", "; out += s.name; }
    return out;
}

// ---- the bundled multichannel true-peak limiter -------------------------------------------------
//
// A wide bed (channels > 2) needs a limiter that reaches ALL mains with a single LINKED gain reduction;
// stock 2-ch ReaLimit only touches ch 1-2. We ship a JSFX (`src/jsfx/mcp_mc_limiter.jsfx`, installed to
// REAPER's `Effects/MCP/` on `cmake --install`) that does exactly that (LFE-exempt, ceiling = target
// dBTP). JSFX is part of REAPER, so the zero-dependency invariant holds. TrackFX_AddByName's resolvable
// string for a JSFX ("JS: <path-relative-to-Effects>") can vary across REAPER builds, so the executor
// tries these candidates in order and falls back to the in-box stock limiter if none resolve (the same
// TrackFX_AddByName != display-name discipline as the rest of the palette).
inline std::vector<std::string> mcLimiterAddNames() {
    return {"JS: MCP/mcp_mc_limiter", "JS:MCP/mcp_mc_limiter", "MCP/mcp_mc_limiter",
            "JS: MCP/mcp_mc_limiter.jsfx"};
}
// The plug-in's `desc:` line — REAPER reports it as "JS: MCP Multichannel True-Peak Limiter", so this
// substring is what the idempotency FX-tail match and the dryRun plan key on.
inline std::string mcLimiterDisplay() { return "MCP Multichannel True-Peak Limiter"; }

// ---- resolution (pure: picks preferred-if-installed else in-box; fills the limiter ceiling) -----

struct ResolvedStep {
    std::string role;
    std::string fx;                 // the chosen add-name (preferred if detected, else the in-box stock FX)
    std::string inboxFx;            // the guaranteed in-box stock FX — the executor's fallback if `fx` won't add
    bool detected = false;          // true => a preferred (detected) plug-in; false => in-box fallback
    bool isLimiter = false;
    bool lfeExempt = false;
    bool multichannel = false;      // true => the bundled multichannel limiter JSFX (wide immersive bed)
    std::vector<ParamIntent> params;
    std::string note;
};

struct ResolvedChain {
    std::string style;
    bool immersiveAware = false;
    double ceilingDb = -1.0;
    std::vector<ResolvedStep> steps;
};

// Does some installed FX display name (already lowercased) contain EVERY whitespace-delimited token of
// `needleLower`? Token-subset matching so a candidate like "fabfilter pro-l" matches a real display name
// like "vst3: pro-l 2 (fabfilter)" regardless of token order / surrounding format decoration.
inline bool isInstalled(const std::vector<std::string>& installedLower, const std::string& needleLower) {
    if (needleLower.empty()) return false;
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < needleLower.size()) {
        while (i < needleLower.size() && needleLower[i] == ' ') ++i;
        size_t j = i;
        while (j < needleLower.size() && needleLower[j] != ' ') ++j;
        if (j > i) tokens.push_back(needleLower.substr(i, j - i));
        i = j;
    }
    if (tokens.empty()) return false;
    for (const auto& s : installedLower) {
        bool all = true;
        for (const auto& tk : tokens) if (s.find(tk) == std::string::npos) { all = false; break; }
        if (all) return true;
    }
    return false;
}

// Resolve `st` against the set of installed plug-in names (lowercased), a dBTP ceiling, and the target's
// channel count. For each step, the first preferred candidate that is installed wins (detected=true);
// otherwise the in-box stock FX (detected=false). Limiter ParamIntents flagged fromSpecTruePeak are set
// to `ceilingDb`. For an IMMERSIVE-MASTER style on a WIDE bed (channels > 2) the limiter step is swapped
// for the bundled multichannel linked-GR true-peak JSFX, which reaches all mains — a stock 2-ch
// limiter would leave the surround/height mains unlimited. Non-immersive styles and stereo targets are
// unchanged (immersive-master first; generalize later).
inline ResolvedChain resolveChain(const Style& st, const std::vector<std::string>& installedLower,
                                  double ceilingDb, int channels = 2) {
    ResolvedChain rc;
    rc.style = st.name;
    rc.immersiveAware = st.immersiveAware;
    rc.ceilingDb = ceilingDb;
    const bool wideImmersive = st.immersiveAware && channels > 2;
    for (const auto& step : st.chain) {
        ResolvedStep rs;
        rs.role = step.role;
        rs.isLimiter = step.isLimiter;
        rs.lfeExempt = step.lfeExempt;
        rs.note = step.note;
        rs.inboxFx = step.inboxFx;
        rs.fx = step.inboxFx;
        rs.detected = false;
        if (step.isLimiter && wideImmersive) {
            // The bundled multichannel limiter is the correct choice regardless of any detected 2-ch
            // third-party limiter: only it guarantees linked GR + LFE exemption across every main.
            rs.multichannel = true;
            rs.fx = mcLimiterDisplay();
            rs.inboxFx = "ReaLimit";  // final fallback if the JSFX can't be added
        } else {
            for (const auto& pref : step.preferred) {
                std::string p;
                for (char c : pref) p.push_back((char)std::tolower((unsigned char)c));
                if (isInstalled(installedLower, p)) { rs.fx = pref; rs.detected = true; break; }
            }
        }
        for (auto pi : step.params) {
            if (pi.fromSpecTruePeak) pi.value = ceilingDb;
            rs.params.push_back(std::move(pi));
        }
        rc.steps.push_back(std::move(rs));
    }
    return rc;
}

// A structured, human-diff-friendly view of a resolved chain (for dryRun plans + tool output).
inline Json toJson(const ResolvedChain& rc) {
    Json steps = Json::array();
    for (const auto& s : rc.steps) {
        Json params = Json::array();
        for (const auto& p : s.params)
            params.push_back(Json{{"key", p.key}, {"value", p.value}, {"normalized", p.normalized},
                                  {"unit", p.unit}, {"note", p.note}});
        steps.push_back(Json{{"role", s.role}, {"fx", s.fx}, {"inboxFx", s.inboxFx},
                             {"detected", s.detected}, {"isLimiter", s.isLimiter},
                             {"lfeExempt", s.lfeExempt}, {"multichannel", s.multichannel},
                             {"params", params}, {"note", s.note}});
    }
    return Json{{"style", rc.style}, {"immersiveAware", rc.immersiveAware},
                {"ceilingDb", rc.ceilingDb}, {"steps", steps}};
}

}  // namespace style
}  // namespace reaper_mcp
