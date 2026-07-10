// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_elicitation.cpp — Phase 6 (d): host-side unit test for the async-elicitation substrate.
//
// Two SDK-free pieces, exercised with no REAPER and no HTTP:
//   1. ElicitationRegistry — the pending-elicitation correlator that bridges the SSE stream (which
//      awaits an answer) to the follow-up POST route (which delivers it): create / deliver / cancel /
//      poll-in-slices / blocking wait / timeout / idempotency / cross-thread delivery.
//   2. requireElicitation() + ElicitationScope — the control flow a destructive verb uses: throw
//      ElicitationRequest when the current call may elicit (scope set), no-op otherwise; plus the flat
//      confirm schema and RAII restore of the thread-local.
//
// The transport that consumes these (SSE-answered tools/call, response routing) is proven end-to-end
// over real HTTP in test_mcp_server.cpp; the live client interop is Session P6-B.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "composite_support.h"  // requireElicitation, elicitationConfirmSchema
#include "elicitation.h"        // ElicitationRegistry, ElicitationScope, ElicitationRequest

using nlohmann::json;
using namespace reaper_mcp;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) {
        std::fprintf(stderr, "  FAIL: %s\n", what.c_str());
        ++g_failures;
    } else {
        std::fprintf(stderr, "  ok:   %s\n", what.c_str());
    }
}

static json answerMsg(const std::string& id, const std::string& action, json content = json::object()) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"action", action}, {"content", content}}}};
}

int main() {
    using namespace std::chrono;

    // --- 1. registry: create / has / count ---
    {
        ElicitationRegistry reg;
        check(reg.pendingCount() == 0, "registry starts empty");
        check(reg.create("elic-1"), "create(elic-1) succeeds");
        check(reg.has("elic-1"), "has(elic-1) after create");
        check(reg.pendingCount() == 1, "pendingCount == 1 after one create");
        check(!reg.create("elic-1"), "duplicate create(elic-1) rejected");
        check(reg.create("elic-2"), "create(elic-2) succeeds");
        check(reg.pendingCount() == 2, "two pending");
        reg.remove("elic-2");
        check(!reg.has("elic-2") && reg.pendingCount() == 1, "remove drops the slot");
    }

    // --- 2. deliver -> wait returns Answered with the response ---
    {
        ElicitationRegistry reg;
        reg.create("a");
        check(reg.deliver("a", answerMsg("a", "accept", json{{"confirm", true}})), "deliver(a) accepted");
        ElicitationOutcome out = reg.wait("a", milliseconds(50));
        check(out.kind == ElicitationOutcome::Answered, "wait after deliver -> Answered");
        check(out.response["result"]["action"] == "accept", "response carries action:accept");
        check(out.response["result"]["content"].value("confirm", false), "response carries content.confirm");
        check(reg.pendingCount() == 0, "wait() removes the slot");
        check(!reg.deliver("a", answerMsg("a", "accept")), "deliver to an already-drained id is a no-op");
    }

    // --- 3. cancel -> wait returns Cancelled ---
    {
        ElicitationRegistry reg;
        reg.create("c");
        check(reg.cancel("c"), "cancel(c) settles the slot");
        check(!reg.cancel("c"), "second cancel is idempotent no-op");
        ElicitationOutcome out = reg.wait("c", milliseconds(50));
        check(out.kind == ElicitationOutcome::Cancelled, "wait after cancel -> Cancelled");
    }

    // --- 4. timeout -> wait returns TimedOut, no answer ever delivered ---
    {
        ElicitationRegistry reg;
        reg.create("t");
        auto t0 = steady_clock::now();
        ElicitationOutcome out = reg.wait("t", milliseconds(60));
        auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        check(out.kind == ElicitationOutcome::TimedOut, "wait with no delivery -> TimedOut");
        check(elapsed >= 55, "timeout actually waited the full slice (~60ms)");
        check(reg.pendingCount() == 0, "timed-out wait() still removes the slot");
    }

    // --- 5. deliver to an unknown id fails; wait on an unknown id is terminal-Cancelled ---
    {
        ElicitationRegistry reg;
        check(!reg.deliver("nope", answerMsg("nope", "accept")), "deliver to unknown id -> false");
        ElicitationOutcome out = reg.wait("nope", milliseconds(5));
        check(out.kind == ElicitationOutcome::Cancelled, "wait on unknown id -> Cancelled (terminal)");
    }

    // --- 6. poll in slices: not-terminal until delivered, then terminal ---
    {
        ElicitationRegistry reg;
        reg.create("p");
        ElicitationOutcome out;
        check(!reg.poll("p", milliseconds(10), out), "poll before delivery is not terminal");
        check(reg.has("p"), "poll leaves the slot in place");
        reg.deliver("p", answerMsg("p", "decline"));
        // may need a couple of slices for the CV wake; loop briefly.
        bool terminal = false;
        for (int i = 0; i < 20 && !terminal; ++i) terminal = reg.poll("p", milliseconds(10), out);
        check(terminal, "poll after delivery becomes terminal");
        check(out.kind == ElicitationOutcome::Answered, "polled outcome is Answered");
        check(out.response["result"].value("action", "") == "decline", "polled response carries action:decline");
        reg.remove("p");
    }

    // --- 7. cross-thread: a background 'client' delivers while the 'stream' polls ---
    {
        ElicitationRegistry reg;
        reg.create("x");
        std::atomic<bool> delivered{false};
        std::thread client([&] {
            std::this_thread::sleep_for(milliseconds(40));
            reg.deliver("x", answerMsg("x", "accept", json{{"confirm", true}}));
            delivered.store(true);
        });
        ElicitationOutcome out;
        bool terminal = false;
        for (int i = 0; i < 200 && !terminal; ++i) terminal = reg.poll("x", milliseconds(10), out);
        client.join();
        check(delivered.load(), "background client delivered");
        check(terminal && out.kind == ElicitationOutcome::Answered, "stream saw the delivery as Answered");
        reg.remove("x");
    }

    // --- 8. requireElicitation control flow via ElicitationScope ---
    {
        // No scope -> not capable -> no-op (falls through to the confirm:true fallback).
        check(elicitationCapableFlag() == false, "thread-local defaults to not-capable");
        bool threw = false;
        try {
            requireElicitation("proceed?");
        } catch (const ElicitationRequest&) {
            threw = true;
        }
        check(!threw, "requireElicitation is a no-op when not elicitation-capable");

        // Inside a capable scope -> throws ElicitationRequest carrying schema + message.
        threw = false;
        ElicitationRequest captured;
        {
            ElicitationScope scope(true);
            check(elicitationCapableFlag() == true, "scope(true) sets the thread-local");
            try {
                requireElicitation("This restructures routing. Proceed?");
            } catch (const ElicitationRequest& er) {
                threw = true;
                captured = er;
            }
        }
        check(threw, "requireElicitation throws inside a capable scope");
        check(captured.message.find("Proceed?") != std::string::npos, "ElicitationRequest carries the message");
        check(captured.requestedSchema.value("type", "") == "object", "requestedSchema is an object schema");
        check(captured.requestedSchema["properties"].contains("confirm"), "schema exposes a confirm property");
        check(captured.requestedSchema["properties"]["confirm"].value("type", "") == "boolean",
              "confirm property is boolean");
        bool requiresConfirm = false;
        for (const auto& r : captured.requestedSchema.value("required", json::array()))
            if (r == "confirm") requiresConfirm = true;
        check(requiresConfirm, "schema marks confirm as required");

        // RAII restore: after the scope, the flag is back to not-capable.
        check(elicitationCapableFlag() == false, "ElicitationScope restores the thread-local on exit");
    }

    // --- 9. nested scopes restore correctly ---
    {
        check(elicitationCapableFlag() == false, "outer: not capable");
        {
            ElicitationScope s1(true);
            check(elicitationCapableFlag() == true, "s1 -> capable");
            {
                ElicitationScope s2(false);
                check(elicitationCapableFlag() == false, "s2 -> not capable (nested)");
            }
            check(elicitationCapableFlag() == true, "back to s1 -> capable");
        }
        check(elicitationCapableFlag() == false, "back to outer -> not capable");
    }

    // --- 10. the standalone confirm schema helper ---
    {
        json s = elicitationConfirmSchema();
        check(s.value("type", "") == "object", "confirm schema is type object");
        check(s["properties"]["confirm"].value("type", "") == "boolean", "confirm schema property is boolean");
        check(!s["required"].empty(), "confirm schema has a required list");
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "ALL ELICITATION UNIT CHECKS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d ELICITATION UNIT CHECK(S) FAILED\n", g_failures);
    return 1;
}
