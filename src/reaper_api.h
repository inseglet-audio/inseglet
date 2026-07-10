// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// reaper_api.h
//
// Single place that pulls in the REAPER SDK and declares exactly the native functions we import.
//
// We use REAPERAPI_MINIMAL + REAPERAPI_WANT_<fn> so the extension imports only the functions it
// actually calls. This keeps us forward/backward compatible across REAPER builds: REAPERAPI_LoadAPI
// only fails if one of *these* functions is missing, not some unrelated API that changed.
//
// Usage:
//   - reaper_mcp.cpp (the ONE implementing TU) does:  #define REAPERAPI_IMPLEMENT  then includes this.
//   - every other TU (tools_*.cpp, control_surface.h) just includes this to get extern declarations.
//
// The whole thing is gated on REAPER_MCP_HAVE_SDK so host/test builds (no SDK) skip it entirely.

#pragma once

#ifdef REAPER_MCP_HAVE_SDK

#define REAPERAPI_MINIMAL

// --- transport / project ---
#define REAPERAPI_WANT_GetPlayState
#define REAPERAPI_WANT_GetCursorPosition
#define REAPERAPI_WANT_GetPlayPosition
#define REAPERAPI_WANT_Master_GetTempo
#define REAPERAPI_WANT_GetSet_LoopTimeRange
#define REAPERAPI_WANT_GetSetRepeat
#define REAPERAPI_WANT_GetProjectLength
#define REAPERAPI_WANT_GetProjectName
#define REAPERAPI_WANT_GetSetProjectInfo
#define REAPERAPI_WANT_GetSetProjectInfo_String

// --- tracks ---
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_InsertTrackAtIndex
#define REAPERAPI_WANT_TrackList_AdjustWindows
#define REAPERAPI_WANT_DeleteTrack
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_GetMediaTrackInfo_Value
#define REAPERAPI_WANT_SetMediaTrackInfo_Value
#define REAPERAPI_WANT_SetTrackSelected
#define REAPERAPI_WANT_SetOnlyTrackSelected
#define REAPERAPI_WANT_GetTrackGUID
#define REAPERAPI_WANT_guidToString
#define REAPERAPI_WANT_ValidatePtr2

// --- transport control / commands ---
#define REAPERAPI_WANT_Main_OnCommand
#define REAPERAPI_WANT_SetEditCurPos
#define REAPERAPI_WANT_CSurf_OnPlay
#define REAPERAPI_WANT_CSurf_OnStop
#define REAPERAPI_WANT_CSurf_OnRecord

// --- undo ---
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2
// Transactional-composite rollback — revert a just-closed undo block when a multi-edit verb is
// abandoned mid-plan (see composite_support.h UndoTransaction). One new WANT.
#define REAPERAPI_WANT_Undo_DoUndo2

// --- console / feature detection / resource path ---
#define REAPERAPI_WANT_ShowConsoleMsg
#define REAPERAPI_WANT_APIExists
#define REAPERAPI_WANT_GetResourcePath

// --- markers / regions ---
#define REAPERAPI_WANT_AddProjectMarker2
#define REAPERAPI_WANT_CountProjectMarkers
#define REAPERAPI_WANT_EnumProjectMarkers3
#define REAPERAPI_WANT_DeleteProjectMarker

// --- media items ---
#define REAPERAPI_WANT_AddMediaItemToTrack
#define REAPERAPI_WANT_CountTrackMediaItems
#define REAPERAPI_WANT_GetTrackMediaItem
#define REAPERAPI_WANT_GetMediaItemInfo_Value
#define REAPERAPI_WANT_SetMediaItemInfo_Value
#define REAPERAPI_WANT_GetSetMediaItemInfo_String
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_UpdateTimeline

// --- track FX (add / list / param) ---
#define REAPERAPI_WANT_TrackFX_AddByName
#define REAPERAPI_WANT_TrackFX_GetCount
#define REAPERAPI_WANT_TrackFX_GetFXName
#define REAPERAPI_WANT_TrackFX_GetNumParams
#define REAPERAPI_WANT_TrackFX_GetParamName
#define REAPERAPI_WANT_TrackFX_GetParam
#define REAPERAPI_WANT_TrackFX_GetParamNormalized
// Read a param's real degree range from its formatted display at the [0,1] endpoints,
// so we can map a tracked/requested angle onto plug-ins that expose angles as normalized [0,1] params.
#define REAPERAPI_WANT_TrackFX_FormatParamValueNormalized
#define REAPERAPI_WANT_TrackFX_SetParam
#define REAPERAPI_WANT_TrackFX_SetParamNormalized
#define REAPERAPI_WANT_TrackFX_Delete
#define REAPERAPI_WANT_TrackFX_GetEnabled
#define REAPERAPI_WANT_TrackFX_SetEnabled

// --- spatial — ReaSurroundPan named config (NUMCHANNELS/NUMSPEAKERS/RESETCHANNELS) ---
#define REAPERAPI_WANT_TrackFX_GetNamedConfigParm
#define REAPERAPI_WANT_TrackFX_SetNamedConfigParm

// --- ambisonics — FX pin/connector matrix, plug-in IO size, installed-FX probe ---
// TrackFX_Get/SetPinMappings: per-pin channel bitmask (low 32 in the return/arg, high 32 via the
//   extra param) — this is how we wire an encoder's ACN outputs onto the track's HOA channels.
// TrackFX_GetIOSize: the plug-in's actual input/output pin counts (bounds the pin loop; an IEM
//   encoder reports (order+1)^2 output pins once its Order Setting/bus width settle).
// EnumInstalledFX: enumerate every installed FX (name+ident) so we can detect IEM/SPARTA/ATK/…
//   without instantiating anything (read-only suite detection).
#define REAPERAPI_WANT_TrackFX_GetPinMappings
#define REAPERAPI_WANT_TrackFX_SetPinMappings
#define REAPERAPI_WANT_TrackFX_GetIOSize
#define REAPERAPI_WANT_EnumInstalledFX

// --- sends / routing ---
#define REAPERAPI_WANT_CreateTrackSend
#define REAPERAPI_WANT_RemoveTrackSend
#define REAPERAPI_WANT_GetTrackNumSends
#define REAPERAPI_WANT_GetTrackSendInfo_Value
#define REAPERAPI_WANT_SetTrackSendInfo_Value
#define REAPERAPI_WANT_GetSetTrackSendInfo
#define REAPERAPI_WANT_CSurf_TrackToID

// --- track envelopes / automation ---
#define REAPERAPI_WANT_CountTrackEnvelopes
#define REAPERAPI_WANT_GetTrackEnvelope
#define REAPERAPI_WANT_GetTrackEnvelopeByName
#define REAPERAPI_WANT_GetEnvelopeName
#define REAPERAPI_WANT_InsertEnvelopePoint
#define REAPERAPI_WANT_Envelope_SortPoints
#define REAPERAPI_WANT_CountEnvelopePoints
#define REAPERAPI_WANT_GetEnvelopePoint

// --- tempo ---
#define REAPERAPI_WANT_SetCurrentBPM

// --- takes + MIDI (items/takes MIDI CRUD) ---
#define REAPERAPI_WANT_CreateNewMIDIItemInProj
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_GetTake
#define REAPERAPI_WANT_GetMediaItemTake
#define REAPERAPI_WANT_CountTakes
#define REAPERAPI_WANT_GetMediaItemNumTakes
#define REAPERAPI_WANT_SetActiveTake
#define REAPERAPI_WANT_TakeIsMIDI
#define REAPERAPI_WANT_GetMediaItemTrack
#define REAPERAPI_WANT_GetSetMediaItemTakeInfo_String
#define REAPERAPI_WANT_GetMediaItemTakeInfo_Value
#define REAPERAPI_WANT_MIDI_InsertNote
#define REAPERAPI_WANT_MIDI_InsertCC
#define REAPERAPI_WANT_MIDI_CountEvts
#define REAPERAPI_WANT_MIDI_GetNote
#define REAPERAPI_WANT_MIDI_GetCC
#define REAPERAPI_WANT_MIDI_DeleteNote
#define REAPERAPI_WANT_MIDI_DeleteCC
#define REAPERAPI_WANT_MIDI_Sort
#define REAPERAPI_WANT_MIDI_GetPPQPosFromProjQN
#define REAPERAPI_WANT_MIDI_GetProjQNFromPPQPos
#define REAPERAPI_WANT_MIDI_GetProjTimeFromPPQPos

// --- resource provider (.RPP chunk / routing graph) ---
#define REAPERAPI_WANT_GetTrackStateChunk
#define REAPERAPI_WANT_GetMasterTrack

// --- track state-chunk write (elicitation-gated wholesale replace) ---
#define REAPERAPI_WANT_SetTrackStateChunk

// --- editing breadth ---
// The granular track/item field setters reuse the already-imported Set*Info_Value/String; only a
// handful of genuinely new primitives are needed here (split/delete/move an item, client-facing
// redo + undo/redo availability, selection getters + bulk (de)select, and a marker/region editor).
#define REAPERAPI_WANT_SplitMediaItem          // item.split — split at a position, returns right half
#define REAPERAPI_WANT_DeleteTrackMediaItem    // item.delete
#define REAPERAPI_WANT_MoveMediaItemToTrack    // item.move — reparent to another track
#define REAPERAPI_WANT_Undo_DoRedo2            // edit.redo (Undo_DoUndo2 already imported)
#define REAPERAPI_WANT_Undo_CanUndo2           // edit.get_undo_state — name of the next undo, or null
#define REAPERAPI_WANT_Undo_CanRedo2           // edit.get_undo_state — name of the next redo, or null
#define REAPERAPI_WANT_CountSelectedTracks     // selection.get
#define REAPERAPI_WANT_GetSelectedTrack        // selection.get
#define REAPERAPI_WANT_CountSelectedMediaItems // selection.get
#define REAPERAPI_WANT_GetSelectedMediaItem    // selection.get
#define REAPERAPI_WANT_SetMediaItemSelected    // selection.set / item.set_selected
#define REAPERAPI_WANT_SelectAllMediaItems     // selection.set — bulk (de)select items
#define REAPERAPI_WANT_SetProjectMarker3       // marker.edit / region.edit (by displayed ID number)

// --- actions / command-execution passthrough ---
// action.run runs a numeric command via the already-imported Main_OnCommand. The rest:
//   NamedCommandLookup   — resolve a named/_SWS_/_RS… command string to its numeric id (action.run_by_name)
//   GetToggleCommandState— read an action's toggle/on-off state (action.get_toggle_state + post-run echo)
//   SectionFromUniqueID  — the main action section (uniqueID 0) for kbd_getTextFromCmd
//   kbd_getTextFromCmd   — the action's display name, for the D2 name-layer deny-list (see actions_policy.h)
#define REAPERAPI_WANT_NamedCommandLookup
#define REAPERAPI_WANT_GetToggleCommandState
#define REAPERAPI_WANT_SectionFromUniqueID
#define REAPERAPI_WANT_kbd_getTextFromCmd

// --- depth (FX enable/preset + take-FX, sends/routing, tempo markers, project) ---
// Sends/routing depth and master-track tools add ZERO new primitives — they reuse the already-imported
// Get/SetTrackSendInfo_Value + CreateTrackSend + GetMasterTrack + TrackFX_* + Set/GetMediaTrackInfo_Value.
// The genuinely new primitives:

// FX preset navigation (fx.get_preset / fx.set_preset). GetEnabled/SetEnabled already imported (P2).
#define REAPERAPI_WANT_TrackFX_GetPreset       // fx.get_preset — current preset name
#define REAPERAPI_WANT_TrackFX_GetPresetIndex  // fx.get_preset — index + total preset count
#define REAPERAPI_WANT_TrackFX_SetPreset       // fx.set_preset — activate a preset by name
#define REAPERAPI_WANT_TrackFX_NavigatePresets // fx.set_preset — move +/- N presets (by "move")

// Take FX chain (takefx.add/list/remove/get_param/set_param/set_enabled) — the take-level mirror of
// the track-FX family.
#define REAPERAPI_WANT_TakeFX_AddByName
#define REAPERAPI_WANT_TakeFX_GetCount
#define REAPERAPI_WANT_TakeFX_GetFXName
#define REAPERAPI_WANT_TakeFX_GetNumParams
#define REAPERAPI_WANT_TakeFX_GetParamName
#define REAPERAPI_WANT_TakeFX_GetParam
#define REAPERAPI_WANT_TakeFX_GetParamNormalized
#define REAPERAPI_WANT_TakeFX_SetParam
#define REAPERAPI_WANT_TakeFX_SetParamNormalized
#define REAPERAPI_WANT_TakeFX_Delete
#define REAPERAPI_WANT_TakeFX_GetEnabled
#define REAPERAPI_WANT_TakeFX_SetEnabled

// Tempo / time-signature markers (tempo.add_marker/edit_marker/delete_marker/list_markers) + QN<->time.
// AddTempoTimeSigMarker is deprecated (== SetTempoTimeSigMarker ptidx=-1), so we don't import it.
#define REAPERAPI_WANT_CountTempoTimeSigMarkers
#define REAPERAPI_WANT_GetTempoTimeSigMarker
#define REAPERAPI_WANT_SetTempoTimeSigMarker   // ptidx=-1 inserts, >=0 edits (read-merge-write)
#define REAPERAPI_WANT_DeleteTempoTimeSigMarker
#define REAPERAPI_WANT_TimeMap2_QNToTime       // time.qn_to_time
#define REAPERAPI_WANT_TimeMap2_timeToQN       // time.time_to_qn

// Project file + render (project.save/open/new/render). The render leg uses the already-imported
// Main_OnCommand (no-dialog action 41824) + GetSetProjectInfo[_String] (RENDER_*), so no new WANT there.
// The modal-hazard tools (open/new) are gated on IsProjectDirty + Main_openProject's 'noprompt:' prefix
// so a blocking save-prompt can never stall the main-thread pump (see the App Nap invariant).
#define REAPERAPI_WANT_Main_SaveProject        // project.save — save to the existing path (no dialog)
#define REAPERAPI_WANT_Main_SaveProjectEx      // project.save — save-as to a path (options&8), no dialog
#define REAPERAPI_WANT_Main_openProject        // project.open — 'noprompt:' prefix suppresses the save-prompt
#define REAPERAPI_WANT_GetProjectPath          // project.save/open — resolve the project directory
#define REAPERAPI_WANT_IsProjectDirty          // project.open/new — refuse when there are unsaved changes

// --- depth fill (envelope/MIDI depth, rec-mode, freeze, metering) ---
// Rec-mode reuses Set/GetMediaTrackInfo_Value (I_RECMODE); freeze/unfreeze reuse Main_OnCommand
// (40901 mono / 41223 stereo / 40877 multichannel / 41644 unfreeze) + the selection primitives;
// envelope auto-activation reuses Get/SetTrackStateChunk (the D3 substrate). Genuinely new:
#define REAPERAPI_WANT_DeleteEnvelopePointEx   // envelope.delete_point (autoitem_idx=-1 = the envelope)
#define REAPERAPI_WANT_DeleteEnvelopePointRange// envelope.clear_range
#define REAPERAPI_WANT_GetTrackAutomationMode  // envelope.set_automation_mode — echo the previous mode
#define REAPERAPI_WANT_SetTrackAutomationMode  // envelope.set_automation_mode
#define REAPERAPI_WANT_MIDI_SetNote            // midi.set_note — in-place edit (NULL = keep field)
#define REAPERAPI_WANT_MIDI_SetCC              // midi.set_cc — in-place edit (NULL = keep field)
#define REAPERAPI_WANT_MIDI_SelectAll          // midi.select_notes — bulk (de)select fast path
#define REAPERAPI_WANT_Track_GetPeakInfo       // track.get_peak — live peak meter readback

// --- native ADM object-audio authoring (spatial.export_adm object-trajectory sampling) ---
// Sample an object panner's position OVER TIME for the ADM audioBlockFormat trajectory: read the
// FX-parameter automation envelope for azimuth/elevation/distance. The envelope-point readers
// (CountEnvelopePoints/GetEnvelopePoint) + TrackFX_Get/FormatParamValueNormalized are already imported;
// only the FX-parameter-envelope accessor is genuinely new.
#define REAPERAPI_WANT_GetFXEnvelope           // spatial.export_adm — object position automation handle

// --- audio accessors (render-free direct sample reads for the metering DSP) ---
// A track/take AudioAccessor streams decoded PCM without a render round-trip. We create one, read a
// bounded window with GetAudioAccessorSamples (interleaved ReaSample), and destroy it — all inside a
// single main-thread tools/call. StateChanged/ValidateState guard against reading a stale accessor.
// A TRACK accessor reads the track's summed media-item/take content (pre track-FX/volume/pan); a TAKE
// accessor reads that take's source through its take FX. GetMediaItemTake_Source + the source
// channel/sample-rate probes size a take read to its native width/rate.
#define REAPERAPI_WANT_CreateTrackAudioAccessor
#define REAPERAPI_WANT_CreateTakeAudioAccessor
#define REAPERAPI_WANT_GetAudioAccessorStartTime
#define REAPERAPI_WANT_GetAudioAccessorEndTime
#define REAPERAPI_WANT_GetAudioAccessorSamples
#define REAPERAPI_WANT_AudioAccessorStateChanged
#define REAPERAPI_WANT_AudioAccessorValidateState
#define REAPERAPI_WANT_DestroyAudioAccessor
#define REAPERAPI_WANT_GetMediaItemTake_Source
#define REAPERAPI_WANT_GetMediaSourceNumChannels
#define REAPERAPI_WANT_GetMediaSourceSampleRate

// --- Batch B2 breadth (groove/quantize + takes/comping + transport extras) ---
// Groove/quantize (midi.*) reuse the already-imported MIDI CRUD (MIDI_Get/SetNote, PPQ<->QN,
// MIDI_CountEvts, MIDI_Sort) — no new primitives. Takes/comping and transport extras add:
#define REAPERAPI_WANT_AddTakeToMediaItem       // take.add — append an empty take to an item
#define REAPERAPI_WANT_SetMediaItemTakeInfo_Value  // take.set_vol/pan/pitch/playrate (D_*; getter already imported)
#define REAPERAPI_WANT_Master_GetPlayRate       // transport.set_playrate — read back the master play rate
#define REAPERAPI_WANT_CSurf_OnPlayRateChange   // transport.set_playrate — set the master play rate
// take.delete/crop_to_active + items.implode_to_takes + transport.set_metronome reuse the already-imported
// Main_OnCommand / GetToggleCommandState / SelectAllMediaItems / SetMediaItemSelected / SetActiveTake / GetSetRepeat.

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

// SWELL (pulled in by reaper_plugin.h on macOS/Linux) defines min()/max() as function-like macros,
// which break <limits>, <algorithm>, <random> and nlohmann/json. Drop them before any STL/JSON
// header is parsed. We never rely on the macro forms; use std::min/std::max explicitly.
#ifdef min
  #undef min
#endif
#ifdef max
  #undef max
#endif

#endif  // REAPER_MCP_HAVE_SDK
