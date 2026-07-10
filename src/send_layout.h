// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// send_layout.h — SDK-free object send-layout orchestration math (Batch L1).
//
// The bed + object tracks of an immersive session route into an external Dolby Atmos Renderer /
// DAPS input bus on a fixed 1-based input roster: the bed occupies renderer channels 1..bedW, then
// each mono object takes one subsequent channel (bed-first — the same roster the ADM/DAMF authoring
// emits). This header holds the pure logic: the roster, the REAPER send channel encode/decode
// (I_SRCCHAN / I_DSTCHAN bit layout, mirroring encodeSend in tools_spatial.cpp), the Dolby master
// constraint validator, the idempotent reconciliation diff (reuse/add/fix, strays reported), and the
// read-back inspect validator. No REAPER, no SDK — every function is unit-tested deterministically in
// tests/unit/test_send_layout.cpp and reused verbatim by spatial.orchestrate_sends (tools_spatial.cpp)
// and analysis.send_layout_inspect (tools_analysis.cpp).

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace reaper_mcp {
namespace send_layout {

// ------------------------------------------------------------------------------------------------
// Dolby Atmos master constraints (mirrors adm::profile::normalizeModel): a master carries at most
// 128 renderer channels and at most 118 objects; the canonical bed is 7.1.2 (10 ch).
// ------------------------------------------------------------------------------------------------
constexpr int kMaxRendererChannels = 128;
constexpr int kMaxObjects          = 118;
constexpr int kCanonicalBedWidth   = 10;  // 7.1.2

// ------------------------------------------------------------------------------------------------
// REAPER send channel encode/decode. I_SRCCHAN low 10 bits = source channel offset, bits >=10 = a
// width code (mono=1, stereo=0, N>=4 even -> N/2). I_DSTCHAN low 10 bits = dest channel offset,
// bit 10 (1024) = "mix to mono". A source raw of -1 means the send carries no audio (MIDI-only).
// ------------------------------------------------------------------------------------------------

// Channel-count -> width code. Returns -1 for an invalid width (odd and != 1, or < 1).
inline int widthToCode(int width) {
    if (width == 1) return 1;                              // mono
    if (width == 2) return 0;                              // stereo
    if (width >= 4 && (width % 2) == 0) return width / 2;  // 4->2, 6->3, 8->4, ...
    return -1;
}

// Width code -> channel count. (0->2 stereo, 1->1 mono, n>=2 -> 2n.)
inline int codeToWidth(int code) {
    if (code == 0) return 2;
    if (code == 1) return 1;
    return code * 2;
}

struct ChanEnc {
    int src = -1;        // I_SRCCHAN value (-1 = invalid/no-audio)
    int dst = -1;        // I_DSTCHAN value
    int widthCode = -1;  // resolved width code (-1 = invalid)
    bool valid = false;
};

// Encode a mono/multichannel send: source offset+width -> I_SRCCHAN, dest offset(+monoMix) -> I_DSTCHAN.
inline ChanEnc encodeSend(int srcOff, int width, int dstOff, bool monoMix) {
    ChanEnc e;
    const int code = widthToCode(width);
    if (code < 0 || srcOff < 0 || dstOff < 0) return e;  // invalid -> valid=false
    e.widthCode = code;
    e.src = (srcOff & 0x3FF) | (code << 10);
    e.dst = (dstOff & 0x3FF) | (monoMix ? 1024 : 0);
    e.valid = true;
    return e;
}

struct DecodedSend {
    bool audio = true;    // false when I_SRCCHAN == -1 (no audio / MIDI-only)
    int  srcOff = 0;      // source channel offset
    int  width = 0;       // channel count (0 when !audio)
    int  dstOff = 0;      // destination channel offset on the renderer bus
    bool monoMix = false; // dest "mix to mono" flag
};

// Decode the raw I_SRCCHAN / I_DSTCHAN a REAPER send/receive reports.
inline DecodedSend decodeSend(int srcRaw, int dstRaw) {
    DecodedSend d;
    if (srcRaw == -1) {
        d.audio = false;
        d.width = 0;
        d.srcOff = 0;
    } else {
        d.audio = true;
        d.srcOff = srcRaw & 0x3FF;
        d.width = codeToWidth((srcRaw >> 10) & 0x3F);
    }
    d.dstOff = dstRaw & 0x3FF;
    d.monoMix = (dstRaw & 1024) != 0;
    return d;
}

// ------------------------------------------------------------------------------------------------
// Roster: the canonical bed-first 1-based renderer input assignment.
// ------------------------------------------------------------------------------------------------
enum class Kind { Bed, Object };

struct RosterEntry {
    Kind kind = Kind::Object;
    int  sourceTrack = -1;   // 0-based REAPER track index
    int  objectNumber = 0;   // 1-based for objects; 0 for the bed
    int  rendererChan0 = 0;  // 0-based destination channel offset on the renderer bus
    int  width = 1;          // bed width for the bed; 1 for a (mono) object
};

// One 1-based input id for reporting (bed uses a range; object a single channel).
inline int inputId1(const RosterEntry& e) { return e.rendererChan0 + 1; }

// Build the roster: bed -> channels [0,bedW) (when bedTrack>=0 && bedW>0); object i -> channel bedW+i.
inline std::vector<RosterEntry> buildRoster(int bedTrack, int bedW,
                                            const std::vector<int>& objectTracks) {
    std::vector<RosterEntry> r;
    const bool haveBed = (bedTrack >= 0 && bedW > 0);
    if (haveBed)
        r.push_back(RosterEntry{Kind::Bed, bedTrack, 0, 0, bedW});
    const int base = haveBed ? bedW : 0;
    for (std::size_t i = 0; i < objectTracks.size(); ++i)
        r.push_back(RosterEntry{Kind::Object, objectTracks[i], (int)i + 1,
                                base + (int)i, 1});
    return r;
}

inline int rosterTotalChannels(int bedW, int objectCount, bool haveBed) {
    return (haveBed ? bedW : 0) + objectCount;
}

// ------------------------------------------------------------------------------------------------
// Constraint validation (fail-closed on fatal).
// ------------------------------------------------------------------------------------------------
struct Constraint {
    std::string code;
    std::string message;
    bool fatal = false;
};

inline std::vector<Constraint> checkConstraints(int bedW, int objectCount, bool haveBed) {
    std::vector<Constraint> v;
    if (objectCount > kMaxObjects)
        v.push_back({"over_118",
                     "object count " + std::to_string(objectCount) +
                     " exceeds the Dolby Atmos master limit of 118 objects", true});
    const int total = rosterTotalChannels(bedW, objectCount, haveBed);
    if (total > kMaxRendererChannels)
        v.push_back({"over_128",
                     "total renderer channels " + std::to_string(total) +
                     " exceed the Dolby Atmos master limit of 128", true});
    if (haveBed && bedW > kCanonicalBedWidth)
        v.push_back({"wide_bed",
                     "bed width " + std::to_string(bedW) +
                     " exceeds the 7.1.2 (10-ch) canonical Atmos bed; heights beyond 7.1.2 are "
                     "usually carried as objects", false});
    return v;
}

inline bool hasFatal(const std::vector<Constraint>& v) {
    for (const auto& c : v) if (c.fatal) return true;
    return false;
}

// ------------------------------------------------------------------------------------------------
// Reconciliation: match a desired roster against the renderer bus's existing receives so re-running
// is idempotent (no duplicate sends). reuse = a send already lands at the right channel + width;
// fix = a send from the right source lands at the wrong channel/width (re-point it); add = missing.
// Existing audio receives with no roster slot are strays (reported; removed only when prune=true).
// ------------------------------------------------------------------------------------------------
struct ExistingSend {
    int sendIndex = -1;   // receive index on the renderer bus (category -1)
    int sourceTrack = -1; // 0-based source track
    int srcOff = 0;
    int width = 0;
    int dstOff = 0;
    bool monoMix = false;
    bool audio = true;
};

enum class Action { Reuse, Add, Fix, Prune };

inline const char* actionName(Action a) {
    switch (a) {
        case Action::Reuse: return "reuse";
        case Action::Add:   return "add";
        case Action::Fix:   return "fix";
        case Action::Prune: return "prune";
    }
    return "add";
}

struct ReconEntry {
    Kind kind = Kind::Object;
    int  sourceTrack = -1;
    int  objectNumber = 0;
    int  rendererChan0 = 0;
    int  width = 1;
    Action action = Action::Add;
    int  sendIndex = -1;  // existing receive index for reuse/fix; -1 for add
};

struct StrayEntry {
    int sendIndex = -1;
    int sourceTrack = -1;
    int dstOff = 0;
    int width = 0;
};

struct ReconResult {
    std::vector<ReconEntry> entries;
    std::vector<StrayEntry> strays;
};

inline ReconResult reconcile(const std::vector<RosterEntry>& roster,
                             const std::vector<ExistingSend>& existing) {
    ReconResult out;
    std::vector<bool> used(existing.size(), false);
    for (const auto& e : roster) {
        int match = -1;
        // Prefer an exact match (same source, right channel + width).
        for (std::size_t i = 0; i < existing.size(); ++i) {
            if (used[i] || !existing[i].audio) continue;
            if (existing[i].sourceTrack == e.sourceTrack &&
                existing[i].dstOff == e.rendererChan0 && existing[i].width == e.width) {
                match = (int)i;
                break;
            }
        }
        // Else any audio send from the same source (it just needs its channels fixed).
        if (match < 0) {
            for (std::size_t i = 0; i < existing.size(); ++i) {
                if (used[i] || !existing[i].audio) continue;
                if (existing[i].sourceTrack == e.sourceTrack) { match = (int)i; break; }
            }
        }
        ReconEntry re{e.kind, e.sourceTrack, e.objectNumber, e.rendererChan0, e.width,
                      Action::Add, -1};
        if (match >= 0) {
            used[(std::size_t)match] = true;
            const ExistingSend& ex = existing[(std::size_t)match];
            re.sendIndex = ex.sendIndex;
            re.action = (ex.dstOff == e.rendererChan0 && ex.width == e.width)
                            ? Action::Reuse : Action::Fix;
        }
        out.entries.push_back(re);
    }
    // Leftover audio receives carry no roster slot -> strays.
    for (std::size_t i = 0; i < existing.size(); ++i) {
        if (used[i] || !existing[i].audio) continue;
        out.strays.push_back(StrayEntry{existing[i].sendIndex, existing[i].sourceTrack,
                                        existing[i].dstOff, existing[i].width});
    }
    return out;
}

// Count the reconciliation actions (for the report / idempotency check).
struct ReconCounts { int reuse = 0; int add = 0; int fix = 0; int stray = 0; };
inline ReconCounts countActions(const ReconResult& r) {
    ReconCounts c;
    for (const auto& e : r.entries) {
        if (e.action == Action::Reuse) ++c.reuse;
        else if (e.action == Action::Add) ++c.add;
        else if (e.action == Action::Fix) ++c.fix;
    }
    c.stray = (int)r.strays.size();
    return c;
}

// ------------------------------------------------------------------------------------------------
// Inspect: validate an existing renderer bus's incoming layout (read-only QC — the analysis tool).
// Reconstructs channel occupancy from the audio receives and flags collisions, gaps, over-limit,
// bed-width mismatch, wide (non-mono) objects, and (when an expected object count is declared) any
// unrouted objects.
// ------------------------------------------------------------------------------------------------
struct Issue {
    std::string code;
    std::string message;
};

struct InspectResult {
    bool ok = true;
    int  busChannels = 0;      // the renderer bus's declared channel count
    int  coveredChannels = 0;  // highest channel (1-based) any audio receive reaches
    int  objectChannels = 0;   // count of mono receives at/above the bed
    bool bedPresent = false;
    std::vector<Issue> issues;
};

// receives: the renderer bus's incoming audio sends. bedW: declared bed width (0 = undeclared).
// expectedObjects: declared object count (-1 = undeclared). busChannels: renderer I_NCHAN.
inline InspectResult inspect(const std::vector<ExistingSend>& receives, int bedW,
                             int expectedObjects, int busChannels) {
    InspectResult r;
    r.busChannels = busChannels;

    // Highest covered channel (1-based) across audio receives.
    int maxUsed = 0;
    for (const auto& s : receives) {
        if (!s.audio) continue;
        const int top = s.dstOff + std::max(1, s.width);
        if (top > maxUsed) maxUsed = top;
    }
    r.coveredChannels = maxUsed;

    // Channel occupancy (count of audio receives covering each channel).
    const int span = std::max(maxUsed, std::max(busChannels, bedW));
    std::vector<int> occ((std::size_t)std::max(0, span), 0);
    for (const auto& s : receives) {
        if (!s.audio) continue;
        for (int c = s.dstOff; c < s.dstOff + std::max(1, s.width); ++c)
            if (c >= 0 && c < (int)occ.size()) ++occ[(std::size_t)c];
    }

    // Collisions: any channel covered by more than one receive.
    for (int c = 0; c < (int)occ.size(); ++c)
        if (occ[(std::size_t)c] > 1)
            r.issues.push_back({"collision",
                "renderer channel " + std::to_string(c + 1) + " is fed by " +
                std::to_string(occ[(std::size_t)c]) + " sends"});

    // Gaps: an unused channel below the highest covered one.
    for (int c = 0; c < maxUsed; ++c)
        if (occ[(std::size_t)c] == 0)
            r.issues.push_back({"gap",
                "renderer channel " + std::to_string(c + 1) +
                " is unused but lies below the highest routed channel " + std::to_string(maxUsed)});

    // Bed presence / width (only when a bed width is declared).
    if (bedW > 0) {
        for (const auto& s : receives) {
            if (s.audio && s.dstOff == 0 && s.width == bedW) { r.bedPresent = true; break; }
        }
        if (!r.bedPresent) {
            bool wrongWidth = false;
            for (const auto& s : receives)
                if (s.audio && s.dstOff == 0 && s.width != 1) { wrongWidth = true; break; }
            r.issues.push_back({wrongWidth ? "bed_width_mismatch" : "bed_missing",
                wrongWidth ? "the bed send at channel 1 is not the declared width " + std::to_string(bedW)
                           : "no bed send covering channels 1.." + std::to_string(bedW) + " was found"});
        }
    }

    // Objects: mono receives at/above the bed (or all mono receives when no bed declared).
    const int objBase = (bedW > 0) ? bedW : 0;
    for (const auto& s : receives) {
        if (!s.audio) continue;
        if (s.dstOff < objBase) continue;
        if (s.width == 1) ++r.objectChannels;
        else
            r.issues.push_back({"object_width",
                "object send at renderer channel " + std::to_string(s.dstOff + 1) +
                " is " + std::to_string(s.width) + "-ch; objects must be mono"});
    }

    // Dolby master limits.
    if (r.objectChannels > kMaxObjects)
        r.issues.push_back({"over_118",
            "object count " + std::to_string(r.objectChannels) + " exceeds the 118-object limit"});
    if (maxUsed > kMaxRendererChannels)
        r.issues.push_back({"over_128",
            "highest routed channel " + std::to_string(maxUsed) + " exceeds the 128-channel limit"});

    // Unrouted objects (only when an expected object count is declared).
    if (expectedObjects >= 0 && r.objectChannels < expectedObjects)
        r.issues.push_back({"unrouted",
            std::to_string(expectedObjects - r.objectChannels) + " of " +
            std::to_string(expectedObjects) + " expected objects are not routed"});

    r.ok = r.issues.empty();
    return r;
}

}  // namespace send_layout
}  // namespace reaper_mcp
