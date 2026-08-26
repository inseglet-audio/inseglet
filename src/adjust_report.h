// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// adjust_report.h — the shape a mutating tool uses to tell its caller that the input it was
// given is not the input it used.
//
// A tool that silently repairs its arguments and answers `ok: true` has told the caller
// nothing: the caller cannot tell "I asked for that and got it" from "I asked for something
// else and this is what you get instead". The declared inputSchema does not help, because
// nothing in this server validates a declared inputSchema — the bounds are a description,
// not a guard. So the RESULT has to carry the difference.
//
// The convention is not new here. It already shipped in `spatial.*` and in the accessor
// tools, and `track.set_channels` adopted it one tool at a time. This header only gives the
// SENTENCE one definition instead of fifteen.
//
// ⛔ THE ADJUSTMENT ITSELF STAYS INLINE IN EVERY HANDLER, ON PURPOSE, AND THIS IS NOT A
//    STYLE PREFERENCE. Our audit for silently-adjusting handlers reads source text and cannot
//    see an adjustment made in a helper. Folding these clamps into a shared function would
//    drive its count to zero while telling callers nothing at all, and the audit would read
//    clean. A repair that satisfies its own instrument by hiding from it is not a repair.

#pragma once

#include <string>
#include <vector>

namespace reaper_mcp {

// Compact, round-trippable rendering: 2 prints as "2", 0.25 as "0.25", never "2.000000".
inline std::string adjNum(double v) {
    if (v == static_cast<long long>(v) && v > -1e15 && v < 1e15)
        return std::to_string(static_cast<long long>(v));
    std::string s = std::to_string(v);
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

struct AdjustLog {
    std::vector<std::string> warnings;

    bool any() const { return !warnings.empty(); }

    // `asked` is always the value the CALLER supplied, never an intermediate — a value
    // adjusted twice reports the distance it actually travelled.
    void note(const char* field, double asked, double used, const char* why) {
        warnings.push_back(std::string("requested ") + field + " " + adjNum(asked) +
                           ", used " + adjNum(used) + " \xe2\x80\x94 " + why + ".");
    }

    // For a PER-ELEMENT clamp, which is a different animal from an input adjustment: the
    // caller's argument was honoured exactly, and N elements still could not be placed where
    // it implied.  Saying "requested X, used Y" here would be false — nothing they typed was
    // changed, so this class reports a COUNT instead.
    void pinned(int n, const char* what, const char* bound, const char* why) {
        warnings.push_back(std::to_string(n) + " " + what + (n == 1 ? " was" : " were") +
                           " pinned to " + bound + " \xe2\x80\x94 " + why + ".");
    }

    // For a read-after-write that disagrees with what was written: REAPER, not us.
    void reaper(const char* field, double wrote, double reports) {
        warnings.push_back(std::string("REAPER reports ") + field + " " + adjNum(reports) +
                           " where " + adjNum(wrote) + " was written.");
    }
};

}  // namespace reaper_mcp
