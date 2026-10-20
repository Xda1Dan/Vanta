#!/bin/bash
# Build script for Vanta

set -e

cd "$(dirname "$0")/Vanta"

echo "Building Vanta..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

echo ""
echo "✅ Build complete!"
echo "Dylib: $(pwd)/build/libVanta.dylib"
echo ""
echo "Launch with: /Users/user/Documents/Vanta/launch-vanta.sh [PLACE_ID]"
