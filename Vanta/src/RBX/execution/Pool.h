//
//  pool.hpp
//  vanta-base
//
//  A bounded pool of worker threads.
//
//  Everything that must not run on Roblox's scheduler thread - an HTTP
//  transfer, a WebSocket handshake - used to get a `std::thread(...).detach()`
//  of its own. Three of the six references do exactly that (mac's `Yielding`,
//  Vicna's `YieldExecution`, sourcessss's `yielding::yield`) and all three
//  inherit the same two problems:
//
//    * it is unbounded. A script looping over `HttpGet` creates a thread per
//      call, and when the process runs out `std::thread`'s constructor throws
//      `std::system_error`. Nothing catches it inside a detached lambda, so it
//      reaches `std::terminate` and the client dies.
//    * it is not cheap. A thread is a syscall plus half a megabyte of stack,
//      paid per request.
//
//  RbxStu is the one that got this right - its yields go through an
//  `m_asyncExecutor`, and its job drains at most twenty per cycle - and the
//  macOS reference already carries a `ThreadPool(hardware_concurrency())` for
//  its socket clients. This is that, with one addition: the pool grows on
//  demand rather than starting at full size, because a client that never runs a
//  script should not be paying for idle threads.
//
//  Workers are spawned lazily up to `maximum` and never exit. A worker that
//  exited would need joining, and joining is what turns a teardown into a hang;
//  a handful of threads parked on a condition variable cost nothing measurable.
//

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace pool {

    class Pool {
    public:
        explicit Pool(size_t maximum) : maximum(maximum) {}

        Pool(const Pool&) = delete;
        Pool& operator=(const Pool&) = delete;

        // Queues `work`. Never throws, never blocks, and never runs the work on
        // the calling thread. Answers false only when the process would not
        // give us a single thread to run it on, which the caller has to treat
        // as "this job will not happen".
        bool run(std::function<void()> work)
        {
            std::unique_lock<std::mutex> lock(mutex);
            pending.push_back(std::move(work));

            // Grow only when the work outruns the workers parked on the
            // condition. `idle` counts workers that are waiting *or* have been
            // notified but not yet re-acquired the lock, and each of those will
            // claim exactly one entry - so `pending.size() > idle` is precisely
            // "some job here has nobody committed to it". Both are read under
            // this lock, so the test cannot race.
            //
            // `idle == 0` was the old test and it under-spawned: two workers
            // parked, three jobs pushed back to back, and the third saw
            // idle == 2 and got no worker of its own - it waited for one of the
            // first two to finish. That serialises exactly the case the pool
            // exists to keep parallel (two 2 s transfers in 2.42 s, not 4.84).
            if (pending.size() > idle && workers < maximum) {
                if (spawnLocked())
                    workers++;
                else if (workers == 0) {
                    // No worker exists and the process will not make one. Drop
                    // the job rather than leave it queued for a thread that is
                    // never coming.
                    pending.pop_back();
                    lock.unlock();
                    return false;
                }
            }

            lock.unlock();
            wakeup.notify_one();
            return true;
        }

        size_t depth()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return pending.size();
        }

    private:
        bool spawnLocked()
        {
            try {
                std::thread([this] { loop(); }).detach();
                return true;
            } catch (...) {
                // EAGAIN: the process is out of threads. Whoever is already
                // running will pick the work up.
                return false;
            }
        }

        void loop()
        {
            while (true) {
                std::function<void()> work;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    idle++;
                    wakeup.wait(lock, [this] { return !pending.empty(); });
                    idle--;
                    work = std::move(pending.front());
                    pending.pop_front();
                }
                // A worker that lets an exception out takes the process with
                // it, and the work here is supplied by callers who cannot know
                // that. Swallowing is right: the job's own bookkeeping is what
                // reports the failure to whoever was waiting.
                try {
                    work();
                } catch (...) {
                }
            }
        }

        std::mutex mutex;
        std::condition_variable wakeup;
        std::deque<std::function<void()>> pending;
        size_t workers = 0;
        size_t idle = 0;
        const size_t maximum;
    };

}   // namespace pool
