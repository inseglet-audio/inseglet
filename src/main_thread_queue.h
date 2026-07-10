// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// main_thread_queue.h
//
// Thread-safe command queue that marshals work from the MCP server worker thread(s)
// onto REAPER's MAIN THREAD, where (almost) all of the REAPER API must be called.
//
// The queue is drained by McpControlSurface::Run() (called by REAPER on the main thread every
// UI tick, ~30 Hz) and, as a fallback, by a registered "timer" callback. Callers submit a
// task and block on a std::future for the result, so an MCP tool handler reads as ordinary
// synchronous code while actually executing on the main thread.
//
// Design notes:
//  - We cap the wall-clock time spent draining per Run() (kBudgetMicros) so a burst of tool
//    calls can never stall REAPER's UI/audio-servicing thread. Anything left over runs next tick.
//  - No REAPER API call is made from this header; it only moves std::function work items.
//
// TODO: replace the mutex+deque with an MPSC lock-free ring if profiling shows contention.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>

namespace reaper_mcp {

class MainThreadQueue {
public:
    // Submit `fn` to run on the main thread; returns a future for its result.
    template <typename Fn>
    auto submit(Fn&& fn) -> std::future<decltype(fn())> {
        using R = decltype(fn());
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            queue_.emplace_back([task]() { (*task)(); });
        }
        return fut;
    }

    // Called ONLY from the main thread (McpControlSurface::Run or the timer fallback).
    // Drains up to a time budget so the UI never stalls.
    void drainMainThread() {
        using clock = std::chrono::steady_clock;
        const auto deadline = clock::now() + std::chrono::microseconds(kBudgetMicros);
        for (;;) {
            std::function<void()> job;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job();  // executes a REAPER API call on the main thread
            if (clock::now() >= deadline) return;  // yield; finish the rest next tick
        }
    }

    size_t pending() {
        std::lock_guard<std::mutex> lk(mutex_);
        return queue_.size();
    }

private:
    static constexpr long kBudgetMicros = 4000;  // ~4 ms/tick ceiling — tune in soak tests
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

}  // namespace reaper_mcp
