//
//  trace.hpp
//  vanta-base
//
//  Seeing what a closed-source suite actually asked for.
//
//  sUNC is VM-obfuscated and its string constants are encrypted, so a check
//  that fails cannot be read - the only way to learn what it handed us is to
//  log it from inside the C function it called. It must be from inside: sUNC
//  checks closure identity with iscclosure, isexecutorclosure and
//  getfunctionhash, so wrapping a global in Lua changes the thing under test.
//
//  Tracing is off unless a script asks for it by name:
//
//      __vanta_trace("getrawmetatable", "checkcaller")   -- on
//      __vanta_trace()                                   -- off
//
//  and when it is off the cost is one relaxed atomic load per traced call.
//

#pragma once

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "luau/VM/include/lua.h"
#include "Journal.h"
#include "LuaObject.h"
#include "ClientAPI.h"

namespace trace {

    // Read on every traced call, so it is checked before the mutex is ever
    // touched. Nothing is traced until a script turns something on.
    inline std::atomic<bool> anyEnabled{false};
    inline std::mutex mutex;
    inline std::unordered_set<std::string> names;

    // sUNC's own VM calls some of these in a hot loop - one run made 422,334
    // calls to getrawmetatable - so an uncapped trace does not just fill the
    // log, it changes the timings the suite is measuring. Each name gets a
    // budget and then goes quiet.
    // 120 was too small to read a whole run: `getproto` spent its budget
    // before the `debug.setupvalue` check ran, so any call it made was
    // invisible and the check looked as though it called nothing at all. The
    // budget exists to stop an uncapped trace changing the timings it measures,
    // and the per-call cost once a name is spent is a handful of relaxed loads,
    // so it can afford to be much larger than the number of calls a check makes.
    inline constexpr int kBudget = 1500;
    inline std::unordered_map<std::string, int> budget;

    // Names that have spent their budget, by the address of the literal each
    // call site passes. A spent name is then rejected by a handful of relaxed
    // pointer compares instead of a mutex and two std::string constructions.
    //
    // This matters far more than it looks. sUNC calls `getrawmetatable`
    // **422,334 times** in a run, and the budget check used to sit *behind* the
    // lookup - so once the budget was spent every one of those calls still paid
    // for it. Measured on the same suite, same 74/13 either way: worst
    // scheduler stall **18 s with tracing off, 154 s with it on**. A diagnostic
    // that changes what it is measuring by an order of magnitude is worse than
    // no diagnostic, and this one was hiding behind "it goes quiet after 120".
    inline constexpr int kQuietSlots = 64;
    inline std::atomic<const void*> quiet[kQuietSlots];

    inline bool isQuiet(const char* name)
    {
        for (int i = 0; i < kQuietSlots; i++) {
            const void* held = quiet[i].load(std::memory_order_relaxed);
            if (!held)
                return false;
            if (held == name)
                return true;
        }
        return false;
    }

    inline void markQuiet(const char* name)
    {
        for (int i = 0; i < kQuietSlots; i++) {
            const void* expected = nullptr;
            if (quiet[i].load(std::memory_order_relaxed) == name)
                return;
            if (quiet[i].compare_exchange_strong(expected, name,
                                                 std::memory_order_relaxed))
                return;
        }
    }

    inline bool on(const char* name)
    {
        if (!anyEnabled.load(std::memory_order_relaxed))
            return false;
        if (isQuiet(name))
            return false;
        std::lock_guard<std::mutex> lock(mutex);
        if (names.count(name) == 0 && names.count("*") == 0) {
            markQuiet(name);            // never traced: never look again
            return false;
        }
        int& left = budget[name];
        if (left >= kBudget) {
            markQuiet(name);
            return false;
        }
        left++;
        if (left == kBudget) {
            markQuiet(name);
            // Braces matter here: without them the journal line was written on
            // *every* traced call rather than once, so the log carried a
            // "going quiet" line in front of every trace line.
            rbx::print(0, "TRC %s | budget spent, going quiet", name);
            journal::writef("TRC %s | budget spent, going quiet", name);
        }
        return true;
    }

    // A short, side-effect-free description of a stack slot.
    //
    // lua_tolstring is deliberately not used for numbers: in Luau it converts
    // the value on the stack in place, so describing an argument would change
    // it. Numbers are read straight out of the TValue instead.
    inline std::string describe(uint64_t state, int index)
    {
        char buffer[160];
        const int type = rbx::lua_type(state, index);
        if (type < 0)
            return "none";
        switch (type) {
            case luaobj::kTagNil:
                return "nil";
            case luaobj::kTagBoolean:
                return rbx::lua_toboolean(state, index) ? "true" : "false";
            case luaobj::kTagNumber:
            case luaobj::kTagInteger: {
                // lua_tointegerx truncates - it turned every out-of-range value
                // sUNC passed into INT_MIN, which hid what was really given -
                // and lua_tolstring would rewrite the slot. So the double is
                // read straight out of the frame: a positive index is
                // base + (index - 1) by definition.
                if (index > 0) {
                    auto* base = luaobj::read<uint8_t*>(reinterpret_cast<void*>(state),
                                                        luaobj::state::base);
                    if (base) {
                        const uint8_t* slot = base + (index - 1) * luaobj::kValueSize;
                        snprintf(buffer, sizeof(buffer), "%.14g",
                                 luaobj::read<double>(slot, luaobj::value::gc));
                        return buffer;
                    }
                }
                int isnum = 0;
                const int whole = rbx::lua_tointegerx(state, index, &isnum);
                snprintf(buffer, sizeof(buffer), "%d", whole);
                return buffer;
            }
            case luaobj::kTagString: {
                size_t length = 0;
                const char* text = rbx::lua_tolstring(state, index, &length);
                if (!text)
                    return "string(?)";
                std::string out(text, length > 48 ? 48 : length);
                for (char& c : out) {
                    if (c == '\n' || c == '\r' || c == '\t')
                        c = ' ';
                }
                snprintf(buffer, sizeof(buffer), "\"%s\"%s", out.c_str(), length > 48 ? "..." : "");
                return buffer;
            }
            case luaobj::kTagFunction: {
                const void* closure = rbx::lua_topointer(state, index);
                const bool isC = closure && luaobj::byte(closure, luaobj::closure::isC);
                snprintf(buffer, sizeof(buffer), "%s:%p", isC ? "cfunc" : "lfunc", closure);
                return buffer;
            }
            case luaobj::kTagTable:
                snprintf(buffer, sizeof(buffer), "table:%p", rbx::lua_topointer(state, index));
                return buffer;
            case luaobj::kTagUserdata:
                snprintf(buffer, sizeof(buffer), "userdata:%p", rbx::lua_topointer(state, index));
                return buffer;
            default:
                snprintf(buffer, sizeof(buffer), "type%d:%p", type, rbx::lua_topointer(state, index));
                return buffer;
        }
    }

    inline std::string slots(uint64_t state, int from, int to)
    {
        std::string out;
        for (int i = from; i <= to; i++) {
            if (!out.empty())
                out += ", ";
            out += describe(state, i);
        }
        return out;
    }

    // Traces one call. The arguments are read at construction, before the
    // function has had a chance to move them; the results are read from done().
    struct Scope {
        uint64_t state;
        const char* name;
        bool active;
        int arguments = 0;

        Scope(uint64_t state, const char* name) : state(state), name(name), active(on(name))
        {
            if (!active)
                return;
            arguments = rbx::lua_gettop(state);
            const std::string text = slots(state, 1, arguments);
            rbx::print(0, "TRC %s(%s)", name, text.c_str());
            journal::writef("TRC %s(%s)", name, text.c_str());
        }

        // Answers `count` unchanged so a traced function can end in
        // `return trace.done(n);` without changing what it returns.
        int done(int count)
        {
            if (!active || count < 0)
                return count;
            const int top = rbx::lua_gettop(state);
            const std::string text = slots(state, top - count + 1, top);
            rbx::print(0, "TRC %s -> %s", name, text.c_str());
            journal::writef("TRC %s -> %s", name, text.c_str());
            return count;
        }

        // For anything worth saying mid-call.
        void note(const std::string& what) const
        {
            if (!active)
                return;
            rbx::print(0, "TRC %s | %s", name, what.c_str());
            journal::writef("TRC %s | %s", name, what.c_str());
        }
    };

    // __vanta_trace(...): every name given is traced, no names turns it off.
    inline int set(lua_State* L)
    {
        uint64_t state = reinterpret_cast<uint64_t>(L);
        const int count = rbx::lua_gettop(state);

        std::lock_guard<std::mutex> lock(mutex);
        names.clear();
        budget.clear();
        for (int i = 1; i <= count; i++) {
            size_t length = 0;
            if (const char* name = rbx::lua_tolstring(state, i, &length))
                names.insert(std::string(name, length));
        }
        for (int i = 0; i < kQuietSlots; i++)
            quiet[i].store(nullptr, std::memory_order_relaxed);
        anyEnabled.store(!names.empty(), std::memory_order_relaxed);
        rbx::lua_pushinteger(state, static_cast<int>(names.size()));
        return 1;
    }
}
