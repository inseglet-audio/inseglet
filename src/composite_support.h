// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// composite_support.h — the agent-ergonomics substrate for the composite verbs.
//
// The cross-cutting machinery every composite verb (spatialize_stems, check_deliverable,
// apply_style, run_dsl, …) reuses so a long autonomous session stays safe:
//
//   1. Dry-run plan model   — accumulate the graph mutations a verb intends; with dryRun:true return
//                             the plan + a human-readable diff and mutate nothing. Also the primitive
//                             for "explain-the-diff".
//   2. Structured errors    — {"error":<code>,"detail":...,"remediation":...} that an agent can act
//                             on, not just a bare string; CompositeError carries that payload through
//                             an exception so a mid-plan failure unwinds cleanly.
//   3. Transactional edit   — UndoTransaction wraps a verb's whole plan in ONE REAPER undo block
//                             (Undo_BeginBlock2/EndBlock2) => a single undo point. If it is abandoned
//                             (an exception unwinds past it, or commit() is never called) and it made
//                             real edits, it issues one Undo_DoUndo2 so the partial work is reverted
//                             => all-or-nothing. The REAPER calls are injected through TxnOps so the
//                             commit/rollback *logic* is unit-tested host-side with a mock (no REAPER).
//
// The plan + error pieces are SDK-free and fully unit-testable. defaultTxnOps() binds to the real
// Undo_* API under REAPER_MCP_HAVE_SDK and is a harmless no-op host-side.
//
// Include order: reaper_api.h BEFORE any STL/JSON header (SWELL min/max — see reaper_api.h), matching
// tool_helpers.h. Everything here is inline/header-only (safe to include from multiple TUs).

#pragma once

#include "reaper_api.h"  // no-op unless REAPER_MCP_HAVE_SDK; pulls + de-fangs SWELL before <json>

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "elicitation.h"  // ElicitationRequest + the per-call thread-local scope

namespace reaper_mcp {

using Json = nlohmann::json;

// ---------------------------------------------------------------------------------------------
// 1. Structured, remediable errors
// ---------------------------------------------------------------------------------------------

// Build a structured error object: {"error":<code>, "detail":<detail>, "remediation":<remediation?>}.
// `code` is a stable machine token (e.g. "no_ambisonic_suite"); detail is human context; remediation
// (optional) is the concrete next action an agent can take.
inline Json makeError(const std::string& code, const std::string& detail,
                      const std::string& remediation = std::string()) {
    Json e{{"error", code}, {"detail", detail}};
    if (!remediation.empty()) e["remediation"] = remediation;
    return e;
}

// True if `j` is one of our structured error objects (has an "error" code field).
inline bool isError(const Json& j) { return j.is_object() && j.contains("error"); }

// Thrown by a composite to unwind its UndoTransaction and surface a structured error. The server's
// tools/call path already turns a thrown std::exception into an isError result; carrying the JSON
// payload lets a caller (or a future structured-error channel) recover the code/remediation.
struct CompositeError : std::runtime_error {
    Json payload;
    explicit CompositeError(Json p)
        : std::runtime_error(p.value("detail", p.value("error", std::string("composite error")))),
          payload(std::move(p)) {}
    CompositeError(const std::string& code, const std::string& detail,
                   const std::string& remediation = std::string())
        : CompositeError(makeError(code, detail, remediation)) {}
};

// ---------------------------------------------------------------------------------------------
// 1b. Destructive-action confirmation via MCP elicitation
// ---------------------------------------------------------------------------------------------

// The flat, primitive-only schema MCP permits for `elicitation/create` — here a single `confirm`
// boolean. Clients render it (with the message) as a schema-driven prompt rather than the LLM
// paraphrasing intent. Kept tiny so any destructive verb reuses it.
inline Json elicitationConfirmSchema() {
    return Json{{"type", "object"},
                {"properties",
                 {{"confirm",
                   {{"type", "boolean"},
                    {"title", "Confirm"},
                    {"description", "Set true to proceed with this change."}}}}},
                {"required", Json::array({"confirm"})}};
}

// One reusable confirmation gate for every destructive verb. Call it (on the MAIN thread, BEFORE
// opening an UndoTransaction) when the action needs the human's OK:
//
//   * If the client advertised `capabilities.elicitation`, this THROWS ElicitationRequest — the
//     transport streams `elicitation/create`, awaits the human off the main thread, and on accept
//     re-invokes this tool call with `confirm:true` folded in (so control never returns here on that
//     path). On decline/cancel/timeout the transport aborts with no mutation.
//   * If the client CANNOT elicit, this is a no-op: the caller then applies its `confirm:true`
//     stopgap (typically `return makeError("confirmation_required", …)`).
//
// Because the elicitation-capable path unwinds via throw before any edit, the "catch structure
// before any mutation" guarantee is preserved.
inline void requireElicitation(const std::string& message, Json schema = elicitationConfirmSchema()) {
    if (elicitationCapableFlag()) throw ElicitationRequest{std::move(schema), message};
    // else: no-op — the caller falls through to its confirm:true fallback.
}

// ---------------------------------------------------------------------------------------------
// 2. Dry-run plan model
// ---------------------------------------------------------------------------------------------

// One intended (or executed) graph mutation. `action`/`target`/`detail` render the human diff; `data`
// carries the structured payload (indices, params, fx names) for programmatic consumers.
struct PlanStep {
    std::string action;  // "add_track", "insert_fx", "create_send", "set_param", "set_channels", …
    std::string target;  // subject, e.g. "track 'Drums' (idx 3)" or "bus '7.1.4 bed'"
    std::string detail;  // e.g. "VST3: StereoEncoder (IEM)" or "az=30 el=0 width=narrow"
    Json data;           // structured details (default: empty object)
};

class Plan {
public:
    Plan& add(std::string action, std::string target, std::string detail = std::string(),
              Json data = Json::object()) {
        steps_.push_back(PlanStep{std::move(action), std::move(target), std::move(detail),
                                  std::move(data)});
        return *this;
    }

    bool empty() const { return steps_.empty(); }
    size_t size() const { return steps_.size(); }
    const std::vector<PlanStep>& steps() const { return steps_; }

    // Structured step list for the tool result.
    Json toJson() const {
        Json arr = Json::array();
        for (const auto& s : steps_)
            arr.push_back(Json{{"action", s.action}, {"target", s.target},
                               {"detail", s.detail}, {"data", s.data}});
        return arr;
    }

    // Human-readable diff: one "+ <action> <target> - <detail>" line per step.
    std::string diff() const {
        std::string out;
        for (const auto& s : steps_) {
            out += "+ " + s.action + " " + s.target;
            if (!s.detail.empty()) out += " - " + s.detail;
            out += "\n";
        }
        return out;
    }

    // Standard dryRun envelope a composite returns when dryRun:true.
    Json dryRunResult() const {
        return Json{{"dryRun", true},
                    {"stepCount", steps_.size()},
                    {"plan", toJson()},
                    {"diff", diff()}};
    }

private:
    std::vector<PlanStep> steps_;
};

// ---------------------------------------------------------------------------------------------
// 3. Transactional multi-edit (single undo point; all-or-nothing rollback)
// ---------------------------------------------------------------------------------------------

// The REAPER undo calls, injected so the transaction's control flow is testable without REAPER.
//   begin : Undo_BeginBlock2(nullptr)
//   end   : Undo_EndBlock2(nullptr, desc, flags)
//   undo  : Undo_DoUndo2(nullptr)   — reverts the just-closed block on rollback
// Any field may be empty (host-side default) — the transaction guards every call.
struct TxnOps {
    std::function<void()> begin;
    std::function<void(const std::string& desc, int flags)> end;
    std::function<void()> undo;
};

// Bind to the real REAPER API under the SDK; harmless no-ops host-side. Undo_DoUndo2 is WANTed in
// reaper_api.h (the one API this substrate adds).
inline TxnOps defaultTxnOps() {
#ifdef REAPER_MCP_HAVE_SDK
    TxnOps ops;
    ops.begin = [] { Undo_BeginBlock2(nullptr); };
    ops.end = [](const std::string& desc, int flags) { Undo_EndBlock2(nullptr, desc.c_str(), flags); };
    ops.undo = [] { Undo_DoUndo2(nullptr); };
    return ops;
#else
    return TxnOps{};  // no REAPER: empty ops, every call site guarded
#endif
}

class UndoTransaction {
public:
    enum class State { Open, Committed, RolledBack };

    // flags == -1 => "all state" for Undo_EndBlock2 (the conventional catch-all).
    explicit UndoTransaction(std::string desc, int flags = -1, TxnOps ops = defaultTxnOps())
        : desc_(std::move(desc)), flags_(flags), ops_(std::move(ops)) {
        if (ops_.begin) ops_.begin();
    }

    // An abandoned transaction (never committed) rolls back on destruction.
    ~UndoTransaction() {
        if (state_ == State::Open) rollback();
    }

    UndoTransaction(const UndoTransaction&) = delete;
    UndoTransaction& operator=(const UndoTransaction&) = delete;

    // Record a mutation the composite JUST performed: marks the txn dirty (so an abandoned txn knows
    // there is something to undo — and we never Undo_DoUndo2 an empty block, which would pop an
    // unrelated prior action) and accumulates the "how far it got" report.
    UndoTransaction& recordExecuted(std::string action, std::string target,
                                    std::string detail = std::string(), Json data = Json::object()) {
        dirty_ = true;
        executed_.add(std::move(action), std::move(target), std::move(detail), std::move(data));
        return *this;
    }

    void markDirty() { dirty_ = true; }
    bool dirty() const { return dirty_; }
    const Plan& executed() const { return executed_; }
    State state() const { return state_; }

    // Success: close the block with the description => one undo point, edits kept.
    void commit() {
        if (state_ != State::Open) return;
        state_ = State::Committed;
        if (ops_.end) ops_.end(desc_, flags_);
    }

    // Failure/abandon: close the block; if we made real edits, issue one undo so nothing partial
    // survives (all-or-nothing). If nothing was recorded, just close — never undo a prior action.
    void rollback() {
        if (state_ != State::Open) return;
        state_ = State::RolledBack;
        if (ops_.end) ops_.end(desc_ + " (rolled back)", flags_);
        if (dirty_ && ops_.undo) ops_.undo();
    }

private:
    std::string desc_;
    int flags_;
    TxnOps ops_;
    State state_ = State::Open;
    bool dirty_ = false;
    Plan executed_;
};

}  // namespace reaper_mcp
