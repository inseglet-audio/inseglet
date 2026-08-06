// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_adm_dolby_meta.cpp — pins the doc-141 `dolbyMetadataChunk` switch: the placeholder `dbmd`
// chunk, the RoomCentric* bed rename that RIDES it, the refusals, and the RIFF-size arithmetic.
//
// Why the chunk and the rename are ONE switch (measured from iamf-tools' own source, 2026-08-06):
// `dbmd`'s presence alone selects the importer's Dolby branch (bw64_reader.cc:200-203) and only
// that branch ever reads bed channel names (xml_to_adm.cc:511-516, CreatePackLayout's sole call
// site at :487). Room-centric names on a file with no `dbmd` are therefore unread — and still make
// a Dolby provenance claim. Coupling them is the correctness property; this file pins it.
//
// ⚠️ The load-bearing case is `riff_size_counts_dbmd`. docset 140 §7 wrote down, BEFORE the edit,
// that a new chunk must enter BOTH the projected-size sum that picks RIFF vs BW64 AND the append
// sequence. Adding it only to the append sequence is silent corruption near the 4 GiB boundary and
// would pass every other test here.
//
// No REAPER, no SDK — synthetic models only.

#include <cstdio>
#include <string>
#include <vector>

#include "adm_bwf.h"

using namespace reaper_mcp;
using namespace reaper_mcp::adm;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Count non-overlapping occurrences.
static size_t countOf(const std::string& hay, const std::string& needle) {
    size_t n = 0, p = 0;
    while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
    return n;
}

// A bed of `layout` plus `nobj` objects, mirroring tools_spatial.cpp's admBedSpeakers().
static Model makeModel(const std::string& layout, int nobj, int bits = 24, int rate = 48000) {
    static const std::vector<std::pair<std::string, std::vector<const char*>>> kBeds = {
        {"5.1",   {"L", "R", "C", "LFE", "Ls", "Rs"}},
        {"7.1",   {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs"}},
        {"7.1.2", {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs", "Ltf", "Rtf"}},
        {"7.1.4", {"L", "R", "C", "LFE", "Lss", "Rss", "Lrs", "Rrs", "Ltf", "Rtf", "Ltr", "Rtr"}},
        {"22.2",  {"FL", "FR", "FC", "LFE1", "BL", "BR"}},
    };
    Model m;
    m.bedLayoutName = layout;
    m.sampleRate = rate;
    m.bitDepth = bits;
    m.durationSec = 0.1;
    for (const auto& b : kBeds)
        if (b.first == layout)
            for (const char* l : b.second) {
                BedSpeaker s;
                s.label = l;
                const SpeakerPos p = speakerPosFor(layout, l);
                s.az = p.az; s.el = p.el; s.speakerLabel = p.speakerLabel;
                s.lfe = (s.label == "LFE" || s.label == "LFE1");
                m.bed.push_back(s);
            }
    for (int j = 0; j < nobj; ++j) {
        Object o; o.name = "Obj" + std::to_string(j + 1); o.objectNumber = j + 1;
        Block b; b.rtime = 0; b.duration = 0.1; b.az = 30.0 * (j + 1); b.gain = 1.0;
        o.blocks.push_back(b);
        m.objects.push_back(std::move(o));
    }
    return m;
}

static WriteResult writeOf(const Model& m, uint64_t threshold = 0xFFFFFFFFull) {
    const size_t frames = 128;
    std::vector<std::vector<float>> ch((size_t)m.channelCount(), std::vector<float>(frames, 0.1f));
    return writeAdmImage(m, ch, frames, threshold);
}

int main() {
    std::fprintf(stderr, "== doc 141: dolbyMetadataChunk ==\n");

    // ---- 1. DEFAULT is inert: no `dbmd`, plain bed names ------------------------------------
    {
        Model m = makeModel("5.1", 2);
        check(m.dolbyMetadataChunk == DolbyMetadata::None, "Model defaults to DolbyMetadata::None");
        WriteResult r = writeOf(m);
        check(r.ok, "default model writes");
        check(!contains(r.bytes.substr(0, 4096), "dbmd"), "default output carries NO dbmd chunk");
        const std::string ax = buildAxml(m);
        check(contains(ax, "audioChannelFormatName=\"L\""), "default bed name stays \"L\"");
        check(!contains(ax, "RoomCentric"), "default axml has NO RoomCentric names");
    }

    // ---- 2. Placeholder 5.1: dbmd present, names renamed, 1:1 --------------------------------
    {
        Model m = makeModel("5.1", 2);
        m.dolbyMetadataChunk = DolbyMetadata::Placeholder;
        WriteResult r = writeOf(m);
        check(r.ok, "placeholder 5.1 writes");
        check(contains(r.bytes, "dbmd"), "placeholder output carries a dbmd chunk");
        check(buildDbmdPlaceholder().size() == 32, "dbmd placeholder payload is 32 bytes");
        // the corpus generator's version word: little-endian 0x01000000
        const std::string p = buildDbmdPlaceholder();
        check(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1, "dbmd version word matches the corpus");
        const std::string ax = buildAxml(m);
        check(contains(ax, "audioChannelFormatName=\"RoomCentricLeft\""),   "L  -> RoomCentricLeft");
        check(contains(ax, "audioChannelFormatName=\"RoomCentricLFE\""),    "LFE -> RoomCentricLFE");
        check(contains(ax, "audioChannelFormatName=\"RoomCentricLeftSurround\""),
              "5.1's Ls -> RoomCentricLeftSurround (not SideSurround)");
        check(!contains(ax, "audioChannelFormatName=\"L\""), "no plain bed label survives the rename");
        check(countOf(ax, "RoomCentric") == 6, "exactly 6 bed names renamed, no more");
        // the OBJECT channel names must NOT be touched — only the bed rides the switch
        check(contains(ax, "audioChannelFormatName=\"Obj1\""), "object names are untouched");
    }

    // ---- 3. 7.1.2's height pair: the one mapping that is a JUDGEMENT, not a transcription -----
    {
        Model m = makeModel("7.1.2", 1);
        m.dolbyMetadataChunk = DolbyMetadata::Placeholder;
        check(writeOf(m).ok, "placeholder 7.1.2 writes");
        const std::string ax = buildAxml(m);
        check(contains(ax, "audioChannelFormatName=\"RoomCentricLeftTopSurround\""),
              "Ltf -> RoomCentricLeftTopSurround (top FRONT written as top SURROUND)");
        check(contains(ax, "audioChannelFormatName=\"RoomCentricRightTopSurround\""),
              "Rtf -> RoomCentricRightTopSurround");
        check(dolbyPackLayoutOf(m) == "L,R,C,LFE,Lss,Rss,Lrs,Rrs,Lts,Rts",
              "7.1.2 maps to the 10-channel allow-list entry, IN ORDER");
    }

    // ---- 4. C4: the pack-layout allow-list, and the near-miss that proves it is not a set test -
    {
        check(dolbyPackLayoutAllowed("L,R,C,LFE,Ls,Rs"), "5.1 layout is on the allow-list");
        check(dolbyPackLayoutAllowed("L,R,C,LFE,Lss,Rss,Lrs,Rrs"), "7.1 layout is on the allow-list");
        check(dolbyPackLayoutAllowed("L,R,C,LFE,Lss,Rss,Lrs,Rrs,Lts,Rts"), "7.1.2 layout is allowed");
        // Every NAME here is inside the twelve and the count is under the 10-channel cap, but the
        // LAYOUT is not on the list. Measured against iamf-tools 2026-08-06: it answers
        // "Invalid pack layout= L,R,C,LFE,Lss,Rss". Set membership is necessary, NOT sufficient.
        check(!dolbyPackLayoutAllowed("L,R,C,LFE,Lss,Rss"), "a legal-name layout OFF the list is refused");
        check(!dolbyPackLayoutAllowed("LFE,L,R,C,Ls,Rs"), "order matters — a permutation is refused");
        check(dolbyBedName("Ltr") == nullptr, "Ltr has no room-centric name");
        check(dolbyBedName("Lw") == nullptr, "Lw has no room-centric name");
        check(dolbyBedName("Ls") != nullptr && std::string(dolbyBedName("Ls")->code) == "Ls",
              "Ls maps to itself");
    }

    // ---- 5. Refusals — fail closed, before a byte is written ---------------------------------
    {
        struct Case { const char* what; Model m; };
        Model m16 = makeModel("5.1", 1, 16);    m16.dolbyMetadataChunk = DolbyMetadata::Placeholder;
        Model m32 = makeModel("5.1", 1, 32);    m32.dolbyMetadataChunk = DolbyMetadata::Placeholder;
        Model m44 = makeModel("5.1", 1, 24, 44100);
        m44.dolbyMetadataChunk = DolbyMetadata::Placeholder;
        Model m96 = makeModel("5.1", 1, 24, 96000);
        m96.dolbyMetadataChunk = DolbyMetadata::Placeholder;
        Model m714 = makeModel("7.1.4", 1);     m714.dolbyMetadataChunk = DolbyMetadata::Placeholder;
        Model m222 = makeModel("22.2", 1);      m222.dolbyMetadataChunk = DolbyMetadata::Placeholder;

        check(!writeOf(m16).ok,  "16-bit + placeholder REFUSED (C1)");
        check(!writeOf(m32).ok,  "32-bit + placeholder REFUSED (C1)");
        check(!writeOf(m44).ok,  "44.1 kHz + placeholder REFUSED (C2)");
        check(writeOf(m96).ok,   "96 kHz + placeholder ACCEPTED (C2 allows 48k and 96k)");
        check(!writeOf(m714).ok, "7.1.4 + placeholder REFUSED (C4)");
        check(!writeOf(m222).ok, "22.2 + placeholder REFUSED (C4)");
        // the refusal must NAME the remedy, not merely fail
        check(contains(writeOf(m16).error, "24-bit"), "the 16-bit refusal names the required depth");
        check(contains(writeOf(m714).error, "7.1.2"), "the 7.1.4 refusal names the layouts that work");
        // and the SAME models must all write cleanly with the switch off
        m16.dolbyMetadataChunk = DolbyMetadata::None;
        m714.dolbyMetadataChunk = DolbyMetadata::None;
        m222.dolbyMetadataChunk = DolbyMetadata::None;
        check(writeOf(m16).ok && writeOf(m714).ok && writeOf(m222).ok,
              "every refused model writes fine with dolbyMetadataChunk None");
    }

    // ---- 6. ⚠️ THE TRAP (docset 140 §7): the dbmd MUST enter the projected-size sum -----------
    // ⚠️ ISOLATION MATTERS HERE. A BEDDED model is the wrong probe: the switch also renames the bed,
    // and "RoomCentricLeftSideSurround" is far longer than "Lss", so the axml grows too and a
    // container flip could be caused by either. An OBJECTS-ONLY model has no bed to rename, so the
    // 40-byte chunk is the ONLY thing that differs between the two writes — which is what lets the
    // flip below be attributed to it. (Objects-only + placeholder is legal: with no DirectSpeakers
    // pack there is no pack layout to validate.)
    {
        Model none = makeModel("", 3);   // no bed => no rename => identical axml both sides
        Model dbmd = makeModel("", 3);
        dbmd.dolbyMetadataChunk = DolbyMetadata::Placeholder;
        check(none.bed.empty() && dbmd.bed.empty(), "the probe really is objects-only");
        check(buildAxml(none) == buildAxml(dbmd),
              "with no bed, the switch changes the axml NOT AT ALL — so the size delta is the chunk");

        const size_t sNone = writeOf(none).bytes.size();
        const size_t sDbmd = writeOf(dbmd).bytes.size();
        check(sDbmd == sNone + 40, "the dbmd adds exactly 40 bytes (8 header + 32 payload)");

        // Put the threshold BETWEEN the two file sizes. The no-dbmd model must stay RIFF; the
        // dbmd model must flip to BW64 — which it can only do if its 40 bytes were counted in
        // `projected`. If the chunk were appended but not counted, this model would compute the
        // same projected size as `none` and wrongly stay RIFF.
        const uint64_t threshold = (uint64_t)sNone + 20;
        check(!writeOf(none, threshold).bw64, "under the threshold: no-dbmd model stays RIFF");
        check(writeOf(dbmd, threshold).bw64,
              "over the threshold ONLY because of the dbmd: model flips to BW64 "
              "(if this fails, the chunk is missing from the projected-size sum)");
        // and the flipped file must still be structurally sane
        WriteResult big = writeOf(dbmd, threshold);
        check(big.ok && big.bytes.compare(0, 4, "BW64") == 0, "the BW64 form is actually emitted");
        check(contains(big.bytes, "dbmd"), "the dbmd survives into the BW64 form");
    }

    // ---- 7. The runtime advisory fires precisely ----------------------------------------------
    {
        Model withObj = makeModel("5.1", 2);
        Model bedOnly = makeModel("5.1", 0);
        Model dolby   = makeModel("5.1", 2);
        dolby.dolbyMetadataChunk = DolbyMetadata::Placeholder;

        ParseResult pObj = parseAdmImage(writeOf(withObj).bytes);
        ParseResult pBed = parseAdmImage(writeOf(bedOnly).bytes);
        ParseResult pDlb = parseAdmImage(writeOf(dolby).bytes);
        check(pObj.ok && pBed.ok && pDlb.ok, "all three parse");

        check(!dolbyIngestAdvisory(pObj.summary).empty(), "objects + no dbmd => advisory FIRES");
        check(dolbyIngestAdvisory(pBed.summary).empty(),  "bed-only => advisory silent");
        check(dolbyIngestAdvisory(pDlb.summary).empty(),  "dbmd present => advisory silent");
        // the message must carry the date it was measured — it is a claim about someone else's code
        check(contains(dolbyIngestAdvisory(pObj.summary), "2026-08-06"),
              "the advisory carries its measurement date");
        check(contains(dolbyIngestAdvisory(pObj.summary), "dolbyMetadataChunk"),
              "the advisory names the knob");
        check(dolbyIngestAdvisory(Json(nullptr)).empty(), "a non-object summary is handled");
    }

    if (g_failures) {
        std::fprintf(stderr, "\n%d DOLBY-META TEST(S) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nALL DOLBY-META TESTS PASSED\n");
    return 0;
}
