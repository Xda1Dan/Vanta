#!/bin/bash
# Builds the native Vanta UI prototype into build/ui/Vanta Prototype.app.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD="build/ui"
APP="$BUILD/Vanta Prototype.app"
BINARY="$BUILD/VantaUI"

mkdir -p "$BUILD"
echo "compiling ui sources…"
swiftc -Onone -j 8 -o "$BINARY" ui/*.swift -framework SwiftUI -framework AppKit

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BINARY" "$APP/Contents/MacOS/VantaUI"

# App icon
if [ -f ui/icon.png ] && command -v iconutil >/dev/null 2>&1; then
    ICONSET="$BUILD/AppIcon.iconset"
    rm -rf "$ICONSET" "$BUILD/AppIcon.icns"
    mkdir -p "$ICONSET"
    for sz in 16 32 128 256 512; do
        sips -z "$sz" "$sz" ui/icon.png --out "$ICONSET/icon_${sz}x${sz}.png" >/dev/null
        sips -z $((sz * 2)) $((sz * 2)) ui/icon.png \
            --out "$ICONSET/icon_${sz}x${sz}@2x.png" >/dev/null
    done
    iconutil -c icns "$ICONSET" -o "$BUILD/AppIcon.icns" >/dev/null 2>&1 || true
    rm -rf "$ICONSET"
fi

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>                 <string>Vanta</string>
    <key>CFBundleDisplayName</key>          <string>Vanta Prototype</string>
    <key>CFBundleExecutable</key>           <string>VantaUI</string>
    <key>CFBundleIdentifier</key>           <string>com.vanta.prototype</string>
    <key>CFBundlePackageType</key>          <string>APPL</string>
    <key>CFBundleShortVersionString</key>   <string>1.0.0</string>
    <key>CFBundleVersion</key>              <string>1</string>
    <key>CFBundleIconFile</key>             <string>AppIcon</string>
    <key>NSPrincipalClass</key>             <string>NSApplication</string>
    <key>NSAppTransportSecurity</key>
    <dict><key>NSAllowsLocalNetworking</key><true/></dict>
    <key>NSHighResolutionCapable</key>      <true/>
    <key>LSMinimumSystemVersion</key>       <string>13.0</string>
    <key>NSHumanReadableCopyright</key>     <string>Vanta UI prototype</string>
    <key>LSApplicationCategoryType</key>    <string>public.app-category.developer-tools</string>
</dict>
</plist>
PLIST

if [ -f "$BUILD/AppIcon.icns" ]; then
    cp "$BUILD/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
fi

# Ad-hoc signature so LaunchServices and the keychain treat it as a real app.
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || true
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
    -f "$APP" >/dev/null 2>&1 || true

echo "built: $APP"
