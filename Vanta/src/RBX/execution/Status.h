//
//  Status.h
//  Vanta
//
//  What the executor is doing, in one place the init thread writes and the
//  HTTP thread reads. LunarModule keeps the same idea in Shared::Globals -
//  a couple of atomics and a single "initialized on game" flag - rather than
//  inferring readiness from the presence of a listener.
//
//  The listener comes up before the environment does, so "HTTP answered" is
//  not "scripts can run". The GUI needs the difference: a script sent while
//  we are still waiting for a game is queued, and a client on a build the
//  offsets do not match will never run it at all.
//
#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>

namespace runtime {

enum class State {
    starting,   // dylib is up, hooks and signatures are being installed
    waiting,    // no game yet, or the last one went away
    ready,      // environment built, pump armed
    reinstall,  // Roblox is not the pinned build; offsets are stale
    failed      // the dylib could not stand itself up (unresolved pointers)
};

inline const char* name(State state)
{
    switch (state) {
    case State::starting:  return "starting";
    case State::waiting:   return "waiting";
    case State::ready:     return "ready";
    case State::reinstall: return "reinstall";
    case State::failed:    return "failed";
    }
    return "unknown";
}

inline std::atomic<int> stateValue{static_cast<int>(State::starting)};

// Written by the Heartbeat pump from the bootstrap's answer to
// game:IsLoaded() + LocalPlayer. A gated script waits for this to go true.
inline std::atomic<bool> gameReady{false};

// Pump::arm()/disarm() mirror into this, so /status does not have to include
// the whole registry header.
inline std::atomic<bool> pumpArmed{false};

// Entries waiting to run. Includes the socket queue, not just the pump's.
inline std::atomic<size_t> queuedScripts{0};

inline std::mutex detailMutex;
inline std::string detailValue = "dylib loaded";

inline void set(State state, std::string detail)
{
    {
        std::lock_guard<std::mutex> lock(detailMutex);
        detailValue = std::move(detail);
    }
    stateValue.store(static_cast<int>(state), std::memory_order_release);
}

inline State state()
{
    return static_cast<State>(stateValue.load(std::memory_order_acquire));
}

inline std::string detail()
{
    std::lock_guard<std::mutex> lock(detailMutex);
    return detailValue;
}

inline void setGameReady(bool ready)
{
    gameReady.store(ready, std::memory_order_release);
}

}  // namespace runtime
