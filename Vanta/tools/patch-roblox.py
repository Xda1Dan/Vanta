#!/usr/bin/env python3
"""
Adds an LC_LOAD_WEAK_DYLIB entry to RobloxPlayer so the executor is loaded on
every launch, however Roblox is started - from the Dock, from a deep link, or
from LaunchServices - without DYLD_INSERT_LIBRARIES being involved at all.

Weak rather than plain LOAD_DYLIB on purpose: if the dylib is missing or moved,
dyld carries on and Roblox still starts, instead of the app becoming unlaunchable
because a build directory got deleted.

The new command is written into the zero padding that already sits between the
end of the load commands and the first section, so nothing in the file moves and
every offset stays valid.
"""

import argparse
import os
import shutil
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
HEADER_SIZE = 32

LC_REQ_DYLD = 0x80000000
LC_SEGMENT_64 = 0x19
LC_LOAD_DYLIB = 0x0C
LC_LOAD_WEAK_DYLIB = 0x18 | LC_REQ_DYLD
LC_CODE_SIGNATURE = 0x1D

BACKUP_SUFFIX = ".unix-backup"


def load_commands(data):
    ncmds, sizeofcmds = struct.unpack_from("<II", data, 16)
    offset = HEADER_SIZE
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, offset)
        if cmdsize <= 0:
            raise ValueError("malformed load command at offset %d" % offset)
        yield offset, cmd, cmdsize
        offset += cmdsize


def linked_dylibs(data):
    found = []
    for offset, cmd, cmdsize in load_commands(data):
        if cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB):
            name_offset = struct.unpack_from("<I", data, offset + 8)[0]
            raw = data[offset + name_offset:offset + cmdsize]
            found.append((offset, cmd, raw.split(b"\0")[0].decode("utf-8", "replace")))
    return found


def first_section_offset(data):
    lowest = None
    for offset, cmd, cmdsize in load_commands(data):
        if cmd != LC_SEGMENT_64:
            continue
        nsects = struct.unpack_from("<I", data, offset + 64)[0]
        section = offset + 72
        for _ in range(nsects):
            file_offset = struct.unpack_from("<I", data, section + 48)[0]
            if file_offset > 0 and (lowest is None or file_offset < lowest):
                lowest = file_offset
            section += 80
    return lowest


def has_code_signature(data):
    return any(cmd == LC_CODE_SIGNATURE for _, cmd, _ in load_commands(data))


def build_command(dylib_path):
    encoded = dylib_path.encode("utf-8") + b"\0"
    cmdsize = (24 + len(encoded) + 7) // 8 * 8
    command = struct.pack("<IIIIII",
                          LC_LOAD_WEAK_DYLIB,
                          cmdsize,
                          24,          # name offset within the command
                          0,           # timestamp
                          0x10000,     # current version 1.0.0
                          0x10000)     # compatibility version 1.0.0
    command += encoded
    return command + b"\0" * (cmdsize - len(command))


def patch(binary, dylib_path, quiet=False):
    def say(message):
        if not quiet:
            print(message)

    with open(binary, "rb") as handle:
        data = bytearray(handle.read())

    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != MH_MAGIC_64:
        raise SystemExit("not a thin 64-bit Mach-O (magic %08x); a fat binary would need lipo first" % magic)

    # The pre-rebrand name is checked too: a RobloxPlayer patched by an older
    # build carries a weak load of libUnix.dylib, and leaving it in place would
    # load two executors into one process the moment both files exist.
    executor_libs = ("libNexa.dylib", "libUnix.dylib")
    existing = [entry for entry in linked_dylibs(data)
                if any(lib in entry[2] for lib in executor_libs)]
    if existing:
        if existing[0][2] == dylib_path:
            say("already patched -> %s" % dylib_path)
            return False
        say("patched with a different path (%s); restoring first" % existing[0][2])
        restore(binary, quiet=quiet)
        return patch(binary, dylib_path, quiet=quiet)

    if has_code_signature(data):
        raise SystemExit("binary still carries a code signature; run:\n"
                         "  codesign --remove-signature %s" % binary)

    command = build_command(dylib_path)
    ncmds, sizeofcmds = struct.unpack_from("<II", data, 16)
    end_of_commands = HEADER_SIZE + sizeofcmds
    available = first_section_offset(data) - end_of_commands

    if len(command) > available:
        raise SystemExit("only %d bytes of padding after the load commands, need %d"
                         % (available, len(command)))

    # Refuse to write over anything that is not padding - if this region ever
    # holds real data, silently overwriting it would corrupt the binary.
    if any(data[end_of_commands:end_of_commands + len(command)]):
        raise SystemExit("the space after the load commands is not zero padding; refusing to write")

    backup = binary + BACKUP_SUFFIX
    if not os.path.exists(backup):
        shutil.copy2(binary, backup)
        say("backed up original -> %s" % backup)

    data[end_of_commands:end_of_commands + len(command)] = command
    struct.pack_into("<II", data, 16, ncmds + 1, sizeofcmds + len(command))

    with open(binary, "wb") as handle:
        handle.write(data)
    os.chmod(binary, 0o755)

    say("inserted LC_LOAD_WEAK_DYLIB -> %s" % dylib_path)
    return True


def restore(binary, quiet=False):
    backup = binary + BACKUP_SUFFIX
    if not os.path.exists(backup):
        raise SystemExit("no backup at %s" % backup)
    shutil.copy2(backup, binary)
    os.chmod(binary, 0o755)
    if not quiet:
        print("restored original from %s" % backup)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default="/Applications/Roblox.app/Contents/MacOS/RobloxPlayer")
    parser.add_argument("--dylib", help="absolute path to libNexa.dylib")
    parser.add_argument("--restore", action="store_true", help="undo the patch")
    parser.add_argument("--status", action="store_true", help="report whether the patch is present")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.restore:
        restore(args.binary, quiet=args.quiet)
        return

    if args.status:
        with open(args.binary, "rb") as handle:
            data = handle.read()
        entries = [entry for entry in linked_dylibs(data) if "libNexa.dylib" in entry[2]]
        print("patched -> %s" % entries[0][2] if entries else "not patched")
        return

    if not args.dylib:
        raise SystemExit("--dylib is required")
    dylib = os.path.abspath(args.dylib)
    if not os.path.exists(dylib):
        raise SystemExit("dylib not found: %s" % dylib)
    patch(args.binary, dylib, quiet=args.quiet)


if __name__ == "__main__":
    main()
