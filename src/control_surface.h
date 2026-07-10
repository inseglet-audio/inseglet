// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// control_surface.h
//
// IReaperControlSurface subclass that serves two jobs:
//   1. Run()  — REAPER calls this on the MAIN THREAD every UI tick. We use it to drain the
//               MainThreadQueue (our marshaling pump).
//   2. Set*() — REAPER pushes state changes to us here (play state, selection, volume, track-list
//               changes, touch state, ...). We forward these as change events so the MCP layer can
//               emit `notifications/resources/updated` to subscribed clients — true push, no polling.
//
// State is delivered event-driven, straight from these callbacks — no polling required.
//
// Under REAPER_MCP_HAVE_SDK this derives from the real IReaperControlSurface (from reaper_plugin.h)
// and marks the callbacks `override`. Without the SDK it compiles standalone for structural review.

#pragma once

#include <functional>

#include "head_track_bridge.h"  // apply the latest head-tracked orientation each Run() tick
#include "main_thread_queue.h"
#include "reaper_api.h"  // brings in IReaperControlSurface + MediaTrack when REAPER_MCP_HAVE_SDK

#ifdef REAPER_MCP_HAVE_SDK
  #define REAPER_MCP_CSURF_BASE : public IReaperControlSurface
  #define REAPER_MCP_OVERRIDE override
#else
  #define REAPER_MCP_CSURF_BASE
  #define REAPER_MCP_OVERRIDE
  class MediaTrack;  // forward decl so the signatures below are well-formed without the SDK
#endif

namespace reaper_mcp {

// Coarse change-event stream fed to the MCP resource/subscription layer. The entry point installs a
// sink that maps each event to the affected resource URI(s) and marks them dirty in the
// SubscriptionHub; the server's SSE channel then flushes notifications/resources/updated.
struct StateEvent {
    enum class Kind {
        TrackList, PlayState, Volume, Pan, Mute, Solo, RecArm, Selection, TrackTitle, Other
    } kind;
    MediaTrack* track = nullptr;  // the affected track, or nullptr for project-wide events
    double value = 0.0;
};

using StateEventSink = std::function<void(const StateEvent&)>;

class McpControlSurface REAPER_MCP_CSURF_BASE {
public:
    McpControlSurface(MainThreadQueue& queue, StateEventSink sink)
        : queue_(queue), sink_(std::move(sink)) {}

    // --- Required identity strings (REAPER shows these in Preferences > Control surfaces) ---
    const char* GetTypeString() REAPER_MCP_OVERRIDE { return "REAPERMCP"; }
    const char* GetDescString() REAPER_MCP_OVERRIDE { return "REAPER MCP (Model Context Protocol) bridge"; }
    const char* GetConfigString() REAPER_MCP_OVERRIDE { return ""; }

    // --- The main-thread pump ---
    void Run() REAPER_MCP_OVERRIDE {
        queue_.drainMainThread();
        // StateEvents are forwarded to the sink synchronously from the Set* callbacks below (also
        // main thread); coalescing per resource URI happens in the SubscriptionHub, and the SSE
        // channel flushes them — so Run() has nothing extra to do there.
        // Apply the latest head-tracked orientation to the binaural monitor's rotator. The
        // OSC worker thread only stores the latest yaw/pitch/roll; the actual TrackFX_SetParam runs
        // here on the MAIN THREAD, coalesced to at most one write per UI tick (no-op if idle).
        HeadTrackBridge::instance().applyOnMainThread();
    }

    // --- Push state callbacks (REAPER -> us, main thread). Forward as StateEvents. ---
    void SetTrackListChange() REAPER_MCP_OVERRIDE { emit({StateEvent::Kind::TrackList, nullptr, 0}); }
    void SetSurfaceVolume(MediaTrack* t, double v) REAPER_MCP_OVERRIDE { emit({StateEvent::Kind::Volume, t, v}); }
    void SetSurfacePan(MediaTrack* t, double v) REAPER_MCP_OVERRIDE { emit({StateEvent::Kind::Pan, t, v}); }
    void SetSurfaceMute(MediaTrack* t, bool mute) REAPER_MCP_OVERRIDE {
        emit({StateEvent::Kind::Mute, t, mute ? 1.0 : 0.0});
    }
    void SetSurfaceSolo(MediaTrack* t, bool solo) REAPER_MCP_OVERRIDE {
        emit({StateEvent::Kind::Solo, t, solo ? 1.0 : 0.0});
    }
    void SetSurfaceRecArm(MediaTrack* t, bool rec) REAPER_MCP_OVERRIDE {
        emit({StateEvent::Kind::RecArm, t, rec ? 1.0 : 0.0});
    }
    void SetSurfaceSelected(MediaTrack* t, bool sel) REAPER_MCP_OVERRIDE {
        emit({StateEvent::Kind::Selection, t, sel ? 1.0 : 0.0});
    }
    void SetPlayState(bool play, bool pause, bool rec) REAPER_MCP_OVERRIDE {
        emit({StateEvent::Kind::PlayState, nullptr,
              (play ? 1.0 : 0.0) + (pause ? 2.0 : 0.0) + (rec ? 4.0 : 0.0)});
    }
    void SetTrackTitle(MediaTrack* t, const char* /*title*/) REAPER_MCP_OVERRIDE {
        emit({StateEvent::Kind::TrackTitle, t, 0});
    }
    // GetTouchState / OnTrackSelection / Extended(...) remain available as future subscription
    // sources (e.g. fine-grained FX/parameter change events for a spatial state resource).

private:
    void emit(const StateEvent& e) {
        if (sink_) sink_(e);
    }

    MainThreadQueue& queue_;
    StateEventSink sink_;
};

}  // namespace reaper_mcp
