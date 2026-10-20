#!/usr/bin/env python3
"""
Generates gui/Resources/monaco/globals.js from src/UNC.hpp.

The editor's autocomplete is only useful if it lists what this executor
actually exposes, and that list moves every time a function is added. Reading
it out of the source at build time means the two can never disagree - a
function that stops being registered stops being suggested, without anyone
remembering to edit a list.

Two registration idioms are recognised:
    RegisterFunction(L, "name", ...)   -- bound from C++, in src/UNC.hpp
    getgenv().name = ...               -- bound from the Lua bootstrap, src/lua/*.lua
Names beginning with __nexa_ are internal plumbing and are skipped.

The bootstrap used to be scraped out of UNC.hpp alone, from before it moved
into its own files - so everything namecall.lua defines (cloneref,
getconnections, gethui, firesignal, request, ...) had silently dropped out of
the editor's list. Every file under src/lua is read now.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "src" / "UNC.hpp"
LUA_DIR = ROOT / "src" / "lua"
TARGET = ROOT / "gui" / "Resources" / "monaco" / "globals.js"

# Members of the table-valued globals.
#
# This used to be the whole answer and it was wrong in both directions: it
# claimed a `Drawing.clear` that has never existed, and it was missing
# `crypt.base64_decode`, `crypt.lz4compress`, `crypt.lz4decompress` and
# `Signal.Connection`, all of which the source plainly defines. The editor
# then squiggled working code. So the source is scraped now (see
# scrapeLibraries below) and this is only the floor - a name here that the
# scrape does not confirm is reported, not shipped.
LIBRARIES = {
    "crypt":     ["encrypt", "decrypt", "hash", "hmac", "random", "generatebytes",
                  "generatekey", "base64encode", "base64decode", "base64"],
    "cache":     ["invalidate", "iscached", "replace"],
    "base64":    ["encode", "decode"],
    "http":      ["request"],
    "Drawing":   ["new", "Fonts"],
    "WebSocket": ["connect"],
    "Signal":    ["new"],
    "debug":     ["getinfo", "getconstant", "getconstants", "getproto", "getprotos",
                  "getstack", "setstack", "getupvalue", "getupvalues", "setupvalue",
                  "setconstant", "traceback", "profilebegin", "profileend"],
}

# Roblox's own surface. Not derivable from this repo - the engine provides it.
ROBLOX = [
    "game", "workspace", "script", "Enum", "Instance", "Vector2", "Vector3",
    "CFrame", "UDim", "UDim2", "Color3", "ColorSequence", "ColorSequenceKeypoint",
    "NumberSequence", "NumberSequenceKeypoint", "NumberRange", "BrickColor",
    "Ray", "Region3", "Rect", "TweenInfo", "Random", "Faces", "Axes",
    "PhysicalProperties", "DateTime", "Font", "OverlapParams", "RaycastParams",
    "shared", "_G", "require", "typeof", "warn", "print", "tick", "time",
    "elapsedTime", "wait", "spawn", "delay", "settings", "stats", "UserSettings",
    "DebuggerManager", "PluginManager", "version", "gcinfo",
]

LUAU = [
    "assert", "collectgarbage", "error", "getmetatable", "setmetatable",
    "ipairs", "pairs", "next", "pcall", "xpcall", "select", "rawequal",
    "rawget", "rawlen", "rawset", "tonumber", "tostring", "type", "unpack",
    "newproxy", "coroutine", "string", "table", "math", "os", "bit32", "utf8",
    "buffer", "vector",
]

SERVICES = [
    "Players", "ReplicatedStorage", "ReplicatedFirst", "Workspace", "Lighting",
    "RunService", "UserInputService", "TweenService", "HttpService", "StarterGui",
    "CoreGui", "TeleportService", "ServerStorage", "ServerScriptService",
    "SoundService", "Debris", "MarketplaceService", "ContextActionService",
    "PathfindingService", "CollectionService", "TextService", "TextChatService",
    "GuiService", "VirtualUser", "VirtualInputManager", "StarterPlayer",
    "StarterPack", "Teams", "Chat", "LocalizationService", "PhysicsService",
    "ProximityPromptService", "Stats", "InsertService", "AssetService",
    "BadgeService", "DataStoreService", "GamePassService", "LogService",
    "MemStorageService", "NetworkClient", "PolicyService", "RobloxReplicatedStorage",
    "ScriptContext", "Selection", "SocialService", "SoundGroup", "TestService",
    "VRService", "VoiceChatService",
]

INTERNAL = re.compile(r"^__nexa_")

# `getgenv().name = function(a, b)` and `... = newcclosure(function(a, b)`.
# The bootstrap is the only place a parameter has a name at all - the C++
# registrations are plain lua_CFunctions - so this is where a signature can be
# read rather than written out by hand and left to rot.
DEFINITION = re.compile(
    r"getgenv\(\)\.([A-Za-z_0-9]+)\s*=\s*(?:newcclosure\s*\(\s*)?function\s*\(([^)]*)\)")
# `getgenv().alias = getgenv().real`
ALIAS = re.compile(r"getgenv\(\)\.([A-Za-z_0-9]+)\s*=\s*getgenv\(\)\.([A-Za-z_0-9]+)")


def balanced(text, start):
    """The span of a `{...}` beginning at `start`, braces matched."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
    return ""


# `RegisterLibrary(L, "crypt", { {"base64decode", ...}, ... })` - the C++ half.
REGISTER_LIBRARY = re.compile(r'RegisterLibrary\(\s*L\s*,\s*"([A-Za-z_0-9]+)"\s*,\s*\{')
# `getgenv().cache = {`, `getgenv().Signal = {` - a library built as a literal.
GENV_TABLE = re.compile(r'getgenv\(\)\.([A-Za-z_0-9]+)\s*=\s*\{')
# The keys at the top level of a table literal.
#
# A regex cannot do this. `getgenv().WebSocket = { connect = function(url) ...`
# has `local closed = false` inside the function body, and any pattern loose
# enough to find `connect` finds `closed` too - so the editor offered
# WebSocket.closed, WebSocket.err and WebSocket.socket, none of which exist.
# This walks the body instead, skipping strings, comments and block bodies, and
# takes a name only where a key can actually appear: at depth zero, straight
# after the opening brace or a comma.

BLOCK_OPEN = {"function", "do", "if"}      # `for`/`while` are closed by their `do`


def topLevelKeys(body):
    """The `key =` names at depth zero of a table literal's body."""
    out = []
    i, n = 0, len(body)
    depth = 0
    expecting = True                        # a key may start here
    while i < n:
        ch = body[i]
        if ch in " \t\r\n":
            i += 1
            continue
        if body.startswith("--", i):        # a comment, long or short
            if body.startswith("--[[", i):
                j = body.find("]]", i)
                i = n if j < 0 else j + 2
            else:
                j = body.find("\n", i)
                i = n if j < 0 else j + 1
            continue
        if ch in "\"'":                      # a string
            quote, i = ch, i + 1
            while i < n and body[i] != quote:
                i += 2 if body[i] == "\\" else 1
            i += 1
            expecting = False
            continue
        if body.startswith("[[", i):
            j = body.find("]]", i)
            i = n if j < 0 else j + 2
            expecting = False
            continue
        if ch in "{([":
            depth += 1
            i += 1
            expecting = False
            continue
        if ch in "})]":
            depth -= 1
            i += 1
            expecting = False
            continue
        if ch == "," and depth == 0:
            expecting = True
            i += 1
            continue
        word = re.match(r"[A-Za-z_][A-Za-z_0-9]*", body[i:])
        if word:
            name = word.group(0)
            after = i + len(name)
            if name in BLOCK_OPEN:
                depth += 1
            elif name == "end":
                depth -= 1
            elif expecting and depth == 0:
                rest = body[after:]
                if re.match(r"\s*=(?!=)", rest):
                    out.append(name)
            expecting = False
            i = after
            continue
        i += 1
        expecting = False
    return out


# `crypt.generatekey = function`, `Drawing.Fonts = {`, `function Drawing.new(`
MEMBER_ASSIGN = re.compile(r'(?:^|\n)\s*(?:local\s+)?([A-Za-z_][A-Za-z_0-9]*)\.([A-Za-z_0-9]+)\s*=')
MEMBER_FUNC = re.compile(r'\bfunction\s+([A-Za-z_][A-Za-z_0-9]*)\.([A-Za-z_0-9]+)\s*\(')


def scrapeLibraries(text, floor):
    """{library: [members]} read out of the source rather than remembered.

    Four idioms carry them: RegisterLibrary from C++, a Lua table literal
    assigned to getgenv(), a member assigned after the fact, and a
    `function Lib.member()` definition.
    """
    found = {name: set() for name in floor}

    for match in REGISTER_LIBRARY.finditer(text):
        body = balanced(text, match.end() - 1)
        members = re.findall(r'\{\s*"([A-Za-z_0-9]+)"', body)
        found.setdefault(match.group(1), set()).update(members)

    # A library written out as a literal is exactly what the literal says.
    # `getgenv().Signal = { new = Signal.new, Connection = Connection }` exposes
    # two names; the local `Signal` class table beside it has a dozen, and they
    # are not reachable through the global.
    literal = set()
    for match in GENV_TABLE.finditer(text):
        name = match.group(1)
        if name not in found:
            continue                       # a plain table, not a library
        body = balanced(text, match.end() - 1)
        found[name].update(topLevelKeys(body))
        literal.add(name)

    # A library is often built up under a local name and only then bound:
    # `local stock = {} ... stock.getupvalues = ... getgenv().debug = stock`.
    # Follow that binding so the members under the local name count.
    under = {}
    for alias, name in re.findall(
            r'getgenv\(\)\.([A-Za-z_0-9]+)\s*=\s*([A-Za-z_][A-Za-z_0-9]*)\s*(?:$|\n)', text):
        if alias in found:
            under.setdefault(name, alias)

    for pattern in (MEMBER_ASSIGN, MEMBER_FUNC):
        for owner, member in pattern.findall(text):
            target = owner if owner in found else under.get(owner)
            if target and target not in literal:
                found[target].add(member)

    out = {}
    for name in sorted(found):
        members = {m for m in found[name] if not INTERNAL.match(m)}
        missing = sorted(set(floor.get(name, [])) - members)
        if missing:
            print(f"gen-globals: warning: {name} is listed with {missing} "
                  f"but the source does not define them", file=sys.stderr)
        out[name] = sorted(members | set(floor.get(name, [])))
    return out


# The console family is bound by a loop over a table of spellings rather than
# one assignment per name, so the alias regex above cannot see it:
#
#     for rname, cname in pairs({ rconsoleprint = "consoleprint", ... }) do
#         getgenv()[cname] = getgenv()[rname]
#     end
LOOP_HEAD = re.compile(r'for\s+([A-Za-z_0-9]+)\s*,\s*([A-Za-z_0-9]+)\s+in\s+pairs\(\s*\{')


def loopAliases(text):
    """{alias: real} for the table-driven alias loops."""
    out = {}
    for match in LOOP_HEAD.finditer(text):
        key, value = match.group(1), match.group(2)
        body = balanced(text, match.end() - 1)
        after = text[match.end() + len(body):match.end() + len(body) + 400]
        binds = re.search(r'getgenv\(\)\[\s*%s\s*\]\s*=\s*getgenv\(\)\[\s*%s\s*\]'
                          % (re.escape(value), re.escape(key)), after)
        if not binds:
            continue
        for real, alias in re.findall(r'([A-Za-z_0-9]+)\s*=\s*"([A-Za-z_0-9]+)"', body):
            out[alias] = real
    return out


def scrape(text):
    registered = set(re.findall(r'RegisterFunction\(\s*L\s*,\s*"([A-Za-z_0-9]+)"', text))
    bootstrap = set(re.findall(r'getgenv\(\)\.([A-Za-z_0-9]+)\s*=', text))
    names = {n for n in registered | bootstrap if not INTERNAL.match(n)}
    return sorted(names)


def signatures(text):
    """{name: ["a", "b"]} for everything the bootstrap defines as a function."""
    out = {}
    for name, params in DEFINITION.findall(text):
        if INTERNAL.match(name):
            continue
        clean = [p.strip() for p in params.split(",") if p.strip()]
        # A later definition wins, the way it does at runtime.
        out[name] = clean
    return out


def aliases(text):
    """{alias: real} - the bootstrap binds a dozen alternative spellings."""
    out = {}
    for alias, real in ALIAS.findall(text):
        if INTERNAL.match(alias) or INTERNAL.match(real) or alias == real:
            continue
        out[alias] = real
    return out


def main():
    if not SOURCE.exists():
        print(f"gen-globals: {SOURCE} not found", file=sys.stderr)
        return 1
    text = SOURCE.read_text(encoding="utf-8", errors="replace")
    for lua in sorted(LUA_DIR.glob("*.lua")) if LUA_DIR.is_dir() else []:
        text += "\n" + lua.read_text(encoding="utf-8", errors="replace")

    executor = scrape(text)
    # The console family arrives through a loop, not an assignment.
    loops = loopAliases(text)
    executor = sorted(set(executor) | set(loops))
    # The table-valued names are suggested as libraries, not as bare functions,
    # so they are not offered twice; and the bootstrap re-binds a few engine
    # names (`script`) that already belong to Roblox's own list.
    flat = [n for n in executor
            if n not in LIBRARIES and n not in ROBLOX and n not in LUAU]

    sigs = signatures(text)
    alias = aliases(text)
    alias.update(loops)
    libraries = scrapeLibraries(text, LIBRARIES)
    # An alias inherits the signature of what it points at, so `http_request`
    # is offered with `request`'s arguments rather than none.
    for name, real in alias.items():
        if name not in sigs and real in sigs:
            sigs[name] = sigs[real]

    payload = {
        "executor": flat,
        "signatures": {k: v for k, v in sorted(sigs.items()) if k in flat},
        "aliases": {k: v for k, v in sorted(alias.items()) if k in flat},
        "libraries": libraries,
        "roblox": ROBLOX,
        "luau": LUAU,
        "services": SERVICES,
    }

    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_text(
        "// Generated by tools/gen-globals.py from src/UNC.hpp and src/lua. Do not edit.\n"
        "globalThis.NEXA_GLOBALS = " + json.dumps(payload, indent=2) + ";\n",
        encoding="utf-8",
    )
    print(f"gen-globals: {len(flat)} functions "
          f"({len(payload['signatures'])} with argument names, {len(payload['aliases'])} aliases), "
          f"{len(libraries)} libraries -> {TARGET.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
