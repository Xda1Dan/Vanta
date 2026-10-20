//
//  console.hpp
//  vanta-base
//
//  The rconsole family. The dylib does not own a window: every call is turned
//  into a bounded in-memory record, and the GUI reads the recent history over
//  the HTTP control server (GET /console?since=N). With no GUI attached the
//  records still go to a log file, so output is never lost.
//
//  Records are deliberately NOT echoed to Roblox's own output - that lands in
//  LogService, which anti-cheats scan. See [[console-output-discipline]].
//
//  Nothing here touches a socket on the caller's thread: `send` appends to a
//  queue and one writer thread owns the connection. See the queue below.
//

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>


#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "Filesystem.h"
#include "ClientAPI.h"

namespace console {

    inline std::mutex mutex;

    inline const std::filesystem::path& directory()
    {
        static const std::filesystem::path path = [] {
            // Beside the workspace, not inside it: listfiles("") is part of the
            // tested surface and the console's plumbing has no business showing
            // up in it.
            std::filesystem::path directory = workspace::root().parent_path() / "console";
            std::error_code ignored;
            std::filesystem::create_directories(directory, ignored);
            return directory;
        }();
        return path;
    }

    inline std::filesystem::path logPath()   { return directory() / "console.log"; }
    inline std::filesystem::path inputPath() { return directory() / "input.txt"; }

    inline std::string escape(const std::string& text)
    {
        std::string out;
        out.reserve(text.size() + 8);
        for (const unsigned char c : text) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char buffer[7];
                        snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                        out += buffer;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        return out;
    }

    // The queue between the Lua thread and the HTTP history writer.
    //
    // `send` used to do socket -> connect -> write -> close for *every record*,
    // synchronously, inside the lua_CFunction. A script doing
    // `for i = 1, 10000 do rconsoleprint(i) end` paid 10 000 connect/close
    // cycles on Roblox's own frame thread. Now `send` appends and returns; one
    // writer thread owns the history and the log file.
    //
    // One writer, not src/pool.hpp: console output is ordered, and a pool would
    // interleave two scripts' lines into nonsense.
    struct Record {
        std::string name;
        std::string message;
    };

    struct Entry {
        uint64_t seq = 0;
        std::string name;
        std::string message;
    };

    // The HTTP reader polls from a sequence number. This is the server-side
    // history; the cap keeps a chatty script from growing memory forever.
    inline std::deque<Entry> history;
    inline uint64_t nextSeq = 1;
    inline constexpr size_t kHistory = 512;

    inline uint64_t lastSeq()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return nextSeq == 0 ? 0 : nextSeq - 1;
    }

    inline std::string recordsJson(size_t since)
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::string out = "{\"records\":[";
        bool first = true;
        for (const Entry& entry : history) {
            if (entry.seq <= since)
                continue;
            if (!first)
                out += ",";
            first = false;
            out += "{\"seq\":" + std::to_string(entry.seq);
            out += ",\"name\":\"" + escape(entry.name) + "\"";
            out += ",\"message\":\"" + escape(entry.message) + "\"}";
        }
        out += "]}";
        return out;
    }

    inline std::deque<Record> pending;
    inline std::condition_variable wake;
    inline std::atomic<bool> running{false};
    inline std::atomic<size_t> dropped{0};

    // Deep enough that a burst is never lost, shallow enough that a script
    // printing into a closed GUI cannot grow it without bound.
    inline constexpr size_t kMaxPending = 1024;

    inline void worker()
    {
        while (running.load(std::memory_order_acquire)) {
            Record record;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(250), [] {
                    return !pending.empty() || !running.load(std::memory_order_acquire);
                });
                if (!running.load(std::memory_order_acquire))
                    break;
                if (pending.empty())
                    continue;
                record = std::move(pending.front());
                pending.pop_front();
            }

            // The log file is the worker's too. Doing it in `send` meant an
            // open/append/close per record on Roblox's frame thread - measured
            // at 1440 ms for 20 000 lines, which is the whole cost of that
            // loop; the socket was never the expensive part.
            if (record.name == "rconsoleclear") {
                std::ofstream(logPath(), std::ios::trunc | std::ios::binary);
            } else if (record.name != "rconsolename" && record.name != "rconsoleshow"
                       && record.name != "rconsolehide") {
                std::ofstream out(logPath(), std::ios::app | std::ios::binary);
                if (out)
                    out << record.message << "\n";
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                Entry entry;
                entry.seq = nextSeq++;
                entry.name = record.name;
                entry.message = record.message;
                history.push_back(std::move(entry));
                while (history.size() > kHistory)
                    history.pop_front();
            }
        }
    }

    inline void start()
    {
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true))
            return;
        std::thread(worker).detach();
    }

    // Called from a lua_CFunction on Roblox's own thread. Does no I/O: it
    // appends and returns.
    inline void send(const std::string& name, const std::string& message)
    {
        start();

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (pending.size() >= kMaxPending) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            pending.push_back(Record{name, message});
        }
        wake.notify_one();

        // rconsole output stays in the executor's own console history and
        // console.log, and is deliberately NOT mirrored to Roblox's console.
        // rbx::print lands in FLog::CreatorOutput / LogService, which
        // anti-cheats scan (Adonis's Anti_Cheat plugin watches
        // LogService.MessageOut for exploit-console signatures). See
        // [[executor-thread-isolation]].
    }

    // rconsoleinput is two halves on purpose.
    //
    // `prompt()` is the cheap half - clear any stale answer, send the record -
    // and runs on whichever thread called, because it is a file append and a
    // print.
    //
    // `await()` is the half that waits up to thirty seconds for the console app
    // to drop the typed line in a file, and it must **not** run on Roblox's
    // scheduler: doing so froze the entire client for the whole timeout. The
    // macOS reference is the only other tree with rconsole and it does not make
    // that mistake - its `rconsoleinput` wraps the same wait in
    // `Yielding::Yield`, so the Lua thread is suspended and the client keeps
    // running. `UNC::Console::input` puts this on a worker through
    // `Yield::start` for the same reason.
    inline void prompt(const std::string& text)
    {
        std::error_code ignored;
        std::filesystem::remove(inputPath(), ignored);
        send("rconsoleinput", text);
    }

    inline std::string await()
    {
        const auto path = inputPath();
        std::error_code ignored;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!std::filesystem::exists(path, ignored)) {
            if (std::chrono::steady_clock::now() >= deadline)
                return "";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::ifstream file(path, std::ios::binary);
        if (!file)
            return "";
        std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        std::filesystem::remove(path, ignored);

        while (!contents.empty() && (contents.back() == '\n' || contents.back() == '\r'))
            contents.pop_back();
        return contents;
    }
}
