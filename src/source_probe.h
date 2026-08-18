// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// source_probe.h — the RAW source-validity reading behind every accessor read.
//
// WHY THIS IS ITS OWN HEADER. The predicate below decides whether REAPER's answer about a
// PCM_source means "this source is unreadable right now" or "this source is fine". Getting it
// wrong is expensive in BOTH directions, and the cost has already been paid once: three separate
// investigations measured a null on source validity before we found that `audio_accessor.h`'s own
// defensive fallbacks had substituted a plausible value for exactly the two fields that carry the
// signal — `GetMediaSourceSampleRate` returns 0 and `GetMediaSourceLength` returns 0.0 on an
// unavailable source, and we overwrote both before anyone could look.
//
//   A DEFENSIVE DEFAULT IS A DETECTOR-KILLER.
//
// So the predicate lives here, free of every REAPER symbol, and `tests/unit/test_source_probe.cpp`
// walks its whole truth table on a host build — including the negative control that REPRODUCES the
// defect (drop the QN guard and the detector fires on every MIDI take in the project).
// The REAPER-side reader that fills one of these in is `probeTakeSource()` in audio_accessor.h.

#pragma once

namespace reaper_mcp {

// RAW readings off one take's PCM_source, taken BEFORE any fallback can stand in for them.
struct SourceProbe {
    bool present = false;      // a PCM_source* was actually there to ask
    int channels = 0;          // GetMediaSourceNumChannels, verbatim
    int sampleRate = 0;        // GetMediaSourceSampleRate, verbatim
    double lengthSec = 0.0;    // GetMediaSourceLength, verbatim
    bool lengthIsQN = false;   // that length is in QUARTER NOTES, not seconds

    // An unopenable source reports a zero rate, a zero length and a sub-1 width; any ONE of those
    // is enough, because a readable audio source never reports a zero sample rate.
    //
    // The `!lengthIsQN` term is load-bearing, not defensive. A MIDI / tempo-domain source measures
    // its length in quarter notes and legitimately has NO sample rate, so without this term the
    // detector would report every MIDI take in the project as an unavailable source — a false
    // positive on ordinary, healthy content. `present` is the other guard: nothing asked is
    // reported as nothing known, never as "available".
    bool unavailable() const {
        return present && !lengthIsQN && (sampleRate <= 0 || channels < 1 || lengthSec <= 0.0);
    }
};

}  // namespace reaper_mcp
