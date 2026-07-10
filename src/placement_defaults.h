// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// placement_defaults.h — the descriptor -> geometry map.
//
// The bridge from *words* to *numbers* for the spatial composite verbs (spatialize_stems,
// setup_immersive_session). A source's placement may be given either as explicit numeric
// {az,el,width,divergence,lfe} — which ALWAYS wins — or as one of the semantic descriptors below,
// which this table resolves to the same geometry. One place, so any future tuning is a one-file change.
//
// Convention (matches the live IEM build, which reports ±180 azimuth, ±90 elevation):
//   azimuth   0° = front, POSITIVE = counter-clockwise = toward the LEFT (IEM / ACN convention);
//   elevation 0° = ear level, POSITIVE = up.
// So the ITU-R BS.775 front mains are L = +30°, R = −30°; the 7.1 side pair is ±105°, the rear/back
// pair ±135°, the Atmos height layer +45° el, a single overhead "voice of god" +90° el.
//
// Descriptor table:
//   front-center            0° / 0°   narrow   lead vocal / dialog
//   front-left/-right     ±30° / 0°   narrow   BS.775 L/R mains
//   wide            (source az) / 0°  large    paired ±spread encoders or ↑divergence
//   side / surround      ±105° / 0°   med      7.1 side pair (±105°)
//   rear / back          ±135° / 0°   med      7.1 back pair (distinct from side)
//   overhead / height (source az)/+45° med      Atmos top layer (.4/.6)
//   top / zenith            0° / +90° narrow   single overhead ("voice of god")
//   lfe / sub               — / —     —        route to LFE, band-limited (<120 Hz)
//   diffuse / ambient   spread / 0°   max      decorrelated / low-order bed
//
// This header is intentionally SDK-free and JSON-free: pure STL so it compiles on the host branch and
// the SDK path identically, and its numbers are unit-tested with no REAPER (test_placement_map.cpp).
// Include order is irrelevant (no SWELL / no <json> here).

#pragma once

#include <cctype>
#include <string>

namespace reaper_mcp {
namespace placement {

// Perceptual source width. The verb decides how to realize it (paired ±spread encoders, an encoder
// "width"/"size" param, or divergence); the table only names the intent + a default spread in degrees.
enum class Width { Narrow, Med, Large, Max };

inline const char* widthName(Width w) {
    switch (w) {
        case Width::Narrow: return "narrow";
        case Width::Med:    return "med";
        case Width::Large:  return "large";
        case Width::Max:    return "max";
    }
    return "narrow";
}

// A conventional half-spread in degrees for a given width label (0 = a point source). Used when a verb
// realizes width as a pair of encoders at (az ± spread) or as an encoder width param.
inline double widthSpreadDeg(Width w) {
    switch (w) {
        case Width::Narrow: return 0.0;
        case Width::Med:    return 20.0;
        case Width::Large:  return 45.0;
        case Width::Max:    return 90.0;
    }
    return 0.0;
}

struct Placement {
    double az = 0.0;              // degrees, 0 = front, + = CCW / left
    double el = 0.0;              // degrees, + = up
    Width  width = Width::Narrow;
    double divergence = 0.0;      // 0..1 spatial spread hint for panners that expose it
    bool   lfe = false;           // route to the LFE band (<120 Hz), no az/el placement
    bool   usesSourceAz = false;  // az above is a placeholder — substitute the source's intrinsic az
    bool   diffuse = false;       // decorrelated / spread across the bed (az is not a point)
};

// Normalize a descriptor to its canonical key: lowercase, trim, and fold spaces/underscores to '-'.
inline std::string normalize(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == ' ' || c == '_') s.push_back('-');
        else s.push_back(static_cast<char>(std::tolower(uc)));
    }
    // trim leading/trailing hyphens/space produced by padding
    size_t b = s.find_first_not_of("- ");
    size_t e = s.find_last_not_of("- ");
    if (b == std::string::npos) return std::string();
    return s.substr(b, e - b + 1);
}

// Resolve a semantic descriptor to its default geometry. Returns false for an unknown descriptor (the
// caller should surface a structured error listing the known descriptors). Synonyms are folded;
// laterally-paired descriptors accept an explicit `-left` / `-right` suffix, and a bare lateral name
// (e.g. "side") defaults to the LEFT (+az) per the CCW convention — documented, and always overridable
// by an explicit numeric az.
inline bool lookup(const std::string& descriptorRaw, Placement& out) {
    const std::string d = normalize(descriptorRaw);
    Placement p;

    // --- front / center ---
    if (d == "front-center" || d == "front-centre" || d == "center" || d == "centre" || d == "front") {
        p.az = 0.0; p.el = 0.0; p.width = Width::Narrow; out = p; return true;
    }
    if (d == "front-left") { p.az = 30.0;  p.width = Width::Narrow; out = p; return true; }
    if (d == "front-right"){ p.az = -30.0; p.width = Width::Narrow; out = p; return true; }

    // --- wide (uses the source's own azimuth, widened) ---
    if (d == "wide") { p.usesSourceAz = true; p.width = Width::Large; out = p; return true; }

    // --- side / surround (±105°) ---
    if (d == "side-left"  || d == "surround-left")  { p.az = 105.0;  p.width = Width::Med; out = p; return true; }
    if (d == "side-right" || d == "surround-right") { p.az = -105.0; p.width = Width::Med; out = p; return true; }
    if (d == "side" || d == "surround")             { p.az = 105.0;  p.width = Width::Med; out = p; return true; }

    // --- rear / back (±135°) ---
    if (d == "rear-left"  || d == "back-left")  { p.az = 135.0;  p.width = Width::Med; out = p; return true; }
    if (d == "rear-right" || d == "back-right") { p.az = -135.0; p.width = Width::Med; out = p; return true; }
    if (d == "rear" || d == "back")             { p.az = 135.0;  p.width = Width::Med; out = p; return true; }

    // --- overhead / height (source az, +45° el) ---
    if (d == "overhead" || d == "height") {
        p.usesSourceAz = true; p.el = 45.0; p.width = Width::Med; out = p; return true;
    }

    // --- top / zenith (single overhead, +90° el) ---
    if (d == "top" || d == "zenith" || d == "voice-of-god") {
        p.az = 0.0; p.el = 90.0; p.width = Width::Narrow; out = p; return true;
    }

    // --- lfe / sub ---
    if (d == "lfe" || d == "sub" || d == "subwoofer") { p.lfe = true; out = p; return true; }

    // --- diffuse / ambient (decorrelated bed) ---
    if (d == "diffuse" || d == "ambient" || d == "bed") {
        p.diffuse = true; p.width = Width::Max; out = p; return true;
    }

    return false;
}

// A comma-separated list of the canonical descriptors, for error remediation text.
inline const char* knownDescriptors() {
    return "front-center, front-left, front-right, wide, side[-left|-right], surround[-left|-right], "
           "rear[-left|-right], back[-left|-right], overhead, height, top, zenith, lfe, sub, "
           "diffuse, ambient";
}

}  // namespace placement
}  // namespace reaper_mcp
