//
//  version.hpp
//  vanta-base
//
//  The single source of truth for the executor's version.
//
//  publish.sh bumps the line below and nothing else. Everything that needs to
//  name a version reads it from here:
//    - identifyexecutor() and the `request` User-Agent, from this macro;
//    - tools/make-app.sh, which greps it into the app bundle's
//      CFBundleShortVersionString so the GUI can read its own installed version
//      (Bundle.main) without launching Roblox;
//    - publish.sh, which greps it to name the dist archive and fill
//      update.json's executor.version.
//
//  Format is a plain semver triple in one double-quoted literal on the #define
//  line, so a one-line sed/grep can pull it out from shell.
//
#pragma once

#define VANTA_VERSION_STR "1.0.0"
