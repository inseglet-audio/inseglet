// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_dsl.cpp — pure unit test for the Phase-5e deterministic macro-DSL core (src/dsl/dsl.h):
// tokenizing, value coercion (typed scalars, quoted strings, comma-lists, literal JSON), the verb
// alias table + key sugar + dotted-key nesting, structured parse errors, and the explain-the-diff
// rendering. No REAPER, no SDK — every piece here is SDK-free (the run_dsl executor is protocol-tested
// separately in test_mcp_server.cpp).

#include <cstdio>
#include <map>
#include <string>

#include "dsl/dsl.h"

using namespace reaper_mcp;
using namespace reaper_mcp::dsl;

static int g_failures = 0;
static void check(bool cond, const std::string& what) {
    if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", what.c_str()); ++g_failures; }
    else       { std::fprintf(stderr, "  ok:   %s\n", what.c_str()); }
}

// Coerce a value token, returning a sentinel {"__err":why} on failure so tests can assert both paths.
static Json cv(const std::string& raw, bool forceString = false) {
    Json out;
    std::string why;
    if (!coerceValue(raw, forceString, out, why)) return Json{{"__err", why}};
    return out;
}

int main() {
    // ---- 1. scalar coercion: bool / null / int / float / string ----
    {
        check(coerceScalar("true") == true && coerceScalar("true").is_boolean(), "scalar true -> bool");
        check(coerceScalar("false") == false, "scalar false -> bool");
        check(coerceScalar("null").is_null(), "scalar null -> null");
        check(coerceScalar("42").is_number_integer() && coerceScalar("42") == 42, "scalar 42 -> int");
        check(coerceScalar("-7") == -7, "scalar -7 -> negative int");
        Json f = coerceScalar("1.5");
        check(f.is_number_float() && f.get<double>() > 1.49 && f.get<double>() < 1.51, "scalar 1.5 -> float");
        check(coerceScalar("7.1.4") == "7.1.4", "scalar 7.1.4 (two dots) -> string, not a number");
        check(coerceScalar("SN3D") == "SN3D", "scalar bareword -> string");
        check(coerceScalar("0x10") == "0x10", "scalar 0x10 -> string (hex not numerified)");
        check(coerceScalar("") == "", "scalar empty -> empty string");
    }

    // ---- 2. value coercion: quoted / JSON / comma-list / forceString ----
    {
        check(cv("\"lead vocal\"") == "lead vocal", "quoted value keeps spaces");
        check(cv("[1,2,3]").is_array() && cv("[1,2,3]").size() == 3, "bracket value -> JSON array");
        Json list = cv("1,2,3");
        check(list.is_array() && list.size() == 3 && list[0] == 1, "comma list -> array of ints");
        Json descr = cv("wide,front-center,overhead");
        check(descr.is_array() && descr[1] == "front-center", "comma list of barewords -> string array");
        Json mixed = cv("[\"wide\",{\"az\":30}]");
        check(mixed.is_array() && mixed[1].is_object() && mixed[1]["az"] == 30, "JSON array with object element");
        check(cv("{\"a\":1}").is_object() && cv("{\"a\":1}")["a"] == 1, "brace value -> JSON object");
        Json el = cv("wide,\"lead vocal\",overhead");
        check(el.is_array() && el[1] == "lead vocal", "quoted element inside a comma list");
        // string pinning: layout-like values must NOT numerify.
        check(cv("5.1", /*forceString=*/true) == "5.1" && cv("5.1", true).is_string(), "forceString keeps 5.1 a string");
        check(cv("5.1", /*forceString=*/false).is_number_float(), "auto coercion turns 5.1 into a float (why pinning matters)");
        check(cv("\"7.1.4\"", true) == "7.1.4", "forceString unwraps a quoted string");
        // malformed
        check(cv("\"unterminated").contains("__err"), "unterminated quote -> error");
        check(cv("[1,2").contains("__err"), "malformed JSON array -> error");
        check(cv("{oops").contains("__err"), "malformed JSON object -> error");
    }

    // ---- 3. tokenizer: whitespace / quotes / brackets / comments ----
    {
        auto a = splitTokens("spatialize sources=1,2,3 layout=7.1.4");
        check(a.size() == 3 && a[0] == "spatialize" && a[2] == "layout=7.1.4", "splits on top-level whitespace");
        auto b = splitTokens("apply_style track=0 style=pop-bright   # trailing comment");
        check(b.size() == 3 && b[2] == "style=pop-bright", "strips a top-level trailing comment");
        auto c = splitTokens("x a=\"b c\" d=[1, 2, 3]");
        check(c.size() == 3 && c[1] == "a=\"b c\"" && c[2] == "d=[1, 2, 3]", "whitespace inside quotes/brackets is kept");
        check(splitTokens("     ").empty(), "all-whitespace line -> no tokens");
        check(splitTokens("# whole-line comment").empty(), "comment-only line -> no tokens");
        check(splitTokens("verb key=a#b").size() == 2, "'#' mid-token is not a comment");
    }

    // ---- 4. splitTop respects strings + nesting ----
    {
        check(splitTop("a.b.c", '.').size() == 3, "dotted path splits into 3");
        check(splitTop("1,2,3", ',').size() == 3, "comma split into 3");
        check(splitTop("[1,2],3", ',').size() == 2, "bracket protects the inner comma");
        check(splitTop("\"a,b\",c", ',').size() == 2, "quotes protect the inner comma");
    }

    // ---- 5. assignPath: dotted nesting ----
    {
        Json o = Json::object();
        assignPath(o, "target.layout", Json("7.1.4"));
        assignPath(o, "target.normalization", Json("SN3D"));
        assignPath(o, "monitor", Json(true));
        check(o["target"]["layout"] == "7.1.4" && o["target"]["normalization"] == "SN3D", "nested keys merge under one object");
        check(o["monitor"] == true, "flat key stays flat alongside nested");
    }

    // ---- 6. verb table + resolution ----
    {
        check(findVerb("spatialize") && findVerb("spatialize")->tool == std::string("spatial.spatialize_stems"), "spatialize -> spatial.spatialize_stems");
        check(findVerb("apply_style")->tool == std::string("mix.apply_style"), "apply_style -> mix.apply_style");
        check(findVerb("check")->tool == std::string("analysis.check_deliverable"), "check -> analysis.check_deliverable");
        check(findVerb("to_ambisonic")->tool == std::string("spatial.stereo_to_ambisonic"), "to_ambisonic -> stereo_to_ambisonic");
        check(findVerb("nope") == nullptr, "unknown verb -> nullptr");
        check(std::string(previewName(Preview::InjectDryRun)) == "dry-run", "previewName(InjectDryRun)");
        check(std::string(previewName(Preview::ReadOnly)) == "read-only", "previewName(ReadOnly)");
        check(knownVerbs().find("spatialize") != std::string::npos, "knownVerbs lists spatialize");
        check(knownVerbs().find('\n') == std::string::npos, "knownVerbs is newline-free");
    }

    // ---- 7. parse: flagship spatialize line, sugar + nesting + typing ----
    {
        Program p = parse("# spatialize 8 stems into a 7.1.4 bed\n"
                          "spatialize sources=1,2,3,4,5,6,7,8 layout=7.1.4 placements=wide,front-center,overhead monitor=true");
        check(p.ok() && p.statements.size() == 1, "one statement parsed (comment ignored)");
        const Statement& s = p.statements[0];
        check(s.tool == "spatial.spatialize_stems" && s.aliased, "resolved to the flagship tool");
        check(s.preview == Preview::InjectDryRun, "flagship is dry-run previewable");
        check(s.args["sources"].is_array() && s.args["sources"].size() == 8 && s.args["sources"][0].is_number_integer(),
              "sources -> int array of 8");
        check(s.args["target"]["layout"] == "7.1.4" && s.args["target"]["layout"].is_string(),
              "layout sugar -> target.layout (string-pinned)");
        check(!s.args.contains("layout"), "no leftover flat 'layout' key");
        check(s.args["placements"] == Json::array({"wide", "front-center", "overhead"}), "placements comma-list -> string array");
        check(s.args["monitor"] == true, "monitor -> bool");
    }

    // ---- 8. parse: ambisonic target + normalization sugar (spatialize vs to_ambisonic) ----
    {
        Program p = parse("spatialize sources=1 order=3 norm=N3D");
        const Statement& s = p.statements[0];
        check(s.args["target"]["ambisonicOrder"] == 3, "spatialize order -> target.ambisonicOrder");
        check(s.args["target"]["normalization"] == "N3D", "spatialize norm -> target.normalization");

        Program q = parse("to_ambisonic source=1 order=1 norm=SN3D monitor=true");
        const Statement& t = q.statements[0];
        check(t.tool == "spatial.stereo_to_ambisonic", "to_ambisonic tool");
        check(t.args["order"] == 1 && !t.args.contains("target"), "to_ambisonic order stays flat (no target nesting)");
        check(t.args["normalization"] == "SN3D", "to_ambisonic norm -> flat normalization");
        check(t.args["source"] == 1 && t.args["monitor"] == true, "source int + monitor bool");
    }

    // ---- 9. parse: immersive_session, apply_style spec sugar, check measured JSON ----
    {
        Program p = parse("immersive_session bed=7.1.4 objectCount=4 monitor=true rendererSends=true confirm=true");
        const Statement& s = p.statements[0];
        check(s.tool == "spatial.setup_immersive_session", "immersive_session tool");
        check(s.args["bed"] == "7.1.4" && s.args["bed"].is_string(), "bed string-pinned to 7.1.4");
        check(s.args["objectCount"] == 4 && s.args["rendererSends"] == true && s.args["confirm"] == true, "ints + bools");

        Program q = parse("apply_style track=0 style=immersive-master-atmos spec=atmos-music");
        const Statement& t = q.statements[0];
        check(t.tool == "mix.apply_style", "apply_style tool");
        check(t.args["style"] == "immersive-master-atmos", "style passes through");
        check(t.args["targetSpec"] == "atmos-music" && !t.args.contains("spec"), "spec sugar -> targetSpec");

        Program r = parse("check spec=ebu-r128 measured={\"lufsIntegrated\":-23,\"truePeak\":-1,\"channels\":2}");
        const Statement& u = r.statements[0];
        check(u.tool == "analysis.check_deliverable", "check tool");
        check(u.args["spec"] == "ebu-r128", "spec string");
        check(u.args["measured"]["lufsIntegrated"] == -23 && u.args["measured"]["channels"] == 2, "measured JSON object preserved");
    }

    // ---- 10. literal fully-qualified tool name (the generic escape) ----
    {
        Program p = parse("fx.add track=0 name=ReaEQ");
        check(p.ok() && p.statements.size() == 1, "literal tool line parses");
        const Statement& s = p.statements[0];
        check(!s.aliased && s.tool == "fx.add" && s.preview == Preview::None, "unaliased dotted verb -> literal tool, no preview");
        check(s.args["track"] == 0 && s.args["name"] == "ReaEQ", "literal-tool args shaped normally");
    }

    // ---- 11. structured errors (and that parse collects them all, mutating nothing) ----
    {
        Program p = parse("sources=1,2,3");  // starts with an arg, no verb
        check(!p.ok() && p.errors.size() == 1 && p.errors[0].code == "missing_verb", "arg-first line -> missing_verb");

        Program q = parse("frobnicate x=1");  // no alias, no dot
        check(!q.ok() && q.errors[0].code == "unknown_verb", "unknown non-dotted verb -> unknown_verb");

        Program r = parse("spatialize sources");  // no '='
        check(!r.ok() && r.errors[0].code == "bad_arg", "arg without '=' -> bad_arg");

        Program j = parse("spatialize target={bad json");  // malformed JSON value
        check(!j.ok() && j.errors[0].code == "bad_arg", "malformed JSON value -> bad_arg");

        Program multi = parse("frobnicate a=1\nspatialize sources\nto_ambisonic source=1");
        check(multi.errors.size() == 2, "parse collects every error in one pass");
        check(multi.statements.size() == 1 && multi.statements[0].tool == "spatial.stereo_to_ambisonic",
              "valid lines still parse alongside errors");
    }

    // ---- 12. empty / whitespace / comment-only scripts ----
    {
        check(parse("").statements.empty() && parse("").ok(), "empty script -> no statements, ok");
        check(parse("\n\n   \n# just a comment\n").statements.empty(), "blank + comment script -> no statements");
    }

    // ---- 13. explain-the-diff: planJson + renderDiff ----
    {
        Program p = parse("spatialize sources=1,2 layout=7.1.4\napply_style track=0 style=pop-bright");
        Json plan = planJson(p);
        check(plan.is_array() && plan.size() == 2, "planJson has one entry per statement");
        check(plan[0]["tool"] == "spatial.spatialize_stems" && plan[0]["preview"] == "dry-run", "plan entry carries tool + preview mode");
        check(plan[0]["step"] == 1 && plan[1]["step"] == 2, "plan steps are numbered");
        std::string diff = renderDiff(p);
        check(diff.find("spatial.spatialize_stems") != std::string::npos && diff.find("mix.apply_style") != std::string::npos,
              "renderDiff names both tools");
        check(diff.find("1. ") != std::string::npos && diff.find("2. ") != std::string::npos, "renderDiff numbers the steps");
    }

    // ---- 14. Phase-6(a): $ref value coercion ----
    {
        check(cv("$bed").is_object() && cv("$bed")["$ref"] == "bed", "$bed -> {\"$ref\":\"bed\"}");
        check(cv("$bed.busTrack")["$ref"] == "bed.busTrack", "dotted $ref keeps the whole path");
        check(cv("$x", /*forceString=*/true).is_object() && cv("$x", true)["$ref"] == "x",
              "$ref overrides string-pinning (checked before the pin)");
        check(cv("\"$x\"") == "$x" && cv("\"$x\"").is_string(), "quoted \"$x\" stays a literal string (escape hatch)");
        check(cv("$").contains("__err"), "bare $ -> bad ref");
        check(cv("$a.0").contains("__err"), "$a.0 (array index) -> bad ref (dotted-only in v1)");
        check(cv("$.x").contains("__err"), "$.x (empty leading segment) -> bad ref");
        check(cv("$a.").contains("__err"), "$a. (trailing dot) -> bad ref");
        Json refs = cv("$a,$b");
        check(refs.is_array() && refs.size() == 2 && refs[0]["$ref"] == "a" && refs[1]["$ref"] == "b",
              "comma list of refs -> array of two $ref wrappers");
        Json mixed = cv("$a,plain,\"lead vocal\"");
        check(mixed.is_array() && mixed[0]["$ref"] == "a" && mixed[1] == "plain" && mixed[2] == "lead vocal",
              "refs coexist with scalars/quoted in a comma list");
    }

    // ---- 15. Phase-6(a): capture-form parsing ($name = verb …) ----
    {
        Program p = parse("$bed = build_bed layout=7.1.4");
        check(p.ok() && p.statements.size() == 1, "spaced capture line parses");
        const Statement& s = p.statements[0];
        check(s.capture == "bed", "capture name recorded on the Statement");
        check(s.tool == "spatial.build_bed" && s.aliased, "verb after the capture resolves normally");
        check(s.args["layout"] == "7.1.4" && s.args["layout"].is_string(), "args after the capture shape + pin normally");

        check(!parse("$bed=build_bed layout=7.1.4").ok(), "no-space capture rejected (collides with key=val)");
        check(parse("$bed=build_bed").errors[0].code == "bad_capture", "no-space form -> bad_capture");
        check(parse("$1 = build_bed").errors[0].code == "bad_capture", "numeric capture name -> bad_capture");
        check(parse("$a-b = build_bed").errors[0].code == "bad_capture", "non-identifier capture name -> bad_capture");
        check(parse("$ = build_bed").errors[0].code == "bad_capture", "empty capture name -> bad_capture");
        check(parse("$bed =").errors[0].code == "bad_capture", "capture with no verb -> bad_capture");

        Program q = parse("$bed = build_bed layout=7.1.4\napply_style track=$bed.busTrack style=immersive-master-atmos");
        check(q.ok() && q.statements.size() == 2, "capture + consumer both parse");
        check(q.statements[0].capture == "bed" && q.statements[1].capture.empty(), "only the producer carries a capture");
        check(q.statements[1].args["track"].is_object() && q.statements[1].args["track"]["$ref"] == "bed.busTrack",
              "consumer arg carries the $ref wrapper");
        check(q.statements[1].args["style"] == "immersive-master-atmos", "sibling arg still shapes normally");
    }

    // ---- 16. Phase-6(a): collectRefs / hasRefs / static declared-before-use ----
    {
        Program good = parse("$bed = build_bed layout=7.1.4\napply_style track=$bed.busTrack style=pop-bright");
        DslError e;
        check(!staticCheckRefs(good, e), "declared-before-use passes the static check");
        check(hasRefs(good.statements[1].args) && !hasRefs(good.statements[0].args),
              "hasRefs distinguishes ref-bearing steps");
        auto names = collectRefs(good.statements[1].args);
        check(names.size() == 1 && names[0] == "bed", "collectRefs returns the capture NAME (first path segment)");

        Program fwd = parse("apply_style track=$bed.busTrack style=pop-bright\n$bed = build_bed layout=7.1.4");
        DslError e2;
        check(staticCheckRefs(fwd, e2) && e2.code == "dsl_ref_undeclared" && e2.line == 1,
              "forward/undeclared ref -> dsl_ref_undeclared at the offending line");

        Program dup = parse("$bed = build_bed layout=7.1.4\n$bed = build_bed layout=5.1");
        DslError e3;
        check(staticCheckRefs(dup, e3) && e3.code == "dsl_dup_capture", "duplicate capture -> dsl_dup_capture");
    }

    // ---- 17. Phase-6(a): resolveRefs + refsToDisplay ----
    {
        std::map<std::string, Json> caps;
        caps["bed"] = Json{{"busTrack", 5}, {"channels", 12}};
        Json args = Json{{"track", Json{{"$ref", "bed.busTrack"}}}, {"style", "pop-bright"}};
        Json out; std::string missing;
        check(resolveRefs(caps, args, out, missing) && out["track"] == 5 && out["style"] == "pop-bright",
              "resolveRefs substitutes the captured field, leaves siblings intact");
        Json whole; std::string m2;
        check(resolveRefs(caps, Json{{"$ref", "bed"}}, whole, m2) && whole["channels"] == 12,
              "whole-capture ref resolves to the entire result object");
        Json listOut; std::string m3;
        Json listArgs = Json::array({Json{{"$ref", "bed.channels"}}, 3});
        check(resolveRefs(caps, listArgs, listOut, m3) && listOut[0] == 12 && listOut[1] == 3,
              "refs inside an array resolve element-wise");
        Json bad; std::string miss;
        check(!resolveRefs(caps, Json{{"$ref", "bed.nope"}}, bad, miss) && miss == "bed.nope",
              "missing field -> unresolved, reports the offending path");
        check(refsToDisplay(args)["track"] == "$bed.busTrack", "refsToDisplay renders a ref as $path");
    }

    if (g_failures == 0) { std::fprintf(stderr, "ALL DSL CHECKS PASSED\n"); return 0; }
    std::fprintf(stderr, "%d DSL CHECK(S) FAILED\n", g_failures);
    return 1;
}
