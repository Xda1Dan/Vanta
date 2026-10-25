#!/bin/bash
# Vanta launcher script

DYLIB_PATH="/Users/user/Documents/Vanta/Vanta/build/libVanta.dylib"
# Roblox installs as Roblox.app; some staged/pinned builds use RobloxPlayer.app.
if [ -d /Applications/Roblox.app ]; then
    ROBLOX_APP="/Applications/Roblox.app"
elif [ -d /Applications/RobloxPlayer.app ]; then
    ROBLOX_APP="/Applications/RobloxPlayer.app"
fi
ROBLOX_PATH="$ROBLOX_APP/Contents/MacOS/RobloxPlayer"
PATCHER="/Users/user/Documents/Vanta/Vanta/build/vanta-machopatch"
PLACE_ID="${1:-90441122676618}"

# Check if dylib exists
if [ ! -f "$DYLIB_PATH" ]; then
    echo "Error: Vanta dylib not found at $DYLIB_PATH"
    echo "Build it first with: cd /Users/user/Documents/Vanta/Vanta && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8"
    exit 1
fi

# Check if Roblox is installed
if [ ! -f "$ROBLOX_PATH" ]; then
    echo "Error: Roblox not found at $ROBLOX_PATH"
    exit 1
fi

# Check codesign status
if codesign -v "$ROBLOX_PATH" 2>&1 | grep -q "valid on disk"; then
    echo "Warning: Roblox has a valid signature. Stripping it..."
    codesign --remove-signature "$ROBLOX_PATH"
    echo "Signature removed."
fi

# A RobloxPlayer patched by an older executor still carries an LC_LOAD_WEAK_DYLIB
# for it, so launching Vanta loads both executors into one client and freezes it
# grey before a game can start. Strip any such command; Vanta is injected below.
if [ -x "$PATCHER" ]; then
    if ! "$PATCHER" --binary "$ROBLOX_PATH" --status | grep -q "^not patched$"; then
        echo "Warning: RobloxPlayer still loads another executor. Removing it..."
        "$PATCHER" --binary "$ROBLOX_PATH" --remove || exit 1
    fi
else
    echo "Warning: $PATCHER not found; cannot check for a stale executor load command."
fi

echo "Launching Roblox with Vanta injected..."
echo "Place ID: $PLACE_ID"
echo "Dylib: $DYLIB_PATH"
echo ""

# Launch Roblox with dylib injected and open the place
DYLD_INSERT_LIBRARIES="$DYLIB_PATH" open "roblox://experiences/start?placeId=$PLACE_ID"
