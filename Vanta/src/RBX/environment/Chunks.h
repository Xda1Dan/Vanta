//
//  chunks.hpp
//  vanta-base
//
//  A record of every chunk the executor has loaded, keyed by the name Luau
//  reports for it. isexecutorclosure asks "did we compile the function's
//  source", and debug.info(f, "s") is the only handle on that source a Lua
//  answer can get - so the names have to be stored in exactly the form
//  debug.info hands back, not as the chunknames we passed to luau_load.
//

#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>

namespace chunks {

    inline std::mutex mutex;
    inline std::unordered_set<std::string> ours;
    // Insertion order, so the cap can evict rather than refuse. See mark().
    inline std::deque<std::string> order;
    inline constexpr size_t kMax = 4096;

    // Mirrors luaO_chunkid, which is what turns a chunkname into the short_src
    // debug.info reports: '=' and '@' are stripped, anything else is quoted as
    // a string literal. Roblox's own scripts use the '=' form, so ours never
    // collide with theirs unless a script asks loadstring for a name that does.
    inline std::string id(const std::string& chunkname)
    {
        if (!chunkname.empty() && (chunkname[0] == '=' || chunkname[0] == '@'))
            return chunkname.substr(1);

        // luaO_chunkid stops a plain source at its first newline.
        const size_t line = chunkname.find('\n');
        return "[string \"" + chunkname.substr(0, line) + "\"]";
    }

    inline void mark(const std::string& chunkname)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const std::string name = id(chunkname);
        if (!ours.insert(name).second)
            return;                       // already known - ExecuteScript's "@vanta" every time
        order.push_back(name);

        // A script looping over loadstring with a fresh name each time grows
        // this without bound, so it is capped - but the cap **evicts the
        // oldest** rather than refusing the newest. Refusing was the old
        // behaviour and it is the worse failure: past 4096 chunks every new
        // one silently read as not-ours, for ever, and isexecutorclosure is
        // the thing that answer feeds. Evicting keeps the recent chunks - the
        // ones anything is actually about to ask about - correct.
        //
        // The exact answer would be mac's: it tests
        // `closure->l.p->userdata == &MaxCapabilities`, which needs no set at
        // all. That does **not** transfer to this build - tests/proto.lua scans
        // the whole Proto struct (0x00..0x100) across executor chunks,
        // loadstring results and game closures, and no field holds a value the
        // executor's protos share and the game's protos lack. vic hardcodes
        // three chunk names (`=loadstring`, `@Vicna`, `@Leafy`), which cannot
        // answer for a caller-supplied name at all; src and skid guess from
        // `linedefined == -1`, which a script can forge; stu uses a Proto flags
        // bit and mod a closure set. So the name set stays, bounded honestly.
        while (order.size() > kMax) {
            ours.erase(order.front());
            order.pop_front();
        }
    }

    inline bool isOurs(const std::string& shortSource)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return ours.find(shortSource) != ours.end();
    }
}
