// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// elicitation.h — async MCP elicitation transport substrate.
//
// MCP `elicitation/create` is a SERVER->CLIENT request (carrying a flat JSON schema for the input it
// wants) that must be answered mid-tool-call. A human's answer is unbounded in time — incompatible
// with the 5 s synchronous, main-thread-bound `tools/call`. The spec-faithful async model is:
//
//   1. A destructive verb, running on the MAIN THREAD, calls requireElicitation() (composite_support.h).
//      If the client advertised `capabilities.elicitation`, that THROWS ElicitationRequest — unwinding
//      the handler BEFORE any mutation (no UndoTransaction opened yet), handing the transport a
//      schema + human message. If the client can't elicit, it is a no-op and the caller falls back to
//      the `confirm:true` stopgap.
//   2. The transport (mcp_server.cpp) catches ElicitationRequest, answers that one `tools/call` POST
//      with an SSE stream, emits `elicitation/create` on it, and AWAITS the client's response OFF the
//      main thread (the httplib connection thread) so REAPER keeps pumping Run().
//   3. The client's elicitation response arrives on a correlated follow-up POST; the ElicitationRegistry
//      (keyed by the server-generated JSON-RPC id) bridges it back to the waiting stream.
//   4. On accept the transport re-submits the handler to the MainThreadQueue with `confirm:true` folded
//      in (the mutation re-marshals to the main thread); on decline/cancel/timeout it aborts with ZERO
//      mutation. The `tools/call` result is streamed on the same SSE response.
//
// This header is intentionally SDK-FREE (no reaper_api.h): it is pure C++ + nlohmann/json so the
// registry + the thread-local scope are unit-tested host-side with no REAPER. The one REAPER coupling
// (the destructive verb's trigger) lives in the tool; this is only the plumbing.
//
// Include order note: this pulls <nlohmann/json.hpp>. When included via composite_support.h (which
// pulls reaper_api.h first to de-fang SWELL's min/max), that order is preserved; when included by the
// transport / a host test there is no SWELL in the TU, so order is moot.

#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace reaper_mcp {

using Json = nlohmann::json;

// -------------------------------------------------------------------------------------------------
// requireElicitation()'s signal to the transport
// -------------------------------------------------------------------------------------------------

// Thrown (on the MAIN thread) by requireElicitation() when the current call may elicit. Carries the
// flat primitive schema MCP restricts elicitation to, plus the human-readable message. NOT a
// std::exception subclass on purpose: the transport catches it explicitly and it must never be
// swallowed by a generic `catch (std::exception&)` in a non-elicitation code path.
struct ElicitationRequest {
    Json requestedSchema;  // {"type":"object","properties":{...flat primitives...},"required":[...]}
    std::string message;   // human-readable summary of what the destructive action will do
};

// -------------------------------------------------------------------------------------------------
// Per-call "may elicit" flag — a thread-local set on the MAIN thread around the handler invocation
// -------------------------------------------------------------------------------------------------
//
// The handler runs on the MAIN thread (via MainThreadQueue), NOT the connection thread, so the
// transport can't hand it a plain bool. The transport wraps the queued handler closure in an
// ElicitationScope, which sets this thread-local ON THE MAIN THREAD; requireElicitation() (also on the
// main thread, inside the handler) reads it. drainMainThread() runs jobs serially on one thread, so
// each call's scope is isolated and RAII-restored even if the handler throws.

inline bool& elicitationCapableFlag() {
    static thread_local bool capable = false;
    return capable;
}

struct ElicitationScope {
    bool prev_;
    explicit ElicitationScope(bool capable) : prev_(elicitationCapableFlag()) {
        elicitationCapableFlag() = capable;
    }
    ~ElicitationScope() { elicitationCapableFlag() = prev_; }
    ElicitationScope(const ElicitationScope&) = delete;
    ElicitationScope& operator=(const ElicitationScope&) = delete;
};

// -------------------------------------------------------------------------------------------------
// The pending-elicitation correlator
// -------------------------------------------------------------------------------------------------

// Terminal outcome of one elicitation round-trip.
struct ElicitationOutcome {
    enum Kind { Answered, Cancelled, TimedOut };
    Kind kind = TimedOut;
    Json response;  // the client's JSON-RPC response message (only meaningful for Answered)
};

// Bridges the SSE stream (which awaits an answer) to the follow-up POST route (which delivers it),
// keyed by the server-generated `elicitation/create` request id. Thread-safe, SDK-free, unit-tested.
//
// Lifecycle per elicitation: create(id) -> [stream waits via poll()/wait()] -> deliver(id,resp)
// (from the follow-up POST — a decline/cancel arrives as a delivered response whose action is
// "decline"/"cancel") or the stream times out -> remove(id). cancel(id) is an equivalent helper that
// settles the slot as Cancelled directly.
class ElicitationRegistry {
public:
    // Register a pending slot. Returns false if the id is already registered (id collision — the
    // caller should regenerate).
    bool create(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        if (pending_.count(id)) return false;
        pending_.emplace(id, std::make_shared<Slot>());
        return true;
    }

    // Deliver the client's response for `id`. Returns true iff a slot was waiting (idempotent: a
    // second deliver, or one that races a timeout/cancel, is a harmless no-op returning false).
    bool deliver(const std::string& id, Json response) {
        return settle(id, ElicitationOutcome::Answered, std::move(response));
    }

    // Cancel the pending elicitation for `id` (client sent action:"cancel", or a cancellation notice).
    bool cancel(const std::string& id) {
        return settle(id, ElicitationOutcome::Cancelled, Json());
    }

    // Block up to `slice` for a terminal outcome without removing the slot (the stream state machine
    // polls in slices so it can also enforce an overall wall-clock timeout + emit heartbeats). Returns
    // true iff terminal; `out` is then set. A missing id resolves as Cancelled/terminal.
    bool poll(const std::string& id, std::chrono::milliseconds slice, ElicitationOutcome& out) {
        std::shared_ptr<Slot> slot = lookup(id);
        if (!slot) { out = {ElicitationOutcome::Cancelled, Json()}; return true; }
        std::unique_lock<std::mutex> lk(slot->m);
        if (slot->cv.wait_for(lk, slice, [&] { return slot->done; })) {
            out = {slot->kind, slot->response};
            return true;
        }
        return false;
    }

    // Blocking convenience (used by the unit test): wait up to `timeout`, then remove the slot.
    ElicitationOutcome wait(const std::string& id, std::chrono::milliseconds timeout) {
        std::shared_ptr<Slot> slot = lookup(id);
        if (!slot) return {ElicitationOutcome::Cancelled, Json()};
        ElicitationOutcome out{ElicitationOutcome::TimedOut, Json()};
        {
            std::unique_lock<std::mutex> lk(slot->m);
            if (slot->cv.wait_for(lk, timeout, [&] { return slot->done; }))
                out = {slot->kind, slot->response};
        }
        remove(id);
        return out;
    }

    void remove(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.erase(id);
    }

    std::size_t pendingCount() {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_.size();
    }

    bool has(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_.count(id) != 0;
    }

private:
    struct Slot {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        ElicitationOutcome::Kind kind = ElicitationOutcome::TimedOut;
        Json response;
    };

    std::shared_ptr<Slot> lookup(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pending_.find(id);
        return it == pending_.end() ? nullptr : it->second;
    }

    bool settle(const std::string& id, ElicitationOutcome::Kind kind, Json response) {
        std::shared_ptr<Slot> slot = lookup(id);
        if (!slot) return false;
        {
            std::lock_guard<std::mutex> lk(slot->m);
            if (slot->done) return false;  // already settled (idempotent)
            slot->done = true;
            slot->kind = kind;
            slot->response = std::move(response);
        }
        slot->cv.notify_all();
        return true;
    }

    std::mutex mu_;
    std::map<std::string, std::shared_ptr<Slot>> pending_;
};

}  // namespace reaper_mcp
