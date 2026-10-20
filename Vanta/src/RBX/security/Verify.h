//
//  verify.hpp
//  vanta-base
//
//  Validates that an offset still points at a real function entry before it is
//  called or patched. RobloxPlayer ships a __unwind_info section listing the
//  entry point of every function in the image, so a stale offset - one that
//  lands in the middle of a function after a Roblox update - can be rejected
//  instead of silently corrupting live code.
//

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

#include <mach-o/dyld.h>
#include <mach-o/loader.h>

#include "Journal.h"
#include "Offsets.h"

namespace verify {

    struct UnwindInfoHeader {
        uint32_t version;
        uint32_t commonEncodingsArraySectionOffset;
        uint32_t commonEncodingsArrayCount;
        uint32_t personalityArraySectionOffset;
        uint32_t personalityArrayCount;
        uint32_t indexSectionOffset;
        uint32_t indexCount;
    };

    struct IndexEntry {
        uint32_t functionOffset;
        uint32_t secondLevelPagesSectionOffset;
        uint32_t lsdaIndexArraySectionOffset;
    };

    inline const uint8_t* findUnwindInfo(const mach_header_64* header, uint32_t* sizeOut)
    {
        const load_command* command =
            reinterpret_cast<const load_command*>(reinterpret_cast<uintptr_t>(header) + sizeof(mach_header_64));

        for (uint32_t i = 0; i < header->ncmds; i++) {
            if (command->cmd == LC_SEGMENT_64) {
                const segment_command_64* segment = reinterpret_cast<const segment_command_64*>(command);
                const section_64* section =
                    reinterpret_cast<const section_64*>(reinterpret_cast<uintptr_t>(segment) + sizeof(segment_command_64));

                for (uint32_t s = 0; s < segment->nsects; s++, section++) {
                    if (std::strncmp(section->sectname, "__unwind_info", 16) == 0) {
                        *sizeOut = static_cast<uint32_t>(section->size);
                        return reinterpret_cast<const uint8_t*>(section->addr + hostImage().slide);
                    }
                }
            }
            command = reinterpret_cast<const load_command*>(reinterpret_cast<uintptr_t>(command) + command->cmdsize);
        }
        return nullptr;
    }

    // Function entry points, as offsets from the image base, sorted.
    inline const std::vector<uint32_t>& functionStarts()
    {
        static const std::vector<uint32_t> starts = [] {
            std::vector<uint32_t> result;

            const mach_header_64* header = nullptr;
            const uint32_t count = _dyld_image_count();
            for (uint32_t i = 0; i < count; i++) {
                const char* path = _dyld_get_image_name(i);
                if (!path)
                    continue;
                const char* name = std::strrchr(path, '/');
                name = name ? name + 1 : path;
                if (std::strcmp(name, "RobloxPlayer") == 0) {
                    header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
                    break;
                }
            }
            if (!header)
                return result;

            uint32_t size = 0;
            const uint8_t* info = findUnwindInfo(header, &size);
            if (!info || size < sizeof(UnwindInfoHeader))
                return result;

            UnwindInfoHeader unwind;
            std::memcpy(&unwind, info, sizeof(unwind));
            if (unwind.version != 1)
                return result;

            for (uint32_t i = 0; i < unwind.indexCount; i++) {
                IndexEntry index;
                std::memcpy(&index, info + unwind.indexSectionOffset + i * sizeof(IndexEntry), sizeof(index));
                if (index.secondLevelPagesSectionOffset == 0 ||
                    index.secondLevelPagesSectionOffset >= size)
                    continue;

                const uint8_t* page = info + index.secondLevelPagesSectionOffset;
                uint32_t kind;
                std::memcpy(&kind, page, sizeof(kind));

                if (kind == 2) { // UNWIND_SECOND_LEVEL_REGULAR
                    uint16_t entryPageOffset, entryCount;
                    std::memcpy(&entryPageOffset, page + 4, 2);
                    std::memcpy(&entryCount, page + 6, 2);
                    for (uint16_t e = 0; e < entryCount; e++) {
                        uint32_t functionOffset;
                        std::memcpy(&functionOffset, page + entryPageOffset + e * 8, 4);
                        result.push_back(functionOffset);
                    }
                } else if (kind == 3) { // UNWIND_SECOND_LEVEL_COMPRESSED
                    uint16_t entryPageOffset, entryCount;
                    std::memcpy(&entryPageOffset, page + 4, 2);
                    std::memcpy(&entryCount, page + 6, 2);
                    for (uint16_t e = 0; e < entryCount; e++) {
                        uint32_t entry;
                        std::memcpy(&entry, page + entryPageOffset + e * 4, 4);
                        result.push_back(index.functionOffset + (entry & 0xFFFFFF));
                    }
                }
            }

            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }();
        return starts;
    }

    // True when `addr` is the first instruction of a function in RobloxPlayer.
    inline bool isFunctionEntry(uintptr_t addr)
    {
        const std::vector<uint32_t>& starts = functionStarts();
        if (starts.empty())
            return true; // cannot tell; do not block on a parsing failure

        const uintptr_t base = 0x100000000 + hostImage().slide;
        if (addr < base)
            return false;

        const uintptr_t offset = addr - base;
        if (offset > 0xFFFFFFFFull)
            return false;

        if (std::binary_search(starts.begin(), starts.end(), static_cast<uint32_t>(offset)))
            return true;

        // Compact unwind merges adjacent entries that share an encoding, so a
        // real entry point can be absent from the table. Fall back to the
        // prologue every frame-pointer function in this binary begins with.
        const uint8_t* code = reinterpret_cast<const uint8_t*>(addr);
        return code[0] == 0x55 && code[1] == 0x48 && code[2] == 0x89 && code[3] == 0xe5;
    }

    // Logs every offset that no longer lines up, and returns how many are stale.
    inline int reportStaleOffsets()
    {
        struct Entry { const char* name; uintptr_t addr; };
        const Entry entries[] = {
            {"luau_load", offsets::luau_load},
            {"lua_newthread", offsets::lua_newthread},
            {"lua_pushcclosurek", offsets::lua_pushcclosurek},
            {"lua_pushlstring", offsets::lua_pushlstring},
            {"lua_pushnil", offsets::lua_pushnil},
            {"lua_pushvalue", offsets::lua_pushvalue},
            {"lua_setfield", offsets::lua_setfield},
            {"lua_getthreaddata", offsets::lua_getthreaddata},
            {"luaL_sandboxthread", offsets::luaL_sandboxthread},
            {"lua_gettop", offsets::lua_gettop},
            {"lua_settop", offsets::lua_settop},
            {"lua_tolstring", offsets::lua_tolstring},
            {"lua_tointegerx", offsets::lua_tointegerx},
            {"lua_toboolean", offsets::lua_toboolean},
            {"lua_createtable", offsets::lua_createtable},
            {"lua_rawseti", offsets::lua_rawseti},
            {"lua_setmetatable", offsets::lua_setmetatable},
            {"lua_pushinteger", offsets::lua_pushinteger},
            {"lua_setsafeenv", offsets::lua_setsafeenv},
            {"lua_getfield", offsets::lua_getfield},
            {"lua_iscfunction", offsets::lua_iscfunction},
            {"lua_islfunction", offsets::lua_islfunction},
            {"lua_pushboolean", offsets::lua_pushboolean},
            {"lua_ref", offsets::lua_ref},
            {"lua_unref", offsets::lua_unref},
            {"luaL_checktype", offsets::luaL_checktype},
            {"lua_getmetatable", offsets::lua_getmetatable},
            {"lua_setreadonly", offsets::lua_setreadonly},
            {"lua_getreadonly", offsets::lua_getreadonly},
            {"lua_namecallatom", offsets::lua_namecallatom},
            {"lua_error", offsets::lua_error},
            {"print", offsets::print},
            {"getglobalstate", offsets::getglobalstate},
            {"identitytocapabilities", offsets::identitytocapabilities},
            {"setprotocapabilities", offsets::setprotocapabilities},
            {"taskspawn", offsets::taskspawn},
            {"checkbytecodesignature", offsets::checkbytecodesignature},
            {"hook_pl", offsets::hook_pl},
            {"hook_tp", offsets::hook_tp},

            // Everything below was derived after this list was first written
            // and was never added to it, so 23 of the 62 rebased addresses in
            // offsets.hpp went unchecked - including every engine function that
            // is *called with arguments we build*, which is the class that
            // corrupts the process rather than merely failing. 22 of the 23
            // are code; the 23rd is excluded below.
            //
            // `offsets::taskscheduler` and `offsets::debugappupdatestatus`
            // are deliberately absent: both are __DATA addresses, not function
            // entries, so isFunctionEntry would reject perfectly good ones.
            // debugappupdatestatus checks itself instead - silenceUpdatePrompt
            // refuses to write unless the address still holds an empty
            // std::string.
            {"lua_call", offsets::lua_call},
            {"lua_type", offsets::lua_type},
            {"lua_topointer", offsets::lua_topointer},
            {"lua_touserdata", offsets::lua_touserdata},
            {"lua_tocfunction", offsets::lua_tocfunction},
            {"lua_rawgeti", offsets::lua_rawgeti},
            {"lua_rawset", offsets::lua_rawset},
            {"lua_insert", offsets::lua_insert},
            {"lua_remove", offsets::lua_remove},
            {"lua_getfenv", offsets::lua_getfenv},
            {"lua_setfenv", offsets::lua_setfenv},
            {"lua_gc", offsets::lua_gc},
            {"lua_clonefunction", offsets::lua_clonefunction},
            {"lua_pushlightuserdatatagged", offsets::lua_pushlightuserdatatagged},
            {"luaF_newLclosure", offsets::luaF_newLclosure},
            {"luaM_visitgco", offsets::luaM_visitgco},
            {"identitypropagator", offsets::identitypropagator},
            {"islegalsendevent", offsets::islegalsendevent},
            {"decompressbytecode", offsets::decompressbytecode},
            {"pushinstance", offsets::pushinstance},
            {"fireproximityprompt", offsets::fireproximityprompt},
            {"firetouchinterest", offsets::firetouchinterest},

            // The two callable halves of the load clock. `loadclocklimit` and
            // `loadclockkey` are its other two and are absent for the same
            // reason taskscheduler is: they are __DATA, not function entries.
            // Both are read through gameldr.hpp, which treats a null slot as
            // "do not stamp" rather than writing through it.
            {"loadclocknow", offsets::loadclocknow},
            {"threadlocalslot", offsets::threadlocalslot},
        };

        const uintptr_t base = 0x100000000 + hostImage().slide;
        int stale = 0;
        for (const Entry& entry : entries) {
            // Offsets that this build genuinely does not have are zeroed in
            // offsets.hpp (the load clock is absent on 0.742). They are not
            // called; skip them rather than declaring the whole build stale.
            if (entry.addr == 0)
                continue;
            if (isFunctionEntry(entry.addr))
                continue;
            stale++;
            // The journal as well as stdout: a stale offset makes main.cpp
            // stand down and run *nothing*, and stdout goes nowhere under any
            // of the harnesses, so this was the one failure that could not be
            // diagnosed from a test run. See the diagnosing-a-frozen-client
            // memory - that is what journal.hpp exists for.
            journal::writef("OFFSET stale %s 0x%llx (unslid 0x%llx)", entry.name,
                            static_cast<unsigned long long>(entry.addr),
                            static_cast<unsigned long long>(entry.addr - base + 0x100000000));
            printf("[vanta] stale offset: %-24s 0x%llx (unslid 0x%llx)\n",
                   entry.name,
                   static_cast<unsigned long long>(entry.addr),
                   static_cast<unsigned long long>(entry.addr - base + 0x100000000));
        }
        journal::writef("OFFSET %d of %zu stale", stale, sizeof(entries) / sizeof(entries[0]));
        printf("[vanta] %d of %zu offsets are stale for this Roblox build\n",
               stale, sizeof(entries) / sizeof(entries[0]));
        fflush(stdout);
        return stale;
    }
}
