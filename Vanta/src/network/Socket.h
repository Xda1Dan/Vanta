//
//  Socket.h
//  vanta-base
//
//  The script queue between the HTTP control server and the executor loop.
//  It no longer owns a listener: the GUI talks to ControlServer.h, and this
//  file is only the thread-safe hand-off.
//
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <string>

namespace execsock {

// A cap on how many scripts may wait for a game to exist. Scripts are only
// drained while a game is live, so a GUI left sending at the home screen would
// otherwise grow this without bound.
inline constexpr size_t kMaxQueued = 256;

// Scripts pushed here from HTTP worker threads, popped by the executor thread.
inline std::mutex g_mutex;
inline std::condition_variable g_arrived;
inline std::queue<std::string> g_queue;

inline bool popScript(std::string& out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.empty())
        return false;
    out = std::move(g_queue.front());
    g_queue.pop();
    return true;
}

// Blocks until a script arrives or `timeout` passes.
template <typename Duration>
inline void waitForScript(Duration timeout)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_queue.empty())
        return;
    g_arrived.wait_for(lock, timeout);
}

}  // namespace execsock
