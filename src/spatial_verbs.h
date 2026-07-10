// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// spatial_verbs.h — the SDK-free planning core for the spatial composite verbs.
//
// The three headline verbs — spatialize_stems, stereo_to_ambisonic, setup_immersive_session — are
// thin, deterministic compositions over the spatial primitives. Everything that does NOT need a
// running REAPER lives here so it is unit-tested host-side (test_spatial_verbs.cpp): parsing the
// `target` (bed layout vs ambisonic order), resolving a per-source placement (descriptor via
// placement_defaults.h, or explicit numeric), and building the dryRun Plan (the step list + human
// diff). The SDK execution that actually mutates the project stays in tools_spatial.cpp and mirrors
// these plans step-for-step.
//
// Include order: composite_support.h pulls reaper_api.h before <json> (SWELL min/max); placement_
// defaults.h is pure STL. Nothing here calls a REAPER API, so it compiles identically on the host
// and SDK branches.

#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "composite_support.h"   // Json, Plan, makeError / isError, CompositeError
#include "placement_defaults.h"  // placement::lookup / Width / widthName

namespace reaper_mcp {
namespace spatial_verbs {

// ---------------------------------------------------------------------------------------------
// Target: a bed layout (channel-based) OR an ambisonic scene (order + normalization).
// ---------------------------------------------------------------------------------------------

struct Target {
    bool        ambisonic = false;
    int         order = 0;                 // ambisonic order (when ambisonic)
    int         channels = 0;              // even-clamped channel count (bed width or (order+1)^2)
    std::string layout;                    // bed layout name (when !ambisonic)
    std::string normalization = "SN3D";    // ambisonic normalization
};

inline int hoaChannelsFor(int order) { return (order + 1) * (order + 1); }

// Even-clamp to REAPER's legal I_NCHAN range (mirrors tools_spatial clampChannels) so the planner's
// channel numbers agree with what execution will actually set.
inline int evenClamp(int ch) {
    if (ch < 2) ch = 2;
    if (ch > 128) ch = 128;
    if (ch & 1) ++ch;
    return ch;
}

inline int bedChannelsForLayout(const std::string& layout) {
    if (layout == "5.1")   return 6;
    if (layout == "7.1")   return 8;
    if (layout == "7.1.4") return 12;
    if (layout == "9.1.6") return 16;
    if (layout == "22.2")  return 24;
    return -1;
}

inline const char* knownLayouts() { return "5.1, 7.1, 7.1.4, 9.1.6, 22.2"; }

// Parse a `target` object. On success fills `out` and returns Json(nullptr); on failure returns a
// structured error (makeError) the caller can return verbatim as an agent-actionable result.
inline Json parseTarget(const Json& target, Target& out) {
    if (!target.is_object())
        return makeError("bad_target",
                         "target must be an object with either 'layout' or 'ambisonicOrder'",
                         "e.g. {\"layout\":\"7.1.4\"} or {\"ambisonicOrder\":3,\"normalization\":\"SN3D\"}");
    const bool hasLayout = target.contains("layout") && target["layout"].is_string();
    const bool hasOrder  = target.contains("ambisonicOrder") && target["ambisonicOrder"].is_number();
    if (hasLayout && hasOrder)
        return makeError("ambiguous_target",
                         "target has both 'layout' and 'ambisonicOrder'; give exactly one",
                         "drop one of them");
    if (hasOrder) {
        int order = target["ambisonicOrder"].get<int>();
        if (order < 1 || order > 7)
            return makeError("bad_order", "ambisonicOrder must be 1..7", "1 = FOA, up to 7 = HOA");
        out.ambisonic = true;
        out.order = order;
        out.channels = evenClamp(hoaChannelsFor(order));
        out.normalization = "SN3D";
        if (target.contains("normalization") && target["normalization"].is_string()) {
            const std::string n = target["normalization"].get<std::string>();
            if (n != "SN3D" && n != "N3D" && n != "FuMa")
                return makeError("bad_normalization", "normalization must be SN3D | N3D | FuMa",
                                 "default is SN3D");
            out.normalization = n;
        }
        return Json(nullptr);
    }
    if (hasLayout) {
        const std::string layout = target["layout"].get<std::string>();
        const int ch = bedChannelsForLayout(layout);
        if (ch < 0)
            return makeError("unknown_layout", "unknown bed layout: " + layout,
                             std::string("known layouts: ") + knownLayouts());
        out.ambisonic = false;
        out.layout = layout;
        out.channels = ch;
        return Json(nullptr);
    }
    return makeError("no_target", "target needs either 'layout' or 'ambisonicOrder'",
                     "e.g. {\"layout\":\"7.1.4\"} or {\"ambisonicOrder\":1}");
}

// ---------------------------------------------------------------------------------------------
// Placement: a semantic descriptor (placement_defaults) or explicit numeric {az,el,width,...}.
// ---------------------------------------------------------------------------------------------

struct ResolvedPlacement {
    std::string     label;                     // the descriptor, or "explicit"
    double          az = 0.0, el = 0.0;
    placement::Width width = placement::Width::Narrow;
    double          divergence = 0.0;
    bool            lfe = false;
    bool            diffuse = false;
    bool            explicitNumeric = false;   // az/el were given as numbers
};

inline bool widthFromString(const std::string& w, placement::Width& into) {
    if (w == "narrow")                    { into = placement::Width::Narrow; return true; }
    if (w == "med" || w == "medium")      { into = placement::Width::Med;    return true; }
    if (w == "large" || w == "wide")      { into = placement::Width::Large;  return true; }
    if (w == "max")                       { into = placement::Width::Max;    return true; }
    return false;
}

// Resolve one placement spec. `spec` is a descriptor string, or an object with any of
// {descriptor, az, el, width, divergence, lfe}. Explicit numeric az/el always win over a descriptor.
// `intrinsicAz` seeds descriptors that ride the source's own azimuth (wide/overhead); pass 0 when
// unknown. Returns Json(nullptr) on success, or a structured error.
inline Json resolvePlacementSpec(const Json& spec, double intrinsicAz, ResolvedPlacement& out) {
    if (spec.is_string()) {
        placement::Placement p;
        const std::string d = spec.get<std::string>();
        if (!placement::lookup(d, p))
            return makeError("unknown_descriptor", "unknown placement descriptor: " + d,
                             std::string("known: ") + placement::knownDescriptors());
        out.label = d;
        out.az = p.usesSourceAz ? intrinsicAz : p.az;
        out.el = p.el;
        out.width = p.width;
        out.divergence = p.divergence;
        out.lfe = p.lfe;
        out.diffuse = p.diffuse;
        return Json(nullptr);
    }
    if (spec.is_object()) {
        placement::Placement base;
        bool haveBase = false;
        if (spec.contains("descriptor") && spec["descriptor"].is_string()) {
            const std::string d = spec["descriptor"].get<std::string>();
            if (!placement::lookup(d, base))
                return makeError("unknown_descriptor", "unknown placement descriptor: " + d,
                                 std::string("known: ") + placement::knownDescriptors());
            haveBase = true;
            out.label = d;
        }
        out.az = haveBase ? (base.usesSourceAz ? intrinsicAz : base.az) : 0.0;
        out.el = haveBase ? base.el : 0.0;
        out.width = haveBase ? base.width : placement::Width::Narrow;
        out.divergence = haveBase ? base.divergence : 0.0;
        out.lfe = haveBase ? base.lfe : false;
        out.diffuse = haveBase ? base.diffuse : false;
        if (spec.contains("az") && spec["az"].is_number()) { out.az = spec["az"].get<double>(); out.explicitNumeric = true; }
        if (spec.contains("el") && spec["el"].is_number()) { out.el = spec["el"].get<double>(); out.explicitNumeric = true; }
        if (spec.contains("divergence") && spec["divergence"].is_number()) out.divergence = spec["divergence"].get<double>();
        if (spec.contains("lfe") && spec["lfe"].is_boolean()) out.lfe = spec["lfe"].get<bool>();
        if (spec.contains("width") && spec["width"].is_string()) {
            placement::Width w;
            if (widthFromString(spec["width"].get<std::string>(), w)) out.width = w;
        }
        if (out.label.empty()) out.label = out.explicitNumeric ? "explicit" : "front-center";
        return Json(nullptr);
    }
    return makeError("bad_placement", "placement must be a descriptor string or an object",
                     "e.g. \"front-center\" or {\"az\":30,\"el\":0}");
}

// ---------------------------------------------------------------------------------------------
// Plan builders (dryRun). Pure: they consume already-resolved sources/placements and emit the step
// list + human diff. Execution mirrors these step-for-step.
// ---------------------------------------------------------------------------------------------

struct SourceRef {
    int         index = -1;   // resolved track index (-1 if a name did not resolve / host build)
    std::string name;
    int         channels = 0; // 1 mono, 2 stereo, ...
};

inline std::string targetTag(const Target& t) {
    if (t.ambisonic)
        return std::to_string(t.order) + "-order ambisonics (" + std::to_string(t.channels) +
               "ch, " + t.normalization + ")";
    return t.layout + " bed (" + std::to_string(t.channels) + "ch)";
}

inline std::string placeTag(const ResolvedPlacement& p) {
    if (p.lfe) return "-> LFE (band-limited)";
    char buf[112];
    std::snprintf(buf, sizeof(buf), "az=%.1f el=%.1f width=%s%s", p.az, p.el,
                  placement::widthName(p.width), p.diffuse ? " diffuse" : "");
    return buf;
}

inline std::string srcTag(const SourceRef& s) {
    return "track '" + s.name + "' (idx " + std::to_string(s.index) + ")";
}

// spatialize_stems.
inline Plan planSpatialize(const Target& tgt, const std::vector<SourceRef>& srcs,
                           const std::vector<ResolvedPlacement>& places, const std::string& busName,
                           bool wantMonitor, bool busExists) {
    Plan plan;
    if (busExists)
        plan.add("reuse_bus", busName, targetTag(tgt));
    else
        plan.add(tgt.ambisonic ? "create_ambisonic_bus" : "build_bed", busName, targetTag(tgt));

    const size_t n = srcs.size() < places.size() ? srcs.size() : places.size();
    for (size_t i = 0; i < n; ++i) {
        const SourceRef& s = srcs[i];
        const ResolvedPlacement& p = places[i];
        const std::string sName = srcTag(s);
        if (tgt.ambisonic) {
            if (p.lfe) { plan.add("create_send", sName, "-> scene bus (full; LFE band n/a in a soundfield)"); continue; }
            plan.add("set_channels", sName, std::to_string(tgt.channels) + " ch");
            plan.add("insert_encoder", sName, "ambisonic encoder @ " + placeTag(p));
            plan.add("create_send", sName, "-> ambisonic bus");
        } else {
            if (p.lfe) { plan.add("create_send", sName, "-> bed LFE channel (mono)"); continue; }
            plan.add("insert_panner", sName, "ReaSurroundPan @ " + placeTag(p));
            plan.add("create_send", sName, "-> " + tgt.layout + " bed");
        }
    }
    if (wantMonitor)
        plan.add("add_binaural_monitor", "monitor bus", tgt.ambisonic ? "ambisonic (binaural decode)"
                                                                       : "stereo reference downmix");
    return plan;
}

// stereo_to_ambisonic.
inline Plan planStereoToAmbi(const SourceRef& s, int order, int channels,
                             const std::string& normalization, double spreadDeg, bool wantMonitor) {
    Plan plan;
    const std::string sName = srcTag(s);
    plan.add("set_channels", sName,
             std::to_string(channels) + " ch (" + std::to_string(order) + "-order sketch)");
    plan.add("insert_encoder", sName, "ambisonic encoder (" + normalization + ")");
    char b[80];
    std::snprintf(b, sizeof(b), "L az=+%.1f, R az=-%.1f", spreadDeg, spreadDeg);
    plan.add("set_param", sName, b);
    if (wantMonitor) plan.add("add_binaural_monitor", "monitor bus", "ambisonic (binaural decode)");
    return plan;
}

// setup_immersive_session.
inline Plan planSetupImmersive(const std::string& bedLayout, int bedChannels, int objectCount,
                               bool wantMonitor, bool rendererSends) {
    Plan plan;
    plan.add("build_bed", bedLayout + " Bed", std::to_string(bedChannels) + " ch");
    for (int i = 0; i < objectCount; ++i)
        plan.add("add_object_track", "Object " + std::to_string(i + 1), "mono object track");
    if (wantMonitor) plan.add("add_binaural_monitor", "monitor bus", "object");
    if (rendererSends)
        plan.add("atmos_send_layout", "Dolby Renderer bus",
                 std::to_string(bedChannels) + "-ch bed + " + std::to_string(objectCount) + " objects");
    return plan;
}

}  // namespace spatial_verbs
}  // namespace reaper_mcp
