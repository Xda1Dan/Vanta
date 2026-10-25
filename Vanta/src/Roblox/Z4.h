//
//  Z4.hpp
//  vanta-base
//
//  The Z4 dump for 0.744.0.7441080 (build version-546c6da637854de3): 769
//  addresses, per-class field offsets, remote/event entry points and the
//  encoded Luau struct layouts. These are the offsets the executor was NOT
//  already reading through Offsets.h - Offsets.h keeps everything the runtime
//  path needs (the two dumps agree wherever they overlap: luau_load, print,
//  identitytocapabilities, fireproximityprompt) - and this is the surface new
//  work should reach for first, because it has orders of magnitude more.
//
//  The Z4 dumper writes its addresses WITHOUT the Mach-O vmaddr base: its
//  0x2b3bc62 is Offsets.h's 0x102B3BC62. REBASE is the macro its values are
//  written against; it has to be defined before the includes and it has to
//  add the 0x100000000 base as well as the slide, which is what rebase()
//  plus the literal does. DataModelPointer::* and DataModel::Pointer() are
//  the exception: they are consumed as raw offsets from the image base, so
//  call that helper with the image header address (0x100000000 + slide),
//  never with a rebased pointer.
//
#pragma once

#include "Offsets.h"

#define REBASE(x) (rebase((x) + 0x100000000ull))
#include "offsets.hpp"
#include "structs.hpp"
#undef REBASE
