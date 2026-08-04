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

// Profile-legal bed channel counts (2.0/3.0/5.0/5.1/7.0/7.1/7.0.2/7.1.2) — the file-level gate for
// CUSTOM DirectSpeakers packs, where only the chna track count is knowable. A count in this set is
// necessary, not sufficient (a 10-ch 5.1.4 collides with 7.1.2) — such packs pass with a note.
inline bool bedChannelCountAllowed(int n) {
    return n == 2 || n == 3 || n == 5 || n == 6 || n == 7 || n == 8 || n == 9 || n == 10;
}

// BS.2094 common-definitions DirectSpeakers packs (IDs and layouts are the published standard's
// data; channel counts from the common-definitions XML). atmosAllowed = the layout is one of the
// profile's legal beds. An ID in this table settles the bed question exactly — no count ambiguity.
struct CommonDefBed { const char* id; const char* name; int channels; bool atmosAllowed; };
inline const CommonDefBed* commonDefBedFor(const std::string& packId) {
    static const CommonDefBed k[] = {
        {"AP_00010001", "mono (0+1+0)",            1,  false},
        {"AP_00010002", "stereo 2.0 (0+2+0)",      2,  true},
        {"AP_0001000a", "3.0 (0+3+0)",             3,  true},
        {"AP_0001000b", "4.0 quad (0+4+0)",        4,  false},
        {"AP_0001000c", "5.0 (0+5+0)",             5,  true},
        {"AP_00010003", "5.1 (0+5+0)",             6,  true},
        {"AP_0001000d", "6.1 (0+6+0)",             7,  false},
        {"AP_0001000e", "7.1front (0+7+0)",        8,  false},
        {"AP_0001000f", "7.1back (0+7+0)",         8,  true},
        {"AP_00010004", "7.1top / 5.1.2 (2+5+0)",  8,  false},
        {"AP_00010012", "7.1side 5.1+sc (0+7+0)",  8,  false},
        {"AP_00010013", "7.1topside 5.1.2 (2+5+0)",8,  false},
        {"AP_00010014", "9.1screen 5.1.2+sc",      10, false},
        {"AP_00010016", "9.1 / 7.1.2 (2+7+0)",     10, true},
        {"AP_00010005", "9.1 / 5.1.4 (4+5+0)",     10, false},
        {"AP_00010010", "10.1 (4+5+1)",            11, false},
        {"AP_00010007", "10.2 (3+7+0)",            12, false},
        {"AP_00010015", "11.1 5.1.4+sc (4+7+0)",   12, false},
        {"AP_00010017", "11.1 / 7.1.4 (4+7+0)",    12, false},
        {"AP_00010008", "13.1 (4+9+0)",            14, false},
        {"AP_00010011", "Auro-3D (9+9+0)",         19, false},
        {"AP_00010009", "22.2 (9+10+3)",           24, false},
    };
    for (const auto& e : k) if (packId == e.id) return &e;
    return nullptr;
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
// Hardened per the cross-validation findings (I-1..I-4): pack types are classified from the
// chna packRef IDs (so BS.2094 common-definitions content — the dominant broadcast dialect, which
// defines nothing inline — is no longer invisible), the bed cap / single-programme / 24-bit rules
// are enforced, and the interpolationLength check verifies the actual 0-then-250-samples ramp.
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

    // bit depth (I-3: the profile's masters are 24-bit PCM)
    const int bits = fmt.contains("bitDepth") ? fmt["bitDepth"].get<int>() : 0;
    if (bits != 24)
        r.violations.push_back({"bit_depth", "bitDepth " + std::to_string(bits) + " != 24"});

    // channel cap
    const int ch = fmt.contains("channels") ? fmt["channels"].get<int>() : 0;
    if (ch > kMaxChannels)
        r.violations.push_back({"channel_cap", std::to_string(ch) + " channels > 128"});

    // ---- pack classification from chna (I-1): per-pack track counts + type census ----
    std::vector<std::pair<std::string, int>> packCount;   // distinct packRef -> #tracks
    int nObjPacks = 0, nHOA = 0, nBin = 0, nMatrix = 0;
    bool sawDS = false;
    if (s.contains("chna") && s["chna"].is_object() && s["chna"].contains("tracks")) {
        for (const auto& t : s["chna"]["tracks"]) {
            const std::string prf = t.value("packRef", std::string());
            if (prf.size() < 11 || prf.compare(0, 3, "AP_") != 0) continue;
            bool found = false;
            for (auto& pc : packCount)
                if (pc.first == prf) { ++pc.second; found = true; break; }
            if (!found) packCount.push_back({prf, 1});
        }
        for (const auto& pc : packCount) {
            const std::string type = pc.first.substr(3, 4);
            if      (type == "0001") sawDS = true;
            else if (type == "0002") ++nMatrix;
            else if (type == "0003") ++nObjPacks;
            else if (type == "0004") ++nHOA;
            else if (type == "0005") ++nBin;
        }
    }

    // content types (I-1): the profile's content is DirectSpeakers beds + Objects, nothing else
    if (nHOA > 0)
        r.violations.push_back({"content_type", std::to_string(nHOA) + " HOA pack(s) — the profile carries beds + objects only"});
    if (nBin > 0)
        r.violations.push_back({"content_type", "binaural pack present — the profile carries beds + objects only"});
    if (nMatrix > 0)
        r.violations.push_back({"content_type", "Matrix pack present — the profile carries beds + objects only"});

    // bed layouts (I-2): common-definitions packs settle exactly; custom packs gate on track count
    for (const auto& pc : packCount) {
        if (pc.first.compare(0, 7, "AP_0001") != 0) continue;
        if (const CommonDefBed* cd = commonDefBedFor(pc.first)) {
            if (!cd->atmosAllowed)
                r.violations.push_back({"bed_layout", "bed " + pc.first + " = " + cd->name +
                                                          " is not an Atmos bed (max 7.1.2)"});
            else
                r.notes.push_back("bed " + pc.first + " = " + std::string(cd->name) +
                                  " (common definitions).");
        } else if (!bedChannelCountAllowed(pc.second)) {
            r.violations.push_back({"bed_layout", "bed " + pc.first + " has " +
                                                      std::to_string(pc.second) +
                                                      " channels — not a profile bed layout"});
        } else {
            r.notes.push_back("bed " + pc.first + " layout identity unverified (count-compatible, " +
                              std::to_string(pc.second) + " ch).");
        }
    }

    const bool hasObjects = (admS.is_object() && admS.value("hasObjects", false)) || nObjPacks > 0;
    const bool hasBed = sawDS || (admS.is_object() && admS.value("hasDirectSpeakers", false));

    // object cap: distinct Objects packs when chna is decodable (1 pack per object in both the
    // Dolby dialect and this writer); fall back to the audioObject count arithmetic otherwise
    if (nObjPacks > 0) {
        if (nObjPacks > kMaxObjects)
            r.violations.push_back({"object_cap", std::to_string(nObjPacks) + " objects > 118"});
    } else if (admS.is_object()) {
        const int nObjTot = admS.value("audioObjects", 0);
        const int nObjects = nObjTot - (hasBed ? 1 : 0);
        if (nObjects > kMaxObjects)
            r.violations.push_back({"object_cap", std::to_string(nObjects) + " objects > 118"});
    }
    if (!hasBed && hasObjects) r.notes.push_back("objects-only (no bed).");

    if (admS.is_object()) {
        // objects must be cartesian
        if (hasObjects && admS.value("coordinateMode", std::string()) != "cartesian")
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

    // programme ID + single-programme rule (I-3)
    if (!pr.axml.empty()) {
        if (pr.axml.find(std::string("audioProgrammeID=\"") + kProgrammeID + "\"") == std::string::npos)
            r.violations.push_back({"programme_id", "audioProgramme ID is not APR_1001"});
        const int nProg = countOccur(pr.axml, "<audioProgramme ");
        if (nProg > 1)
            r.violations.push_back({"programme_count", std::to_string(nProg) +
                                                           " audioProgrammes — the profile carries exactly one"});
    }

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

        // interpolationLength (I-4): presence, then the actual ramp — 0 on the first block of each
        // object channel, 250 samples on every subsequent one
        if (hasObjects) {
            if (pr.axml.find("interpolationLength") == std::string::npos) {
                r.violations.push_back({"interpolation",
                                        "objects carry no interpolationLength (profile: 0 then 250 samples)"});
            } else {
                const double want = (double)kInterpSubsequentSamples / (double)(sr > 0 ? sr : kSampleRate);
                bool rampBad = false;
                std::string rampDetail;
                size_t pos = 0;
                while (!rampBad && (pos = pr.axml.find("<audioChannelFormat ", pos)) != std::string::npos) {
                    size_t end = pr.axml.find("</audioChannelFormat>", pos);
                    if (end == std::string::npos) break;
                    const std::string cf = pr.axml.substr(pos, end - pos);
                    pos = end + 21;
                    if (cf.find("typeDefinition=\"Objects\"") == std::string::npos) continue;
                    size_t bp = 0;
                    int k = 0;
                    while (!rampBad && (bp = cf.find("<audioBlockFormat", bp)) != std::string::npos) {
                        size_t be = cf.find("</audioBlockFormat>", bp);
                        if (be == std::string::npos) be = cf.size();
                        const std::string bf = cf.substr(bp, be - bp);
                        bp = be;
                        const auto il = extractAttr(bf, "interpolationLength");
                        double v = -1.0;
                        if (!il.empty()) { try { v = std::stod(il[0]); } catch (...) { v = -1.0; } }
                        if (k == 0) {
                            if (!il.empty() && !(v >= 0.0 && v < 1e-9)) {
                                rampBad = true;
                                rampDetail = "first block interpolationLength " + il[0] + " != 0";
                            }
                        } else if (il.empty() || v < 0.0 || std::fabs(v - want) > 1e-4) {
                            rampBad = true;
                            rampDetail = "subsequent block interpolationLength " +
                                         (il.empty() ? std::string("missing") : il[0]) + " != " +
                                         fmtNum(want) + " (250 samples)";
                        }
                        ++k;
                    }
                }
                if (rampBad)
                    r.violations.push_back({"interpolation_ramp", rampDetail});
            }
        }
    }

    r.conformant = r.violations.empty();
    return r;
}

}  // namespace profile
}  // namespace adm
}  // namespace reaper_mcp
