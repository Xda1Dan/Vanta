//
//  loadguard.hpp
//  vanta-base
//
//  Everything Roblox's own loader does around a luau_load, done around ours.
//
//  There are two of them and they are easy to get half-right, which is why they
//  are one object: the chunk has to be opted out of the signature footer check
//  (signature.hpp), and the thread has to carry the load clock while the load
//  runs (gameldr.hpp). Miss the first and a script over 280 bytes will not
//  parse; miss the second and nothing visibly breaks at all - the engine simply
//  reports the load as untimed. A single guard at every call site means adding
//  a load path cannot silently skip one of them.
//

#pragma once

#include "LoadClock.h"
#include "Signature.h"

namespace load {

    // Construct immediately before luau_load, with the bytecode buffer that is
    // about to be handed to it, and keep it alive across the call.
    class Ours {
    public:
        explicit Ours(const char* bytecode) : claim(bytecode) {}

        Ours(const Ours&) = delete;
        Ours& operator=(const Ours&) = delete;

    private:
        signature::Claim claim;
        gameldr::Stamp stamp;
    };
}
