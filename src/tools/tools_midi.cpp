// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// tools_midi.cpp — takes + MIDI note/CC CRUD, and
// the authoring substrate the immersive layer needs to place object/bed automation events.
//
// MIDI in REAPER lives in a *take* on a media *item*. The workflow these tools expose:
//   midi.create_item            -> make an empty MIDI item+take on a track (returns its item index)
//   midi.insert_note / list / delete
//   midi.insert_cc   / list / delete
//   take.list / set_active / set_name
//
// Note timing is exposed in **quarter-note beats relative to the item start** (the musician-facing
// unit), converted to the take's native PPQ through MIDI_GetPPQPosFromProjQN/MIDI_GetProjQNFromPPQPos
// so it is exact regardless of the take's PPQ resolution. Channels are 0..15 (REAPER MIDI API native).
// Dual-path (native under REAPER_MCP_HAVE_SDK, schema-valid echo otherwise); mutations are single-undo.

#include <string>

#include "tool_helpers.h"
#include "../tool_registry.h"
#include "../groove.h"

namespace reaper_mcp {

#ifdef REAPER_MCP_HAVE_SDK
namespace {

std::string itemGuidOf(MediaItem* it) {
    char buf[64] = {0};
    if (it) GetSetMediaItemInfo_String(it, "GUID", buf, false);
    return buf;
}

// Index of an item within its track's item list (items are position-sorted, so a freshly created
// item is not necessarily the last). Returns -1 if not found.
int itemIndexOf(MediaTrack* t, MediaItem* it) {
    const int n = CountTrackMediaItems(t);
    for (int i = 0; i < n; ++i)
        if (GetTrackMediaItem(t, i) == it) return i;
    return -1;
}

// Active take index (or -1) for take.list reporting.
int activeTakeIndex(MediaItem* it) {
    MediaItem_Take* act = GetActiveTake(it);
    const int n = CountTakes(it);
    for (int i = 0; i < n; ++i)
        if (GetTake(it, i) == act) return i;
    return -1;
}

// Project quarter-note position of a take's start (PPQ 0), used to convert item-relative beats.
double takeStartQN(MediaItem_Take* tk) { return MIDI_GetProjQNFromPPQPos(tk, 0.0); }

// chanmsg high-nibble -> wire message-type string (list_cc reporting).
const char* ccTypeName(int chanmsg) {
    switch (chanmsg & 0xF0) {
        case 0xA0: return "poly_pressure";
        case 0xB0: return "cc";
        case 0xC0: return "program";
        case 0xD0: return "channel_pressure";
        case 0xE0: return "pitch";
        default:   return "other";
    }
}

// messageType (wire) -> chanmsg high nibble.
int chanMsgForType(const std::string& t) {
    if (t == "program") return 0xC0;
    if (t == "channel_pressure") return 0xD0;
    if (t == "pitch") return 0xE0;
    if (t == "poly_pressure") return 0xA0;
    return 0xB0;  // "cc"
}

// One note snapshot in item-relative beats — read once so a batch edit can address notes by their
// stable index while sorting is deferred to a single MIDI_Sort() at the end.
struct NoteRec {
    int idx; bool sel; bool mut; double sBeats; double eBeats; int chan; int pitch; int vel;
};
std::vector<NoteRec> readNoteRecs(MediaItem_Take* tk, double startQN) {
    int notes = 0, ccs = 0, syx = 0;
    MIDI_CountEvts(tk, &notes, &ccs, &syx);
    std::vector<NoteRec> v;
    v.reserve(notes > 0 ? (size_t)notes : 0);
    for (int i = 0; i < notes; ++i) {
        bool s = false, m = false; double sp = 0, ep = 0; int c = 0, p = 0, ve = 0;
        if (!MIDI_GetNote(tk, i, &s, &m, &sp, &ep, &c, &p, &ve)) continue;
        v.push_back(NoteRec{i, s, m, MIDI_GetProjQNFromPPQPos(tk, sp) - startQN,
                            MIDI_GetProjQNFromPPQPos(tk, ep) - startQN, c, p, ve});
    }
    return v;
}
// Write one note's new start/length (beats) + optional new velocity/pitch back in place (deferred sort).
void writeNote(MediaItem_Take* tk, double startQN, const NoteRec& r, double newSBeats,
               double newEBeats, int newVel, int newPitch, bool* noSort) {
    double sppq = MIDI_GetPPQPosFromProjQN(tk, startQN + newSBeats);
    double eppq = MIDI_GetPPQPosFromProjQN(tk, startQN + newEBeats);
    bool sel = r.sel, mut = r.mut; int chan = r.chan; int pitch = newPitch; int vel = newVel;
    MIDI_SetNote(tk, r.idx, &sel, &mut, &sppq, &eppq, &chan, &pitch, &vel, noSort);
}
}  // namespace
#endif  // REAPER_MCP_HAVE_SDK

void registerMidiTools(ToolRegistry& reg) {
    // ---- midi.create_item (mutating; undo-wrapped) ----
    reg.add(Tool{
        "midi.create_item",
        "Create a new empty MIDI item (with one active MIDI take) on a track, at a position and "
        "length in seconds. Returns the item's index for use with the midi.* / take.* tools.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "position":{"type":"number","minimum":0},"length":{"type":"number","exclusiveMinimum":0},
            "name":{"type":"string"}},
            "required":["track"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},"item":{"type":"integer"},
            "position":{"type":"number"},"length":{"type":"number"},"guid":{"type":"string"}},
            "required":["track","item","position","length"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ false, /*idempotent*/ false},
        Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const double pos = optNum(a, "position", 0.0);
            const double len = optNum(a, "length", 2.0);
            const std::string name = optStr(a, "name", "");
#ifdef REAPER_MCP_HAVE_SDK
            MediaTrack* t = requireTrack(trackIdx);
            Undo_BeginBlock2(kCur);
            MediaItem* it = CreateNewMIDIItemInProj(t, pos, pos + len, nullptr);
            if (!name.empty() && it) {
                if (MediaItem_Take* tk = GetActiveTake(it)) setTakeString(tk, "P_NAME", name);
            }
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: create MIDI item", -1);
            if (!it) throw std::runtime_error("could not create MIDI item");
            return Json{{"track", trackIdx}, {"item", itemIndexOf(t, it)},
                        {"position", pos}, {"length", len}, {"guid", itemGuidOf(it)}};
#else
            (void)name;
            return Json{{"track", trackIdx}, {"item", 0}, {"position", pos}, {"length", len},
                        {"guid", "{00000000-0000-0000-0000-000000000000}"}};
#endif
        }});

    // ---- take.list (read-only) ----
    reg.add(Tool{
        "take.list",
        "List an item's takes with index, name, whether active, and whether MIDI.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},"item":{"type":"integer"},
            "takeCount":{"type":"integer"},"activeTake":{"type":"integer"},
            "takes":{"type":"array","items":{"type":"object","properties":{"index":{"type":"integer"},
                "name":{"type":"string"},"active":{"type":"boolean"},"isMidi":{"type":"boolean"}}}}},
            "required":["track","item","takeCount","activeTake","takes"]})"),
        ToolAnnotations{true, false, true}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            const int n = CountTakes(it);
            const int active = activeTakeIndex(it);
            Json arr = Json::array();
            for (int i = 0; i < n; ++i) {
                MediaItem_Take* tk = GetTake(it, i);
                arr.push_back(Json{{"index", i}, {"name", takeNameStr(tk)},
                                   {"active", i == active}, {"isMidi", tk && TakeIsMIDI(tk)}});
            }
            return Json{{"track", trackIdx}, {"item", itemIdx}, {"takeCount", n},
                        {"activeTake", active}, {"takes", std::move(arr)}};
#else
            return Json{{"track", trackIdx}, {"item", itemIdx}, {"takeCount", 0},
                        {"activeTake", -1}, {"takes", Json::array()}};
#endif
        }});

    // ---- take.set_active (mutating; idempotent; undo-wrapped) ----
    reg.add(Tool{
        "take.set_active", "Set which take of an item is active, by take index.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":0}},
            "required":["track","item","take"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"activeTake":{"type":"integer"}},
            "required":["ok","activeTake"]})"),
        ToolAnnotations{false, false, true}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = reqInt(a, "take");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            Undo_BeginBlock2(kCur);
            SetActiveTake(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set active take", -1);
            return Json{{"ok", true}, {"activeTake", activeTakeIndex(it)}};
#else
            (void)trackIdx; (void)itemIdx;
            return Json{{"ok", true}, {"activeTake", takeIdx}};
#endif
        }});

    // ---- take.set_name (mutating; idempotent; undo-wrapped) ----
    reg.add(Tool{
        "take.set_name", "Set a take's name (default: the active take).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "name":{"type":"string"}},
            "required":["track","item","name"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"take":{"type":"integer"},
            "name":{"type":"string"}},"required":["ok","name"]})"),
        ToolAnnotations{false, false, true}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const std::string name = reqStr(a, "name");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            Undo_BeginBlock2(kCur);
            setTakeString(tk, "P_NAME", name);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: set take name", -1);
            return Json{{"ok", true}, {"take", takeIdx}, {"name", name}};
#else
            (void)trackIdx; (void)itemIdx;
            return Json{{"ok", true}, {"take", takeIdx}, {"name", name}};
#endif
        }});

    // ---- midi.insert_note (mutating; undo-wrapped) ----
    reg.add(Tool{
        "midi.insert_note",
        "Insert a MIDI note into a take. Timing is in quarter-note beats relative to the item start "
        "(startBeats, lengthBeats). pitch 0..127 (60 = middle C), velocity 1..127, channel 0..15.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "startBeats":{"type":"number","minimum":0},"lengthBeats":{"type":"number","exclusiveMinimum":0},
            "pitch":{"type":"integer","minimum":0,"maximum":127},
            "velocity":{"type":"integer","minimum":1,"maximum":127,"default":96},
            "channel":{"type":"integer","minimum":0,"maximum":15,"default":0},
            "selected":{"type":"boolean","default":false},"muted":{"type":"boolean","default":false}},
            "required":["track","item","startBeats","pitch"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"pitch":{"type":"integer"},
            "startBeats":{"type":"number"},"lengthBeats":{"type":"number"},"channel":{"type":"integer"},
            "velocity":{"type":"integer"},"noteCount":{"type":"integer"}},
            "required":["ok","pitch","startBeats","noteCount"]})"),
        ToolAnnotations{false, false, false}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double startBeats = reqNum(a, "startBeats");
            const double lengthBeats = optNum(a, "lengthBeats", 1.0);
            const int pitch = reqInt(a, "pitch");
            const int vel = optInt(a, "velocity", 96);
            const int chan = optInt(a, "channel", 0);
            const bool selected = optBool(a, "selected", false);
            const bool muted = optBool(a, "muted", false);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double startQN = takeStartQN(tk);
            const double ppqStart = MIDI_GetPPQPosFromProjQN(tk, startQN + startBeats);
            const double ppqEnd = MIDI_GetPPQPosFromProjQN(tk, startQN + startBeats + lengthBeats);
            bool noSort = true;  // defer sorting; MIDI_Sort() once below
            Undo_BeginBlock2(kCur);
            const bool ok = MIDI_InsertNote(tk, selected, muted, ppqStart, ppqEnd, chan, pitch, vel, &noSort);
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: insert MIDI note", -1);
            int notes = 0, ccs = 0, syx = 0;
            MIDI_CountEvts(tk, &notes, &ccs, &syx);
            return Json{{"ok", ok}, {"pitch", pitch}, {"startBeats", startBeats},
                        {"lengthBeats", lengthBeats}, {"channel", chan}, {"velocity", vel},
                        {"noteCount", notes}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)selected; (void)muted;
            return Json{{"ok", true}, {"pitch", pitch}, {"startBeats", startBeats},
                        {"lengthBeats", lengthBeats}, {"channel", chan}, {"velocity", vel},
                        {"noteCount", 1}};
#endif
        }});

    // ---- midi.list_notes (read-only) ----
    reg.add(Tool{
        "midi.list_notes", "List the notes in a MIDI take (default: active take), with beats, pitch, "
                           "velocity, channel and flags.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},"item":{"type":"integer"},
            "noteCount":{"type":"integer"},"notes":{"type":"array","items":{"type":"object","properties":{
                "index":{"type":"integer"},"startBeats":{"type":"number"},"lengthBeats":{"type":"number"},
                "startTime":{"type":"number"},"pitch":{"type":"integer"},"velocity":{"type":"integer"},
                "channel":{"type":"integer"},"selected":{"type":"boolean"},"muted":{"type":"boolean"}}}}},
            "required":["track","item","noteCount","notes"]})"),
        ToolAnnotations{true, false, true}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            int notes = 0, ccs = 0, syx = 0;
            MIDI_CountEvts(tk, &notes, &ccs, &syx);
            const double startQN = takeStartQN(tk);
            Json arr = Json::array();
            for (int i = 0; i < notes; ++i) {
                bool sel = false, mut = false;
                double sppq = 0, eppq = 0;
                int chan = 0, pitch = 0, vel = 0;
                if (!MIDI_GetNote(tk, i, &sel, &mut, &sppq, &eppq, &chan, &pitch, &vel)) continue;
                const double startBeats = MIDI_GetProjQNFromPPQPos(tk, sppq) - startQN;
                const double endBeats = MIDI_GetProjQNFromPPQPos(tk, eppq) - startQN;
                arr.push_back(Json{{"index", i}, {"startBeats", startBeats},
                                   {"lengthBeats", endBeats - startBeats},
                                   {"startTime", MIDI_GetProjTimeFromPPQPos(tk, sppq)},
                                   {"pitch", pitch}, {"velocity", vel}, {"channel", chan},
                                   {"selected", sel}, {"muted", mut}});
            }
            return Json{{"track", trackIdx}, {"item", itemIdx}, {"noteCount", notes},
                        {"notes", std::move(arr)}};
#else
            (void)takeIdx;
            return Json{{"track", trackIdx}, {"item", itemIdx}, {"noteCount", 0},
                        {"notes", Json::array()}};
#endif
        }});

    // ---- midi.delete_note (mutating; DESTRUCTIVE; undo-wrapped) ----
    reg.add(Tool{
        "midi.delete_note", "Delete a note from a MIDI take by its index (from midi.list_notes).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "index":{"type":"integer","minimum":0}},
            "required":["track","item","index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"deletedIndex":{"type":"integer"},
            "noteCount":{"type":"integer"}},"required":["ok","deletedIndex"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false},
        Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const int noteIdx = reqInt(a, "index");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            Undo_BeginBlock2(kCur);
            const bool ok = MIDI_DeleteNote(tk, noteIdx);
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: delete MIDI note", -1);
            int notes = 0, ccs = 0, syx = 0;
            MIDI_CountEvts(tk, &notes, &ccs, &syx);
            return Json{{"ok", ok}, {"deletedIndex", noteIdx}, {"noteCount", notes}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx;
            return Json{{"ok", true}, {"deletedIndex", noteIdx}, {"noteCount", 0}};
#endif
        }});

    // ---- midi.insert_cc (mutating; undo-wrapped) ----
    reg.add(Tool{
        "midi.insert_cc",
        "Insert a MIDI CC / program / channel-pressure / pitch-bend event into a take at a "
        "quarter-note beat position (relative to item start). messageType default 'cc': set 'cc' "
        "(controller 0..127) and 'value' 0..127. For 'pitch', value is 0..16383 (8192 = center).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "beats":{"type":"number","minimum":0},
            "messageType":{"type":"string","enum":["cc","program","channel_pressure","pitch","poly_pressure"],
                "default":"cc"},
            "channel":{"type":"integer","minimum":0,"maximum":15,"default":0},
            "cc":{"type":"integer","minimum":0,"maximum":127,"default":1},
            "value":{"type":"integer","minimum":0,"maximum":16383,"default":0},
            "selected":{"type":"boolean","default":false},"muted":{"type":"boolean","default":false}},
            "required":["track","item","beats"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"messageType":{"type":"string"},
            "channel":{"type":"integer"},"cc":{"type":"integer"},"value":{"type":"integer"},
            "ccCount":{"type":"integer"}},"required":["ok","messageType","ccCount"]})"),
        ToolAnnotations{false, false, false}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double beats = reqNum(a, "beats");
            const std::string type = optStr(a, "messageType", "cc");
            const int chan = optInt(a, "channel", 0);
            const int cc = optInt(a, "cc", 1);
            const int value = optInt(a, "value", 0);
            const bool selected = optBool(a, "selected", false);
            const bool muted = optBool(a, "muted", false);
#ifdef REAPER_MCP_HAVE_SDK
            const int chanmsg = chanMsgForType(type);
            // Encode msg2/msg3 per message class.
            int msg2 = 0, msg3 = 0;
            if (chanmsg == 0xB0) { msg2 = cc & 0x7F; msg3 = value & 0x7F; }          // cc: num, val
            else if (chanmsg == 0xE0) { msg2 = value & 0x7F; msg3 = (value >> 7) & 0x7F; }  // pitch 14-bit
            else { msg2 = value & 0x7F; msg3 = 0; }  // program / channel_pressure / poly: single data byte
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double ppq = MIDI_GetPPQPosFromProjQN(tk, takeStartQN(tk) + beats);
            Undo_BeginBlock2(kCur);
            const bool ok = MIDI_InsertCC(tk, selected, muted, ppq, chanmsg, chan, msg2, msg3);
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: insert MIDI CC", -1);
            int notes = 0, ccs = 0, syx = 0;
            MIDI_CountEvts(tk, &notes, &ccs, &syx);
            return Json{{"ok", ok}, {"messageType", type}, {"channel", chan}, {"cc", cc},
                        {"value", value}, {"ccCount", ccs}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)beats; (void)selected; (void)muted;
            return Json{{"ok", true}, {"messageType", type}, {"channel", chan}, {"cc", cc},
                        {"value", value}, {"ccCount", 1}};
#endif
        }});

    // ---- midi.list_cc (read-only) ----
    reg.add(Tool{
        "midi.list_cc", "List the CC / program / pitch / pressure events in a MIDI take.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},"item":{"type":"integer"},
            "ccCount":{"type":"integer"},"events":{"type":"array","items":{"type":"object","properties":{
                "index":{"type":"integer"},"beats":{"type":"number"},"messageType":{"type":"string"},
                "channel":{"type":"integer"},"msg2":{"type":"integer"},"msg3":{"type":"integer"},
                "value":{"type":"integer"}}}}},
            "required":["track","item","ccCount","events"]})"),
        ToolAnnotations{true, false, true}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            int notes = 0, ccs = 0, syx = 0;
            MIDI_CountEvts(tk, &notes, &ccs, &syx);
            const double startQN = takeStartQN(tk);
            Json arr = Json::array();
            for (int i = 0; i < ccs; ++i) {
                bool sel = false, mut = false;
                double ppq = 0;
                int chanmsg = 0, chan = 0, msg2 = 0, msg3 = 0;
                if (!MIDI_GetCC(tk, i, &sel, &mut, &ppq, &chanmsg, &chan, &msg2, &msg3)) continue;
                const char* type = ccTypeName(chanmsg);
                // Decode a human "value": 14-bit for pitch, msg3 for cc, msg2 otherwise.
                int value = msg2;
                if ((chanmsg & 0xF0) == 0xB0) value = msg3;
                else if ((chanmsg & 0xF0) == 0xE0) value = (msg2 & 0x7F) | ((msg3 & 0x7F) << 7);
                arr.push_back(Json{{"index", i},
                                   {"beats", MIDI_GetProjQNFromPPQPos(tk, ppq) - startQN},
                                   {"messageType", type}, {"channel", chan},
                                   {"msg2", msg2}, {"msg3", msg3}, {"value", value}});
            }
            return Json{{"track", trackIdx}, {"item", itemIdx}, {"ccCount", ccs},
                        {"events", std::move(arr)}};
#else
            (void)takeIdx;
            return Json{{"track", trackIdx}, {"item", itemIdx}, {"ccCount", 0},
                        {"events", Json::array()}};
#endif
        }});

    // ---- midi.delete_cc (mutating; DESTRUCTIVE; undo-wrapped) ----
    reg.add(Tool{
        "midi.delete_cc", "Delete a CC/other event from a MIDI take by its index (from midi.list_cc).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "index":{"type":"integer","minimum":0}},
            "required":["track","item","index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"deletedIndex":{"type":"integer"},
            "ccCount":{"type":"integer"}},"required":["ok","deletedIndex"]})"),
        ToolAnnotations{/*readOnly*/ false, /*destructive*/ true, /*idempotent*/ false},
        Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const int ccIdx = reqInt(a, "index");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            Undo_BeginBlock2(kCur);
            const bool ok = MIDI_DeleteCC(tk, ccIdx);
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: delete MIDI CC", -1);
            int notes = 0, ccs = 0, syx = 0;
            MIDI_CountEvts(tk, &notes, &ccs, &syx);
            return Json{{"ok", ok}, {"deletedIndex", ccIdx}, {"ccCount", ccs}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx;
            return Json{{"ok", true}, {"deletedIndex", ccIdx}, {"ccCount", 0}};
#endif
        }});

    // =====================================================================================
    // MIDI depth — in-place note/CC edits, batch insert, criteria select.
    // =====================================================================================

    // ---- midi.set_note (mutating; edit in place; undo-wrapped) ----
    reg.add(Tool{
        "midi.set_note",
        "Edit a MIDI note in place by its index (from midi.list_notes). Only provided fields change "
        "(startBeats/lengthBeats/pitch/velocity/channel/selected/muted). Note indices are re-sorted "
        "by time after an edit — re-list before further index-addressed edits.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "index":{"type":"integer","minimum":0},
            "startBeats":{"type":"number","minimum":0},"lengthBeats":{"type":"number","exclusiveMinimum":0},
            "pitch":{"type":"integer","minimum":0,"maximum":127},
            "velocity":{"type":"integer","minimum":1,"maximum":127},
            "channel":{"type":"integer","minimum":0,"maximum":15},
            "selected":{"type":"boolean"},"muted":{"type":"boolean"}},
            "required":["track","item","index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"index":{"type":"integer"},
            "noteCount":{"type":"integer"}},"required":["ok","index"]})"),
        ToolAnnotations{false, false, true}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const int noteIdx = reqInt(a, "index");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            // Read the current note so a partial start/length edit keeps the other timing leg.
            bool curSel = false, curMut = false;
            double curSppq = 0, curEppq = 0;
            int curChan = 0, curPitch = 0, curVel = 0;
            if (!MIDI_GetNote(tk, noteIdx, &curSel, &curMut, &curSppq, &curEppq, &curChan, &curPitch,
                              &curVel))
                throw std::runtime_error("note index out of range: " + std::to_string(noteIdx));
            const double startQN = takeStartQN(tk);
            const double curStartBeats = MIDI_GetProjQNFromPPQPos(tk, curSppq) - startQN;
            const double curEndBeats = MIDI_GetProjQNFromPPQPos(tk, curEppq) - startQN;
            const bool hasStart = a.contains("startBeats") && a["startBeats"].is_number();
            const bool hasLen = a.contains("lengthBeats") && a["lengthBeats"].is_number();
            double sppq = curSppq, eppq = curEppq;
            if (hasStart || hasLen) {
                const double sb = hasStart ? a["startBeats"].get<double>() : curStartBeats;
                const double lb = hasLen ? a["lengthBeats"].get<double>()
                                         : (curEndBeats - curStartBeats);
                sppq = MIDI_GetPPQPosFromProjQN(tk, startQN + sb);
                eppq = MIDI_GetPPQPosFromProjQN(tk, startQN + sb + lb);
            }
            const bool sel = optBool(a, "selected", curSel);
            const bool mut = optBool(a, "muted", curMut);
            const int chan = optInt(a, "channel", curChan);
            const int pitch = optInt(a, "pitch", curPitch);
            const int vel = optInt(a, "velocity", curVel);
            bool noSort = true;
            Undo_BeginBlock2(kCur);
            const bool ok = MIDI_SetNote(tk, noteIdx, &sel, &mut, &sppq, &eppq, &chan, &pitch, &vel,
                                         &noSort);
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: edit MIDI note", -1);
            int notes = 0, ccs = 0, syx = 0;
            MIDI_CountEvts(tk, &notes, &ccs, &syx);
            if (!ok) throw std::runtime_error("could not edit note " + std::to_string(noteIdx));
            return Json{{"ok", true}, {"index", noteIdx}, {"noteCount", notes}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx;
            return Json{{"ok", true}, {"index", noteIdx}, {"noteCount", 1}};
#endif
        }});

    // ---- midi.set_cc (mutating; edit in place; undo-wrapped) ----
    reg.add(Tool{
        "midi.set_cc",
        "Edit a CC/program/pressure/pitch event in place by its index (from midi.list_cc). Only "
        "provided fields change (beats/channel/cc/value/selected/muted). 'value' is decoded per the "
        "event's message type (pitch = 14-bit 0..16383, cc = controller value, else the data byte).",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "index":{"type":"integer","minimum":0},"beats":{"type":"number","minimum":0},
            "channel":{"type":"integer","minimum":0,"maximum":15},
            "cc":{"type":"integer","minimum":0,"maximum":127},
            "value":{"type":"integer","minimum":0,"maximum":16383},
            "selected":{"type":"boolean"},"muted":{"type":"boolean"}},
            "required":["track","item","index"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"index":{"type":"integer"},
            "ccCount":{"type":"integer"}},"required":["ok","index"]})"),
        ToolAnnotations{false, false, true}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const int ccIdx = reqInt(a, "index");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            bool curSel = false, curMut = false;
            double curPpq = 0;
            int chanmsg = 0, curChan = 0, curMsg2 = 0, curMsg3 = 0;
            if (!MIDI_GetCC(tk, ccIdx, &curSel, &curMut, &curPpq, &chanmsg, &curChan, &curMsg2,
                            &curMsg3))
                throw std::runtime_error("CC index out of range: " + std::to_string(ccIdx));
            double ppq = curPpq;
            if (a.contains("beats") && a["beats"].is_number())
                ppq = MIDI_GetPPQPosFromProjQN(tk, takeStartQN(tk) + a["beats"].get<double>());
            int msg2 = curMsg2, msg3 = curMsg3;
            if (a.contains("cc") && a["cc"].is_number() && (chanmsg & 0xF0) == 0xB0)
                msg2 = a["cc"].get<int>() & 0x7F;
            if (a.contains("value") && a["value"].is_number()) {
                const int value = a["value"].get<int>();
                if ((chanmsg & 0xF0) == 0xB0) msg3 = value & 0x7F;                      // cc value
                else if ((chanmsg & 0xF0) == 0xE0) {                                    // pitch 14-bit
                    msg2 = value & 0x7F;
                    msg3 = (value >> 7) & 0x7F;
                } else {                                                                // single byte
                    msg2 = value & 0x7F;
                }
            }
            const bool sel = optBool(a, "selected", curSel);
            const bool mut = optBool(a, "muted", curMut);
            const int chan = optInt(a, "channel", curChan);
            bool noSort = true;
            Undo_BeginBlock2(kCur);
            const bool ok = MIDI_SetCC(tk, ccIdx, &sel, &mut, &ppq, &chanmsg, &chan, &msg2, &msg3,
                                       &noSort);
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: edit MIDI CC", -1);
            int notes = 0, ccs = 0, syx = 0;
            MIDI_CountEvts(tk, &notes, &ccs, &syx);
            if (!ok) throw std::runtime_error("could not edit CC " + std::to_string(ccIdx));
            return Json{{"ok", true}, {"index", ccIdx}, {"ccCount", ccs}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx;
            return Json{{"ok", true}, {"index", ccIdx}, {"ccCount", 1}};
#endif
        }});

    // ---- midi.insert_notes (mutating; batch; one undo block, one sort) ----
    reg.add(Tool{
        "midi.insert_notes",
        "Insert a batch of MIDI notes into a take in one undo block (deferred sort — much faster "
        "than repeated midi.insert_note for chords/phrases). Each note: startBeats + pitch required; "
        "lengthBeats default 1, velocity default 96, channel default 0.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "notes":{"type":"array","minItems":1,"maxItems":4096,"items":{"type":"object","properties":{
                "startBeats":{"type":"number","minimum":0},
                "lengthBeats":{"type":"number","exclusiveMinimum":0,"default":1},
                "pitch":{"type":"integer","minimum":0,"maximum":127},
                "velocity":{"type":"integer","minimum":1,"maximum":127,"default":96},
                "channel":{"type":"integer","minimum":0,"maximum":15,"default":0},
                "selected":{"type":"boolean","default":false},"muted":{"type":"boolean","default":false}},
                "required":["startBeats","pitch"]}}},
            "required":["track","item","notes"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"inserted":{"type":"integer"},
            "noteCount":{"type":"integer"}},"required":["ok","inserted"]})"),
        ToolAnnotations{false, false, false}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            if (!a.contains("notes") || !a["notes"].is_array() || a["notes"].empty())
                throw std::runtime_error("missing/invalid array arg: notes");
            const Json& notesArg = a["notes"];
            for (const auto& n : notesArg) {
                if (!n.is_object() || !n.contains("startBeats") || !n["startBeats"].is_number() ||
                    !n.contains("pitch") || !n["pitch"].is_number())
                    throw std::runtime_error("each note needs numeric startBeats + pitch");
            }
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double startQN = takeStartQN(tk);
            bool noSort = true;
            int inserted = 0;
            Undo_BeginBlock2(kCur);
            for (const auto& n : notesArg) {
                const double sb = n["startBeats"].get<double>();
                const double lb = (n.contains("lengthBeats") && n["lengthBeats"].is_number())
                                      ? n["lengthBeats"].get<double>() : 1.0;
                const int pitch = n["pitch"].get<int>();
                const int vel = optInt(n, "velocity", 96);
                const int chan = optInt(n, "channel", 0);
                const bool sel = optBool(n, "selected", false);
                const bool mut = optBool(n, "muted", false);
                const double sppq = MIDI_GetPPQPosFromProjQN(tk, startQN + sb);
                const double eppq = MIDI_GetPPQPosFromProjQN(tk, startQN + sb + lb);
                if (MIDI_InsertNote(tk, sel, mut, sppq, eppq, chan, pitch, vel, &noSort)) ++inserted;
            }
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: insert MIDI notes (batch)", -1);
            int notes = 0, ccs = 0, syx = 0;
            MIDI_CountEvts(tk, &notes, &ccs, &syx);
            return Json{{"ok", inserted == (int)notesArg.size()}, {"inserted", inserted},
                        {"noteCount", notes}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx;
            return Json{{"ok", true}, {"inserted", (int)notesArg.size()},
                        {"noteCount", (int)notesArg.size()}};
#endif
        }});

    // ---- midi.select_notes (mutating; selection only; idempotent) ----
    reg.add(Tool{
        "midi.select_notes",
        "Select or deselect notes in a MIDI take. With no filters, applies to ALL MIDI content "
        "(fast path). Filters: pitchMin/pitchMax, channel, startBeats/endBeats window.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "selected":{"type":"boolean","default":true},
            "pitchMin":{"type":"integer","minimum":0,"maximum":127},
            "pitchMax":{"type":"integer","minimum":0,"maximum":127},
            "channel":{"type":"integer","minimum":0,"maximum":15},
            "startBeats":{"type":"number","minimum":0},"endBeats":{"type":"number","minimum":0}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"matched":{"type":"integer"},
            "selected":{"type":"boolean"}},"required":["ok","selected"]})"),
        ToolAnnotations{false, false, true}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track");
            const int itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const bool select = optBool(a, "selected", true);
            const bool hasFilter = a.contains("pitchMin") || a.contains("pitchMax") ||
                                   a.contains("channel") || a.contains("startBeats") ||
                                   a.contains("endBeats");
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            Undo_BeginBlock2(kCur);
            int matched = 0;
            if (!hasFilter) {
                MIDI_SelectAll(tk, select);
                int notes = 0, ccs = 0, syx = 0;
                MIDI_CountEvts(tk, &notes, &ccs, &syx);
                matched = notes;
            } else {
                const int pMin = optInt(a, "pitchMin", 0);
                const int pMax = optInt(a, "pitchMax", 127);
                const bool hasChan = a.contains("channel") && a["channel"].is_number();
                const int wantChan = optInt(a, "channel", 0);
                const bool hasStart = a.contains("startBeats") && a["startBeats"].is_number();
                const bool hasEnd = a.contains("endBeats") && a["endBeats"].is_number();
                const double sbMin = optNum(a, "startBeats", 0.0);
                const double sbMax = optNum(a, "endBeats", 0.0);
                const double startQN = takeStartQN(tk);
                int notes = 0, ccs = 0, syx = 0;
                MIDI_CountEvts(tk, &notes, &ccs, &syx);
                bool noSort = true;
                for (int i = 0; i < notes; ++i) {
                    bool sel = false, mut = false;
                    double sppq = 0, eppq = 0;
                    int chan = 0, pitch = 0, vel = 0;
                    if (!MIDI_GetNote(tk, i, &sel, &mut, &sppq, &eppq, &chan, &pitch, &vel)) continue;
                    const double sb = MIDI_GetProjQNFromPPQPos(tk, sppq) - startQN;
                    if (pitch < pMin || pitch > pMax) continue;
                    if (hasChan && chan != wantChan) continue;
                    if (hasStart && sb < sbMin) continue;
                    if (hasEnd && sb >= sbMax) continue;
                    MIDI_SetNote(tk, i, &select, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                 &noSort);
                    ++matched;
                }
                MIDI_Sort(tk);
            }
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: select MIDI notes", -1);
            return Json{{"ok", true}, {"matched", matched}, {"selected", select}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)hasFilter;
            return Json{{"ok", true}, {"matched", 0}, {"selected", select}};
#endif
        }});

    // =====================================================================================
    // Groove & quantization — timing/velocity transforms over the existing MIDI CRUD. The math is
    // the SDK-free reaper_mcp::groove:: layer (unit-tested in unit.groove); the handlers only marshal
    // notes in/out. Positions are item-relative beats; each edit is one undo block + one MIDI_Sort.
    // =====================================================================================

    // ---- midi.quantize ----
    reg.add(Tool{
        "midi.quantize",
        "Quantize note start positions to a grid (beats). strength 0..1 (1 = full snap), swing 0..1 "
        "pushes off-beat grid slots later. quantizeEnds also snaps note ends; otherwise length is kept. "
        "selectedOnly limits the edit to selected notes.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "grid":{"type":"number","exclusiveMinimum":0,"default":0.25},
            "strength":{"type":"number","minimum":0,"maximum":1,"default":1},
            "swing":{"type":"number","minimum":0,"maximum":1,"default":0},
            "quantizeEnds":{"type":"boolean","default":false},
            "selectedOnly":{"type":"boolean","default":false}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"changed":{"type":"integer"},
            "noteCount":{"type":"integer"}},"required":["ok","changed"]})"),
        ToolAnnotations{false, false, false}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track"), itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double grid = optNum(a, "grid", 0.25);
            const double strength = optNum(a, "strength", 1.0);
            const double swing = optNum(a, "swing", 0.0);
            const bool qEnds = optBool(a, "quantizeEnds", false);
            const bool selOnly = optBool(a, "selectedOnly", false);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double startQN = takeStartQN(tk);
            auto recs = readNoteRecs(tk, startQN);
            bool noSort = true; int changed = 0;
            Undo_BeginBlock2(kCur);
            for (auto& r : recs) {
                if (selOnly && !r.sel) continue;
                double ns = groove::quantizePos(r.sBeats, grid, strength, swing);
                if (ns < 0.0) ns = 0.0;
                double ne;
                if (qEnds) {
                    ne = groove::quantizePos(r.eBeats, grid, strength, swing);
                    double len = ne - ns;
                    if (len < 1e-6) len = r.eBeats - r.sBeats;
                    ne = ns + len;
                } else {
                    ne = ns + (r.eBeats - r.sBeats);
                }
                writeNote(tk, startQN, r, ns, ne, r.vel, r.pitch, &noSort);
                ++changed;
            }
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: quantize MIDI", -1);
            int nn = 0, cc = 0, sx = 0; MIDI_CountEvts(tk, &nn, &cc, &sx);
            return Json{{"ok", true}, {"changed", changed}, {"noteCount", nn}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)grid; (void)strength; (void)swing;
            (void)qEnds; (void)selOnly;
            return Json{{"ok", true}, {"changed", 0}, {"noteCount", 0}};
#endif
        }});

    // ---- midi.humanize ----
    reg.add(Tool{
        "midi.humanize",
        "Randomly nudge note timing (+/- timing beats) and velocity (+/- velocity) for a human feel. "
        "Deterministic given 'seed' (same seed -> same result). selectedOnly limits to selected notes.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "timing":{"type":"number","minimum":0,"default":0.02},
            "velocity":{"type":"integer","minimum":0,"default":8},
            "seed":{"type":"integer","minimum":0,"default":1},
            "selectedOnly":{"type":"boolean","default":false}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"changed":{"type":"integer"},
            "noteCount":{"type":"integer"}},"required":["ok","changed"]})"),
        ToolAnnotations{false, false, false}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track"), itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double timing = optNum(a, "timing", 0.02);
            const int vamt = optInt(a, "velocity", 8);
            const unsigned seed = (unsigned)optInt(a, "seed", 1);
            const bool selOnly = optBool(a, "selectedOnly", false);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double startQN = takeStartQN(tk);
            auto recs = readNoteRecs(tk, startQN);
            groove::Rng rng((uint64_t)seed);
            bool noSort = true; int changed = 0;
            Undo_BeginBlock2(kCur);
            for (auto& r : recs) {
                if (selOnly && !r.sel) continue;
                double ns = groove::humanizeTiming(r.sBeats, timing, rng);
                if (ns < 0.0) ns = 0.0;
                double ne = ns + (r.eBeats - r.sBeats);
                int nv = groove::humanizeVelocity(r.vel, vamt, rng, 1, 127);
                writeNote(tk, startQN, r, ns, ne, nv, r.pitch, &noSort);
                ++changed;
            }
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: humanize MIDI", -1);
            int nn = 0, cc = 0, sx = 0; MIDI_CountEvts(tk, &nn, &cc, &sx);
            return Json{{"ok", true}, {"changed", changed}, {"noteCount", nn}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)timing; (void)vamt; (void)seed;
            (void)selOnly;
            return Json{{"ok", true}, {"changed", 0}, {"noteCount", 0}};
#endif
        }});

    // ---- midi.apply_groove ----
    reg.add(Tool{
        "midi.apply_groove",
        "Apply a groove template to notes: per-grid-slot timing offsets + velocity scaling. Provide a "
        "'template' array of {timeOffset (beats), velScale} slots (e.g. from midi.extract_groove), or a "
        "named 'groove' (swing8 | swing16) with an 'amount' 0..1. strength 0..1 scales the applied feel.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "grid":{"type":"number","exclusiveMinimum":0,"default":0.25},
            "strength":{"type":"number","minimum":0,"maximum":1,"default":1},
            "template":{"type":"array","items":{"type":"object","properties":{
                "timeOffset":{"type":"number"},"velScale":{"type":"number"}}}},
            "groove":{"type":"string","enum":["swing8","swing16"]},
            "amount":{"type":"number","minimum":0,"maximum":1,"default":0.33},
            "selectedOnly":{"type":"boolean","default":false}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"changed":{"type":"integer"},
            "slots":{"type":"integer"},"noteCount":{"type":"integer"}},"required":["ok","changed"]})"),
        ToolAnnotations{false, false, false}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track"), itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double strength = optNum(a, "strength", 1.0);
            const bool selOnly = optBool(a, "selectedOnly", false);
            double grid = optNum(a, "grid", 0.25);
            // Build the template from an explicit array or a named groove (SDK-independent).
            std::vector<groove::GrooveStep> tmpl;
            if (a.contains("template") && a["template"].is_array() && !a["template"].empty()) {
                for (const auto& e : a["template"]) {
                    groove::GrooveStep g;
                    g.timeOffset = e.value("timeOffset", 0.0);
                    g.velScale = e.value("velScale", 1.0);
                    tmpl.push_back(g);
                }
            } else if (a.contains("groove") && a["groove"].is_string()) {
                const std::string gname = a["groove"].get<std::string>();
                const double amount = optNum(a, "amount", 0.33);
                if (!a.contains("grid")) grid = (gname == "swing16") ? 0.25 : 0.5;
                tmpl.push_back(groove::GrooveStep{0.0, 1.0});
                tmpl.push_back(groove::GrooveStep{amount * grid, 1.0});
            } else {
                throw std::runtime_error("provide 'template' (array of slots) or 'groove' (name)");
            }
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double startQN = takeStartQN(tk);
            auto recs = readNoteRecs(tk, startQN);
            bool noSort = true; int changed = 0;
            Undo_BeginBlock2(kCur);
            for (auto& r : recs) {
                if (selOnly && !r.sel) continue;
                double nb; int nv;
                groove::applyGroove(r.sBeats, r.vel, grid, tmpl, strength, nb, nv);
                if (nb < 0.0) nb = 0.0;
                double ne = nb + (r.eBeats - r.sBeats);
                writeNote(tk, startQN, r, nb, ne, nv, r.pitch, &noSort);
                ++changed;
            }
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: apply groove", -1);
            int nn = 0, cc = 0, sx = 0; MIDI_CountEvts(tk, &nn, &cc, &sx);
            return Json{{"ok", true}, {"changed", changed}, {"slots", (int)tmpl.size()},
                        {"noteCount", nn}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)strength; (void)selOnly; (void)grid;
            return Json{{"ok", true}, {"changed", 0}, {"slots", (int)tmpl.size()}, {"noteCount", 0}};
#endif
        }});

    // ---- midi.extract_groove (read-only) ----
    reg.add(Tool{
        "midi.extract_groove",
        "Analyze a MIDI take and return a groove template: per-grid-slot mean timing deviation (beats) "
        "and mean velocity ratio (vs refVelocity). Feed the returned 'template' to midi.apply_groove to "
        "stamp this feel onto another take. Read-only.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "grid":{"type":"number","exclusiveMinimum":0,"default":0.25},
            "steps":{"type":"integer","minimum":1,"maximum":64,"default":2},
            "refVelocity":{"type":"integer","minimum":1,"maximum":127,"default":96},
            "selectedOnly":{"type":"boolean","default":false}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"track":{"type":"integer"},"item":{"type":"integer"},
            "grid":{"type":"number"},"steps":{"type":"integer"},"noteCount":{"type":"integer"},
            "template":{"type":"array","items":{"type":"object","properties":{
                "timeOffset":{"type":"number"},"velScale":{"type":"number"}}}}},
            "required":["grid","steps","template"]})"),
        ToolAnnotations{/*readOnly*/ true, false, /*idempotent*/ true}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track"), itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double grid = optNum(a, "grid", 0.25);
            const int steps = optInt(a, "steps", 2);
            const int refVel = optInt(a, "refVelocity", 96);
            const bool selOnly = optBool(a, "selectedOnly", false);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double startQN = takeStartQN(tk);
            auto recs = readNoteRecs(tk, startQN);
            std::vector<std::pair<double, int>> pairs;
            for (auto& r : recs) {
                if (selOnly && !r.sel) continue;
                pairs.emplace_back(r.sBeats, r.vel);
            }
            auto t = groove::extractGroove(pairs, grid, steps, refVel);
            Json arr = Json::array();
            for (const auto& g : t)
                arr.push_back(Json{{"timeOffset", g.timeOffset}, {"velScale", g.velScale}});
            return Json{{"track", trackIdx}, {"item", itemIdx}, {"grid", grid}, {"steps", (int)t.size()},
                        {"noteCount", (int)pairs.size()}, {"template", std::move(arr)}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)refVel; (void)selOnly;
            return Json{{"track", trackIdx}, {"item", itemIdx}, {"grid", grid}, {"steps", steps},
                        {"noteCount", 0}, {"template", Json::array()}};
#endif
        }});

    // ---- midi.transpose ----
    reg.add(Tool{
        "midi.transpose",
        "Transpose notes by a number of semitones (can be negative). Notes that would leave 0..127 are "
        "left unchanged and counted as skipped. selectedOnly limits to selected notes.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "semitones":{"type":"integer"},"selectedOnly":{"type":"boolean","default":false}},
            "required":["track","item","semitones"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"moved":{"type":"integer"},
            "skipped":{"type":"integer"},"noteCount":{"type":"integer"}},"required":["ok","moved"]})"),
        ToolAnnotations{false, false, false}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track"), itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const int semis = reqInt(a, "semitones");
            const bool selOnly = optBool(a, "selectedOnly", false);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double startQN = takeStartQN(tk);
            auto recs = readNoteRecs(tk, startQN);
            bool noSort = true; int moved = 0, skipped = 0;
            Undo_BeginBlock2(kCur);
            for (auto& r : recs) {
                if (selOnly && !r.sel) continue;
                int np;
                if (groove::transposePitch(r.pitch, semis, np)) {
                    writeNote(tk, startQN, r, r.sBeats, r.eBeats, r.vel, np, &noSort);
                    ++moved;
                } else {
                    ++skipped;
                }
            }
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: transpose MIDI", -1);
            int nn = 0, cc = 0, sx = 0; MIDI_CountEvts(tk, &nn, &cc, &sx);
            return Json{{"ok", true}, {"moved", moved}, {"skipped", skipped}, {"noteCount", nn}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)semis; (void)selOnly;
            return Json{{"ok", true}, {"moved", 0}, {"skipped", 0}, {"noteCount", 0}};
#endif
        }});

    // ---- midi.scale_velocity ----
    reg.add(Tool{
        "midi.scale_velocity",
        "Scale note velocities around a center: v' = center + (v-center)*scale + offset, clamped to "
        "1..127. scale>1 expands dynamics, <1 compresses. selectedOnly limits to selected notes.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "scale":{"type":"number","minimum":0,"default":1},"offset":{"type":"number","default":0},
            "center":{"type":"integer","minimum":0,"maximum":127,"default":64},
            "selectedOnly":{"type":"boolean","default":false}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"changed":{"type":"integer"},
            "noteCount":{"type":"integer"}},"required":["ok","changed"]})"),
        ToolAnnotations{false, false, false}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track"), itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double scale = optNum(a, "scale", 1.0);
            const double offset = optNum(a, "offset", 0.0);
            const int center = optInt(a, "center", 64);
            const bool selOnly = optBool(a, "selectedOnly", false);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double startQN = takeStartQN(tk);
            auto recs = readNoteRecs(tk, startQN);
            bool noSort = true; int changed = 0;
            Undo_BeginBlock2(kCur);
            for (auto& r : recs) {
                if (selOnly && !r.sel) continue;
                int nv = groove::scaleVelocity(r.vel, scale, offset, center, 1, 127);
                writeNote(tk, startQN, r, r.sBeats, r.eBeats, nv, r.pitch, &noSort);
                ++changed;
            }
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: scale MIDI velocity", -1);
            int nn = 0, cc = 0, sx = 0; MIDI_CountEvts(tk, &nn, &cc, &sx);
            return Json{{"ok", true}, {"changed", changed}, {"noteCount", nn}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)scale; (void)offset; (void)center;
            (void)selOnly;
            return Json{{"ok", true}, {"changed", 0}, {"noteCount", 0}};
#endif
        }});

    // ---- midi.legato ----
    reg.add(Tool{
        "midi.legato",
        "Stretch each note's length so it reaches the next note's start (minus 'gap' beats), floored at "
        "'minLength'. Notes are processed in time order; the last note keeps its length. selectedOnly "
        "treats only the selected notes as the sequence.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "gap":{"type":"number","minimum":0,"default":0},
            "minLength":{"type":"number","exclusiveMinimum":0,"default":0.03125},
            "selectedOnly":{"type":"boolean","default":false}},
            "required":["track","item"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"changed":{"type":"integer"},
            "noteCount":{"type":"integer"}},"required":["ok","changed"]})"),
        ToolAnnotations{false, false, false}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track"), itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double gap = optNum(a, "gap", 0.0);
            const double minLen = optNum(a, "minLength", 0.03125);
            const bool selOnly = optBool(a, "selectedOnly", false);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double startQN = takeStartQN(tk);
            auto recs = readNoteRecs(tk, startQN);
            std::vector<NoteRec> proc;
            for (auto& r : recs) { if (!selOnly || r.sel) proc.push_back(r); }
            std::sort(proc.begin(), proc.end(),
                      [](const NoteRec& x, const NoteRec& y) { return x.sBeats < y.sBeats; });
            std::vector<std::pair<double, double>> pairs;
            pairs.reserve(proc.size());
            for (auto& r : proc) pairs.emplace_back(r.sBeats, r.eBeats - r.sBeats);
            auto lens = groove::legatoLengths(pairs, gap, minLen);
            bool noSort = true; int changed = 0;
            Undo_BeginBlock2(kCur);
            for (size_t i = 0; i < proc.size(); ++i) {
                writeNote(tk, startQN, proc[i], proc[i].sBeats, proc[i].sBeats + lens[i],
                          proc[i].vel, proc[i].pitch, &noSort);
                ++changed;
            }
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: legato MIDI", -1);
            int nn = 0, cc = 0, sx = 0; MIDI_CountEvts(tk, &nn, &cc, &sx);
            return Json{{"ok", true}, {"changed", changed}, {"noteCount", nn}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)gap; (void)minLen; (void)selOnly;
            return Json{{"ok", true}, {"changed", 0}, {"noteCount", 0}};
#endif
        }});

    // ---- midi.nudge ----
    reg.add(Tool{
        "midi.nudge",
        "Shift note start positions by 'beats' (can be negative; starts clamp at 0), keeping length. "
        "selectedOnly limits to selected notes.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "beats":{"type":"number"},"selectedOnly":{"type":"boolean","default":false}},
            "required":["track","item","beats"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"moved":{"type":"integer"},
            "noteCount":{"type":"integer"}},"required":["ok","moved"]})"),
        ToolAnnotations{false, false, false}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track"), itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double delta = reqNum(a, "beats");
            const bool selOnly = optBool(a, "selectedOnly", false);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double startQN = takeStartQN(tk);
            auto recs = readNoteRecs(tk, startQN);
            bool noSort = true; int moved = 0;
            Undo_BeginBlock2(kCur);
            for (auto& r : recs) {
                if (selOnly && !r.sel) continue;
                double ns = r.sBeats + delta;
                if (ns < 0.0) ns = 0.0;
                double ne = ns + (r.eBeats - r.sBeats);
                writeNote(tk, startQN, r, ns, ne, r.vel, r.pitch, &noSort);
                ++moved;
            }
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: nudge MIDI", -1);
            int nn = 0, cc = 0, sx = 0; MIDI_CountEvts(tk, &nn, &cc, &sx);
            return Json{{"ok", true}, {"moved", moved}, {"noteCount", nn}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)delta; (void)selOnly;
            return Json{{"ok", true}, {"moved", 0}, {"noteCount", 0}};
#endif
        }});

    // ---- midi.stretch ----
    reg.add(Tool{
        "midi.stretch",
        "Time-scale note positions and lengths around an anchor beat: pos' = anchor + (pos-anchor)*"
        "factor, length' = length*factor. factor>1 slows/spreads, <1 tightens. selectedOnly limits to "
        "selected notes.",
        jparse(R"({"type":"object","properties":{"track":{"type":"integer","minimum":0},
            "item":{"type":"integer","minimum":0},"take":{"type":"integer","minimum":-1,"default":-1},
            "factor":{"type":"number","exclusiveMinimum":0},
            "anchorBeats":{"type":"number","minimum":0,"default":0},
            "selectedOnly":{"type":"boolean","default":false}},
            "required":["track","item","factor"],"additionalProperties":false})"),
        jparse(R"({"type":"object","properties":{"ok":{"type":"boolean"},"changed":{"type":"integer"},
            "noteCount":{"type":"integer"}},"required":["ok","changed"]})"),
        ToolAnnotations{false, false, false}, Profile::Midi,
        [](const Json& a) -> Json {
            const int trackIdx = reqInt(a, "track"), itemIdx = reqInt(a, "item");
            const int takeIdx = optInt(a, "take", -1);
            const double factor = reqNum(a, "factor");
            if (factor <= 0.0) throw std::runtime_error("factor must be > 0");
            const double anchor = optNum(a, "anchorBeats", 0.0);
            const bool selOnly = optBool(a, "selectedOnly", false);
#ifdef REAPER_MCP_HAVE_SDK
            MediaItem* it = requireItem(trackIdx, itemIdx);
            MediaItem_Take* tk = requireTake(it, takeIdx);
            if (!TakeIsMIDI(tk)) throw std::runtime_error("take is not a MIDI take");
            const double startQN = takeStartQN(tk);
            auto recs = readNoteRecs(tk, startQN);
            bool noSort = true; int changed = 0;
            Undo_BeginBlock2(kCur);
            for (auto& r : recs) {
                if (selOnly && !r.sel) continue;
                double ns = groove::stretchPos(r.sBeats, anchor, factor);
                if (ns < 0.0) ns = 0.0;
                double ne = ns + (r.eBeats - r.sBeats) * factor;
                writeNote(tk, startQN, r, ns, ne, r.vel, r.pitch, &noSort);
                ++changed;
            }
            MIDI_Sort(tk);
            UpdateArrange();
            Undo_EndBlock2(kCur, "MCP: stretch MIDI", -1);
            int nn = 0, cc = 0, sx = 0; MIDI_CountEvts(tk, &nn, &cc, &sx);
            return Json{{"ok", true}, {"changed", changed}, {"noteCount", nn}};
#else
            (void)trackIdx; (void)itemIdx; (void)takeIdx; (void)factor; (void)anchor; (void)selOnly;
            return Json{{"ok", true}, {"changed", 0}, {"noteCount", 0}};
#endif
        }});
}

}  // namespace reaper_mcp
