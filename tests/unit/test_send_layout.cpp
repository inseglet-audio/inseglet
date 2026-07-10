// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_send_layout.cpp — pure unit test for the Batch L1 object send-layout math (src/send_layout.h):
// the send I_SRCCHAN/I_DSTCHAN encode/decode round-trip, the bed-first renderer input roster, the
// Dolby master constraint validator, the idempotent reconciliation diff, and the read-back inspect
// validator. No REAPER, no SDK — every number is deterministic.

#include <cstdio>
#include <string>
#include <vector>

#include "send_layout.h"

using namespace reaper_mcp::send_layout;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// Does the issue/constraint list contain a given code?
template <typename V>
static bool has(const V& v, const std::string& code) {
    for (const auto& x : v) if (x.code == code) return true;
    return false;
}

int main() {
    // ---- width <-> code ----
    check(widthToCode(1) == 1 && widthToCode(2) == 0 && widthToCode(10) == 5, "widthToCode mono/stereo/10ch");
    check(widthToCode(3) < 0 && widthToCode(0) < 0, "widthToCode rejects invalid widths");
    check(codeToWidth(1) == 1 && codeToWidth(0) == 2 && codeToWidth(5) == 10, "codeToWidth inverse");

    // ---- encode / decode round-trip ----
    ChanEnc bed = encodeSend(0, 10, 0, false);
    check(bed.valid && bed.src == (5 << 10) && bed.dst == 0, "encode 10ch bed at chan0");
    DecodedSend db = decodeSend(bed.src, bed.dst);
    check(db.audio && db.width == 10 && db.dstOff == 0 && !db.monoMix, "decode 10ch bed round-trips");

    ChanEnc obj = encodeSend(0, 1, 10, false);
    check(obj.valid && obj.src == 1024 && obj.dst == 10, "encode mono object at renderer chan 11");
    DecodedSend dobj = decodeSend(obj.src, obj.dst);
    check(dobj.audio && dobj.width == 1 && dobj.dstOff == 10, "decode mono object round-trips");

    ChanEnc st = encodeSend(2, 2, 4, false);
    DecodedSend dst = decodeSend(st.src, st.dst);
    check(st.valid && dst.width == 2 && dst.srcOff == 2 && dst.dstOff == 4, "stereo send src/dst offsets round-trip");

    ChanEnc mm = encodeSend(0, 1, 5, true);
    DecodedSend dmm = decodeSend(mm.src, mm.dst);
    check(dmm.monoMix && dmm.dstOff == 5, "monoMix flag round-trips");

    check(!encodeSend(0, 3, 0, false).valid, "encode rejects a 3-ch width");
    DecodedSend none = decodeSend(-1, 0);
    check(!none.audio && none.width == 0, "decode -1 srcchan = no audio");

    // ---- roster ----
    std::vector<int> objs = {2, 3, 4};
    auto roster = buildRoster(5, 10, objs);
    check(roster.size() == 4, "roster = bed + 3 objects");
    check(roster[0].kind == Kind::Bed && roster[0].rendererChan0 == 0 && roster[0].width == 10, "bed occupies chan 1..10");
    check(roster[1].kind == Kind::Object && roster[1].objectNumber == 1 && roster[1].rendererChan0 == 10, "object 1 -> renderer chan 11");
    check(roster[3].rendererChan0 == 12 && inputId1(roster[3]) == 13, "object 3 -> renderer chan 13 (1-based)");
    auto noBed = buildRoster(-1, 0, objs);
    check(noBed.size() == 3 && noBed[0].rendererChan0 == 0, "no-bed roster starts objects at chan 1");

    // ---- constraints ----
    check(checkConstraints(10, 3, true).empty(), "10-ch bed + 3 objects is within spec");
    auto c118 = checkConstraints(10, 200, true);
    check(has(c118, "over_118") && has(c118, "over_128") && hasFatal(c118), "200 objects trip over_118 + over_128 (fatal)");
    auto cwide = checkConstraints(12, 3, true);
    check(has(cwide, "wide_bed") && !hasFatal(cwide), "a 12-ch bed warns (wide_bed) but is not fatal");
    check(has(checkConstraints(10, 120, true), "over_118"), "120 objects trips over_118");

    // ---- reconcile: empty bus -> all add ----
    auto rEmpty = reconcile(roster, {});
    auto cEmpty = countActions(rEmpty);
    check(cEmpty.add == 4 && cEmpty.reuse == 0 && cEmpty.stray == 0, "empty renderer -> 4 adds");

    // ---- reconcile: exact match -> all reuse (idempotency) ----
    std::vector<ExistingSend> exact = {
        {0, 5, 0, 10, 0, false, true},   // bed
        {1, 2, 0, 1, 10, false, true},   // object 1 @ chan 11
        {2, 3, 0, 1, 11, false, true},   // object 2 @ chan 12
        {3, 4, 0, 1, 12, false, true},   // object 3 @ chan 13
    };
    auto rReuse = reconcile(roster, exact);
    auto cReuse = countActions(rReuse);
    check(cReuse.reuse == 4 && cReuse.add == 0 && cReuse.fix == 0 && cReuse.stray == 0,
          "re-running against a built layout is idempotent (all reuse)");

    // ---- reconcile: wrong channel -> fix; extra source -> stray ----
    std::vector<ExistingSend> drift = {
        {0, 5, 0, 10, 0, false, true},   // bed ok
        {1, 2, 0, 1, 20, false, true},   // object 1 at the WRONG channel (20)
        {2, 99, 0, 1, 30, false, true},  // a stray source not in the roster
    };
    auto rDrift = reconcile(roster, drift);
    auto cDrift = countActions(rDrift);
    check(cDrift.reuse == 1 && cDrift.fix == 1 && cDrift.add == 2 && cDrift.stray == 1,
          "drifted layout -> 1 reuse (bed) + 1 fix + 2 adds + 1 stray");
    check(rDrift.entries[1].action == Action::Fix && rDrift.entries[1].sendIndex == 1, "the mis-channeled object is fixed in place");
    check(std::string(actionName(Action::Prune)) == "prune", "actionName maps Prune");

    // ---- inspect: clean layout ----
    auto ins = inspect(exact, 10, 3, 13);
    check(ins.ok && ins.bedPresent && ins.objectChannels == 3 && ins.coveredChannels == 13, "clean layout inspects ok");

    // ---- inspect: collision ----
    std::vector<ExistingSend> collide = {
        {0, 5, 0, 10, 0, false, true},
        {1, 2, 0, 1, 10, false, true},
        {2, 3, 0, 1, 10, false, true},  // second object on the SAME channel
    };
    check(has(inspect(collide, 10, -1, 11).issues, "collision"), "two sends on one channel -> collision");

    // ---- inspect: gap ----
    std::vector<ExistingSend> gap = {
        {0, 5, 0, 10, 0, false, true},
        {1, 2, 0, 1, 11, false, true},  // object jumps to chan 12, leaving chan 11 empty
    };
    check(has(inspect(gap, 10, -1, 12).issues, "gap"), "a skipped channel below the top -> gap");

    // ---- inspect: bed missing ----
    std::vector<ExistingSend> noBedR = {
        {0, 2, 0, 1, 10, false, true},
        {1, 3, 0, 1, 11, false, true},
    };
    check(has(inspect(noBedR, 10, -1, 12).issues, "bed_missing"), "declared bed absent -> bed_missing");

    // ---- inspect: object width ----
    std::vector<ExistingSend> wideObj = {
        {0, 5, 0, 10, 0, false, true},
        {1, 2, 0, 2, 10, false, true},  // a 2-ch send in object territory
    };
    check(has(inspect(wideObj, 10, -1, 12).issues, "object_width"), "a stereo object -> object_width");

    // ---- inspect: unrouted ----
    std::vector<ExistingSend> partial = {
        {0, 5, 0, 10, 0, false, true},
        {1, 2, 0, 1, 10, false, true},
    };
    check(has(inspect(partial, 10, 3, 13).issues, "unrouted"), "1 of 3 declared objects routed -> unrouted");

    // ---- inspect: over_118 ----
    std::vector<ExistingSend> many;
    for (int i = 0; i < 119; ++i) many.push_back({i, 100 + i, 0, 1, i, false, true});
    check(has(inspect(many, 0, -1, 119).issues, "over_118"), "119 mono objects -> over_118");

    if (g_failures == 0) std::fprintf(stderr, "\nALL SEND-LAYOUT UNIT TESTS PASSED\n");
    else std::fprintf(stderr, "\n%d SEND-LAYOUT UNIT TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
