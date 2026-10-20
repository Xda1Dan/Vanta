//
//  async.hpp
//  vanta-base
//
//  Work that must not happen on the Lua thread.
//
//  Roblox runs Lua on its scheduler thread, so anything that blocks there stops
//  the whole client - the frame, the network, the render. A curl transfer is the
//  worst offender: bounded only by CURLOPT_TIMEOUT_MS, it could freeze the game
//  for ten seconds at a time.
//
//  So the transfer runs on a worker and the Lua thread yields until it is done.
//  This half is the worker side and knows nothing about Lua: a caller submits a
//  piece of work that returns a `Result` - a closure that will later push the
//  answer onto a Lua stack - and gets back an id. The Lua half (unc::Yield) waits
//  on that id and runs the Result once the job reports ready.
//
//  Nothing here ever captures a `lua_State*`. That is the one place this differs
//  from the references and it is deliberate: mac, Vicna and sourcessss all hand
//  the thread pointer to their worker and then have to re-validate it on the way
//  back (mac checks the DataModel, the Workspace, `IsValidPointer(L)`,
//  `lua_mainthread(L)` and `lua_isyieldable(L)` before daring to resume). A job
//  that holds no Lua state cannot resume into a dead VM, so none of that is
//  needed: the waiting thread resumes itself, or never does and is swept.
//

#pragma once

#include <atomic>
#include <chrono>

#include "Journal.h"
#include "Pool.h"
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace async {

    // Pushes the finished job's values onto the Lua stack and answers how many
    // it pushed. Runs on the Lua thread, never on the worker.
    using Result = std::function<int(uint64_t)>;

    struct Entry {
        std::mutex mutex;
        std::condition_variable finished;
        bool done = false;
        Result result;
        std::chrono::steady_clock::time_point submitted = std::chrono::steady_clock::now();
        // When the answer landed. The sweep ages from here rather than from
        // `submitted`, so a transfer that took twenty seconds still gets its
        // full window to be collected in.
        std::chrono::steady_clock::time_point completed{};
    };

    // A finished job is kept for a while after it has been collected, so
    // collecting twice answers the same thing twice instead of raising. That is
    // not paranoia: a Lua thread can reach its continuation more than once, and
    // destroying the answer on the first collect turned three Myriad checks
    // into "the request was lost before it finished" with the same job id each
    // time - the signature of a second collect, not of a lost job.
    inline constexpr size_t kKeepAtMost = 256;

    // How many transfers may be in flight at once. Comfortably above what any
    // real script does in parallel, and low enough that a runaway loop costs
    // twenty-four threads rather than every thread the process can make.
    inline constexpr size_t kMaxWorkers = 24;

    inline pool::Pool& workers()
    {
        static pool::Pool instance(kMaxWorkers);
        return instance;
    }

    inline std::mutex& registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    inline std::unordered_map<int, std::shared_ptr<Entry>>& registry()
    {
        static std::unordered_map<int, std::shared_ptr<Entry>> entries;
        return entries;
    }

    // A thread whose script errors between submitting and collecting never comes
    // back for its answer, and a game that unloads takes every waiting thread
    // with it. Neither is worth tracking individually - anything finished this
    // long ago is nobody's any more.
    inline constexpr std::chrono::seconds kAbandonedAfter{30};

    // Called with registryMutex held.
    inline void sweep()
    {
        const auto now = std::chrono::steady_clock::now();
        auto& entries = registry();

        // One pass: drop what has aged out, and remember the rest of the
        // finished ones in case the map is over its ceiling. The old version
        // rescanned the whole map once per eviction, which is quadratic exactly
        // when the map is at its biggest.
        std::vector<std::pair<std::chrono::steady_clock::time_point, int>> collectable;
        for (auto it = entries.begin(); it != entries.end();) {
            bool drop = false;
            std::chrono::steady_clock::time_point when{};
            {
                std::lock_guard<std::mutex> lock(it->second->mutex);
                if (it->second->done) {
                    when = it->second->completed;
                    drop = now - when > kAbandonedAfter;
                }
            }
            if (drop) {
                it = entries.erase(it);
                continue;
            }
            if (when.time_since_epoch().count() != 0)
                collectable.emplace_back(when, it->first);
            ++it;
        }

        // A script that never stops asking would otherwise hold every response
        // it ever got for the whole window, so the oldest finished ones go once
        // there are too many.
        if (entries.size() <= kKeepAtMost)
            return;
        const size_t excess = std::min(entries.size() - kKeepAtMost, collectable.size());
        std::partial_sort(collectable.begin(), collectable.begin() + excess, collectable.end());
        for (size_t i = 0; i < excess; i++)
            entries.erase(collectable[i].second);
    }

    inline void complete(const std::shared_ptr<Entry>& entry, Result result)
    {
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->result = std::move(result);
            entry->completed = std::chrono::steady_clock::now();
            entry->done = true;
        }
        entry->finished.notify_all();
    }

    // Runs `work` on a worker thread. The id it answers is what the Lua side
    // waits on; nothing about the Lua state is captured, so a thread that dies
    // while the transfer is in flight costs an abandoned entry and nothing more.
    inline int submit(std::function<Result()> work)
    {
        static std::atomic<int> nextId{1};
        const int id = nextId.fetch_add(1);

        auto entry = std::make_shared<Entry>();
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            sweep();
            registry()[id] = entry;
        }

        const bool queued = workers().run([entry, work = std::move(work), id]() mutable {
            const auto started = std::chrono::steady_clock::now();
            Result result;
            try {
                result = work();
            } catch (...) {
                // A worker that throws is still a worker that finished; the Lua
                // side answers with whatever the empty result means for it.
                journal::writef("ASYNC job %d threw", id);
            }
            complete(entry, std::move(result));
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            journal::writef("ASYNC done id=%d after %lldms", id,
                            static_cast<long long>(elapsed));
        });

        // The pool could not give us a thread at all. Finish the job now with
        // no result: the collector raises "the request was lost", which the
        // caller can pcall, rather than waiting for an answer that is never
        // coming.
        if (!queued) {
            journal::writef("ASYNC id=%d had no worker to run on", id);
            complete(entry, nullptr);
        }

        return id;
    }

    inline std::shared_ptr<Entry> lookup(int id)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        auto found = registry().find(id);
        return found == registry().end() ? nullptr : found->second;
    }

    inline bool ready(int id)
    {
        std::shared_ptr<Entry> entry = lookup(id);
        if (!entry)
            return true;   // nothing to wait for; the collector reports the loss
        std::lock_guard<std::mutex> lock(entry->mutex);
        return entry->done;
    }

    // How long a blocking wait may hold the thread it is on. Comfortably above
    // http's own CURLOPT_TIMEOUT_MS so a transfer that is merely slow still
    // lands, and far below "forever".
    inline constexpr int kBlockTimeoutMs = 20000;

    // Blocks the calling thread until the job finishes, or the deadline passes.
    // Answers whether it finished.
    //
    // This used to wait without a deadline, and that is how the client came to
    // freeze for good. The path is only taken when the Lua thread turns out not
    // to be yieldable - which is exactly what sUNC's results upload does at the
    // end of a run - and the thread it stalls is Roblox's scheduler. A job that
    // never completes therefore took the whole game down with no error, no
    // timeout and nothing in the log: the last thing on disk was sUNC printing
    // its results, then silence. Stalling is better than raising an error the
    // caller cannot expect; stalling *forever* is not better than anything.
    inline bool wait(int id, int timeoutMs = kBlockTimeoutMs)
    {
        std::shared_ptr<Entry> entry = lookup(id);
        if (!entry)
            return false;
        std::unique_lock<std::mutex> lock(entry->mutex);
        const bool ok = entry->finished.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                                 [&] { return entry->done; });
        if (!ok)
            journal::writef("ASYNC block on id=%d TIMED OUT after %dms", id, timeoutMs);
        return ok;
    }

    inline bool exists(int id)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        return registry().find(id) != registry().end();
    }

    // Hands back a finished job's result. The entry is *not* removed - the
    // sweep does that on a timer - so collecting the same id twice answers the
    // same thing twice rather than reporting a job that was lost. An unfinished
    // job answers nothing, and stays where it is.
    inline Result take(int id)
    {
        std::shared_ptr<Entry> entry = lookup(id);
        if (!entry)
            return nullptr;
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->done)
            return nullptr;
        return entry->result;
    }
}
