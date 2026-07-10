// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// subscription_hub.h — MCP resource subscriptions.
//
// A tiny, thread-safe store that connects two sides of the resource-subscription machinery:
//
//   Producer (MAIN THREAD): the control-surface Set* callbacks resolve their change into one or more
//     affected resource URIs and call markDirty(uri). This runs inside REAPER's UI tick.
//
//   Consumer (WORKER THREAD): the MCP server's SSE channel (GET /mcp) periodically calls
//     drainUpdated(), turns each returned URI into a `notifications/resources/updated` JSON-RPC
//     message, and writes it to the subscribed client. resources/subscribe and resources/unsubscribe
//     (served on the POST handler thread) call subscribe()/unsubscribe().
//
// Coalescing falls out of the design for free: markDirty() records into a std::set keyed by URI, so a
// burst of Set* callbacks during a fader drag collapses to a single pending entry per resource, and
// the client sees at most one `updated` per resource per drain tick — never a flood.
//
// This header is intentionally dependency-free (no REAPER SDK, no nlohmann/json): the transport layer
// owns JSON-RPC framing, and the control surface owns REAPER API access. That keeps the hub a pure
// data structure the unit test can drive directly.

#pragma once

#include <cstddef>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace reaper_mcp {

class SubscriptionHub {
public:
    // --- Subscription set (MCP resources/subscribe, resources/unsubscribe) -----------------------
    // Called from the request-handler thread. Both are idempotent.
    void subscribe(const std::string& uri) {
        std::lock_guard<std::mutex> lk(mu_);
        subscribed_.insert(uri);
    }

    // Unsubscribing also drops any pending dirty mark for that URI so a late drain can't deliver an
    // update for a resource the client no longer watches.
    void unsubscribe(const std::string& uri) {
        std::lock_guard<std::mutex> lk(mu_);
        subscribed_.erase(uri);
        dirty_.erase(uri);
    }

    bool isSubscribed(const std::string& uri) const {
        std::lock_guard<std::mutex> lk(mu_);
        return subscribed_.count(uri) != 0;
    }

    std::size_t subscriptionCount() const {
        std::lock_guard<std::mutex> lk(mu_);
        return subscribed_.size();
    }

    // --- Change notification (producer side) -----------------------------------------------------
    // Record that `uri` changed. No-op unless a client is currently subscribed to it, which both
    // avoids useless work and keeps the pending set bounded by the number of subscribed resources.
    // Safe to call at high frequency (fader drags): repeated marks of the same URI coalesce.
    void markDirty(const std::string& uri) {
        std::lock_guard<std::mutex> lk(mu_);
        if (subscribed_.count(uri) != 0) dirty_.insert(uri);
    }

    // --- Drain (consumer side) -------------------------------------------------------------------
    // Return the set of URIs that changed since the last drain AND are still subscribed, clearing the
    // pending set. Deterministic (sorted) order. Called by the SSE channel; if no client holds the
    // stream open the marks simply accumulate (coalesced) until one does — so a reconnecting client
    // gets one update per changed resource rather than a replay of every intermediate edit.
    std::vector<std::string> drainUpdated() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> out;
        out.reserve(dirty_.size());
        for (const auto& uri : dirty_)
            if (subscribed_.count(uri) != 0) out.push_back(uri);
        dirty_.clear();
        return out;
    }

    bool hasPending() const {
        std::lock_guard<std::mutex> lk(mu_);
        return !dirty_.empty();
    }

private:
    mutable std::mutex mu_;
    std::set<std::string> subscribed_;  // URIs the client asked to watch
    std::set<std::string> dirty_;       // subscribed URIs changed since the last drain (coalesced)
};

}  // namespace reaper_mcp
