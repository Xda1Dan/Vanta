//
//  journal.hpp
//  vanta-base
//
//  A log that survives the client.
//
//  Everything diagnostic used to go through rbx::print into Roblox's own log,
//  which is fine until the thing being diagnosed is the client freezing or
//  dying: the last and most interesting lines are exactly the ones still
//  sitting in Roblox's buffer when it stops. This writes to a plain file,
//  flushed on every line, so whatever was happening at the moment of a hang is
//  already on disk - and it can be watched live from outside the process with
//  `tail -f /tmp/vanta_executor.log`.
//
//  It is deliberately dumb: one mutex, one FILE*, line-buffered, no rotation
//  beyond truncating at startup. Nothing here may allocate on a hot path or
//  take a lock that Lua could already be holding.
//

#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <sys/time.h>
#include <unistd.h>

namespace journal {

    inline constexpr const char* kPath = "/tmp/vanta_executor.log";

    inline std::mutex mutex;
    inline FILE* file = nullptr;
    inline bool opened = false;

    // Appended to, with a banner per process.
    //
    // This used to truncate at startup so that a run's log was that run's. That
    // threw away the run it was meant to capture: when the client crashes,
    // Roblox brings up another player process within seconds, and *that*
    // process truncated the file before the harness could copy it - so the one
    // log that would have explained the crash was replaced by nine watchdog
    // lines. Appending cannot lose a run; run.sh keeps a per-run copy either
    // way, and the banner is what separates one process's lines from the next.
    //
    // A ceiling still applies, so a machine left running for days does not fill
    // the disk: past kMaxBytes the file starts again.
    inline constexpr long kMaxBytes = 64L << 20;

    inline void openLocked()
    {
        if (opened)
            return;
        opened = true;

        const char* mode = "a";
        if (FILE* existing = std::fopen(kPath, "r")) {
            std::fseek(existing, 0, SEEK_END);
            if (std::ftell(existing) > kMaxBytes)
                mode = "w";
            std::fclose(existing);
        }

        file = std::fopen(kPath, mode);
        if (file) {
            std::setvbuf(file, nullptr, _IOLBF, 4096);
            std::fprintf(file, "\n===== process %d started =====\n",
                         static_cast<int>(getpid()));
            std::fflush(file);
        }
    }

    inline void write(const char* text)
    {
        std::lock_guard<std::mutex> lock(mutex);
        openLocked();
        if (!file)
            return;

        timeval now{};
        gettimeofday(&now, nullptr);
        tm parts{};
        localtime_r(&now.tv_sec, &parts);
        std::fprintf(file, "%02d:%02d:%02d.%03d %s\n", parts.tm_hour, parts.tm_min,
                     parts.tm_sec, static_cast<int>(now.tv_usec / 1000), text);
        std::fflush(file);
    }

    inline void writef(const char* format, ...)
    {
        char buffer[2048];
        va_list args;
        va_start(args, format);
        std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        write(buffer);
    }

}   // namespace journal
