// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// fx_envelope.h — FX-parameter envelope addressing (iamf-docset doc 136). The SDK-free core
// behind envelope.ensure_fx_envelope, unit-tested without REAPER (unit.fx_envelope).
//
// Why this exists. `envelope.ensure_fx_envelope` wraps one SDK call —
// GetFXEnvelope(track, fx, param, /*create=*/true) — but the verb's real contract is not
// "create an envelope", it is "hand back a name that envelope.add_point will resolve to the
// envelope I just created". add_point finds its target with GetTrackEnvelopeByName, a lookup
// BY STRING. So two pure questions sit between the caller and a correct write, and both are
// answered here rather than inside the SDK path:
//
//   1. Which param did the caller mean? Callers name params ("Azimuth Angle"); REAPER
//      addresses them by index. Resolution has to be tiered, and — the point — it has to
//      REFUSE when the request does not identify one param, instead of picking.
//   2. Does the resulting envelope NAME identify one envelope on the track? If two FX on a
//      track each expose an identically-named param, GetTrackEnvelopeByName returns one of
//      them and a subsequent add_point writes to the wrong FX with ok:true.
//
// Both are the doc-125/126 failure shape: a scan that always yields *some* answer yields one
// for input that does not determine an answer, and the caller gets a successful write whose
// effect lands somewhere else. D3 hit it when a bare-letter scan drove "Quaternion X"; B4 hit
// it when an arg-max classified input outside its domain. The rule that falls out of all
// three: MEASURE WHETHER THE REQUEST DETERMINES THE ANSWER, AND REFUSE WHEN IT DOES NOT.
//
// Conventions: `roster` is the FX's param names in param-INDEX order, exactly as
// TrackFX_GetParamName reports them (original case — the tiers need it). Returned indices are
// param indices into that roster.

#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace reaper_mcp {
namespace fxenv {

// How a param name was matched, in strictly decreasing confidence. Tiers never mix: a tier
// that produces any candidate decides the outcome, so an exact match wins outright even when
// a substring match sits at a lower param index.
enum class MatchKind { None = 0, Exact, CaseInsensitive, Substring };

inline const char* matchKindName(MatchKind k) {
    switch (k) {
        case MatchKind::Exact:           return "exact";
        case MatchKind::CaseInsensitive: return "case-insensitive";
        case MatchKind::Substring:       return "substring";
        case MatchKind::None:            break;
    }
    return "none";
}

struct ParamMatch {
    int index = -1;                      // -1 when unresolved OR ambiguous — never a guess
    MatchKind kind = MatchKind::None;
    bool ambiguous = false;              // >1 candidate at the winning tier
    int candidateCount = 0;              // candidates at the winning tier (0 when kind==None)
    std::vector<int> candidates;         // their indices, so an error can name them
    bool resolved() const { return index >= 0; }
};

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Resolve a requested param name against a roster. Tiers are tried in order — exact (byte
// equality), then case-insensitive equality, then case-insensitive substring — and the FIRST
// tier with any candidate decides. Ambiguity inside the winning tier is REPORTED, never
// resolved: `index` stays -1 and `candidates` carries every match so the caller can say which
// ones it could not choose between.
//
// The tier rule matters on real rosters. An IEM encoder exposes both "Azimuth Angle" and
// "Elevation Angle": a request of "Angle" is genuinely ambiguous and a naive first-substring
// scan would silently return azimuth — a height write that rewrites azimuth, which is exactly
// the live defect D3 was built to kill. A request of "Elevation Angle" is exact and must win
// outright even though "Azimuth Angle" also contains "Angle" and sits at a lower index.
inline ParamMatch resolveParam(const std::vector<std::string>& roster, const std::string& request) {
    ParamMatch m;
    const std::string lreq = lower(request);

    for (int tier = 1; tier <= 3; ++tier) {
        std::vector<int> hits;
        for (int i = 0; i < static_cast<int>(roster.size()); ++i) {
            const std::string& nm = roster[static_cast<std::size_t>(i)];
            bool hit = false;
            if (tier == 1)      hit = (nm == request);
            else if (tier == 2) hit = (lower(nm) == lreq);
            else                hit = !lreq.empty() && lower(nm).find(lreq) != std::string::npos;
            if (hit) hits.push_back(i);
        }
        if (hits.empty()) continue;
        m.kind = (tier == 1) ? MatchKind::Exact
               : (tier == 2) ? MatchKind::CaseInsensitive
                             : MatchKind::Substring;
        m.candidateCount = static_cast<int>(hits.size());
        m.candidates = hits;
        m.ambiguous = hits.size() > 1;
        m.index = m.ambiguous ? -1 : hits.front();
        return m;
    }
    return m;  // None: index -1, candidateCount 0
}

// How many entries of `names` are byte-equal to `target`.
//
// GetTrackEnvelopeByName is a by-string lookup over the track's envelopes, so a count >= 2
// means the name does NOT identify an envelope and the envelope.add_point seam is UNSAFE for
// that name: add_point would resolve to whichever one REAPER returns first and report ok:true
// either way. The verb reports this rather than working around it, because there is no
// by-name workaround to offer — the caller has to address the other FX differently.
inline int nameCollisionCount(const std::vector<std::string>& names, const std::string& target) {
    int n = 0;
    for (const auto& s : names)
        if (s == target) ++n;
    return n;
}

// True when the by-name seam can be trusted for `target`: exactly one envelope carries it.
// Zero is also unsafe — it means the created envelope did not become name-visible, which is
// the E-136.3 branch, and the caller must not be told to go use add_point.
inline bool nameSeamSafe(const std::vector<std::string>& envNames, const std::string& target) {
    return nameCollisionCount(envNames, target) == 1;
}

}  // namespace fxenv
}  // namespace reaper_mcp
