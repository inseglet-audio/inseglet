// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// dsl.h — the deterministic macro-DSL core for session.run_dsl.
//
// By design, the natural-language layer is a *deterministic
// macro-DSL* — `verb key=val` lines that map, one line to one tool call, onto the composite
// verbs. NO LLM sits in the execution path, so a script is reproducible and unit-testable. This header
// is the SDK-free core: it tokenizes a script, coerces each value to typed JSON, resolves the verb to a
// tool via a curated alias table, shapes the flat `key=val`s into each tool's (possibly nested) argument
// object, and renders the explain-the-diff plan. The tools_session.cpp executor adds the live pieces:
// registry existence checks, a non-mutating dry-run/validation pre-flight, and the single-undo-block
// apply. Everything here compiles identically on the host and SDK branches (it never calls a REAPER API).
//
// Grammar (one statement per line; blank lines and `#` comments ignored):
//
//     statement := verb (WS key '=' value)*
//     verb      := an alias (spatialize, apply_style, …) OR a literal tool name (spatial.build_bed)
//     key       := identifier, dotted keys nest       (target.layout=7.1.4  ->  {"target":{"layout":…}})
//     value     := "quoted string" | [json,array] | {json:object} | a,comma,list | scalar
//     scalar    := true | false | null | int | float | bareword-string
//
// Value coercion is deterministic: `true/false/null` -> bool/null; an integer or float literal that
// consumes the whole token -> number; a `[`/`{`-prefixed value -> literal JSON; a top-level comma list
// -> array (each element coerced); anything else -> string. A double-quoted value is always a string
// (so it can hold spaces/commas). Keys whose schema field is a string (layout, bed, spec, style, …) are
// pinned to string via the alias table so `layout=5.1` stays "5.1" and never numerifies to 5.1.
//
// Include order: composite_support.h pulls reaper_api.h before <json> (SWELL min/max), matching the rest
// of the tree.

#pragma once

#include "../composite_support.h"  // Json, Plan, makeError / isError

#include <cstdlib>
#include <cerrno>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace reaper_mcp {
namespace dsl {

// ---------------------------------------------------------------------------------------------
// Verb alias table (curated). Each alias maps a friendly verb to a tool name, declares how the
// dry-run/validation pre-flight may safely preview it, and carries per-key sugar so the common flat
// keys land in the tool's nested schema.
// ---------------------------------------------------------------------------------------------

// How the executor may PREVIEW a statement during dryRun / pre-flight WITHOUT mutating the project:
enum class Preview {
    InjectDryRun,  // composite verb that honors dryRun:true  -> dispatch with dryRun forced on
    ReadOnly,      // read-only tool                          -> dispatch as-is (no mutation)
    None           // a mutating primitive with no dry-run    -> do NOT execute; list it in the plan only
};

// A per-key rewrite: the flat DSL key -> the (possibly dotted) schema path, with an optional type pin.
struct KeyAlias {
    const char* from;    // DSL key as written (e.g. "layout")
    const char* toPath;  // schema path, dotted for nesting (e.g. "target.layout")
    char        type;    // 's' = force string; 'a' = auto-coerce
};

struct VerbSpec {
    const char*             verb;     // the DSL verb (an alias)
    const char*             tool;     // the registered tool name it dispatches to
    Preview                 preview;
    std::vector<KeyAlias>   keys;     // key sugar (empty = pass keys through unchanged)
};

// The table. Multiple aliases for the same tool are separate rows (explicit > clever). The headline
// composite verbs are dry-runnable; read-only analysis is previewed as-is; mutating primitives are
// listed but never executed during preview. A verb NOT found here but containing a '.' is treated as a
// literal tool name (the executor checks the registry) — so run_dsl is also a general macro runner.
inline const std::vector<VerbSpec>& verbTable() {
    static const std::vector<VerbSpec> t = {
        // ---- outcome-level composite verbs (the primary surface) ----
        {"spatialize", "spatial.spatialize_stems", Preview::InjectDryRun,
            {{"layout", "target.layout", 's'},
             {"order", "target.ambisonicOrder", 'a'},
             {"normalization", "target.normalization", 's'},
             {"norm", "target.normalization", 's'}}},
        {"spatialize_stems", "spatial.spatialize_stems", Preview::InjectDryRun,
            {{"layout", "target.layout", 's'},
             {"order", "target.ambisonicOrder", 'a'},
             {"normalization", "target.normalization", 's'},
             {"norm", "target.normalization", 's'}}},
        {"to_ambisonic", "spatial.stereo_to_ambisonic", Preview::InjectDryRun,
            {{"norm", "normalization", 's'},
             {"normalization", "normalization", 's'}}},
        {"stereo_to_ambisonic", "spatial.stereo_to_ambisonic", Preview::InjectDryRun,
            {{"norm", "normalization", 's'},
             {"normalization", "normalization", 's'}}},
        {"immersive_session", "spatial.setup_immersive_session", Preview::InjectDryRun,
            {{"bed", "bed", 's'}}},
        {"setup_immersive_session", "spatial.setup_immersive_session", Preview::InjectDryRun,
            {{"bed", "bed", 's'}}},
        {"check_deliverable", "analysis.check_deliverable", Preview::InjectDryRun,
            {{"spec", "spec", 's'}, {"target", "target", 's'}}},
        {"check", "analysis.check_deliverable", Preview::InjectDryRun,
            {{"spec", "spec", 's'}, {"target", "target", 's'}}},
        {"apply_style", "mix.apply_style", Preview::InjectDryRun,
            {{"style", "style", 's'},
             {"spec", "targetSpec", 's'},
             {"targetSpec", "targetSpec", 's'}}},
        {"style", "mix.apply_style", Preview::InjectDryRun,
            {{"style", "style", 's'},
             {"spec", "targetSpec", 's'},
             {"targetSpec", "targetSpec", 's'}}},
        // ---- curated primitives useful inside a macro script ----
        {"detect_suites", "spatial.detect_spatial_suites", Preview::ReadOnly, {}},
        {"build_bed", "spatial.build_bed", Preview::None,
            {{"layout", "layout", 's'}}},
        {"add_monitor", "spatial.add_binaural_monitor", Preview::None, {}},
        {"rotate", "spatial.rotate_scene", Preview::None, {}},
        {"render", "spatial.render_deliverables", Preview::None,
            {{"spec", "spec", 's'}}},
        {"stop_head_tracking", "spatial.stop_head_tracking", Preview::None, {}},
        {"track_add", "track.add", Preview::None, {}},
        {"set_channels", "track.set_channels", Preview::None, {}},
        {"select", "track.select", Preview::None, {}},
    };
    return t;
}

inline const VerbSpec* findVerb(const std::string& verb) {
    for (const auto& v : verbTable())
        if (verb == v.verb) return &v;
    return nullptr;
}

// A newline-free, comma-joined list of the curated verbs (for remediation hints).
inline std::string knownVerbs() {
    std::string out;
    for (const auto& v : verbTable()) { if (!out.empty()) out += ", "; out += v.verb; }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Parsed model
// ---------------------------------------------------------------------------------------------

struct Statement {
    int          line = 0;               // 1-based source line
    std::string  raw;                    // the source line (trimmed), for diffs/errors
    std::string  verb;                   // verb as written
    std::string  tool;                   // resolved tool name (alias target, or the literal verb)
    bool         aliased = false;        // matched a curated alias (vs. a literal tool name)
    Preview      preview = Preview::None; // how it may be previewed (aliases only; literals = None)
    Json         args = Json::object();  // shaped argument object
    std::string  capture;                // capture name from `$name = …` (empty = none)
};

struct DslError {
    int          line = 0;
    std::string  code;                   // missing_verb | bad_arg | unknown_verb | bad_capture | bad_ref
                                         //   (static pass also emits dsl_ref_undeclared / dsl_dup_capture)
    std::string  message;
};

struct Program {
    std::vector<Statement> statements;
    std::vector<DslError>  errors;
    bool ok() const { return errors.empty(); }
};

// ---------------------------------------------------------------------------------------------
// Low-level lexing helpers (all pure)
// ---------------------------------------------------------------------------------------------

inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// Split a source line into whitespace-separated tokens, respecting "..." strings and [ ]/{ } nesting,
// and dropping a top-level trailing `# comment`. Escapes inside strings are preserved verbatim.
inline std::vector<std::string> splitTokens(const std::string& line) {
    std::vector<std::string> toks;
    std::string cur;
    int  depth = 0;
    bool inStr = false, have = false;
    char q = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inStr) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < line.size()) { cur.push_back(line[++i]); continue; }
            if (c == q) inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; q = c; cur.push_back(c); have = true; continue; }
        if (c == '#' && depth == 0 && cur.empty()) break;  // comment to end of line (token boundary)
        if (c == '[' || c == '{') { ++depth; cur.push_back(c); have = true; continue; }
        if (c == ']' || c == '}') { if (depth > 0) --depth; cur.push_back(c); have = true; continue; }
        if ((c == ' ' || c == '\t' || c == '\r') && depth == 0) {
            if (have) { toks.push_back(cur); cur.clear(); have = false; }
            continue;
        }
        cur.push_back(c); have = true;
    }
    if (have) toks.push_back(cur);
    return toks;
}

// Split a value on a top-level separator (respecting strings + [ ]/{ } nesting).
inline std::vector<std::string> splitTop(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    int  depth = 0;
    bool inStr = false;
    char q = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < s.size()) { cur.push_back(s[++i]); continue; }
            if (c == q) inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; q = c; cur.push_back(c); continue; }
        if (c == '[' || c == '{') { ++depth; cur.push_back(c); continue; }
        if (c == ']' || c == '}') { if (depth > 0) --depth; cur.push_back(c); continue; }
        if (c == sep && depth == 0) { out.push_back(cur); cur.clear(); continue; }
        cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

inline bool parseLongLong(const std::string& s, long long& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

inline bool parseDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    // Restrict to a decimal-number shape so strtod's hex-float / inf / nan forms don't sneak a bareword
    // like "0x10", "inf", or "nan" into a JSON number — keep coercion boringly deterministic.
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E'))
            return false;
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

// ---- $ref / capture identifier helpers (pure, locale-independent) ----
inline bool isIdentChar(char c, bool first) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c == '_') return true;
    if (!first && c >= '0' && c <= '9') return true;
    return false;
}
// A capture name: a single C-style identifier ($[A-Za-z_][A-Za-z0-9_]*).
inline bool isValidCaptureName(const std::string& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (!isIdentChar(s[i], i == 0)) return false;
    return true;
}
// A dotted reference path: one or more identifier segments joined by single dots. Rejects a leading/
// trailing/double dot and a numeric segment (array indexing is out of scope in v1 — pass lists whole).
inline bool isValidRefPath(const std::string& s) {
    if (s.empty()) return false;
    size_t segLen = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '.') {
            if (segLen == 0) return false;  // empty segment
            for (size_t j = i - segLen; j < i; ++j)
                if (!isIdentChar(s[j], j == i - segLen)) return false;
            segLen = 0;
        } else {
            ++segLen;
        }
    }
    return true;
}

// Coerce a bare scalar token to typed JSON: bool/null, then int, then float, else string.
inline Json coerceScalar(const std::string& t) {
    if (t == "true") return Json(true);
    if (t == "false") return Json(false);
    if (t == "null") return Json(nullptr);
    long long iv;
    if (parseLongLong(t, iv)) return Json(iv);
    double dv;
    if (parseDouble(t, dv)) return Json(dv);
    return Json(t);
}

// Coerce a single list ELEMENT: quoted/JSON pass through Json::parse; otherwise a scalar.
inline bool coerceElement(const std::string& raw, Json& out) {
    const std::string e = trim(raw);
    // A $ref list element -> the reserved wrapper (checked before quoted/JSON/scalar).
    if (!e.empty() && e[0] == '$') {
        const std::string path = e.substr(1);
        if (!isValidRefPath(path)) return false;
        out = Json{{"$ref", path}};
        return true;
    }
    if (!e.empty() && (e[0] == '"' || e[0] == '[' || e[0] == '{')) {
        Json j = Json::parse(e, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded()) return false;
        out = std::move(j);
        return true;
    }
    out = coerceScalar(e);
    return true;
}

// Coerce a full value token. forceString pins the result to a string (for schema string fields).
// Returns false + sets `why` on a malformed quoted string / JSON literal.
inline bool coerceValue(const std::string& raw, bool forceString, Json& out, std::string& why) {
    const std::string v = trim(raw);
    // A bareword $ref resolves BEFORE the forceString pin and before the quoted/JSON/scalar
    // branches, so `layout=$x` is a reference (not the literal string "$x"). A top-level comma keeps it a
    // list whose elements coerce individually (refs handled in coerceElement); a quoted "$x" stays a
    // literal string (v[0] is then '"', so this branch is skipped — the escape hatch).
    if (!v.empty() && v[0] == '$' && splitTop(v, ',').size() == 1) {
        const std::string path = v.substr(1);
        if (!isValidRefPath(path)) { why = "invalid $ref '" + v + "'"; return false; }
        out = Json{{"$ref", path}};
        return true;
    }
    if (forceString) {
        if (!v.empty() && v[0] == '"') {
            Json j = Json::parse(v, nullptr, false);
            if (j.is_discarded() || !j.is_string()) { why = "malformed quoted string"; return false; }
            out = std::move(j);
            return true;
        }
        out = Json(v);  // literal text — never numerified (e.g. layout "5.1")
        return true;
    }
    if (v.empty()) { out = Json(std::string()); return true; }
    if (v[0] == '"') {
        Json j = Json::parse(v, nullptr, false);
        if (j.is_discarded() || !j.is_string()) { why = "malformed quoted string"; return false; }
        out = std::move(j);
        return true;
    }
    if (v[0] == '[' || v[0] == '{') {
        Json j = Json::parse(v, nullptr, false);
        if (j.is_discarded()) { why = "malformed JSON value"; return false; }
        out = std::move(j);
        return true;
    }
    // A top-level comma turns the value into an array (each element coerced).
    std::vector<std::string> parts = splitTop(v, ',');
    if (parts.size() > 1) {
        Json arr = Json::array();
        for (const auto& p : parts) {
            Json el;
            if (!coerceElement(p, el)) { why = "malformed list element '" + trim(p) + "'"; return false; }
            arr.push_back(std::move(el));
        }
        out = std::move(arr);
        return true;
    }
    out = coerceScalar(v);
    return true;
}

// Assign `value` at a dotted path inside `obj`, creating intermediate objects as needed.
inline void assignPath(Json& obj, const std::string& dotted, Json value) {
    std::vector<std::string> parts = splitTop(dotted, '.');
    Json* cur = &obj;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        Json& next = (*cur)[parts[i]];
        if (!next.is_object()) next = Json::object();
        cur = &next;
    }
    (*cur)[parts.back()] = std::move(value);
}

// ---------------------------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------------------------

// Parse one line (already split off) into a Statement, appending any error to `errs`. Returns false if
// the line produced no statement (blank/comment) or an error.
inline bool parseLine(int lineNo, const std::string& rawLine, Statement& out, std::vector<DslError>& errs) {
    const std::string line = trim(rawLine);
    std::vector<std::string> toks = splitTokens(line);
    if (toks.empty()) return false;  // blank or comment-only

    // An optional capture prefix `$name = verb …`. The SPACED form is required — splitTokens
    // yields ["$name", "=", verb, …]. The no-space form `$name=verb` collides with the key=val arg check
    // (toks[0] would contain '='), so it is rejected with a clear bad_capture error.
    std::string capture;
    if (!toks[0].empty() && toks[0][0] == '$') {
        const std::string lead = toks[0];
        if (lead.find('=') != std::string::npos) {
            errs.push_back({lineNo, "bad_capture",
                            "capture must be spaced: `$name = verb …` (got '" + lead + "')"});
            return false;
        }
        const std::string name = lead.substr(1);
        if (!isValidCaptureName(name)) {
            errs.push_back({lineNo, "bad_capture",
                            "invalid capture name '" + lead + "' (want $[A-Za-z_][A-Za-z0-9_]*)"});
            return false;
        }
        if (toks.size() < 2 || toks[1] != "=") {
            errs.push_back({lineNo, "bad_capture", "capture must be spaced: `$name = verb …`"});
            return false;
        }
        if (toks.size() < 3) {
            errs.push_back({lineNo, "bad_capture", "capture `$" + name + " =` has no verb"});
            return false;
        }
        capture = name;
        toks.erase(toks.begin(), toks.begin() + 2);  // drop "$name" and "="
    }

    const std::string verb = toks[0];
    if (verb.find('=') != std::string::npos) {
        errs.push_back({lineNo, "missing_verb", "line starts with an argument, not a verb: '" + verb + "'"});
        return false;
    }

    out = Statement{};
    out.line = lineNo;
    out.raw = line;
    out.verb = verb;
    out.capture = capture;

    const VerbSpec* spec = findVerb(verb);
    if (spec) {
        out.aliased = true;
        out.tool = spec->tool;
        out.preview = spec->preview;
    } else if (verb.find('.') != std::string::npos) {
        // A literal, fully-qualified tool name (e.g. spatial.build_bed). Existence is checked by the
        // executor against the live registry; previews are disabled for the generic escape.
        out.aliased = false;
        out.tool = verb;
        out.preview = Preview::None;
    } else {
        errs.push_back({lineNo, "unknown_verb",
                        "unknown verb '" + verb + "' (not an alias and not a tool name)"});
        return false;
    }

    bool anyErr = false;
    for (size_t i = 1; i < toks.size(); ++i) {
        const std::string& tk = toks[i];
        const size_t eq = tk.find('=');
        if (eq == std::string::npos || eq == 0) {
            errs.push_back({lineNo, "bad_arg", "argument '" + tk + "' is not key=value"});
            anyErr = true;
            continue;
        }
        const std::string key = tk.substr(0, eq);
        const std::string val = tk.substr(eq + 1);

        // Resolve key sugar (path + type pin) from the verb table.
        std::string path = key;
        bool forceString = false;
        if (spec) {
            for (const auto& ka : spec->keys) {
                if (key == ka.from) { path = ka.toPath; forceString = (ka.type == 's'); break; }
            }
        }
        Json value;
        std::string why;
        if (!coerceValue(val, forceString, value, why)) {
            // A malformed $ref gets its own parse code; everything else stays bad_arg.
            const std::string vt = trim(val);
            const char* code = (!vt.empty() && vt[0] == '$') ? "bad_ref" : "bad_arg";
            errs.push_back({lineNo, code, "key '" + key + "': " + why});
            anyErr = true;
            continue;
        }
        assignPath(out.args, path, std::move(value));
    }
    return !anyErr;
}

// Parse a whole script. Never throws: malformed lines accumulate into Program::errors while the valid
// statements still parse (so the executor can report every problem in one shot, mutating nothing).
inline Program parse(const std::string& script) {
    Program prog;
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= script.size()) {
        size_t nl = script.find('\n', pos);
        const std::string rawLine =
            (nl == std::string::npos) ? script.substr(pos) : script.substr(pos, nl - pos);
        ++lineNo;
        Statement st;
        if (parseLine(lineNo, rawLine, st, prog.errors))
            prog.statements.push_back(std::move(st));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return prog;
}

// ---------------------------------------------------------------------------------------------
// $ref / capture support — collection, static check, apply-time resolution, display.
// All pure (SDK-free); the executor supplies the captures map.
// ---------------------------------------------------------------------------------------------

// Deep-walk `v`, appending the first path segment (the capture NAME) of every {"$ref":"name.path"}.
inline void collectRefsInto(const Json& v, std::vector<std::string>& out) {
    if (v.is_object()) {
        if (v.size() == 1) {
            auto it = v.find("$ref");
            if (it != v.end() && it->is_string()) {
                const std::string p = it->get<std::string>();
                const size_t dot = p.find('.');
                out.push_back(dot == std::string::npos ? p : p.substr(0, dot));
                return;
            }
        }
        for (const auto& kv : v.items()) collectRefsInto(kv.value(), out);
    } else if (v.is_array()) {
        for (const auto& e : v) collectRefsInto(e, out);
    }
}
inline std::vector<std::string> collectRefs(const Json& args) {
    std::vector<std::string> out;
    collectRefsInto(args, out);
    return out;
}
inline bool hasRefs(const Json& args) { return !collectRefs(args).empty(); }

// Static declared-before-use + duplicate-capture check (mutation-free). Returns true and fills `err`
// (code = dsl_ref_undeclared | dsl_dup_capture) on the FIRST violation, so the executor can reject a
// bad script with ZERO edits — preserving the "catch structure errors before any mutation" guarantee.
inline bool staticCheckRefs(const Program& prog, DslError& err) {
    std::set<std::string> declared;
    for (const auto& st : prog.statements) {
        for (const auto& name : collectRefs(st.args)) {
            if (!declared.count(name)) {
                err = {st.line, "dsl_ref_undeclared", "$" + name + " is used before it is captured"};
                return true;
            }
        }
        if (!st.capture.empty()) {
            if (declared.count(st.capture)) {
                err = {st.line, "dsl_dup_capture", "$" + st.capture + " is captured more than once"};
                return true;
            }
            declared.insert(st.capture);
        }
    }
    return false;
}

// Walk the dotted segments (after the capture name at index 0) into a captured result. Returns false if
// any segment is missing or indexes a non-object. An empty tail (whole-capture ref) yields `root` itself.
inline bool resolveRefPath(const Json& root, const std::vector<std::string>& segs, size_t start,
                           Json& out) {
    const Json* cur = &root;
    for (size_t i = start; i < segs.size(); ++i) {
        if (!cur->is_object()) return false;
        auto it = cur->find(segs[i]);
        if (it == cur->end()) return false;
        cur = &(*it);
    }
    out = *cur;
    return true;
}

// Deep-copy `args`, replacing every {"$ref":"name.path"} with the value looked up in `captures`. On the
// first unresolved ref, set `unresolved` to its full path and return false.
inline bool resolveRefs(const std::map<std::string, Json>& captures, const Json& args,
                        Json& out, std::string& unresolved) {
    if (args.is_object()) {
        if (args.size() == 1) {
            auto it = args.find("$ref");
            if (it != args.end() && it->is_string()) {
                const std::string full = it->get<std::string>();
                std::vector<std::string> segs = splitTop(full, '.');
                const std::string& name = segs.empty() ? full : segs[0];
                auto cit = captures.find(name);
                if (cit == captures.end()) { unresolved = full; return false; }
                Json resolved;
                if (!resolveRefPath(cit->second, segs, 1, resolved)) { unresolved = full; return false; }
                out = std::move(resolved);
                return true;
            }
        }
        Json obj = Json::object();
        for (const auto& kv : args.items()) {
            Json child;
            if (!resolveRefs(captures, kv.value(), child, unresolved)) return false;
            obj[kv.key()] = std::move(child);
        }
        out = std::move(obj);
        return true;
    }
    if (args.is_array()) {
        Json arr = Json::array();
        for (const auto& e : args) {
            Json child;
            if (!resolveRefs(captures, e, child, unresolved)) return false;
            arr.push_back(std::move(child));
        }
        out = std::move(arr);
        return true;
    }
    out = args;
    return true;
}

// Render {"$ref":"x.y"} as the readable literal "$x.y" for the human diff/plan (leaves all else intact).
inline Json refsToDisplay(const Json& v) {
    if (v.is_object()) {
        if (v.size() == 1) {
            auto it = v.find("$ref");
            if (it != v.end() && it->is_string()) return Json("$" + it->get<std::string>());
        }
        Json o = Json::object();
        for (const auto& kv : v.items()) o[kv.key()] = refsToDisplay(kv.value());
        return o;
    }
    if (v.is_array()) {
        Json a = Json::array();
        for (const auto& e : v) a.push_back(refsToDisplay(e));
        return a;
    }
    return v;
}

// ---------------------------------------------------------------------------------------------
// Explain-the-diff (pure): a structured plan + a human-readable rendering.
// ---------------------------------------------------------------------------------------------

inline const char* previewName(Preview p) {
    switch (p) {
        case Preview::InjectDryRun: return "dry-run";
        case Preview::ReadOnly:     return "read-only";
        case Preview::None:         return "apply-only";
    }
    return "apply-only";
}

inline Json planJson(const Program& prog) {
    Json arr = Json::array();
    int n = 0;
    for (const auto& s : prog.statements) {
        Json entry{{"step", ++n},
                   {"line", s.line},
                   {"verb", s.verb},
                   {"tool", s.tool},
                   {"aliased", s.aliased},
                   {"preview", previewName(s.preview)},
                   {"args", refsToDisplay(s.args)}};
        if (!s.capture.empty()) entry["capture"] = s.capture;
        arr.push_back(std::move(entry));
    }
    return arr;
}

inline std::string renderDiff(const Program& prog) {
    std::string out;
    int n = 0;
    for (const auto& s : prog.statements) {
        out += std::to_string(++n) + ". ";
        if (!s.capture.empty()) out += "$" + s.capture + " = ";
        out += s.verb + " -> " + s.tool + " " + refsToDisplay(s.args).dump() + "\n";
    }
    return out;
}

}  // namespace dsl
}  // namespace reaper_mcp
