// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_subscription_hub.cpp — pure unit test for the Phase 3 SubscriptionHub.
//
// The hub is a dependency-free data structure (no REAPER, no JSON), so we can drive both sides
// directly: the producer side (markDirty, as the control surface would call it) and the consumer
// side (drainUpdated, as the SSE channel would call it), plus the subscribe/unsubscribe bookkeeping.

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "subscription_hub.h"

using namespace reaper_mcp;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

static bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

int main() {
    const std::string kState = "reaper://project/state";
    const std::string kGraph = "reaper://routing/graph";
    const std::string kChunk = "reaper://track/0/chunk";

    // --- markDirty is a no-op until someone subscribes ---
    {
        SubscriptionHub h;
        check(h.subscriptionCount() == 0, "new hub has no subscriptions");
        h.markDirty(kState);
        check(!h.hasPending(), "markDirty with no subscribers records nothing");
        check(h.drainUpdated().empty(), "drainUpdated empty when nothing subscribed");
    }

    // --- subscribe -> markDirty -> drain delivers exactly once, then clears ---
    {
        SubscriptionHub h;
        h.subscribe(kState);
        check(h.isSubscribed(kState), "subscribe records the URI");
        check(h.subscriptionCount() == 1, "subscriptionCount reflects one subscription");
        h.markDirty(kState);
        check(h.hasPending(), "markDirty on a subscribed URI is pending");
        auto first = h.drainUpdated();
        check(first.size() == 1 && first[0] == kState, "drainUpdated returns the dirty URI");
        check(!h.hasPending(), "drainUpdated clears the pending set");
        check(h.drainUpdated().empty(), "second drain returns nothing (no new changes)");
    }

    // --- coalescing: a burst of marks on the same URI collapses to one delivery ---
    {
        SubscriptionHub h;
        h.subscribe(kState);
        for (int i = 0; i < 1000; ++i) h.markDirty(kState);
        auto out = h.drainUpdated();
        check(out.size() == 1, "1000 marks of one URI coalesce to a single pending entry");
    }

    // --- multiple distinct URIs all deliver; only-subscribed ones count ---
    {
        SubscriptionHub h;
        h.subscribe(kState);
        h.subscribe(kGraph);
        h.markDirty(kState);
        h.markDirty(kGraph);
        h.markDirty(kChunk);  // not subscribed -> ignored
        auto out = h.drainUpdated();
        check(out.size() == 2, "only subscribed URIs are drained (unsubscribed chunk ignored)");
        check(contains(out, kState) && contains(out, kGraph), "both subscribed URIs present");
        check(!contains(out, kChunk), "unsubscribed URI absent from drain");
    }

    // --- unsubscribe stops future deliveries AND drops any already-pending mark ---
    {
        SubscriptionHub h;
        h.subscribe(kState);
        h.markDirty(kState);           // pending
        h.unsubscribe(kState);         // must also clear the pending mark
        check(!h.isSubscribed(kState), "unsubscribe removes the subscription");
        check(h.drainUpdated().empty(), "unsubscribe drops the already-pending update");
        h.markDirty(kState);
        check(h.drainUpdated().empty(), "post-unsubscribe marks are ignored");
    }

    // --- idempotency: double subscribe/unsubscribe don't corrupt counts ---
    {
        SubscriptionHub h;
        h.subscribe(kState);
        h.subscribe(kState);
        check(h.subscriptionCount() == 1, "double subscribe is idempotent");
        h.unsubscribe(kState);
        h.unsubscribe(kState);
        check(h.subscriptionCount() == 0, "double unsubscribe is idempotent");
    }

    // --- concurrency smoke: a producer marking while a consumer drains never crashes / never
    //     delivers an unsubscribed URI, and every mark after the last drain is eventually seen ---
    {
        SubscriptionHub h;
        h.subscribe(kState);
        std::atomic<bool> stop{false};
        std::atomic<long> delivered{0};
        std::thread consumer([&] {
            while (!stop.load()) {
                for (const auto& u : h.drainUpdated()) {
                    if (u == kState) delivered.fetch_add(1);
                    else { std::fprintf(stderr, "  FAIL: drained unexpected URI %s\n", u.c_str()); ++g_failures; }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
        for (int i = 0; i < 100000; ++i) h.markDirty(kState);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop.store(true);
        consumer.join();
        for (const auto& u : h.drainUpdated()) if (u == kState) delivered.fetch_add(1);  // final drain
        check(delivered.load() >= 1, "concurrent producer/consumer delivered at least one coalesced update");
    }

    if (g_failures == 0) { std::fprintf(stderr, "ALL SUBSCRIPTION-HUB CHECKS PASSED\n"); return 0; }
    std::fprintf(stderr, "%d SUBSCRIPTION-HUB CHECK(S) FAILED\n", g_failures);
    return 1;
}
