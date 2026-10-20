#!/usr/bin/env python3
"""Map lapi.cpp source order onto RobloxPlayer function entry points.

Roblox emits a translation unit in source order, and LC_FUNCTION_STARTS lists
every entry point the linker produced - so counting definitions forward from a
function whose address is already known names the ones after it. The result is
a candidate, never a confirmation: check the body with tools/dis.sh before
trusting any address this prints.

    tools/lapi-map.py lua_getfield 0x102C603CC   # anchor, then list from there
"""
import re, sys, subprocess, os

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from funcs import load, BIN

SRC = os.path.join(HERE, "..", "src", "luau", "VM", "src", "lapi.cpp")

def definitions(path):
    """Top-level function definitions in source order, statics included."""
    out, lines = [], open(path).read().split("\n")
    for i, line in enumerate(lines, 1):
        if line.startswith((" ", "\t", "#", "//", "}", "{", "*")) or not line.strip():
            continue
        m = re.match(r'^(?:static\s+|inline\s+|const\s+|LUA_API\s+)*[A-Za-z_][\w:<>,* ]*?[ *&]([A-Za-z_]\w*)\s*\(', line)
        if not m:
            continue
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "return", "sizeof"):
            continue
        # A declaration ends in ';' on the same line; a definition does not.
        if line.rstrip().endswith(";"):
            continue
        out.append((i, name))
    return out

if __name__ == "__main__":
    anchor_name = sys.argv[1]
    anchor_addr = int(sys.argv[2], 0)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 30

    defs = definitions(SRC)
    idx = next(i for i, (_, n) in enumerate(defs) if n == anchor_name)

    funcs = load(BIN)
    fidx = funcs.index(anchor_addr)

    for k in range(count):
        line, name = defs[idx + k] if idx + k < len(defs) else (0, "?")
        addr = funcs[fidx + k]
        print(f"{line:6}  {name:42} 0x{addr:X}")
