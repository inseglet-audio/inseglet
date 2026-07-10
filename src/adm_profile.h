// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// adm_profile.h — Dolby Atmos Master ADM Profile v1.0 conformance (normalize + validate).
//
// adm_bwf.h authors a generic ITU-R BS.2076 ADM BWF and enriches objects with extent / divergence /
// importance. Neither guarantees a certified Dolby renderer will ingest the file AS an Atmos master —
// that requires the constraints Dolby publishes in the "Dolby Atmos Master ADM Profile v1.0". This
// header adds two things on top of adm_bwf.h, WITHOUT a new container or serializer:
//
//   • normalizeModel() — bend an adm::Model to the profile in place before serialization (force
//     cartesian objects, collapse extent to an identical [0,1] size, strip the prohibited
//     objectDivergence, drop importance, flag the sample-rate / bed-layout / channel-cap invariants).
//     Powers spatial.export_adm's profile:"dolby-atmos" mode. Fails closed (returns false) on an
//     UNFIXABLE invariant — a non-Atmos bed (7.1.4/9.1.6/22.2), a non-48k render, or >118 objects /
//     >128 channels — because those can't be silently coerced without lying about the material.
//
//   • validateParsed() — inspect a PARSED ADM file (adm::ParseResult) and report per-rule conformance.
//     Powers analysis.adm_profile_check: point it at ANY ADM BWF (ours, Nuendo's, a third party's) and
//     it says pass / lists the exact violations. Read-only; a superset of adm_inspect's readout.
//
// The rule set (from the Dolby Atmos Master ADM Profile v1.0):
//   - Sample rate 48000 (mandatory).
//   - Beds are DirectSpeakers, at most 7.1.2 (10 ch) — the Atmos bed maxes at 7.1.2; heights/wides
//     beyond that are OBJECTS, not bed channels. Allowed sets incl. 5.1 / 7.1 / 7.1.2. (2.0/3.0/5.0/
//     7.0/7.0.2 are profile-allowed too but this tool doesn't author them.)
//   - ≤128 channels total, ≤118 objects.
//   - Single audioProgramme, ID = APR_1001.
//   - Objects use CARTESIAN coordinates, X/Y/Z ∈ [-1,1].
//   - Object size (extent): width == depth == height, all in [0,1], if present.
//   - objectDivergence: PROHIBITED ("shall not be used").
//   - importance: only on INACTIVE objects (value 0) — i.e. never on the active objects this tool
//     authors, so it is stripped/flagged.
//   - interpolationLength: 0 samples on the first audioBlockFormat, 250 samples on every subsequent
//     one (carried on jumpPosition, emitted by adm_bwf.h when Model.dolbyProfile is set).
//
// HONESTY CAVEAT (same footing as the rest of the ADM authoring): the output is profile-SHAPED and
// self-validated here; whether a SPECIFIC certified renderer ingests it exactly is your ingest check.
//
// SDK-free + JSON-only (no REAPER) — pulls adm_bwf.h (which pulls composite_support.h → Json + the
// SWELL-safe include order). Host-unit-tested with no DAW (tests/unit/test_adm_profile.cpp).

#pragma once

#include "adm_bwf.h"  // adm::Model / adm::ParseResult / adm::fmtNum / adm::Coord / Json

#include <string>
#include <vector>

namespace reaper_mcp {
namespace adm {
namespace profile {

// ---- constants (Dolby Atmos Master ADM Profile v1.0) --------------------------------------------
constexpr int   kSampleRate               = 48000;
constexpr int   kMaxChannels              = 128;
constexpr int   kMaxObjects               = 118;
constexpr int   kInterpSubsequentSamples  = 250;   // matches adm_bwf.h's profile jumpPosition ramp
constexpr const char* kProgrammeID        = "APR_1001";

// The Atmos bed maxes at 7.1.2 (10 ch). Of the tool's supported layouts, only these conform; a 7.1.4 /
// 9.1.6 / 22.2 "bed" is NOT a valid Atmos bed (route those extra channels as objects). "" = objects-only.
inline bool bedLayoutAllowed(const std::string& name) {
    return name.empty() || name == "5.1" || name == "7.1" || name == "7.1.2";
}

// ---- report types -------------------------------------------------------------------------------
struct Violation {
    std::string code;
    std::string detail;
};

struct Report {
    bool conformant = false;
    std::vector<Violation> violations;  // hard non-conformances (empty => conformant)
    std::vector<std::string> notes;     // informational (e.g. "objects-only, no bed")

    Json toJson() const {
        Json v = Json::array();
        for (const auto& x : violations) v.push_back(Json{{"code", x.code}, {"detail", x.detail}});
        Json n = Json::array();
        for (const auto& s : notes) n.push_back(s);
        return Json{{"profile", "dolby-atmos"},
                    {"conformant", conformant},
                    {"violations", v},
                    {"noteCount", (int)notes.size()},
                    {"notes", n}};
    }
};

// ---- small helpers ------------------------------------------------------------------------------
inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
inline double max3(double a, double b, double c) { double m = a > b ? a : b; return m > c ? m : c; }

// Extract every value of a simple <tag>value</tag> element (best-effort text scan).
inline std::vector<std::string> extractElem(const std::string& hay, const std::string& tag) {
    std::vector<std::string> out;
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    size_t p = 0;
    while ((p = hay.find(open, p)) != std::string::npos) {
        size_t s = p + open.size();
        size_t e = hay.find(close, s);
        if (e == std::string::npos) break;
        out.push_back(hay.substr(s, e - s));
        p = e + close.size();
    }
    return out;
}

// ---- normalizeModel — bend a Model to the profile in place (writer path) ------------------------
// Returns false + sets `err` on an UNFIXABLE invariant; applies fixable deviations and appends a
// human-readable line per change to `warnings`. On success sets m.dolbyProfile so the serializer emits
// the interpolationLength ramp.
inline bool normalizeModel(Model& m, std::vector<std::string>& warnings, std::string& err) {
    if (!bedLayoutAllowed(m.bedLayoutName)) {
        err = "bed_not_profile_conformant";
        return false;
    }
    if (m.sampleRate != kSampleRate) {
        err = "sample_rate_not_48k";
        return false;
    }
    if ((int)m.objects.size() > kMaxObjects) {
        err = "too_many_objects";
        return false;
    }
    if (m.channelCount() > kMaxChannels) {
        err = "too_many_channels";
        return false;
    }

    for (Object& o : m.objects) {
        if (o.coord != Coord::Cartesian) {
            o.coord = Coord::Cartesian;
            warnings.push_back("object '" + o.name +
                               "': forced cartesian coordinates (Dolby profile).");
        }
        if (o.importance >= 0) {
            warnings.push_back("object '" + o.name +
                               "': dropped importance (Dolby profile permits it only on inactive "
                               "objects).");
            o.importance = -1;
        }
        for (Block& b : o.blocks) {
            if (b.hasDivergence) {
                b.hasDivergence = false;
                b.divergence = 0.0;
                b.divergenceRange = 0.0;
                warnings.push_back("object '" + o.name +
                                   "': stripped objectDivergence (prohibited by the Dolby profile).");
            }
            const bool anyExtent = (b.width > 0.0 || b.height > 0.0 || b.depth > 0.0);
            if (anyExtent) {
                const bool identical = (b.width == b.height && b.height == b.depth);
                double s = clamp01(max3(b.width, b.height, b.depth));
                if (!identical || b.width > 1.0 || b.height > 1.0 || b.depth > 1.0) {
                    warnings.push_back(
                        "object '" + o.name + "': extent normalized to identical size " + fmtNum(s) +
                        " in [0,1] (Dolby profile requires width=depth=height; use the 'size' knob).");
                }
                b.width = b.height = b.depth = s;
            }
        }
    }

    m.dolbyProfile = true;
    return true;
}

// ---- validateModel — inspect a Model (pre-serialize), no mutation -------------------------------
inline Report validateModel(const Model& m) {
    Report r;
    if (!bedLayoutAllowed(m.bedLayoutName))
        r.violations.push_back({"bed_layout",
                                "bed '" + m.bedLayoutName +
                                    "' is not an Atmos bed (max 7.1.2 — route extra channels as objects)"});
    if (m.sampleRate != kSampleRate)
        r.violations.push_back({"sample_rate",
                                "sampleRate " + std::to_string(m.sampleRate) + " != 48000"});
    if ((int)m.objects.size() > kMaxObjects)
        r.violations.push_back({"object_cap",
                                std::to_string(m.objects.size()) + " objects > 118"});
    if (m.channelCount() > kMaxChannels)
        r.violations.push_back({"channel_cap",
                                std::to_string(m.channelCount()) + " channels > 128"});
    for (const Object& o : m.objects) {
        if (o.coord != Coord::Cartesian)
            r.violations.push_back({"coordinate", "object '" + o.name + "' is not cartesian"});
        if (o.importance >= 0)
            r.violations.push_back({"importance", "object '" + o.name + "' carries importance"});
        for (const Block& b : o.blocks) {
            if (b.hasDivergence)
                r.violations.push_back({"divergence", "object '" + o.name + "' uses objectDivergence"});
            const bool anyExtent = (b.width > 0.0 || b.height > 0.0 || b.depth > 0.0);
            if (anyExtent && !(b.width == b.height && b.height == b.depth &&
                               b.width <= 1.0 && b.width >= 0.0))
                r.violations.push_back({"extent",
                                        "object '" + o.name + "' extent is not identical [0,1]"});
        }
    }
    if (m.objects.empty() && m.bed.empty())
        r.violations.push_back({"empty", "no bed and no objects"});
    if (m.bed.empty() && !m.objects.empty()) r.notes.push_back("objects-only (no bed).");
    r.conformant = r.violations.empty();
    return r;
}

// ---- validateParsed — inspect a PARSED ADM file (analysis.adm_profile_check) --------------------
inline Report validateParsed(const ParseResult& pr) {
    Report r;
    const Json& s = pr.summary;
    if (!s.contains("isAdm") || !s["isAdm"].get<bool>()) {
        r.violations.push_back({"not_adm", "no chna+axml — not an ADM BWF"});
        r.conformant = false;
        return r;
    }
    const Json& fmt = s["format"];
    const Json& admS = s["adm"];

    // sample rate
    const int sr = fmt.contains("sampleRate") ? (int)fmt["sampleRate"].get<double>() : 0;
    if (sr != kSampleRate)
        r.violations.push_back({"sample_rate", "sampleRate " + std::to_string(sr) + " != 48000"});

    // channel cap
    const int ch = fmt.contains("channels") ? fmt["channels"].get<int>() : 0;
    if (ch > kMaxChannels)
        r.violations.push_back({"channel_cap", std::to_string(ch) + " channels > 128"});

    // object cap (audioObjects minus the bed audioObject, if any)
    if (admS.is_object()) {
        const int nObjTot = admS.value("audioObjects", 0);
        const bool hasBed = admS.value("hasDirectSpeakers", false);
        const int nObjects = nObjTot - (hasBed ? 1 : 0);
        if (nObjects > kMaxObjects)
            r.violations.push_back({"object_cap", std::to_string(nObjects) + " objects > 118"});
        if (!hasBed && admS.value("hasObjects", false)) r.notes.push_back("objects-only (no bed).");

        // objects must be cartesian
        if (admS.value("hasObjects", false) &&
            admS.value("coordinateMode", std::string()) != "cartesian")
            r.violations.push_back({"coordinate", "objects are not in cartesian coordinates"});

        // divergence prohibited
        if (admS.value("hasObjectDivergence", false))
            r.violations.push_back({"divergence", "objectDivergence present (prohibited)"});

        // importance only on inactive (value 0)
        if (admS.value("hasImportance", false)) {
            bool nonZero = false;
            if (admS.contains("objectImportance") && admS["objectImportance"].is_array())
                for (const auto& v : admS["objectImportance"])
                    if (v.is_number() && v.get<int>() != 0) nonZero = true;
            if (nonZero || !admS.contains("objectImportance"))
                r.violations.push_back({"importance", "importance present on active object(s)"});
        }
    }

    // programme ID (axml scan)
    if (!pr.axml.empty() &&
        pr.axml.find(std::string("audioProgrammeID=\"") + kProgrammeID + "\"") == std::string::npos)
        r.violations.push_back({"programme_id", "audioProgramme ID is not APR_1001"});

    // extent identical: the ordered width/height/depth element values must match
    if (!pr.axml.empty()) {
        const auto ws = extractElem(pr.axml, "width");
        const auto hs = extractElem(pr.axml, "height");
        const auto ds = extractElem(pr.axml, "depth");
        bool extentOk = (ws.size() == hs.size() && hs.size() == ds.size());
        if (extentOk) {
            for (size_t i = 0; i < ws.size(); ++i)
                if (ws[i] != hs[i] || hs[i] != ds[i]) { extentOk = false; break; }
        }
        if (!extentOk)
            r.violations.push_back({"extent", "object extent is not identical width=depth=height"});

        // interpolationLength presence (only meaningful when objects carry a trajectory)
        if (admS.is_object() && admS.value("hasObjects", false) &&
            pr.axml.find("interpolationLength") == std::string::npos)
            r.violations.push_back({"interpolation",
                                    "objects carry no interpolationLength (profile: 0 then 250 samples)"});
    }

    r.conformant = r.violations.empty();
    return r;
}

}  // namespace profile
}  // namespace adm
}  // namespace reaper_mcp
