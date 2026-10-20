#!/bin/bash
# Print every reference executor's implementation of one function, so the
# consensus is visible at a glance. The user's standing rule is that a change
# here follows what the majority of the six trees do - see the
# check-references-first memory.
#   tools/refcmp.sh <function-name> [lines-of-context]
R="/Users/evilion/Documents/executor_reference_srcs"
FN="${1:?usage: refcmp.sh <function> [context]}"
N="${2:-28}"
for d in "$R"/*/; do
    name=$(basename "$d")
    hit=$(grep -rn --include=*.cpp --include=*.hpp --include=*.h --include=*.cc \
          -E "^[[:space:]]*(inline[[:space:]]+)?(static[[:space:]]+)?int[[:space:]]+${FN}[[:space:]]*\([[:space:]]*lua_State" \
          "$d" 2>/dev/null | head -1)
    [ -z "$hit" ] && continue
    file=${hit%%:*}; rest=${hit#*:}; line=${rest%%:*}
    echo "===== $name :: ${file#$R/}:$line ====="
    sed -n "${line},$((line + N))p" "$file"
    echo
done
