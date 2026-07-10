// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// audio_accessor.h — render-free sample reads into a meter::AudioBuffer via REAPER audio accessors.
//
// The metering suite (tools_analysis.cpp) normally fills a meter::AudioBuffer with a bounded,
// non-destructive temp render (render_stems.h). For direct, frame-accurate inspection of a track's
// item content or a take's source, an AudioAccessor streams decoded PCM with NO render round-trip.
// This header creates an accessor, reads a bounded window with GetAudioAccessorSamples (interleaved
// ReaSample = double), converts to the same interleaved float32 AudioBuffer the render path produces,
// and destroys the accessor — all within a single main-thread tools/call. Every existing analyzer in
// ambisonic_meter.h then runs on the result unchanged.
//
// SEMANTICS (REAPER's accessor model; the live gate scripts/verify_accessors.py checks this
// empirically):
//   * CreateTrackAudioAccessor reads the track's summed media-item/take content, BEFORE the track's
//     FX / volume / pan — it is NOT the post-FX bus signal (that stays analysis.meter's render path).
//   * CreateTakeAudioAccessor reads a single take's source through its take FX; its native channel
//     count and sample rate come from the take's PCM_source.
//
// Include order: pulls tool_helpers.h -> reaper_api.h BEFORE any STL/JSON header (SWELL min/max), then
// ambisonic_meter.h for meter::AudioBuffer. Everything is inline and SDK-gated; a host (no-SDK) build
// sees an empty header, so the tool layer's host-fallback path never references it.

#pragma once

#include "tools/tool_helpers.h"   // reaper_api.h (SWELL de-fang) + arg helpers + kCur
#include "ambisonic_meter.h"      // meter::AudioBuffer

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace reaper_mcp {

#ifdef REAPER_MCP_HAVE_SDK

// Result of a bounded, render-free accessor read.
struct AccessorRead {
    bool ok = false;
    std::string error, remediation;
    meter::AudioBuffer buf;       // interleaved float32 — identical shape to a rendered stem
    double accStart = 0.0;        // accessor's valid extent (seconds)
    double accEnd = 0.0;
    double readStart = 0.0;       // window actually read, clamped to the extent
    double readDur = 0.0;
    bool clamped = false;         // requested window was clipped to the extent (or the frame cap)
    bool silent = false;          // accessor reported no audio anywhere in the window
    bool stateChanged = false;    // accessor state was stale and revalidated before reading
};

// Cap on frames read in one call so a huge window can't stall the main-thread pump (~60 s at 48 kHz).
// The tool layer surfaces `clamped` when it bites.
inline size_t accessorMaxFrames() { return (size_t)48000 * 60; }

// Default read rate when the caller doesn't force one: the project sample rate (PROJECT_SRATE), or
// 48 kHz if the project doesn't pin a rate.
inline int projectSampleRateOr48k() {
    const int sr = (int)std::llround(GetSetProjectInfo(nullptr, "PROJECT_SRATE", 0.0, false));
    return sr > 0 ? sr : 48000;
}

// Read [startSec, startSec+durSec) (durSec<=0 => to the end of the extent) from an already-created
// accessor into out.buf at rate/chans. Chunked GetAudioAccessorSamples; clamps to [accStart,accEnd].
inline void readAccessorWindow(AudioAccessor* acc, int rate, int chans,
                               double startSec, double durSec, AccessorRead& out) {
    if (chans < 1) chans = 1;
    if (rate < 1) rate = 48000;
    out.accStart = GetAudioAccessorStartTime(acc);
    out.accEnd = GetAudioAccessorEndTime(acc);
    if (out.accEnd <= out.accStart) {
        out.error = "empty_source";
        out.remediation = "the accessor has no audio extent (empty item/take?)";
        return;
    }
    const double lo = std::min(std::max(startSec, out.accStart), out.accEnd);
    const double hi = (durSec <= 0.0) ? out.accEnd
                                      : std::min(std::max(startSec + durSec, out.accStart), out.accEnd);
    if (hi <= lo) {
        out.error = "empty_window";
        out.remediation = "requested window is outside the source extent [" +
                          std::to_string(out.accStart) + "," + std::to_string(out.accEnd) + "] s";
        return;
    }
    if (startSec < out.accStart - 1e-6) out.clamped = true;
    if (durSec > 0.0 && startSec + durSec > out.accEnd + 1e-6) out.clamped = true;

    size_t frames = (size_t)std::llround((hi - lo) * (double)rate);
    if (frames == 0) {
        out.error = "empty_window";
        out.remediation = "window rounds to zero frames — widen duration or raise sampleRate";
        return;
    }
    if (frames > accessorMaxFrames()) { frames = accessorMaxFrames(); out.clamped = true; }

    out.readStart = lo;
    out.readDur = (double)frames / (double)rate;
    out.buf.channels = chans;
    out.buf.sampleRate = (double)rate;
    out.buf.frames = frames;
    out.buf.samples.assign(frames * (size_t)chans, 0.0f);

    const size_t chunk = 65536;                       // frames per GetAudioAccessorSamples call
    std::vector<double> tmp(chunk * (size_t)chans);   // ReaSample (double), interleaved
    bool anyAudio = false;
    for (size_t f0 = 0; f0 < frames; f0 += chunk) {
        const size_t n = std::min(chunk, frames - f0);
        const double t = lo + (double)f0 / (double)rate;
        const int got = GetAudioAccessorSamples(acc, rate, chans, t, (int)n, tmp.data());
        if (got > 0) anyAudio = true;                 // 0 => silence (buffer already zeroed); <0 => error
        const size_t base = f0 * (size_t)chans;
        const size_t cnt = n * (size_t)chans;
        for (size_t i = 0; i < cnt; ++i) out.buf.samples[base + i] = (float)tmp[i];
    }
    out.silent = !anyAudio;
    out.ok = true;
}

// Read a TRACK's item/take content (pre track-FX/volume/pan). rate<=0 => project rate.
inline AccessorRead readTrackContent(int trackIdx, int rate, double startSec, double durSec) {
    AccessorRead r;
    MediaTrack* t = GetTrack(kCur, trackIdx);
    if (!t) {
        r.error = "target_not_found";
        r.remediation = "pass a valid track index or name";
        return r;
    }
    int chans = (int)GetMediaTrackInfo_Value(t, "I_NCHAN");
    if (chans < 1) chans = 2;
    AudioAccessor* acc = CreateTrackAudioAccessor(t);
    if (!acc) {
        r.error = "accessor_failed";
        r.remediation = "REAPER could not create a track audio accessor";
        return r;
    }
    if (AudioAccessorStateChanged(acc)) { AudioAccessorValidateState(acc); r.stateChanged = true; }
    readAccessorWindow(acc, rate > 0 ? rate : projectSampleRateOr48k(), chans, startSec, durSec, r);
    DestroyAudioAccessor(acc);
    return r;
}

// Read a single TAKE's source (through take FX) at its native width/rate unless overridden.
inline AccessorRead readTakeContent(MediaItem_Take* tk, int rate, double startSec, double durSec) {
    AccessorRead r;
    if (!tk) {
        r.error = "take_not_found";
        r.remediation = "pass a valid itemIndex (and optional takeIndex)";
        return r;
    }
    PCM_source* src = GetMediaItemTake_Source(tk);
    int chans = src ? GetMediaSourceNumChannels(src) : 0;
    if (chans < 1) chans = 2;
    const int srcRate = src ? GetMediaSourceSampleRate(src) : 0;
    const int useRate = rate > 0 ? rate : (srcRate > 0 ? srcRate : projectSampleRateOr48k());
    AudioAccessor* acc = CreateTakeAudioAccessor(tk);
    if (!acc) {
        r.error = "accessor_failed";
        r.remediation = "REAPER could not create a take audio accessor (MIDI take?)";
        return r;
    }
    if (AudioAccessorStateChanged(acc)) { AudioAccessorValidateState(acc); r.stateChanged = true; }
    readAccessorWindow(acc, useRate, chans, startSec, durSec, r);
    DestroyAudioAccessor(acc);
    return r;
}

#endif  // REAPER_MCP_HAVE_SDK

}  // namespace reaper_mcp
