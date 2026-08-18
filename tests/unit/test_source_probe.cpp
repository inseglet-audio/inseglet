// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_source_probe.cpp — unit test for the source-validity predicate (src/source_probe.h).
// No REAPER, no SDK.
//
// WHAT THIS PINS, AND WHY IT IS NOT DECORATION. Three documents and a version bump were spent
// measuring a null that our own code manufactured: `audio_accessor.h` substituted a plausible
// sample rate and channel count for the two fields REAPER zeroes on an unavailable source, so the
// accessor reported an IDENTICAL envelope across online -> offline -> online and the programme
// concluded the datum was not there. It was. A DEFENSIVE DEFAULT IS A
// DETECTOR-KILLER.
//
// This suite therefore carries TWO negative controls, because this predicate can be wrong in two
// directions and only one of them is loud:
//
//   1. THE OLD SHAPE — a probe read through the pre-fix fallbacks. It must report an unavailable
//      source as perfectly healthy. If this control ever stops firing, the fallbacks are back and
//      the fix has been silently reverted.
//   2. THE QN GUARD — the predicate WITHOUT its `!lengthIsQN` term. A MIDI take legitimately has
//      no sample rate and measures its length in quarter notes, so the unguarded predicate reports
//      every MIDI take in the project as a broken source. That is the expensive failure: a
//      detector that fires on ordinary healthy content gets switched off by its users.
//
// A control that can only agree is not a control, so both are asserted to FIRE.

#include <cstdio>
#include <string>

#include "source_probe.h"

using reaper_mcp::SourceProbe;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// The PRE-FIX read path, verbatim in its arithmetic: whatever the source said, a sub-1 width
// became 2 and a zero rate became the project rate. Kept ONLY as negative control 1.
struct LegacyRead {
    int channels = 0;
    int sampleRate = 0;
};
static LegacyRead legacyDefaults(const SourceProbe& p, int projectRate) {
    LegacyRead o;
    o.channels = p.channels;
    if (o.channels < 1) o.channels = 2;                       // if (chans < 1) chans = 2;
    o.sampleRate = p.sampleRate > 0 ? p.sampleRate : projectRate;  // srcRate > 0 ? srcRate : ...
    return o;
}

// The predicate WITHOUT its QN term. Negative control 2.
static bool unavailableNoQnGuard(const SourceProbe& p) {
    return p.present && (p.sampleRate <= 0 || p.channels < 1 || p.lengthSec <= 0.0);
}

int main() {
    // The d160 fixture's 12-channel bed, as measured live: 48 kHz, 6.0 s, 12 channels.
    SourceProbe live;
    live.present = true;
    live.channels = 12;
    live.sampleRate = 48000;
    live.lengthSec = 6.0;

    // The SAME source after action.run {command: 40100}, or after a deactivation edge:
    // REAPER zeroes the rate and the length. Measured live, 3/3 takes.
    SourceProbe offline;
    offline.present = true;
    offline.channels = 1;
    offline.sampleRate = 0;
    offline.lengthSec = 0.0;

    // An ordinary, healthy MIDI take: no sample rate, length in QUARTER NOTES.
    SourceProbe midi;
    midi.present = true;
    midi.channels = 0;
    midi.sampleRate = 0;
    midi.lengthSec = 4.0;
    midi.lengthIsQN = true;

    // No PCM_source at all — nothing was asked, so nothing is known.
    SourceProbe absent;

    std::fprintf(stderr, "\n-- 1. NEGATIVE CONTROL: the pre-fix defaults erase the signal --\n");
    {
        const LegacyRead a = legacyDefaults(live, 48000);
        const LegacyRead b = legacyDefaults(offline, 48000);
        check(a.sampleRate == b.sampleRate,
              "control 1: legacy rate is IDENTICAL online vs offline (48000 both) — the null");
        check(b.sampleRate == 48000 && offline.sampleRate == 0,
              "control 1: the legacy path reports 48000 where the source said 0");
        // NOT "and the width too": `if (chans < 1) chans = 2;` does not fire at 1, and the live
        // measured numChannels == 1 on the offline source. ONE papered-over field was enough.
        check(b.channels == 1 && offline.channels == 1,
              "control 1: the width is NOT defaulted at 1 — the RATE alone carried the null");
        SourceProbe zeroWidth = offline; zeroWidth.channels = 0;
        check(legacyDefaults(zeroWidth, 48000).channels == 2,
              "control 1: the width default DOES fire at 0 — a second erasure on other input");
        // The whole original finding, reproduced in four lines.
        check(a.channels != b.channels || a.sampleRate == b.sampleRate,
              "control 1 FIRES: the defect this fix exists to kill is reproduced here");
    }

    std::fprintf(stderr, "\n-- 2. THE PREDICATE'S TRUTH TABLE --\n");
    check(!live.unavailable(), "a live 48 kHz / 12 ch / 6.0 s source is AVAILABLE");
    check(offline.unavailable(), "an offline source (rate 0, length 0.0) is UNAVAILABLE");
    check(!midi.unavailable(), "a MIDI take (QN length, no rate) is NOT called unavailable");
    check(!absent.unavailable(), "no source at all is NOT called unavailable — nothing is claimed");

    std::fprintf(stderr, "\n-- 3. EACH TERM ALONE IS SUFFICIENT --\n");
    {
        SourceProbe rateOnly = live; rateOnly.sampleRate = 0;
        SourceProbe widthOnly = live; widthOnly.channels = 0;
        SourceProbe lenOnly = live; lenOnly.lengthSec = 0.0;
        check(rateOnly.unavailable(), "a zero sample rate alone is enough");
        check(widthOnly.unavailable(), "a sub-1 channel count alone is enough");
        check(lenOnly.unavailable(), "a zero length alone is enough");
    }

    std::fprintf(stderr, "\n-- 4. NEGATIVE CONTROL: dropping the QN guard breaks healthy MIDI --\n");
    {
        check(unavailableNoQnGuard(midi),
              "control 2 FIRES: without !lengthIsQN, an ordinary MIDI take reads UNAVAILABLE");
        check(!midi.unavailable(),
              "control 2: the shipped predicate does NOT — the guard is load-bearing");
        check(unavailableNoQnGuard(offline) && offline.unavailable(),
              "control 2: the guard does not weaken the real case — offline still fires");
    }

    std::fprintf(stderr, "\n-- 5. A QN SOURCE IS NEVER JUDGED, WHATEVER ELSE IT SAYS --\n");
    {
        SourceProbe qnZero = midi; qnZero.lengthSec = 0.0;
        check(!qnZero.unavailable(),
              "a QN source with a zero length is still not judged — the domain, not the value");
    }

    if (g_failures) {
        std::fprintf(stderr, "\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall source-probe checks passed\n");
    return 0;
}
