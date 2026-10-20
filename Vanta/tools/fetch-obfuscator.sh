#!/bin/bash
# Downloads the O-MVLL prebuilt plugin + a matching precompiled upstream LLVM into
# tools/toolchain/. NO source builds, NO Homebrew. See OBFUSCATION.md for how to
# wire the build to it. This only fetches; enabling Layer B is a manual step
# because the clang version must match the O-MVLL release and be verified on this
# macOS before it is trusted in a release.
set -euo pipefail
cd "$(dirname "$0")/.."
DEST="tools/toolchain"; mkdir -p "$DEST"

OMVLL_TAG="1.9.1"
OMVLL_URL=$(curl -fsSL "https://api.github.com/repos/open-obfuscator/o-mvll/releases/tags/$OMVLL_TAG" \
  | python3 -c "import sys,json;print(next(a['browser_download_url'] for a in json.load(sys.stdin)['assets'] if 'macos' in a['name']))")
echo "O-MVLL: $OMVLL_URL"
curl -fL --progress-bar "$OMVLL_URL" -o "$DEST/omvll-macos.tar.gz"
tar -C "$DEST" -xzf "$DEST/omvll-macos.tar.gz"
echo "extracted O-MVLL into $DEST"
echo
echo "Next: install the matching upstream LLVM (see the O-MVLL release notes for"
echo "the exact version), e.g. clang+llvm-<ver>-x86_64-apple-darwin from"
echo "https://github.com/llvm/llvm-project/releases , extract to $DEST/llvm,"
echo "then follow OBFUSCATION.md to point the Nexa TUs at it. Verify the plugin"
echo "loads on THIS macOS ('file $DEST/**/omvll*.dylib' + a trivial compile)"
echo "before trusting it in a release; if it will not load here, build releases"
echo "on a supported macOS. Layer A ships regardless."
