// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// loom_manifest.h — the Inseglet -> iamf-loom bridge (spatial.export_loom_manifest).
//
// iamf-loom (github.com/jlivingston-Cipher/iamf-loom, Apache-2.0) is a manifest-driven IAMF
// packager: a ~13-line YAML manifest + source WAVs in, measured-loudness IAMF/MP4/preview
// deliverables out, every output gated by the iamf-sentinel validator. This header emits that
// manifest (schema `loom: 0`) plus the source WAVs in Loom's expected channel order, from
// buffers the tool renders with the existing render_stems.h machinery. The result is the
// cheapest REAPER -> IAMF story there is: one tool call, then `loom compile manifest.yaml`.
//
// DESIGN RULES (all load-bearing):
//   * NEVER emit a loudness value. Loom MEASURES loudness (that is the point of it); a typed
//     number would be a lie waiting to happen. `policy.loudness.normalize` appears ONLY when
//     the caller explicitly asked for normalization, and then always as mode: measure + the
//     caller's number.
//   * The emitted manifest is SCHEMA-SHAPED for `loom: 0`; `loom compile` is the authority.
//     This header never re-implements Loom's validator (no M-codes here — the §7 non-goal).
//     It only refuses to emit the handful of combinations Loom is KNOWN to reject structurally
//     (each refusal names the Loom code the manifest would have died with).
//   * Objects are OUT of v1 — they ride the ADM route (`kind: adm`, Loom M-309) when it lands;
//     spatial.export_adm is today's object-audio master. The tool says so; it never folds
//     objects into a bed silently.
//   * Beds stereo / 5.1 / 7.1.4 and ambisonics orders 1–4 (ACN/SN3D — Inseglet's native scene
//     convention) — exactly Loom's Phase-1 source set.
//
// ============================================================================================
// CHANNEL-ORDER MAPPING (the load-bearing core — verified, do not edit without re-running the
// identity-tone pin, tests/unit/test_loom_manifest.cpp E-116.1/E-116.2):
//
//   Loom's bed WAV order (loom/layouts.py, `order: bs2051`) vs Inseglet's bed order
//   (tools_analysis.cpp bedChannelLabels / tools_spatial.cpp bedLayouts, SMPTE film order):
//
//     idx     0  1  2  3    4    5    6    7    8    9    10   11
//     Loom    L  R  C  LFE  Lss  Rss  Lrs  Rrs  Ltf  Rtf  Ltb  Rtb   (7.1.4)
//     Inseglet L R  C  LFE  Lss  Rss  Lsr  Rsr  Ltf  Rtf  Ltr  Rtr   (7.1.4)
//
//   SAME PHYSICAL ORDER AT EVERY INDEX — the only deltas are label SPELLING at 6/7
//   (rear-surround: "Lsr/Rsr" vs "Lrs/Rrs"; tools_spatial.cpp's own bedLayouts() already
//   spells them Lrs/Rrs) and 10/11 (top-rear "Ltr/Rtr" vs top-back "Ltb/Rtb" — the same
//   speakers, BS.2051 System J U+135). 5.1 and stereo are spelling-identical. The bridge
//   therefore performs NO channel permutation: rendered buffer index k -> WAV channel k,
//   IDENTITY. Verified empirically 2026-08-04 with per-channel identifier tones
//   (313 + 139·k Hz, the iamf-adm-corpus plan): 12/12, 6/6, 2/2 channels identity-mapped
//   through the writer, off-diagonal rejection > 60 dB (measured ~130 dB class).
//   Ambisonics: ACN index k -> WAV channel k, first (order+1)^2 channels of the scene bus
//   (REAPER's even-channel padding is dropped at write).
// ============================================================================================
//
// SDK-free (no REAPER). Pulls adm_bwf.h for Json / makeError / putLE* / quantizePcm / fmtNum
// (SWELL-safe include order). Host-unit-tested (tests/unit/test_loom_manifest.cpp) with no DAW.
//
// HONESTY CAVEAT (house style, same footing as adm_bwf.h/damf.h): the output is manifest-
// SHAPED and gate-checked against a real `loom compile` in this repo's evidence run; whether
// YOUR loom/toolchain install accepts a specific manifest is `loom compile`'s verdict — the
// tool echoes the exact command to run next.

#pragma once

#include "adm_bwf.h"  // Json / makeError / putLE16/putLE32 / quantizePcm / fmtNum / xmlEscape

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace reaper_mcp {
namespace loomb {

// ============================================================================================
// Layouts — Loom's Phase-1 bed set, with the per-index Inseglet<->Loom label pin.
// ============================================================================================
struct ChanPair { const char* inseglet; const char* loom; };
struct LoomBed {
    const char* name;          // Loom manifest `layout:` string
    int channels;
    std::vector<ChanPair> map; // size == channels; index == WAV channel == Inseglet bed index
};

inline const std::vector<LoomBed>& loomBeds() {
    static const std::vector<LoomBed> kBeds = {
        {"stereo", 2, {{"L", "L"}, {"R", "R"}}},
        {"5.1", 6,
         {{"L", "L"}, {"R", "R"}, {"C", "C"}, {"LFE", "LFE"}, {"Ls", "Ls"}, {"Rs", "Rs"}}},
        {"7.1.4", 12,
         {{"L", "L"}, {"R", "R"}, {"C", "C"}, {"LFE", "LFE"},
          {"Lss", "Lss"}, {"Rss", "Rss"},
          {"Lsr", "Lrs"}, {"Rsr", "Rrs"},      // rear surrounds: spelling only (see header pin)
          {"Ltf", "Ltf"}, {"Rtf", "Rtf"},
          {"Ltr", "Ltb"}, {"Rtr", "Rtb"}}},    // top rear == top back: spelling only
    };
    return kBeds;
}

inline const LoomBed* findLoomBed(const std::string& name) {
    for (const auto& b : loomBeds())
        if (name == b.name) return &b;
    return nullptr;
}

inline int loomMaxAmbiOrder() { return 4; }  // Loom Phase-1 cap (orders 1..4, ACN/SN3D)

// ============================================================================================
// Identifier + name hygiene.
// ============================================================================================
// Loom requires [A-Za-z0-9_-]+ identifiers (M-207). Slug anything else to '_'.
inline std::string slugIdent(const std::string& s, const std::string& fallback) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        out.push_back(ok ? c : '_');
    }
    // strip to nothing => fallback
    bool any = false;
    for (char c : out) if (c != '_') { any = true; break; }
    return (out.empty() || !any) ? fallback : out;
}

// The S-208 trap (template-shaped names): a title/annotation lifted from a default track name
// ("Track 3", "Object 12", empty) ships a placeholder into deliverable metadata. Warn upstream.
inline bool defaultShapedName(const std::string& s) {
    if (s.empty()) return true;
    size_t i = 0;
    auto eat = [&](const char* w) {
        size_t n = 0;
        while (w[n]) ++n;
        if (s.size() < i + n) return false;
        for (size_t k = 0; k < n; ++k)
            if (std::tolower((unsigned char)s[i + k]) != std::tolower((unsigned char)w[k]))
                return false;
        i += n;
        return true;
    };
    if (!(eat("track") || eat("object") || eat("audio"))) return false;
    if (i < s.size() && s[i] == ' ') ++i;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i)
        if (!std::isdigit((unsigned char)s[i])) return false;
    return true;
}

// YAML double-quoted scalar (titles/labels can carry anything a track name can).
inline std::string yamlQuote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n') out += "\\n";
        else out.push_back(c);
    }
    out += "\"";
    return out;
}

// Path scalar for FLOW mappings: `{`/`}` (the {episode} template!), spaces, and other YAML flow
// syntax break an unquoted scalar inside `{ ... }` — Loom's own fixtures quote templated strings
// ("main-{lang}"). Quote unless every char is flow-safe. (Found live at the acceptance gate: an
// unquoted dist/{episode}.iamf is an M-102 YAML parse error.)
inline std::string yamlPathScalar(const std::string& s) {
    bool safe = !s.empty();
    for (char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
        if (!ok) { safe = false; break; }
    }
    return safe ? s : yamlQuote(s);
}

// ============================================================================================
// Manifest model — exactly what the emitter needs, nothing more. The tool builds this from
// the session; the unit tests build it synthetically.
// ============================================================================================
struct BridgeSource {
    std::string name;    // manifest identifier (M-207-safe; the tool uses main / scene / vo_*)
    std::string path;    // manifest-relative WAV path (e.g. "wavs/main.wav")
    std::string kind;    // "bed" | "ambisonics"
    std::string layout;  // bed only: "stereo" | "5.1" | "7.1.4"
};

struct BridgeLanguage {
    std::string lang;    // BCP-47-ish tag the caller supplies, e.g. "en-us"
    std::string vo;      // source/element name of that language's VO bed (stereo)
    std::string label;   // human label for the annotation
};

struct BridgeTarget {
    std::string format;  // "iamf" | "mp4" | "preview"
    std::string out;     // output path (may carry {episode})
    std::string video;   // mp4 only, optional
    std::string preset;  // "" | "youtube" | "archive"
};

struct BridgeModel {
    std::string title;
    std::vector<BridgeSource> sources;    // beds/scene first, then VO beds (emission order)
    std::vector<BridgeLanguage> languages;  // non-empty => languages: expansion block
    std::vector<BridgeTarget> targets;
    std::string codec;         // "" (Loom default opus) | "opus" | "flac" | "lpcm"
    bool haveNormalize = false;
    double normalize = 0.0;    // LUFS, only when haveNormalize
    bool binaural = false;     // declare headphones: binaural on the main presentation elements
};

// ============================================================================================
// manifest.yaml emitter. Deterministic: same model -> same bytes (golden-pinned).
// ============================================================================================
inline std::string writeManifestYaml(const BridgeModel& m) {
    std::string y;
    y.reserve(1024);
    y += "loom: 0\n";
    y += "title: " + yamlQuote(m.title) + "\n";

    y += "sources:\n";
    for (const auto& s : m.sources) {
        y += "  " + s.name + ": { path: " + yamlPathScalar(s.path) + ", kind: " + s.kind;
        if (s.kind == "bed") y += ", layout: \"" + s.layout + "\"";
        y += " }\n";
    }

    y += "elements:\n";
    for (const auto& s : m.sources)
        y += "  " + s.name + ": { from: " + s.name + " }\n";

    // Presentation block. Non-VO sources are the program elements; VO elements enter via the
    // languages: rows ({vo} placeholder), one presentation per language, Loom-expanded.
    std::vector<const BridgeSource*> program;
    for (const auto& s : m.sources) {
        bool isVo = false;
        for (const auto& L : m.languages)
            if (L.vo == s.name) { isVo = true; break; }
        if (!isVo) program.push_back(&s);
    }
    const std::string hp = m.binaural ? std::string(", headphones: binaural") : std::string();
    y += "presentations:\n";
    if (!m.languages.empty()) {
        y += "  - id: \"main-{lang}\"\n";
        y += "    languages:\n";
        for (const auto& L : m.languages)
            y += "      - { lang: " + L.lang + ", vo: " + L.vo +
                 ", label: " + yamlQuote(L.label) + " }\n";
        y += "    elements:\n";
        for (const auto* s : program)
            y += "      - { ref: " + s->name + hp + " }\n";
        y += "      - { ref: \"{vo}\" }\n";
    } else {
        y += "  - id: main\n";
        y += "    annotations: { en-us: " + yamlQuote(m.title) + " }\n";
        y += "    elements:\n";
        for (const auto* s : program)
            y += "      - { ref: " + s->name + hp + " }\n";
    }

    // policy: only when there is something non-default to say. NEVER a loudness value —
    // mode stays `measure` (Loom measures; that is the point), normalize is the caller's.
    const bool wantPolicy = !m.codec.empty() || m.haveNormalize;
    if (wantPolicy) {
        y += "policy:\n";
        if (!m.codec.empty())
            y += "  codec: { name: " + m.codec + " }\n";
        if (m.haveNormalize)
            y += "  loudness: { mode: measure, normalize: " + adm::fmtNum(m.normalize) + " }\n";
    }

    y += "targets:\n";
    for (const auto& t : m.targets) {
        y += "  - { format: " + t.format + ", out: " + yamlPathScalar(t.out);
        if (!t.video.empty()) y += ", video: " + yamlPathScalar(t.video);
        if (!t.preset.empty()) y += ", preset: " + t.preset;
        y += " }\n";
    }
    return y;
}

// ============================================================================================
// season.yaml emitter (batch spec, `loom_batch: 0`) — one job per episode binding. The
// manifest's out paths carry {episode}; the batch engine compiles every job up front and
// dedups collisions (M-421) before any encoder runs.
// ============================================================================================
inline std::string writeSeasonYaml(const std::string& manifestName,
                                   const std::vector<std::string>& episodes) {
    std::string y;
    y += "loom_batch: 0\n";
    y += "manifest: " + yamlPathScalar(manifestName) + "\n";
    y += "jobs:\n";
    for (const auto& ep : episodes)
        y += "  - { id: " + ep + ", vars: { episode: " + ep + " } }\n";
    return y;
}

// ============================================================================================
// RIFF WAV writer — integer PCM (16/24-bit), little-endian, plain fmt+data. Loom's source
// gate (M-308) wants 48 kHz / 16- or 24-bit INTEGER PCM; REAPER project render formats can
// be float, so the bridge always quantizes its own essence. Channel k of `channels` becomes
// interleaved WAV channel k — the IDENTITY mapping the header pin documents.
// ============================================================================================
inline std::string writeWavPcm(const std::vector<std::vector<float>>& channels, size_t frames,
                               int sampleRate, int bitDepth) {
    if (bitDepth != 16 && bitDepth != 24) bitDepth = 24;
    const int nch = (int)channels.size();
    const int bytesPerSample = bitDepth / 8;
    const uint32_t byteRate = (uint32_t)(sampleRate * nch * bytesPerSample);
    const uint16_t blockAlign = (uint16_t)(nch * bytesPerSample);

    std::string data = adm::quantizePcm(channels, frames, bitDepth);

    std::string fmt;
    adm::putLE16(fmt, 1);                    // wFormatTag = PCM
    adm::putLE16(fmt, (uint16_t)nch);
    adm::putLE32(fmt, (uint32_t)sampleRate);
    adm::putLE32(fmt, byteRate);
    adm::putLE16(fmt, blockAlign);
    adm::putLE16(fmt, (uint16_t)bitDepth);

    std::string f;
    f += "RIFF";
    adm::putLE32(f, (uint32_t)(4 + (8 + fmt.size()) + (8 + data.size() + (data.size() & 1))));
    f += "WAVE";
    f += "fmt ";
    adm::putLE32(f, (uint32_t)fmt.size());
    f += fmt;
    f += "data";
    adm::putLE32(f, (uint32_t)data.size());
    f += data;
    if (data.size() & 1) f.push_back('\0');  // RIFF word alignment
    return f;
}

}  // namespace loomb
}  // namespace reaper_mcp
