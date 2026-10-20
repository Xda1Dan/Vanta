//
//  gameldr.hpp
//  vanta-base
//
//  Roblox times its own script loads and reports the ones that were not timed.
//  This stamps the executor's loads the same way its own loader stamps them.
//

#pragma once

#include <cstdint>

#include "Offsets.h"

// ---------------------------------------------------------------------------
// What the engine does, and what it catches
//
// `luaVM::load` is wrapped (0x10141f2dd) by a few lines that put a clock on the
// thread for the duration of the load:
//
//     cmp   dword [rip+0x10722fab8], 0     ; the limit, 0 turns it all off
//     je    .plain
//     call  0x10299eede                    ; unix time, in seconds
//     lea   rdi, [rip+0x1077294d0]         ; the slot's descriptor
//     call  0x1016a51fc                    ; -> this thread's copy of the slot
//     mov   [rax], <now>
//     call  luaVM::load
//     lea   rdi, [rip+0x1077294d0]
//     call  0x1016a51fc
//     mov   qword [rax], 0
//
// and the first thing the body of luau_load does (0x102c98016 +0x45) is call
// 0x10141f0ed, which reads that slot back:
//
//     stamp = *slot;  delta = now - stamp;
//     if ((uint64)delta < limit) return;              // loaded under the clock
//     if (stamp == 0)   report "GameLdr-PerformanceCheck0,GameLdr-PerformanceCheck1"
//     else if (delta<0) report "GameLdr-PerformanceCheck0,GameLdr-PerformanceCheck2"
//     else              report "GameLdr-PerformanceCheck0,GameLdr-PerformanceCheck3"
//
// so the three things it separates are *no stamp at all*, a stamp in the
// future, and a stamp from long ago - and calling luau_load directly, which is
// what every executor does, is the first of the three every single time. The
// report is an interlocked flag the stats ping item reads back alongside the
// three `registry_free` bits (see compiler.hpp for the LOP_CAPTURE one).
//
// The fix is not to defeat the check - it is to load the way the engine loads.
// A stamped thread and a cleared slot afterwards is exactly the state Roblox's
// own wrapper leaves behind, so nothing downstream can tell the two apart.
//
// The slot is per *OS thread*, which is what the guard being scope-bound
// relies on: every luau_load this executor performs runs on Roblox's own script
// thread, under the pump, and a load cannot yield or migrate.

namespace gameldr {

    // The gate and the age limit are one dword: zero means the engine is not
    // timing loads at all this session, in which case its own wrapper does not
    // stamp either and neither does this.
    inline bool timing()
    {
        if (!offsets::loadclocklimit)
            return false; // no load-time clock check reachable on this build
        return *reinterpret_cast<const volatile int32_t*>(offsets::loadclocklimit) != 0;
    }

    // Roblox's own clock, not time(): same source, same units, no chance of
    // disagreeing with the value the check compares against.
    inline int64_t now()
    {
        return reinterpret_cast<int64_t (*)()>(offsets::loadclocknow)();
    }

    // Descriptor -> the calling thread's own 8-byte slot, constructing it on
    // first use. Roblox calls this on every script load on this same thread, so
    // by the time any of ours runs the thread's block already exists.
    inline int64_t* slot()
    {
        return reinterpret_cast<int64_t* (*)(const void*)>(offsets::threadlocalslot)(
            reinterpret_cast<const void*>(offsets::loadclockkey));
    }

    // Holds the clock for as long as it is on the stack. Mirrors the engine's
    // wrapper: a timestamp going in, a zero coming out - including on the error
    // path out of luau_load, which is why this is a destructor and not two
    // calls around the load.
    class Stamp {
    public:
        Stamp()
        {
            if (!timing())
                return;
            held = slot();
            if (held)
                *held = now();
        }

        ~Stamp()
        {
            if (held)
                *held = 0;
        }

        Stamp(const Stamp&) = delete;
        Stamp& operator=(const Stamp&) = delete;

    private:
        int64_t* held = nullptr;
    };

    // -----------------------------------------------------------------------
    // The accessor that gathers all four flags, and the answer a clean client
    // gives.
    //
    // 0x1014a68a6 packs them into the nibble the stats ping item sends. Bits
    // 0..2 are `registry_free`'s 28, 29 and 30 read off both of the two
    // lua_States the ScriptContext keeps and ANDed together - 1 meaning the
    // check passed, which is why a state it cannot reach contributes a 1 - and
    // bit 3 is the load-clock report, which is 1 when it *has* fired. So the
    // clean answer is 7, and `tests/detect.lua` reads 0x7000068D out of a live
    // client's registry_free and an unfired load clock, which is that exact 7.
    //
    // Nothing here is load-bearing: with the load clock stamped and the
    // capture operand written, every one of the four is already clean, which
    // the same test measures directly rather than through this function. It is
    // the belt to that pair of braces - one more thing between a check this
    // executor has not thought of and the report of it.
    inline constexpr uint32_t kClean = 7;

    using StatusFn = uint32_t (*)(void*);
    inline StatusFn statusOriginal = nullptr;

    inline uint32_t status(void* self)
    {
        (void)self;
        return kClean;
    }
}
