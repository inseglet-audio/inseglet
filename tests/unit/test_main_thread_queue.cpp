// SPDX-License-Identifier: MIT
// Copyright (c) 2026 James Livingston

// test_main_thread_queue.cpp — unit test for the main-thread marshaling queue.
//
// Verifies: (1) submitted work executes only when drainMainThread() is called (i.e. on the
// "main thread"), (2) futures deliver return values, (3) the per-tick time budget bounds a
// burst — work left over after the budget is exhausted runs on a later drain.
// No REAPER dependency. Build/run standalone (see tests/CMakeLists.txt).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "main_thread_queue.h"

using reaper_mcp::MainThreadQueue;

static int g_failures = 0;
static void check(bool cond, const char* what) {
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

static void test_executes_only_on_drain() {
    MainThreadQueue q;
    bool ran = false;
    auto fut = q.submit([&] { ran = true; return 42; });
    check(!ran, "work must not run before drain");
    check(q.pending() == 1, "one task pending before drain");
    q.drainMainThread();  // simulates McpControlSurface::Run() on the main thread
    check(ran, "work runs on drain");
    check(fut.get() == 42, "future delivers the return value");
    check(q.pending() == 0, "queue empty after drain");
}

static void test_cross_thread_submit() {
    MainThreadQueue q;
    std::atomic<int> got{-1};
    std::thread worker([&] {
        auto f = q.submit([] { return 7; });
        got = f.get();  // blocks until the main thread drains
    });
    for (int i = 0; i < 1000 && q.pending() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    q.drainMainThread();
    worker.join();
    check(got.load() == 7, "cross-thread future resolves after drain");
}

// A burst of jobs, each longer than the per-tick budget (kBudgetMicros == 4 ms), must NOT all
// run in a single drain: the budget yields and the remainder runs on subsequent drains.
static void test_time_budget_bounds_a_burst() {
    MainThreadQueue q;
    std::atomic<int> ran{0};
    const int kJobs = 6;
    for (int i = 0; i < kJobs; ++i)
        q.submit([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));  // > 4 ms budget
            ran.fetch_add(1);
            return 0;
        });

    q.drainMainThread();  // one tick: the budget must stop the burst before it finishes
    check(ran.load() < kJobs, "per-tick budget yields before finishing a long burst");
    check(q.pending() > 0, "work remains queued after the budget is exhausted");

    int guard = 0;
    while (q.pending() > 0 && guard++ < 1000) q.drainMainThread();  // later ticks finish it
    check(ran.load() == kJobs, "all work eventually runs across ticks");
    check(q.pending() == 0, "queue drains completely");
}

int main() {
    test_executes_only_on_drain();
    test_cross_thread_submit();
    test_time_budget_bounds_a_burst();
    if (g_failures) { std::fprintf(stderr, "%d check(s) failed\n", g_failures); return 1; }
    return 0;
}
