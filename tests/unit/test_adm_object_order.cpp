// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_adm_object_order.cpp — pins the doc-139 writer fix (a): EVERY <audioObject> is emitted
// before ANY <audioChannelFormat>.
//
// Why this is a correctness property and not a style preference (docset 138): iamf-tools'
// ADM importer sets its `gain` tag for any element named `gain` with no parent check, and
// registers no end-element handler, so `parent` is never reset. A per-block <gain> therefore
// latches that tag; if an <audioObject> then STARTS while it is latched, the next character
// data the importer sees — our indentation — is parsed as a gain and the file is refused with
// `INVALID_ARGUMENT: Failed to parse gain`. Emitting objects first removes the adjacency.
//
// The failure is a CONJUNCTION (order AND whitespace); this test pins the axis we control.
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

// Ordered scan of the element starts we care about. Returns the sequence, so a caller can ask
// both "is an audioObject preceded by a gain?" and "are the groups contiguous?".
static std::vector<std::string> elementSequence(const std::string& xml) {
    static const char* kTags[] = {"<audioObject ", "<audioPackFormat ", "<audioChannelFormat ",
                                  "<gain>"};
    static const char* kNames[] = {"audioObject", "audioPackFormat", "audioChannelFormat", "gain"};
    std::vector<std::string> seq;
    for (size_t i = 0; i < xml.size(); ++i) {
        if (xml[i] != '<') continue;
        for (int t = 0; t < 4; ++t) {
            const size_t n = std::string(kTags[t]).size();
            if (xml.compare(i, n, kTags[t]) == 0) { seq.push_back(kNames[t]); break; }
        }
    }
    return seq;
}

// THE defect predicate: does any audioObject start after a gain has been seen?
static bool hasObjectAfterGain(const std::vector<std::string>& seq) {
    bool seenGain = false;
    for (const auto& s : seq) {
        if (s == "gain") seenGain = true;
        else if (s == "audioObject" && seenGain) return true;
    }
    return false;
}

static Model makeModel() {
    Model m;
    m.bedLayoutName = "5.1";
    m.durationSec = 4.0;
    const char* labels[] = {"L", "R", "C", "LFE", "Ls", "Rs"};
    const char* spk[] = {"M+030", "M-030", "M+000", "LFE1", "M+110", "M-110"};
    for (int i = 0; i < 6; ++i) {
        BedSpeaker s;
        s.label = labels[i];
        s.speakerLabel = spk[i];
        s.lfe = (i == 3);
        m.bed.push_back(s);
    }
    for (int j = 0; j < 3; ++j) {  // >1 object: the interleaving needs a NEXT audioObject
        Object o;
        o.name = "Obj" + std::to_string(j + 1);
        o.objectNumber = j + 1;
        for (int k = 0; k < 2; ++k) {
            Block b;
            b.rtime = k * 2.0;
            b.duration = 2.0;
            b.az = 30.0 * (j + 1);
            b.gain = 1.0;   // the latch source
            o.blocks.push_back(b);
        }
        m.objects.push_back(o);
    }
    return m;
}

int main() {
    std::fprintf(stderr, "test_adm_object_order\n");
    const Model m = makeModel();
    const std::string xml = buildAxml(m);
    const std::vector<std::string> seq = elementSequence(xml);

    // --- the instrument can see a presence (directive 14): a hand-built interleaved document
    // in the shape the importer refuses MUST trip the predicate. Without this, a green result
    // below would be indistinguishable from a predicate that never fires.
    const std::string interleaved =
        "<audioObject id=\"A\"></audioObject>"
        "<audioChannelFormat id=\"C\"><audioBlockFormat><gain>1</gain></audioBlockFormat>"
        "</audioChannelFormat>"
        "<audioObject id=\"B\"></audioObject>";
    check(hasObjectAfterGain(elementSequence(interleaved)),
          "negative control: the predicate FIRES on a known-bad interleaved document");
    check(!hasObjectAfterGain(elementSequence(
              "<audioObject id=\"A\"></audioObject><audioObject id=\"B\"></audioObject>"
              "<audioChannelFormat id=\"C\"><gain>1</gain></audioChannelFormat>")),
          "negative control: the predicate is SILENT on a known-good ordered document");

    // --- the property under test
    check(!hasObjectAfterGain(seq), "no <audioObject> starts after a <gain>");

    // --- the groups are contiguous and in order: all objects, then packs, then channels
    int lastRank = -1; bool monotonic = true;
    for (const auto& s : seq) {
        if (s == "gain") continue;
        const int rank = (s == "audioObject") ? 0 : (s == "audioPackFormat") ? 1 : 2;
        if (rank < lastRank) { monotonic = false; break; }
        lastRank = rank;
    }
    check(monotonic, "element groups are audioObject* audioPackFormat* audioChannelFormat*");

    // --- the refactor moved bytes, it must not have dropped or duplicated any
    size_t nObj = 0, nPack = 0, nChan = 0, nGain = 0;
    for (const auto& s : seq) {
        if (s == "audioObject") ++nObj;
        else if (s == "audioPackFormat") ++nPack;
        else if (s == "audioChannelFormat") ++nChan;
        else ++nGain;
    }
    check(nObj == 1 + m.objects.size(), "one audioObject per object plus the bed");
    check(nPack == 1 + m.objects.size(), "one audioPackFormat per object plus the bed");
    check(nChan == m.bed.size() + m.objects.size(), "one audioChannelFormat per data channel");
    check(nGain == m.objects.size() * 2, "per-block <gain> still emitted for every object block");

    // --- every IDRef still resolves to a declaration that is present
    for (size_t j = 0; j < m.objects.size(); ++j) {
        const std::string apId = "AP_0003" + hex4(0x1001 + (unsigned)j);
        const std::string acId = "AC_0003" + hex4(0x1001 + (unsigned)j);
        check(xml.find("audioPackFormatID=\"" + apId + "\"") != std::string::npos &&
              xml.find("<audioPackFormatIDRef>" + apId) != std::string::npos,
              "object " + std::to_string(j) + ": pack id declared and referenced");
        check(xml.find("audioChannelFormatID=\"" + acId + "\"") != std::string::npos &&
              xml.find("<audioChannelFormatIDRef>" + acId) != std::string::npos,
              "object " + std::to_string(j) + ": channel id declared and referenced");
    }
    // the bed's trackUIDRefs must still be 1..bedN and the objects' must follow, unchanged by
    // the buffer split (the `uid` counter is threaded through the original single pass).
    check(xml.find("<audioTrackUIDRef>ATU_" + hex8(1) + "</audioTrackUIDRef>") != std::string::npos,
          "bed trackUIDRef numbering starts at 1");
    check(xml.find("<audioTrackUIDRef>ATU_" + hex8((int)m.bed.size() + 1) + "</audioTrackUIDRef>")
              != std::string::npos,
          "first object trackUIDRef follows the bed");

    std::fprintf(stderr, g_failures ? "FAILED (%d)\n" : "PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
