#!/bin/bash
# Disassemble a range of RobloxPlayer by unslid virtual address.
#   tools/dis.sh 0x102C5F6D2 [bytes]
# __TEXT is mapped at 0x100000000 with fileoff 0, so vmaddr - 0x100000000 is
# the file offset. Apple's objdump will not take a raw binary, so the bytes are
# re-assembled into a throwaway object and disassembled from there; addresses
# in the output are relative to the start of the range.
set -e
BIN="${ROBLOX_BIN:-/Applications/Roblox.app/Contents/MacOS/RobloxPlayer}"
ADDR=$((${1:?usage: dis.sh <0xaddr> [bytes]}))
LEN=${2:-160}
D=$(mktemp -d /tmp/dis.XXXXXX)
dd if="$BIN" of="$D/raw" bs=1 skip=$((ADDR - 0x100000000)) count="$LEN" 2>/dev/null
python3 - "$D" <<'PY'
import sys
d = sys.argv[1]
data = open(d + "/raw", "rb").read()
with open(d + "/a.s", "w") as f:
    f.write(".text\n.globl _start\n_start:\n")
    for i in range(0, len(data), 16):
        f.write(".byte " + ",".join("0x%02x" % b for b in data[i:i+16]) + "\n")
PY
clang -c -o "$D/a.o" "$D/a.s"
otool -tVj "$D/a.o" | tail -n +3 | python3 -c "
import sys
base = int(sys.argv[1], 0)
for line in sys.stdin:
    parts = line.split('\t')
    if parts and parts[0].endswith(':') and parts[0][:-1].strip():
        try:
            off = int(parts[0][:-1], 16)
        except ValueError:
            sys.stdout.write(line); continue
        parts[0] = '%x:' % (base + off)
    sys.stdout.write('\t'.join(parts))
" "$ADDR"
rm -rf "$D"
