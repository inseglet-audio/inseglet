// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_session.cpp — session.run_dsl — the deterministic macro-DSL runner.
//
// One tool that runs a whole script of `verb key=val` lines (dsl.h), each line mapping to exactly one
// tool call over the composite verbs (+ a curated set of primitives, + a fully-qualified
// tool-name escape). The DSL is deterministic — no LLM in the
// execution path — so a script is reproducible and unit-testable. The parsing / value-coercion / verb
// resolution / diff live in the SDK-free dsl.h core; THIS TU is the executor and adds the live pieces:
//
//   * registry existence checks (every resolved tool must be registered),
//   * a NON-MUTATING pre-flight — dryRun mode, and the validation pass that guards a real apply —
//     so a bad spec/style/descriptor/layout is caught before a single edit happens,
//   * a single REAPER undo block around the whole apply (UndoTransaction) => the entire script is ONE
//     undo point; on a failed step it either rolls the whole run back (atomic, the default) or keeps
//     what applied and reports exactly how far it got (transactional multi-edit).
//
// run_dsl dispatches each sub-verb's handler DIRECTLY. In production every tool handler already runs on
// REAPER's main thread (the MainThreadQueue hop happens once, for run_dsl itself), so calling the
// sub-handlers inline is correct and stays on the main thread. Host-side the sub-handlers take their
// SDK-free path, so the whole runner is exercised off a running REAPER by the protocol test.

#include <map>
#include <string>
#include <vector>

#include "tool_helpers.h"          // reaper_api.h (SWELL de-fang) + reqStr/optBool + arg helpers
#include "../dsl/dsl.h"            // the SDK-free macro-DSL core (parse / coerce / alias / diff)
#include "../composite_support.h"  // makeError / isError / UndoTransaction
#include "../tool_registry.h"

namespace reaper_mcp {

namespace {

// Preview one statement without mutating the project: a dry-run-capable composite is dispatched with
// dryRun forced on; a read-only tool is dispatched as-is; a mutating primitive is NOT run (just noted).
Json previewStatement(ToolRegistry* reg, const dsl::Statement& st) {
    // A statement carrying a $ref can't be concretely previewed — the value it depends on
    // won't exist until an earlier step runs. It is covered instead by the static declared-before-use
    // pass, so previewing it as "deferred" (not an error) keeps dryRun + the apply pre-flight edit-free.
    if (dsl::hasRefs(st.args))
        return Json{{"previewed", false}, {"reason", "deferred — depends on $ref"}};
    if (st.preview == dsl::Preview::None)
        return Json{{"previewed", false}, {"reason", "mutating primitive — not executed during preview"}};
    const Tool* t = reg->find(st.tool);
    if (!t) return makeError("unknown_tool", "tool not registered: " + st.tool);
    Json args = st.args;
    if (st.preview == dsl::Preview::InjectDryRun) args["dryRun"] = true;
    try {
        return t->handler(args);
    } catch (const std::exception& e) {
        return makeError("step_threw", std::string("preview threw: ") + e.what());
    }
}

Json stepRecord(int step, const dsl::Statement& st, const Json& result, bool err) {
    return Json{{"step", step}, {"line", st.line}, {"verb", st.verb}, {"tool", st.tool},
                {"result", result}, {"error", err}};
}

}  // namespace

void registerSessionTools(ToolRegistry& reg) {
    ToolRegistry* R = &reg;  // stable: the registry outlives its tools; the handler dispatches siblings.

    reg.add(Tool{
        "session.run_dsl",
        "Run a deterministic macro-DSL script: one `verb key=val` line per tool call, composing the "
        "composite outcome verbs (spatialize, to_ambisonic, immersive_session, apply_style, "
        "check_deliverable) plus curated primitives (build_bed, detect_suites, add_monitor, rotate, "
        "render, track_add, set_channels, select). A verb may also be a fully-qualified tool name "
        "(e.g. spatial.build_bed). Values coerce deterministically: true/false/null, numbers, "
        "\"quoted strings\", comma,lists -> arrays, and [..]/{..} literal JSON; dotted keys nest "
        "(target.layout=7.1.4), and sugar keys map to the schema (layout/order/spec/style/…). No LLM is "
        "in the path. dryRun previews every step (composites dry-run, read-only tools run, mutating "
        "primitives are listed) and mutates nothing. On apply the whole script is ONE undo point; "
        "atomic (default) rolls the entire run back if a step fails, else it keeps what applied and "
        "reports how far it got. A line may capture its result — `$name = verb …` — and a later value "
        "may reference it as $name or $name.dotted.path (resolved at apply from the captured result). "
        "Comments start with #.",
        jparse(R"({"type":"object","properties":{
            "script":{"type":"string"},
            "dryRun":{"type":"boolean","default":false},
            "atomic":{"type":"boolean","default":true},
            "stopOnError":{"type":"boolean","default":true}},
            "required":["script"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{
            "ok":{"type":"boolean"},"dryRun":{"type":"boolean"},"stepCount":{"type":"integer"},
            "steps":{"type":"array"},"executed":{"type":"array"},"diff":{"type":"string"},
            "plan":{"type":"array"},"undo":{"type":"string"},"note":{"type":"string"},
            "failedStep":{"type":"integer"},"stepError":{"type":"object"},"rolledBack":{"type":"boolean"},
            "atomic":{"type":"boolean"},"error":{"type":"string"},"detail":{"type":"string"},
            "line":{"type":"integer"},"errors":{"type":"array"},"unknown":{"type":"array"},
            "remediation":{"type":"string"}}})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ false},
        Profile::Full,
        [R](const Json& a) -> Json {
            const std::string script = reqStr(a, "script");
            const bool dryRun = optBool(a, "dryRun", false);
            const bool atomic = optBool(a, "atomic", true);
            const bool stopOnError = optBool(a, "stopOnError", true);

            dsl::Program prog = dsl::parse(script);

            // 1) Syntax errors — report them all at once; nothing runs, nothing mutates.
            if (!prog.errors.empty()) {
                Json errs = Json::array();
                for (const auto& e : prog.errors)
                    errs.push_back(Json{{"line", e.line}, {"code", e.code}, {"message", e.message}});
                return Json{{"ok", false}, {"error", "dsl_parse_error"},
                            {"detail", std::to_string(prog.errors.size()) + " parse error(s)"},
                            {"errors", errs}, {"stepCount", (int)prog.statements.size()},
                            {"remediation", "fix the flagged line(s); syntax is `verb key=val …`"}};
            }
            if (prog.statements.empty())
                return Json{{"ok", true}, {"stepCount", 0}, {"steps", Json::array()},
                            {"diff", std::string()}, {"note", "empty script (no statements)"}};

            // 2) Existence: every resolved tool (alias target or literal) must be registered.
            Json unknown = Json::array();
            for (const auto& st : prog.statements)
                if (!R->find(st.tool))
                    unknown.push_back(Json{{"line", st.line}, {"verb", st.verb}, {"tool", st.tool}});
            if (!unknown.empty())
                return Json{{"ok", false}, {"error", "unknown_tool"},
                            {"detail", "a verb maps to a tool that is not registered"},
                            {"unknown", unknown}, {"plan", dsl::planJson(prog)},
                            {"remediation", "use a known verb (" + dsl::knownVerbs() +
                                            ") or a registered tool name"}};

            // 2.5) Static declared-before-use + duplicate-capture (mutation-free). A forward
            //      or undeclared $ref, or a duplicate capture name, is rejected before any preview or edit.
            {
                dsl::DslError refErr;
                if (dsl::staticCheckRefs(prog, refErr))
                    return Json{{"ok", false}, {"error", refErr.code},
                                {"detail", "line " + std::to_string(refErr.line) + ": " + refErr.message},
                                {"line", refErr.line}, {"plan", dsl::planJson(prog)},
                                {"remediation", refErr.code == std::string("dsl_ref_undeclared")
                                     ? "capture the value first: `$name = <verb> …` must precede any use of $name"
                                     : "use a distinct capture name for each `$name = …`"}};
            }

            // 3) dryRun: preview every step, mutate nothing, return the composed plan + diff.
            if (dryRun) {
                Json steps = Json::array();
                bool anyErr = false;
                int i = 0;
                for (const auto& st : prog.statements) {
                    ++i;
                    Json res = previewStatement(R, st);
                    const bool err = isError(res);
                    anyErr = anyErr || err;
                    steps.push_back(stepRecord(i, st, res, err));
                    if (err && stopOnError) break;
                }
                return Json{{"ok", !anyErr}, {"dryRun", true},
                            {"stepCount", (int)prog.statements.size()}, {"steps", steps},
                            {"diff", dsl::renderDiff(prog)}, {"plan", dsl::planJson(prog)}};
            }

            // 4) Apply — validation pre-flight first: preview the previewable steps so a bad
            //    spec/style/descriptor/layout is caught with ZERO mutation before any real edit.
            {
                int idx = 0;
                for (const auto& st : prog.statements) {
                    ++idx;
                    if (st.preview == dsl::Preview::None) continue;  // mutating primitive: can't pre-flight
                    Json res = previewStatement(R, st);
                    if (isError(res))
                        return Json{{"ok", false}, {"error", "dsl_validation_error"},
                                    {"detail", "step " + std::to_string(idx) + " (" + st.verb +
                                                   ") failed pre-flight — nothing was applied"},
                                    {"failedStep", idx}, {"stepError", res},
                                    {"plan", dsl::planJson(prog)}};
                }
            }

            // 5) Real apply, wrapped in ONE undo block => the entire script is a single undo point.
            UndoTransaction txn("MCP: run_dsl (" + std::to_string(prog.statements.size()) + " steps)");
            Json executed = Json::array();
            std::map<std::string, Json> captures;  // capture name -> producing step's result
            bool failed = false;
            int failedAt = -1;
            Json stepErr;
            for (size_t i = 0; i < prog.statements.size(); ++i) {
                const dsl::Statement& st = prog.statements[i];
                Json res;
                bool err = false;

                // Resolve any $ref against earlier captures (deep copy). A missing path is a
                // step failure (atomic rollback covers it) — rare given the static declared-before-use
                // pass, but a field can be absent if the producing tool returned a different shape.
                Json args = st.args;
                if (dsl::hasRefs(st.args)) {
                    std::string unresolved;
                    Json resolved;
                    if (dsl::resolveRefs(captures, st.args, resolved, unresolved)) {
                        args = std::move(resolved);
                    } else {
                        res = makeError("dsl_ref_unresolved",
                                        "step " + std::to_string(i + 1) + " (" + st.verb + "): $ref '" +
                                            unresolved + "' did not resolve against an earlier capture");
                        err = true;  // no handler ran => this step made no mutation; txn state unchanged
                    }
                }

                if (!err) {
                    const Tool* t = R->find(st.tool);  // existence already verified above
                    try {
                        res = t->handler(args);
                        // A cleanly-RETURNED makeError is recoverable (no mutation, by codebase
                        // convention); only mark the txn dirty when a step actually did mutate.
                        if (!isError(res)) txn.markDirty();
                    } catch (const std::exception& e) {
                        res = makeError("step_threw", e.what());
                        txn.markDirty();  // a throw may have left a partial edit — ensure rollback covers it
                    }
                    err = isError(res);
                }

                executed.push_back(stepRecord((int)i + 1, st, res, err));
                // Store a successful step's FULL result under its capture name for later $refs.
                if (!err && !st.capture.empty()) captures[st.capture] = res;
                if (err) { failed = true; failedAt = (int)i + 1; stepErr = res; if (stopOnError) break; }
            }

            if (!failed) {
                txn.commit();
                return Json{{"ok", true}, {"stepCount", (int)prog.statements.size()},
                            {"steps", executed}, {"diff", dsl::renderDiff(prog)},
                            {"undo", "one undo point reverts the whole script"}};
            }

            const bool didMutate = txn.dirty();
            if (atomic) {
                txn.rollback();  // EndBlock2 + (if dirty) one Undo_DoUndo2 on the closed outer block
                return Json{{"ok", false}, {"error", "dsl_step_failed"},
                            {"detail", "step " + std::to_string(failedAt) +
                                           " failed; rolled the whole run back (atomic)"},
                            {"failedStep", failedAt}, {"stepError", stepErr}, {"executed", executed},
                            {"rolledBack", didMutate}, {"atomic", true}};
            }
            txn.commit();
            return Json{{"ok", false}, {"error", "dsl_step_failed"},
                        {"detail", "step " + std::to_string(failedAt) +
                                       " failed; kept the steps applied before it"},
                        {"failedStep", failedAt}, {"stepError", stepErr}, {"executed", executed},
                        {"rolledBack", false}, {"atomic", false},
                        {"undo", "one undo point reverts the applied steps"}};
        }});
}

}  // namespace reaper_mcp
