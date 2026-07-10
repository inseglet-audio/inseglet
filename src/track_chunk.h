// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// track_chunk.h — SDK-free helpers for the `track.set_state_chunk` verb.
//
// Two pure-C++ pieces, deliberately free of REAPER and JSON so the risky logic (structural validation
// + the current-vs-proposed diff a human confirms) is unit-tested host-side with no running REAPER:
//
//   1. validateTrackChunk() — a cheap structural sanity check: the text must begin with `<TRACK` and
//      its angle-bracket block nesting must balance (never dips below zero, ends at zero). This is the
//      rail "validate the chunk parses / begins with <TRACK before applying" — REAPER's own parser
//      is the final authority (SetTrackStateChunk returns false on a bad chunk), this just rejects the
//      obvious garbage before we ever touch the graph.
//
//   2. diffTrackChunks() — a git-style unified diff of the current vs proposed chunk, so an agent (or
//      the human answering the elicitation) sees exactly what a wholesale chunk write changes. Line
//      oriented. Strategy: trim the common leading/trailing lines (chunk edits are typically
//      localized), then run a classic LCS on the residual middle. The middle is bounded by a cell cap;
//      beyond it a single replace hunk is emitted rather than blow memory on a pathological rewrite.
//      Either way the diff faithfully represents current -> proposed. The grouping-with-context and
//      @@ range formatting mirror Python difflib.unified_diff so the output reads like real `diff -u`.

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace reaper_mcp {

// ------------------------------------------------------------------------------------------------
// 1. Structural validation (SDK-free)
// ------------------------------------------------------------------------------------------------

// Returns "" if `chunk` looks like a REAPER track state chunk; otherwise a human-readable reason.
// Lenient by design: REAPER's parser (SetTrackStateChunk) is the final gate — this only rejects text
// that plainly is not a `<TRACK ...>` block so we never hand REAPER obvious garbage.
inline std::string validateTrackChunk(const std::string& chunk) {
    // First non-whitespace must open a <TRACK block.
    std::size_t i = 0;
    while (i < chunk.size() && (chunk[i] == ' ' || chunk[i] == '\t' || chunk[i] == '\r' ||
                                chunk[i] == '\n'))
        ++i;
    if (chunk.compare(i, 6, "<TRACK") != 0)
        return "chunk does not begin with '<TRACK'";

    // Angle-bracket block nesting must balance: a line whose first non-space char is '<' opens a
    // block; a line that is exactly '>' closes one. Depth must never go negative and must end at 0.
    int depth = 0;
    std::size_t p = 0;
    const std::size_t n = chunk.size();
    while (p <= n) {
        std::size_t nl = chunk.find('\n', p);
        std::size_t end = (nl == std::string::npos) ? n : nl;
        // trim leading/trailing whitespace (incl. a CR) of the line [p,end)
        std::size_t a = p, b = end;
        while (a < b && (chunk[a] == ' ' || chunk[a] == '\t')) ++a;
        while (b > a && (chunk[b - 1] == ' ' || chunk[b - 1] == '\t' || chunk[b - 1] == '\r')) --b;
        if (a < b) {
            if (chunk[a] == '<') {
                ++depth;
            } else if (b - a == 1 && chunk[a] == '>') {
                --depth;
                if (depth < 0) return "unbalanced chunk: a '>' closes a block that was never opened";
            }
        }
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    if (depth != 0) return "unbalanced chunk: " + std::to_string(depth) + " block(s) left unclosed";
    return "";
}

// ------------------------------------------------------------------------------------------------
// 2. Unified line diff (SDK-free)
// ------------------------------------------------------------------------------------------------
// Unified-diff formatting adapted from CPython's difflib (Python Software Foundation License v2);
// see THIRD_PARTY_NOTICES.md.

struct ChunkDiff {
    std::string text;    // unified-diff text ("" when the line sequences are identical)
    bool changed = false;
    int added = 0;       // count of '+' lines
    int removed = 0;     // count of '-' lines
};

// Split into logical lines, dropping the line terminators (and a trailing CR for CRLF robustness).
// A trailing newline does not yield a final empty line, so "x\n" and "x" compare equal.
inline std::vector<std::string> splitLinesForDiff(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        if (cur.back() == '\r') cur.pop_back();
        lines.push_back(cur);
    }
    return lines;
}

enum class DiffTag { Equal, Replace, Delete, Insert };
struct DiffOp {
    DiffTag tag;
    std::size_t i1, i2;  // range in `a`
    std::size_t j1, j2;  // range in `b`
};

namespace track_chunk_detail {

// LCS backtrace over two line vectors -> ordered list of matched (i,j) index pairs.
inline std::vector<std::pair<std::size_t, std::size_t>> lcsMatches(
        const std::vector<std::string>& am, const std::vector<std::string>& bm) {
    const std::size_t n = am.size(), m = bm.size();
    std::vector<int> dp((n + 1) * (m + 1), 0);
    auto at = [&](std::size_t i, std::size_t j) -> int& { return dp[i * (m + 1) + j]; };
    for (std::size_t i = n; i-- > 0;)
        for (std::size_t j = m; j-- > 0;)
            at(i, j) = (am[i] == bm[j]) ? at(i + 1, j + 1) + 1
                                        : std::max(at(i + 1, j), at(i, j + 1));
    std::vector<std::pair<std::size_t, std::size_t>> matches;
    std::size_t i = 0, j = 0;
    while (i < n && j < m) {
        if (am[i] == bm[j]) { matches.emplace_back(i, j); ++i; ++j; }
        else if (at(i + 1, j) >= at(i, j + 1)) ++i;
        else ++j;
    }
    return matches;
}

}  // namespace track_chunk_detail

// Full opcode list (difflib SequenceMatcher.get_opcodes-equivalent) over a vs b. Common prefix/suffix
// are trimmed first; the residual middle is aligned by LCS, or — if its cost exceeds `lcsCellCap` —
// emitted as one replace/delete/insert op.
inline std::vector<DiffOp> computeChunkOpcodes(const std::vector<std::string>& a,
                                               const std::vector<std::string>& b,
                                               std::size_t lcsCellCap = 4000000) {
    std::vector<DiffOp> ops;
    std::size_t p = 0;
    while (p < a.size() && p < b.size() && a[p] == b[p]) ++p;
    std::size_t sa = a.size(), sb = b.size();
    while (sa > p && sb > p && a[sa - 1] == b[sb - 1]) { --sa; --sb; }

    if (p > 0) ops.push_back({DiffTag::Equal, 0, p, 0, p});

    const std::size_t nMid = sa - p, mMid = sb - p;
    if (nMid == 0 && mMid == 0) {
        // no middle difference
    } else if (nMid == 0) {
        ops.push_back({DiffTag::Insert, p, p, p, sb});
    } else if (mMid == 0) {
        ops.push_back({DiffTag::Delete, p, sa, p, p});
    } else if (nMid * mMid > lcsCellCap) {
        ops.push_back({DiffTag::Replace, p, sa, p, sb});
    } else {
        std::vector<std::string> am(a.begin() + p, a.begin() + sa);
        std::vector<std::string> bm(b.begin() + p, b.begin() + sb);
        auto matches = track_chunk_detail::lcsMatches(am, bm);
        // Coalesce consecutive matches into blocks, then walk blocks (+ a terminator) into opcodes.
        std::size_t i = 0, j = 0, k = 0;
        while (true) {
            std::size_t ai, bj, size;
            if (k < matches.size()) {
                ai = matches[k].first;
                bj = matches[k].second;
                size = 1;
                while (k + 1 < matches.size() && matches[k + 1].first == matches[k].first + 1 &&
                       matches[k + 1].second == matches[k].second + 1) { ++k; ++size; }
                ++k;
            } else {
                ai = nMid; bj = mMid; size = 0;  // terminator
            }
            if (i < ai && j < bj) ops.push_back({DiffTag::Replace, p + i, p + ai, p + j, p + bj});
            else if (i < ai) ops.push_back({DiffTag::Delete, p + i, p + ai, p + j, p + j});
            else if (j < bj) ops.push_back({DiffTag::Insert, p + i, p + i, p + j, p + bj});
            if (size > 0) {
                ops.push_back({DiffTag::Equal, p + ai, p + ai + size, p + bj, p + bj + size});
                i = ai + size; j = bj + size;
            } else {
                break;
            }
        }
    }

    if (a.size() - sa > 0) ops.push_back({DiffTag::Equal, sa, a.size(), sb, b.size()});
    return ops;
}

namespace track_chunk_detail {

// difflib._format_range_unified: [start, stop) -> "beginning" or "beginning,length" (1-based).
inline std::string formatRange(std::size_t start, std::size_t stop) {
    std::size_t beginning = start + 1;
    std::size_t length = stop - start;
    if (length == 1) return std::to_string(beginning);
    if (length == 0) beginning -= 1;
    return std::to_string(beginning) + "," + std::to_string(length);
}

// difflib.SequenceMatcher.get_grouped_opcodes: split into hunks carrying `context` lines around each
// change, trimming the leading/trailing context of the first/last hunk.
inline std::vector<std::vector<DiffOp>> groupOpcodes(std::vector<DiffOp> codes, std::size_t context) {
    std::vector<std::vector<DiffOp>> groups;
    if (codes.empty()) return groups;

    auto backoff = [](std::size_t v, std::size_t n) -> std::size_t { return v > n ? v - n : 0; };
    if (codes.front().tag == DiffTag::Equal) {
        DiffOp& c = codes.front();
        c.i1 = std::max(c.i1, backoff(c.i2, context));
        c.j1 = std::max(c.j1, backoff(c.j2, context));
    }
    if (codes.back().tag == DiffTag::Equal) {
        DiffOp& c = codes.back();
        c.i2 = std::min(c.i2, c.i1 + context);
        c.j2 = std::min(c.j2, c.j1 + context);
    }

    std::vector<DiffOp> group;
    for (const DiffOp& c : codes) {
        if (c.tag == DiffTag::Equal && (c.i2 - c.i1) > context * 2) {
            group.push_back({DiffTag::Equal, c.i1, std::min(c.i2, c.i1 + context), c.j1,
                             std::min(c.j2, c.j1 + context)});
            groups.push_back(group);
            group.clear();
            group.push_back({DiffTag::Equal, std::max(c.i1, backoff(c.i2, context)), c.i2,
                             std::max(c.j1, backoff(c.j2, context)), c.j2});
            continue;
        }
        group.push_back(c);
    }
    if (!group.empty() && !(group.size() == 1 && group.front().tag == DiffTag::Equal))
        groups.push_back(group);
    return groups;
}

}  // namespace track_chunk_detail

// Git-style unified diff of `before` (current) vs `after` (proposed), by line, with `context` lines of
// surrounding context per hunk. Returns changed=false + empty text when the line sequences match.
inline ChunkDiff diffTrackChunks(const std::string& before, const std::string& after,
                                 std::size_t context = 3) {
    ChunkDiff r;
    std::vector<std::string> a = splitLinesForDiff(before);
    std::vector<std::string> b = splitLinesForDiff(after);
    if (a == b) return r;  // changed=false, empty diff
    r.changed = true;

    std::vector<DiffOp> codes = computeChunkOpcodes(a, b);
    for (const DiffOp& c : codes) {
        if (c.tag == DiffTag::Delete || c.tag == DiffTag::Replace) r.removed += (int)(c.i2 - c.i1);
        if (c.tag == DiffTag::Insert || c.tag == DiffTag::Replace) r.added += (int)(c.j2 - c.j1);
    }

    std::vector<std::vector<DiffOp>> groups = track_chunk_detail::groupOpcodes(codes, context);
    if (groups.empty()) return r;  // (shouldn't happen once a != b, but stay safe)

    std::string out = "--- current\n+++ proposed\n";
    for (const std::vector<DiffOp>& group : groups) {
        std::size_t i1 = group.front().i1, i2 = group.back().i2;
        std::size_t j1 = group.front().j1, j2 = group.back().j2;
        out += "@@ -" + track_chunk_detail::formatRange(i1, i2) + " +" +
               track_chunk_detail::formatRange(j1, j2) + " @@\n";
        for (const DiffOp& c : group) {
            if (c.tag == DiffTag::Equal) {
                for (std::size_t k = c.i1; k < c.i2; ++k) out += " " + a[k] + "\n";
            } else {
                if (c.tag == DiffTag::Replace || c.tag == DiffTag::Delete)
                    for (std::size_t k = c.i1; k < c.i2; ++k) out += "-" + a[k] + "\n";
                if (c.tag == DiffTag::Replace || c.tag == DiffTag::Insert)
                    for (std::size_t k = c.j1; k < c.j2; ++k) out += "+" + b[k] + "\n";
            }
        }
    }
    r.text = std::move(out);
    return r;
}

// ------------------------------------------------------------------------------------------------
// 3. Targeted top-level key patch (SDK-free) — the substrate for `track.patch_state`.
// ------------------------------------------------------------------------------------------------
//
// A narrower, safer alternative to a wholesale `track.set_state_chunk`: replace the value of one or
// more TOP-LEVEL scalar keys of the outer <TRACK ...> block (NAME, PEAKCOL, VOLPAN, MUTESOLO, REC …)
// while leaving every nested block (<FXCHAIN>, <ITEM>, envelopes …) byte-for-byte untouched. Keys
// inside nested blocks are deliberately NOT reachable here — those stay the wholesale-write domain.
// Pure C++/std so key resolution + round-trip fidelity are unit-tested with no running REAPER.

struct ChunkKeyPatch {
    std::string key;    // a top-level bareword key, e.g. "NAME"
    std::string value;  // the single-line value that follows the key (may be empty)
};

struct ChunkPatchResult {
    std::string chunk;                      // patched chunk (empty on error)
    std::string error;                      // "" on success, else a machine code ("key_not_found")
    std::string detail;                     // human-readable detail on error
    std::vector<std::string> appliedKeys;   // keys located + replaced, in request order
    std::vector<std::string> missingKeys;   // requested keys absent at the top level, in request order
};

// Shape-check a patch set independent of any chunk: non-empty bareword keys, single-line values, no
// duplicate keys. Returns "" if OK, else a human-readable reason (mirrors validateTrackChunk's role
// for the wholesale path).
inline std::string validateKeyPatches(const std::vector<ChunkKeyPatch>& patches) {
    if (patches.empty()) return "no patches supplied";
    std::vector<std::string> seen;
    for (const ChunkKeyPatch& p : patches) {
        if (p.key.empty()) return "a patch key is empty";
        for (char c : p.key)
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '<' || c == '>')
                return "invalid patch key '" + p.key +
                       "': keys are single barewords (no whitespace or angle brackets)";
        if (p.value.find('\n') != std::string::npos || p.value.find('\r') != std::string::npos)
            return "patch value for '" + p.key + "' must be a single line (no newline)";
        for (const std::string& s : seen)
            if (s == p.key) return "duplicate patch key '" + p.key + "'";
        seen.push_back(p.key);
    }
    return "";
}

// Replace the value of each named TOP-LEVEL key of the outer <TRACK> block. "Top level" = a content
// line at block depth 1 (directly inside <TRACK>, not within a nested <...> sub-block), matched by
// its first whitespace-delimited token. Every other byte (nested blocks, untouched keys, line
// terminators, a trailing newline) is preserved verbatim, so patching a key to its CURRENT value
// yields a byte-identical chunk — a genuine no-op. A requested key absent at the top level is NOT
// invented: it is reported in missingKeys and the whole patch fails closed with error="key_not_found".
inline ChunkPatchResult applyTrackChunkKeyPatches(const std::string& chunk,
                                                  const std::vector<ChunkKeyPatch>& patches) {
    ChunkPatchResult r;

    // Split into lines, dropping '\n' terminators; remember whether a trailing newline was present.
    std::vector<std::string> lines;
    std::string cur;
    for (char c : chunk) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) lines.push_back(cur);
    const bool endsNL = !chunk.empty() && chunk.back() == '\n';

    std::vector<bool> applied(patches.size(), false);
    int depth = 0;
    for (std::string& line : lines) {
        std::size_t a = 0, b = line.size();
        while (a < b && (line[a] == ' ' || line[a] == '\t')) ++a;
        std::size_t e = b;
        while (e > a && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) --e;
        if (a >= e) continue;  // blank line: not an opener/closer/key

        const char first = line[a];
        if (e - a == 1 && first == '>') { if (depth > 0) --depth; continue; }  // block closer
        if (first == '<') { ++depth; continue; }                               // block opener

        if (depth != 1) continue;  // a key, but nested deeper than the track's top level
        std::size_t k = a;
        while (k < e && line[k] != ' ' && line[k] != '\t') ++k;
        const std::string key = line.substr(a, k - a);
        for (std::size_t pi = 0; pi < patches.size(); ++pi) {
            if (applied[pi] || patches[pi].key != key) continue;
            std::string rebuilt = line.substr(0, a) + patches[pi].key;
            if (!patches[pi].value.empty()) rebuilt += " " + patches[pi].value;
            line = rebuilt;
            applied[pi] = true;
            break;
        }
    }

    for (std::size_t pi = 0; pi < patches.size(); ++pi) {
        if (applied[pi]) r.appliedKeys.push_back(patches[pi].key);
        else r.missingKeys.push_back(patches[pi].key);
    }
    if (!r.missingKeys.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < r.missingKeys.size(); ++i) {
            if (i) joined += ", ";
            joined += r.missingKeys[i];
        }
        r.error = "key_not_found";
        r.detail = "top-level track key(s) not present in the chunk: " + joined;
        return r;
    }

    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size()) out += '\n';
    }
    if (endsNL) out += '\n';
    r.chunk = std::move(out);
    return r;
}

}  // namespace reaper_mcp
