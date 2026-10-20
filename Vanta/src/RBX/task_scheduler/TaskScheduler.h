//
//  taskscheduler.hpp
//  vanta-base
//
//  Created by w4ddo on 20/08/2026.
//
//  Finding the ScriptContext of the game we are actually in.
//
//  Every read in this file goes through `luaobj::safeRead`, and that is the
//  point of it. The job list belongs to Roblox and Roblox rewrites it on its own
//  threads - jobs are added and removed exactly when a DataModel is being built
//  or torn down, which is precisely when this is being polled hardest. A plain
//  dereference of a pointer taken out of that vector reads freed or unmapped
//  memory sooner or later, and that is a segfault inside Roblox with our name on
//  it.
//
//  The references agree that an unguarded walk is not on: five of the six never
//  walk the list at all (mac is handed the ScriptContext by the engine, Vicna
//  and SkidBase go through the DataModel pointer, Module-Source and RbxStu
//  through their own DataModel abstractions), and sourcessss - the only one that
//  does walk - validates every job pointer with `VirtualQuery` before touching
//  it. `mach_vm_read_overwrite` is how that is spelled here: the kernel does the
//  copy, so a bad address is a return code instead of a signal.
//

#pragma once

#include <cstring>
#include <cstdint>
#include <cstdio>
#include <string>

#include "luau/VM/include/lua.h"
#include "LuaObject.h"
#include "Offsets.h"
#include "ClientAPI.h"

namespace taskscheduler
{
    // Longest job name we care about; anything larger is a torn read rather than
    // a name. Roblox mutates the job list on its own threads while we walk it,
    // so a length taken straight from that memory cannot be trusted - handing a
    // garbage size to std::string throws bad_alloc and terminates the process.
    inline constexpr size_t maxJobName = 256;

    // Longest string libc++ stores inline before it heap-allocates.
    inline constexpr size_t shortCapacity = 22;

    // A ceiling on the walk. `jobs_end` is read out of live memory, so a torn
    // read can put it gigabytes past `jobs_start`; without this the loop would
    // spend minutes stepping through nothing. Roblox runs a few dozen jobs.
    inline constexpr size_t maxJobs = 512;

    // libc++ has two std::string layouts. arm64 Apple uses the alternate one -
    // characters at +0x00, long flag in the high bit of byte 0x17 - which is
    // what this originally assumed. x86_64 macOS uses the default layout:
    //
    //   long:  __cap_ at +0x00 (long flag in its LOW bit)
    //          __size_ at +0x08
    //          __data_ pointer at +0x10
    //   short: byte 0 holds (size << 1), characters start at byte 1
    //
    // Reading it the arm64 way prepends the size byte to every short name and
    // treats the capacity field as the data pointer for long ones, which is why
    // "WaitingHybridScriptsJob" (23 chars, so a long string) never matched.
    inline std::string readString(uintptr_t strAddr) {
        if (!strAddr) return "";

        // One copy of the whole control block, so the layout decision and the
        // fields it reads cannot disagree with each other because something
        // rewrote the string in between.
        uint8_t header[24];
        if (!luaobj::safeRead(header, strAddr, sizeof(header)))
            return "";

        uintptr_t cap;
        std::memcpy(&cap, header, sizeof(cap));
        const bool isLong = (cap & 1) != 0;

        if (isLong) {
            size_t length;
            uintptr_t dataPtr;
            std::memcpy(&length, header + 0x08, sizeof(length));
            std::memcpy(&dataPtr, header + 0x10, sizeof(dataPtr));
            if (!dataPtr || length == 0 || length > maxJobName)
                return "";
            std::string out(length, '\0');
            if (!luaobj::safeRead(out.data(), dataPtr, length))
                return "";
            return out;
        }

        const size_t length = header[0] >> 1;
        if (length > shortCapacity)
            return "";
        return std::string(reinterpret_cast<const char*>(header + 1), length);
    }

    inline std::string readJobName(uintptr_t job) {
        return readString(job + 0x18);
    }

    // Calls `visit(job)` for every entry in the scheduler's job list, and stops
    // when it answers false. Answers whether the list could be read at all.
    template <typename Visit>
    inline bool forEachJob(Visit visit) {
        // The scheduler singleton does not exist yet for the first moments of
        // startup, and this runs on a timer from the moment we are injected.
        const uintptr_t scheduler = luaobj::safeValue<uintptr_t>(offsets::taskscheduler);
        if (!scheduler)
            return false;

        const uintptr_t start = luaobj::safeValue<uintptr_t>(scheduler + offsets::jobs_start);
        const uintptr_t end = luaobj::safeValue<uintptr_t>(scheduler + offsets::jobs_end);
        if (!start || !end || end < start || (end - start) / 0x10 > maxJobs)
            return false;

        for (uintptr_t i = start; i < end; i += 0x10) {
            const uintptr_t job = luaobj::safeValue<uintptr_t>(i);
            if (!job)
                continue;
            if (!visit(job))
                return true;
        }
        return true;
    }

    // Diagnostic: prints the scheduler pointer and every job name it can see, so
    // a mismatched jobs_start / name offset is visible instead of silent.
    inline void dumpJobs() {
        const uintptr_t scheduler = luaobj::safeValue<uintptr_t>(offsets::taskscheduler);
        printf("[vanta] scheduler=0x%llx\n", (unsigned long long)scheduler);

        int n = 0;
        const bool walked = forEachJob([&](uintptr_t job) {
            printf("[vanta]   [%d] %s\n", n, readJobName(job).c_str());
            return ++n < 80;
        });
        if (!walked)
            printf("[vanta] the job list could not be read\n");
        fflush(stdout);
    }

    // The DataModel a ScriptContext belongs to, and whether that DataModel is a
    // real game rather than the home screen.
    //
    // This is how the references tell the two apart. sourcessss walks to
    // `ScriptContext + parent` and compares the DataModel's *name* - its comment
    // is literally "We do this so we won't get the Homepage
    // WaitingHybridScriptsJob" - and the macOS reference tests the DataModel's
    // **PlaceId** instead (`if (!PlaceId && Workspace == 0) return`). The
    // PlaceId is the better of the two: a name is a string that differs between
    // builds (the macOS tree wants "Ugc" on ARM and rejects it on x86), while
    // "has a place id" means the same thing everywhere.
    inline uintptr_t dataModelOf(uintptr_t scriptContext) {
        if (!scriptContext)
            return 0;
        return luaobj::safeValue<uintptr_t>(scriptContext + offsets::scriptcontext_parent);
    }

    inline int64_t placeIdOf(uintptr_t dataModel) {
        if (!dataModel)
            return 0;
        return luaobj::safeValue<int64_t>(dataModel + offsets::datamodel_placeid);
    }

    // The ScriptContext of the DataModel we are actually playing in.
    //
    // This used to collect the two `WaitingHybridScriptsJob` entries and pick
    // the one at the **higher heap address**. That is not an ordering - the
    // allocator makes no such promise - and it needed there to be exactly two:
    // with one job (which is what the home screen has, and what a client shows
    // for a moment mid-teleport) it returned 0 and the executor waited for a
    // game that was already running. Both halves of that are "no execution on
    // this join".
    //
    // Now every job of that name is checked and the one whose DataModel has a
    // place id wins, which is what the references do and works for one job, two
    // or three.
    inline uintptr_t getScriptContext() {
        uintptr_t found = 0;
        forEachJob([&](uintptr_t job) {
            if (readJobName(job) != "WaitingHybridScriptsJob")
                return true;

            const uintptr_t context = luaobj::safeValue<uintptr_t>(job + offsets::whsjtosc);
            if (!context)
                return true;

            // No place id, no game. There is deliberately no "well, take this
            // one anyway" fallback: building the environment on a DataModel
            // that is the home screen, or one that is being torn down, is worse
            // than waiting one more second for the real one. The macOS
            // reference refuses on exactly this test.
            if (placeIdOf(dataModelOf(context)) == 0)
                return true;

            found = context;
            return false;
        });
        return found;
    }

    // The place we are in, for telling one game from the next.
    inline int64_t currentPlaceId() {
        return placeIdOf(dataModelOf(getScriptContext()));
    }

    inline bool isLoaded() {
        return getScriptContext() != 0;
    }

}
