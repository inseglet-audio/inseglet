// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_mix.cpp — immersive-aware mix/master style chains (mix.apply_style) plus master-track
// controls (master.get/set, add/remove FX, set FX param).
//
// Insert a NAMED FX chain (style_palette.h) on a track/bus: pick each step's plug-in preferred-if-detected
// else the in-box stock FX (ReaEQ/ReaComp/ReaXcomp/ReaLimit — zero dependency), set the params we can
// discover by name, and size the limiter's ceiling to the TARGET DELIVERABLE's true-peak (deliverable_
// specs.h). Immersive-aware: on a bed the LFE is kept OUT of the dynamics (never squashed with the mains)
// by pin-wiring the dynamics FX to exclude the LFE channel where it reaches it. One undo block; dryRun
// returns the resolved chain it would insert without mutating.
//
// The palette + resolution (preferred-vs-inbox, ceiling fill) are the SDK-free, unit-tested core; this TU
// is the executor. Dual-path: real FX insertion under REAPER_MCP_HAVE_SDK; the host build returns the
// resolved-chain plan so the protocol test exercises dispatch + resolution.

#include <string>
#include <vector>

#include "tool_helpers.h"          // reaper_api.h (SWELL de-fang) + arg helpers + requireTrack
#include "../style_palette.h"      // the named style palette + pure resolver
#include "../deliverable_specs.h"  // target-spec true-peak -> limiter ceiling
#include "../tool_registry.h"

namespace reaper_mcp {

namespace {

// Standard bed layouts carry the LFE at channel index 3 (1-based channel 4), matching the bed builders.
// Returns true + sets lfeIdx0 for a recognized bed width; false for stereo / non-bed widths.
bool bedLfe(int nchan, int& lfeIdx0) {
    switch (nchan) {
        case 6: case 8: case 12: case 16: case 24: lfeIdx0 = 3; return true;  // 5.1 / 7.1 / 7.1.4 / 9.1.6 / 22.2
        default: lfeIdx0 = -1; return false;
    }
}

// Resolve the limiter ceiling (dBTP): explicit ceilingDb > targetSpec TP > style.defaultSpec TP > -1.
double resolveCeiling(const Json& a, const style::Style& st) {
    if (a.contains("ceilingDb") && a["ceilingDb"].is_number()) return a["ceilingDb"].get<double>();
    if (a.contains("targetSpec") && a["targetSpec"].is_string()) {
        if (const deliverable::Spec* s = deliverable::findSpec(a["targetSpec"].get<std::string>()))
            return s->truePeakMaxDb;
    }
    if (!st.defaultSpec.empty())
        if (const deliverable::Spec* s = deliverable::findSpec(st.defaultSpec)) return s->truePeakMaxDb;
    return -1.0;
}

#ifdef REAPER_MCP_HAVE_SDK
std::string fxNameM(MediaTrack* t, int fx) {
    char buf[256] = {0}; TrackFX_GetFXName(t, fx, buf, sizeof(buf)); return buf;
}
std::string fxParamNameM(MediaTrack* t, int fx, int p) {
    char buf[256] = {0}; TrackFX_GetParamName(t, fx, p, buf, sizeof(buf)); return buf;
}
// First param whose (lowercased) name contains any of the given keys, in key order; -1 if none.
int discoverParamM(MediaTrack* t, int fx, int np, const std::vector<std::string>& keys) {
    for (const auto& k : keys) {
        const std::string kl = toLower(k);
        for (int p = 0; p < np; ++p)
            if (toLower(fxParamNameM(t, fx, p)).find(kl) != std::string::npos) return p;
    }
    return -1;
}
// Every installed FX display name, lowercased — for the detected-suite preference.
std::vector<std::string> installedLowerNames() {
    std::vector<std::string> out;
    const char* name = nullptr; const char* ident = nullptr;
    for (int i = 0; EnumInstalledFX(i, &name, &ident); ++i)
        if (name) out.push_back(toLower(name));
    return out;
}
// Pin-wire an FX to EXCLUDE the LFE channel: identity map every I/O pin to its own channel, but leave the
// LFE channel's pin unmapped (0) so the FX neither reads nor writes the LFE. Only meaningful when the FX
// actually reaches the LFE channel (out-pins > lfeIdx); returns a small report.
Json wireExcludeLfe(MediaTrack* t, int fx, int nchan, int lfeIdx0) {
    int inPins = 0, outPins = 0;
    TrackFX_GetIOSize(t, fx, &inPins, &outPins);
    const int pins = outPins > 0 ? outPins : inPins;
    if (pins <= lfeIdx0) {
        // e.g. a stock 2-pin FX on a 12-ch bed: it only touches ch 1-2, so the LFE (ch4) is already safe.
        return Json{{"applied", false}, {"reason", "fx has " + std::to_string(pins) +
                    " pins (<= LFE channel index " + std::to_string(lfeIdx0) + "); LFE inherently untouched"},
                    {"inPins", inPins}, {"outPins", outPins}};
    }
    const int span = pins < nchan ? pins : nchan;
    for (int k = 0; k < span; ++k) {
        const bool isLfe = (k == lfeIdx0);
        const int low = (!isLfe && k < 32) ? (int)(1u << k) : 0;
        const int high = (!isLfe && k >= 32) ? (int)(1u << (k - 32)) : 0;
        TrackFX_SetPinMappings(t, fx, 0, k, low, high);  // input pin k
        TrackFX_SetPinMappings(t, fx, 1, k, low, high);  // output pin k
    }
    return Json{{"applied", true}, {"lfeChannelExcluded", lfeIdx0 + 1}, {"pinsWired", span},
                {"inPins", inPins}, {"outPins", outPins}};
}
// Try each candidate add-name in order (JSFX resolvable strings vary across REAPER builds); return the
// FX index of the first that adds, or -1 if none resolve. -1 as the position = "always add new".
int addByAnyName(MediaTrack* t, const std::vector<std::string>& names) {
    for (const auto& n : names) {
        const int fx = TrackFX_AddByName(t, n.c_str(), false, -1);
        if (fx >= 0) return fx;
    }
    return -1;
}
#endif  // REAPER_MCP_HAVE_SDK

}  // namespace

void registerMixTools(ToolRegistry& reg) {
    // ---- mix.apply_style — insert a named, immersive-aware FX chain on a track/bus ----
    reg.add(Tool{
        "mix.apply_style",
        "Insert a NAMED mix/master style chain on a track/bus (pop-bright, cinema-warm, dialog-clear, "
        "immersive-master-atmos, warm-master, bright-master). Each step uses the in-box stock FX "
        "(ReaEQ/ReaComp/ReaXcomp/ReaLimit — zero dependency) unless a preferred third-party plug-in is "
        "detected, then prefers it. Immersive-aware: the limiter ceiling is sized to the target "
        "deliverable's true-peak (targetSpec or the style default), and on a bed the LFE is kept OUT of "
        "the dynamics (never squashed with the mains). On a WIDE immersive bed (channels > 2) the "
        "immersive-master limiter is the bundled multichannel true-peak JSFX (linked gain reduction "
        "across ALL mains, LFE-exempt) instead of the stock 2-ch limiter — so the surround/height mains "
        "are limited too. One undo block. dryRun returns the exact chain it would insert without "
        "mutating.",
        jparse(R"({"type":"object","properties":{
            "track":{"type":"integer","minimum":0},
            "style":{"type":"string"},
            "targetSpec":{"type":"string"},
            "ceilingDb":{"type":"number"},
            "channels":{"type":"integer","minimum":1},
            "dryRun":{"type":"boolean","default":false}},
            "required":["track","style"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"style":{"type":"string"},
            "track":{"type":"integer"},"immersiveAware":{"type":"boolean"},"ceilingDb":{"type":"number"},
            "channels":{"type":"integer"},"hasLfe":{"type":"boolean"},"chain":{"type":"array"},
            "inserted":{"type":"array"},"reused":{"type":"boolean"},"stepCount":{"type":"integer"},
            "diff":{"type":"string"},"warnings":{"type":"array"},"dryRun":{"type":"boolean"},
            "error":{"type":"string"},"detail":{"type":"string"},"remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ true},
        Profile::Mixing,
        [](const Json& a) -> Json {
            const int track = reqInt(a, "track");
            const std::string styleName = reqStr(a, "style");
            const style::Style* st = style::findStyle(styleName);
            if (!st)
                return makeError("unknown_style", "no style named '" + styleName + "'",
                                 "use one of: " + style::knownStyles());
            const bool dryRun = optBool(a, "dryRun", false);
            const double ceilingDb = resolveCeiling(a, *st);
            Json warnings = Json::array();

#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(track);
            const int nchan = (int)GetMediaTrackInfo_Value(t, "I_NCHAN");
            int lfeIdx0 = -1;
            const bool hasLfe = bedLfe(nchan, lfeIdx0);
            const std::vector<std::string> installed = installedLowerNames();
            const style::ResolvedChain rc = style::resolveChain(*st, installed, ceilingDb, nchan);

            // dryRun: report the resolved chain + LFE/ceiling context, mutate nothing.
            if (dryRun) {
                Json out = style::toJson(rc);
                out["ok"] = true; out["dryRun"] = true; out["track"] = track;
                out["channels"] = nchan; out["hasLfe"] = hasLfe;
                if (hasLfe) out["lfeChannel"] = lfeIdx0 + 1;
                out["stepCount"] = (int)rc.steps.size();
                return out;
            }

            // Idempotency: if the tail of the track's FX chain already matches this style's resolved FX
            // names in order, treat the style as already applied (converge, don't stack a second chain).
            const int existing = TrackFX_GetCount(t);
            bool alreadyApplied = existing >= (int)rc.steps.size() && !rc.steps.empty();
            for (size_t i = 0; alreadyApplied && i < rc.steps.size(); ++i) {
                const int fxIdx = existing - (int)rc.steps.size() + (int)i;
                const std::string nm = toLower(fxNameM(t, fxIdx));
                // Match the chosen FX OR the in-box fallback — so a step that fell back to in-box last
                // run (detected name wasn't addable) still counts as "already applied" this run.
                const bool m1 = nm.find(toLower(rc.steps[i].fx)) != std::string::npos;
                const bool m2 = !rc.steps[i].inboxFx.empty() &&
                                nm.find(toLower(rc.steps[i].inboxFx)) != std::string::npos;
                if (!m1 && !m2) alreadyApplied = false;
            }
            if (alreadyApplied) {
                Json out = style::toJson(rc);
                out["ok"] = true; out["reused"] = true; out["track"] = track; out["channels"] = nchan;
                out["hasLfe"] = hasLfe; out["stepCount"] = (int)rc.steps.size();
                out["diff"] = std::string("(no change) style '") + st->name + "' already present on the FX tail";
                return out;
            }

            UndoTransaction txn("MCP: apply style '" + st->name + "' -> track " + std::to_string(track));
            Json inserted = Json::array();
            Plan plan;
            for (const auto& step : rc.steps) {
                int fx = -1;
                std::string usedFx = step.fx;
                bool mcActive = false;  // the bundled multichannel limiter JSFX actually landed
                if (step.multichannel) {
                    // Wide immersive bed: add the bundled multichannel true-peak JSFX (covers all mains
                    // with linked GR). Try each add-name form; fall back to the in-box stock limiter.
                    fx = addByAnyName(t, style::mcLimiterAddNames());
                    if (fx >= 0) { usedFx = fxNameM(t, fx); mcActive = true; }
                    else {
                        warnings.push_back("bundled multichannel limiter JSFX not found (install "
                            "Effects/MCP/mcp_mc_limiter.jsfx via 'cmake --install'); fell back to in-box '" +
                            step.inboxFx + "' — mains beyond ch 2 will be UNLIMITED on this wide bed.");
                        fx = TrackFX_AddByName(t, step.inboxFx.c_str(), false, -1);
                        usedFx = step.inboxFx;
                    }
                } else {
                    fx = TrackFX_AddByName(t, step.fx.c_str(), false, -1);  // -1 = always add new
                    if (fx < 0 && step.detected && !step.inboxFx.empty() && step.inboxFx != step.fx) {
                        // A DETECTED third-party plug-in's display name may not be TrackFX_AddByName-
                        // resolvable (add-name != enumerated display name). Fall back to the guaranteed
                        // in-box stock FX so the zero-dependency invariant holds — the chain always
                        // gets its FX.
                        warnings.push_back("detected '" + step.fx + "' did not resolve by add-name; used "
                                           "in-box '" + step.inboxFx + "'");
                        fx = TrackFX_AddByName(t, step.inboxFx.c_str(), false, -1);
                        usedFx = step.inboxFx;
                    }
                }
                if (fx < 0) {
                    warnings.push_back("could not add '" + usedFx + "' (" + step.role +
                                       ") — is it installed? step skipped.");
                    continue;
                }
                txn.recordExecuted("insert_fx", "track " + std::to_string(track), usedFx);
                plan.add("insert_fx", "track " + std::to_string(track), usedFx + " (" + step.role + ")");
                const int np = TrackFX_GetNumParams(t, fx);
                Json setParams = Json::array();
                for (const auto& pi : step.params) {
                    if (pi.key.empty()) {  // an intent with no discoverable key = descriptive only
                        if (!pi.note.empty())
                            setParams.push_back(Json{{"intent", pi.note}, {"applied", false}});
                        continue;
                    }
                    std::vector<std::string> keys = {pi.key};
                    if (step.isLimiter && pi.fromSpecTruePeak) keys = {"ceiling", "brickwall", "thresh"};
                    const int p = discoverParamM(t, fx, np, keys);
                    Json rep{{"key", pi.key}, {"unit", pi.unit}};
                    if (p < 0) { rep["applied"] = false; rep["reason"] = "no param matched by name"; }
                    else {
                        double mn = 0.0, mx = 0.0;
                        TrackFX_GetParam(t, fx, p, &mn, &mx);
                        double v = pi.value;
                        if (pi.normalized) { TrackFX_SetParamNormalized(t, fx, p, v); }
                        else { if (mx > mn) { if (v < mn) v = mn; if (v > mx) v = mx; } TrackFX_SetParam(t, fx, p, v); }
                        rep["param"] = p; rep["name"] = fxParamNameM(t, fx, p);
                        rep["value"] = TrackFX_GetParam(t, fx, p, &mn, &mx); rep["applied"] = true;
                    }
                    setParams.push_back(std::move(rep));
                }
                Json item{{"role", step.role}, {"fx", fxNameM(t, fx)}, {"fxIndex", fx},
                          {"detected", step.detected}, {"multichannel", mcActive}, {"params", setParams}};
                // Immersive-aware: keep the LFE out of the dynamics on a bed.
                if (step.lfeExempt && (hasLfe || mcActive)) {
                    if (mcActive) {
                        // The multichannel JSFX has its own "LFE channel" slider: it passes the LFE
                        // through unlimited and excludes it from the LINKED gain reduction. Set the
                        // slider (1-based; 0 = none) rather than pin-wiring — pin-wiring around the LFE
                        // would stop the JSFX from passing it through at all.
                        const int lp = discoverParamM(t, fx, np, {"lfe"});
                        const int lfeVal = hasLfe ? lfeIdx0 + 1 : 0;
                        Json lrep{{"method", "jsfx-internal"}, {"lfeChannel", lfeVal}};
                        if (lp >= 0) {
                            double mn = 0.0, mx = 0.0; TrackFX_GetParam(t, fx, lp, &mn, &mx);
                            double v = (double)lfeVal;
                            if (mx > mn) { if (v < mn) v = mn; if (v > mx) v = mx; }
                            TrackFX_SetParam(t, fx, lp, v);
                            lrep["applied"] = true; lrep["param"] = lp;
                        } else { lrep["applied"] = false; lrep["reason"] = "no LFE param on the JSFX"; }
                        item["lfeExemption"] = lrep;
                        if (hasLfe)
                            txn.recordExecuted("exclude_lfe",
                                "track " + std::to_string(track) + " fx " + usedFx,
                                "LFE ch " + std::to_string(lfeIdx0 + 1) + " (jsfx-internal)");
                    } else if (hasLfe) {
                        item["lfeExemption"] = wireExcludeLfe(t, fx, nchan, lfeIdx0);
                        txn.recordExecuted("exclude_lfe", "track " + std::to_string(track) + " fx " + step.fx,
                                           "LFE ch " + std::to_string(lfeIdx0 + 1));
                    }
                }
                // Honest coverage note: a stock 2-ch dynamics FX cannot cover all mains of a wide bed.
                // The bundled multichannel limiter reaches every main, so it is exempt from this warning.
                if ((step.isLimiter || step.role == "multiband" || step.role == "comp") && nchan > 2 &&
                    !(step.isLimiter && mcActive)) {
                    int inPins = 0, outPins = 0; TrackFX_GetIOSize(t, fx, &inPins, &outPins);
                    if (outPins > 0 && outPins < nchan - (hasLfe ? 1 : 0))
                        warnings.push_back(step.role + " '" + fxNameM(t, fx) + "' has " +
                            std::to_string(outPins) + " channels but the bed has " + std::to_string(nchan) +
                            " — only the first channels are processed; use a multichannel " + step.role +
                            " or per-bed instances for full mains coverage.");
                }
                inserted.push_back(std::move(item));
            }
            txn.commit();
            Json out{{"ok", true}, {"style", st->name}, {"track", track},
                     {"immersiveAware", rc.immersiveAware}, {"ceilingDb", ceilingDb},
                     {"channels", nchan}, {"hasLfe", hasLfe}, {"reused", false},
                     {"inserted", inserted}, {"stepCount", (int)rc.steps.size()},
                     {"diff", plan.diff()}};
            if (hasLfe) out["lfeChannel"] = lfeIdx0 + 1;
            if (!warnings.empty()) out["warnings"] = warnings;
            return out;
#else
            // Host build: resolve the chain (no installed-FX probe) and return the plan. `channels`
            // (optional) lets a caller/test preview the wide-bed multichannel-limiter resolution.
            const int hostChannels = optInt(a, "channels", 2);
            const style::ResolvedChain rc = style::resolveChain(*st, {}, ceilingDb, hostChannels);
            Json out = style::toJson(rc);
            out["ok"] = true; out["track"] = track; out["dryRun"] = dryRun;
            out["stepCount"] = (int)rc.steps.size();
            out["note"] = "host build: resolved plan only (no FX inserted).";
            if (!warnings.empty()) out["warnings"] = warnings;
            return out;
#endif
        }});

    // ============================= master track =============================
    // The master track is REAPER's final mix bus (GetMasterTrack) — reached separately from the
    // index-addressed track setters. These mirror track.get / track.set_fader / fx.add / fx.set_param
    // for the master, so an agent can finish a mix on the master bus without a chunk edit.

    // ---- master.get (read-only) ----
    reg.add(Tool{
        "master.get", "Read the master track: volume (dB), mute, channel count, and FX count.",
        jparse(R"({"type":"object","properties":{},"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"volDb":{"type":"number"},"muted":{"type":"boolean"},
            "channels":{"type":"integer"},"fxCount":{"type":"integer"}},
            "required":["volDb","muted","channels","fxCount"]})"),
        ToolAnnotations{true, false, true}, Profile::Mixing,
        [](const Json&) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* m = GetMasterTrack(kCur);
            return Json{{"volDb", gainToDb(GetMediaTrackInfo_Value(m, "D_VOL"))},
                        {"muted", GetMediaTrackInfo_Value(m, "B_MUTE") > 0.5},
                        {"channels", (int)GetMediaTrackInfo_Value(m, "I_NCHAN")},
                        {"fxCount", TrackFX_GetCount(m)}};
#else
            return Json{{"volDb", 0.0}, {"muted", false}, {"channels", 2}, {"fxCount", 0}};
#endif
        }});

    // ---- master.set (mutating; set master volume (dB) and/or mute) ----
    reg.add(Tool{
        "master.set", "Set the master track's volume (dB) and/or mute. Only provided fields change. "
                      "Single-undo.",
        jparse(R"({"type":"object","properties":{"db":{"type":"number"},"mute":{"type":"boolean"}},
            "additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"volDb":{"type":"number"},
            "muted":{"type":"boolean"}},"required":["ok"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ true}, Profile::Mixing,
        [](const Json& a) -> Json {
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* m = GetMasterTrack(kCur);
            Undo_BeginBlock2(kCur);
            if (a.contains("db") && a["db"].is_number())
                SetMediaTrackInfo_Value(m, "D_VOL", dbToGain(a["db"].get<double>()));
            if (a.contains("mute") && a["mute"].is_boolean())
                SetMediaTrackInfo_Value(m, "B_MUTE", a["mute"].get<bool>() ? 1.0 : 0.0);
            Undo_EndBlock2(kCur, "MCP: set master", -1);
            return Json{{"ok", true}, {"volDb", gainToDb(GetMediaTrackInfo_Value(m, "D_VOL"))},
                        {"muted", GetMediaTrackInfo_Value(m, "B_MUTE") > 0.5}};
#else
            return Json{{"ok", true}, {"volDb", optNum(a, "db", 0.0)}, {"muted", optBool(a, "mute", false)}};
#endif
        }});

    // ---- master.add_fx (mutating; add an FX to the master FX chain) ----
    reg.add(Tool{
        "master.add_fx", "Add an FX by name to the master track's FX chain (always a new instance). Name "
                         "matches REAPER's Add-FX search. Single-undo.",
        jparse(R"({"type":"object","properties":{"name":{"type":"string"}},
            "required":["name"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"fxIndex":{"type":"integer"},"name":{"type":"string"},
            "fxCount":{"type":"integer"}},"required":["fxIndex"]})"),
        ToolAnnotations{false, false, false}, Profile::Mixing,
        [](const Json& a) -> Json {
            const std::string name = reqStr(a, "name");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* m = GetMasterTrack(kCur);
            Undo_BeginBlock2(kCur);
            int fx = TrackFX_AddByName(m, name.c_str(), false, -1);  // -1 = always create new
            Undo_EndBlock2(kCur, "MCP: add master FX", -1);
            if (fx < 0) throw std::runtime_error("FX not found / could not add: " + name);
            return Json{{"fxIndex", fx}, {"name", fxNameM(m, fx)}, {"fxCount", TrackFX_GetCount(m)}};
#else
            return Json{{"fxIndex", 0}, {"name", name}, {"fxCount", 1}};
#endif
        }});

    // ---- master.set_fx_param (mutating; set a master-FX parameter) ----
    reg.add(Tool{
        "master.set_fx_param", "Set one master-FX parameter. Provide 'normalized' [0,1] OR 'value' (raw "
                               "plugin units). Single-undo.",
        jparse(R"({"type":"object","properties":{"fx":{"type":"integer","minimum":0},
            "param":{"type":"integer","minimum":0},"value":{"type":"number"},
            "normalized":{"type":"number","minimum":0,"maximum":1}},
            "required":["fx","param"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"value":{"type":"number"},
            "normalized":{"type":"number"}},"required":["ok"]})"),
        ToolAnnotations{false, false, true}, Profile::Mixing,
        [](const Json& a) -> Json {
            const int fx = reqInt(a, "fx"), p = reqInt(a, "param");
            const bool hasNorm = a.contains("normalized") && a["normalized"].is_number();
            const bool hasVal = a.contains("value") && a["value"].is_number();
            if (!hasNorm && !hasVal)
                throw std::runtime_error("master.set_fx_param requires 'normalized' or 'value'");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* m = GetMasterTrack(kCur);
            Undo_BeginBlock2(kCur);
            if (hasNorm) TrackFX_SetParamNormalized(m, fx, p, a["normalized"].get<double>());
            else         TrackFX_SetParam(m, fx, p, a["value"].get<double>());
            Undo_EndBlock2(kCur, "MCP: set master FX param", -1);
            double mn = 0.0, mx = 0.0;
            return Json{{"ok", true}, {"value", TrackFX_GetParam(m, fx, p, &mn, &mx)},
                        {"normalized", TrackFX_GetParamNormalized(m, fx, p)}};
#else
            (void)fx; (void)p;
            return Json{{"ok", true},
                        {"value", hasVal ? a["value"].get<double>() : 0.0},
                        {"normalized", hasNorm ? a["normalized"].get<double>() : 0.0}};
#endif
        }});

    // ---- master.remove_fx (mutating; DESTRUCTIVE) ----
    reg.add(Tool{
        "master.remove_fx",
        "Remove an FX from the master track's FX chain by index (from master.get / fx list order). "
        "Completes the master mirror of the track fx.add/fx.remove family.",
        jparse(R"({"type":"object","properties":{"fx":{"type":"integer","minimum":0}},
            "required":["fx"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"removedIndex":{"type":"integer"},
            "fxCount":{"type":"integer"}},"required":["ok","removedIndex"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false},
        Profile::Mixing,
        [](const Json& a) -> Json {
            const int fx = reqInt(a, "fx");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* m = GetMasterTrack(kCur);
            if (fx >= TrackFX_GetCount(m))
                throw std::runtime_error("master FX index out of range: " + std::to_string(fx));
            Undo_BeginBlock2(kCur);
            const bool ok = TrackFX_Delete(m, fx);
            Undo_EndBlock2(kCur, "MCP: remove master FX", -1);
            if (!ok) throw std::runtime_error("could not remove master FX " + std::to_string(fx));
            return Json{{"ok", true}, {"removedIndex", fx}, {"fxCount", TrackFX_GetCount(m)}};
#else
            return Json{{"ok", true}, {"removedIndex", fx}, {"fxCount", 0}};
#endif
        }});
}

}  // namespace reaper_mcp
