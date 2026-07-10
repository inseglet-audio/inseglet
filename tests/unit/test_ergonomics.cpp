// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_ergonomics.cpp — pure unit test for the Phase-5a agent-ergonomics substrate
// (src/composite_support.h): structured errors, the dry-run Plan model, and the UndoTransaction
// commit/rollback state machine.
//
// The transaction's REAPER undo calls are injected through TxnOps, so this test drives the real
// control flow with a mock that records begin/end/undo into a log — no REAPER, no SDK.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "composite_support.h"

using namespace reaper_mcp;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// A TxnOps that records each call into `log` so we can assert the exact begin/end/undo sequence.
static TxnOps mockOps(std::vector<std::string>& log) {
    TxnOps ops;
    ops.begin = [&log] { log.push_back("begin"); };
    ops.end = [&log](const std::string& desc, int flags) {
        log.push_back("end:" + desc + ":" + std::to_string(flags));
    };
    ops.undo = [&log] { log.push_back("undo"); };
    return ops;
}

int main() {
    // ---- 1. structured errors ----
    {
        const Json e = makeError("no_ambisonic_suite", "IEM/SPARTA/ATK not detected",
                                 "install the IEM Plug-in Suite or pass suite:'ambiX'");
        check(e.value("error", "") == "no_ambisonic_suite", "makeError sets the error code");
        check(e.value("detail", "") == "IEM/SPARTA/ATK not detected", "makeError sets detail");
        check(e.contains("remediation"), "makeError includes remediation when given");
        check(isError(e), "isError() recognizes a structured error");

        const Json e2 = makeError("bad_arg", "value out of range");
        check(!e2.contains("remediation"), "remediation omitted when empty");
        check(isError(e2) && !isError(Json{{"ok", true}}), "isError() distinguishes non-errors");
    }

    // ---- 2. CompositeError carries the payload ----
    {
        try {
            throw CompositeError("no_bed", "no 7.1.4 bed present", "call build_bed first");
        } catch (const CompositeError& ce) {
            check(ce.payload.value("error", "") == "no_bed", "CompositeError carries the code");
            check(ce.payload.value("remediation", "") == "call build_bed first",
                  "CompositeError carries remediation");
            check(std::string(ce.what()).find("no 7.1.4 bed present") != std::string::npos,
                  "CompositeError.what() surfaces the detail");
        }
    }

    // ---- 3. dry-run Plan model ----
    {
        Plan plan;
        plan.add("add_track", "bus '7.1.4 bed'", "12ch", Json{{"channels", 12}})
            .add("insert_fx", "track 'Drums' (idx 3)", "VST3: StereoEncoder (IEM)")
            .add("set_param", "track 'Drums' (idx 3)", "az=30 el=0", Json{{"az", 30}, {"el", 0}});
        check(plan.size() == 3 && !plan.empty(), "Plan accumulates steps (chainable)");

        const Json j = plan.toJson();
        check(j.is_array() && j.size() == 3, "Plan.toJson is a 3-element array");
        check(j[0].value("action", "") == "add_track" && j[0]["data"].value("channels", 0) == 12,
              "Plan step carries action + structured data");

        const std::string d = plan.diff();
        check(d.find("+ add_track bus '7.1.4 bed' - 12ch") != std::string::npos,
              "diff renders a + line with action, target, and detail");
        check(d.find("+ insert_fx track 'Drums' (idx 3) - VST3: StereoEncoder (IEM)") != std::string::npos,
              "diff renders each step");

        const Json dr = plan.dryRunResult();
        check(dr.value("dryRun", false) == true, "dryRunResult sets dryRun:true");
        check(dr.value("stepCount", 0) == 3, "dryRunResult reports stepCount");
        check(dr["plan"].is_array() && dr.contains("diff"), "dryRunResult carries plan[] + diff");
    }

    // ---- 4. UndoTransaction: commit keeps edits, one undo point, no undo call ----
    {
        std::vector<std::string> log;
        {
            UndoTransaction txn("spatialize_stems", -1, mockOps(log));
            txn.recordExecuted("add_track", "bus");
            check(txn.dirty(), "recordExecuted marks the transaction dirty");
            check(txn.executed().size() == 1, "executed() accumulates the how-far-it-got plan");
            txn.commit();
            check(txn.state() == UndoTransaction::State::Committed, "state is Committed after commit()");
        }  // destructor must NOT roll back an already-committed txn
        check(log.size() == 2, "commit path issues exactly begin + end");
        check(log[0] == "begin", "commit path begins the block");
        check(log[1] == "end:spatialize_stems:-1", "commit closes the block with the description");
    }

    // ---- 5. UndoTransaction: abandoned + dirty rolls back (end + undo) ----
    {
        std::vector<std::string> log;
        {
            UndoTransaction txn("spatialize_stems", -1, mockOps(log));
            txn.recordExecuted("insert_fx", "encoder");
            // no commit() -> destructor rolls back
        }
        check(log.size() == 3, "abandoned dirty txn issues begin + end + undo");
        check(log[0] == "begin", "rollback begins the block");
        check(log[1] == "end:spatialize_stems (rolled back):-1", "rollback annotates the end description");
        check(log[2] == "undo", "rollback reverts the partial edits via one undo");
    }

    // ---- 6. UndoTransaction: abandoned but clean (no edits) never undoes a prior action ----
    {
        std::vector<std::string> log;
        {
            UndoTransaction txn("noop_verb", -1, mockOps(log));
            // nothing recorded -> not dirty
        }
        check(log.size() == 2, "clean abandoned txn issues begin + end only");
        check(log[1] == "end:noop_verb (rolled back):-1", "clean rollback still closes the block");
        for (const auto& l : log) check(l != "undo", "clean rollback does NOT call undo (no empty-block pop)");
    }

    // ---- 7. exception unwinding through the transaction rolls back ----
    {
        std::vector<std::string> log;
        try {
            UndoTransaction txn("apply_style", -1, mockOps(log));
            txn.recordExecuted("insert_fx", "ReaEQ");
            throw CompositeError("style_failed", "limiter insert failed");
        } catch (const CompositeError&) {
            // handled
        }
        check(log.size() == 3 && log[2] == "undo", "an exception mid-plan triggers rollback (undo)");
    }

    // ---- 8. explicit rollback + idempotent commit/rollback ----
    {
        std::vector<std::string> log;
        UndoTransaction txn("verb", -1, mockOps(log));
        txn.markDirty();
        txn.rollback();
        check(txn.state() == UndoTransaction::State::RolledBack, "explicit rollback sets RolledBack");
        txn.commit();  // no-op after rollback
        txn.rollback();  // no-op
        check(log.size() == 3, "commit/rollback after a terminal state are no-ops");
    }

    if (g_failures == 0) { std::fprintf(stderr, "ALL ERGONOMICS CHECKS PASSED\n"); return 0; }
    std::fprintf(stderr, "%d ERGONOMICS CHECK(S) FAILED\n", g_failures);
    return 1;
}
