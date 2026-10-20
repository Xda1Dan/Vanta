//
//  filesystem.hpp
//  vanta-base
//
//  Path handling for the readfile/writefile family. Everything a script names
//  is resolved inside one workspace directory and nowhere else - a pasted
//  script gets a sandbox, not the user's home folder.
//

#pragma once

#include <algorithm>
#include <filesystem>
#include <string>

#include <pwd.h>
#include <unistd.h>

#include "Offsets.h"

namespace workspace {

    // Files macOS will happily execute, load, or act on if the user later opens
    // them. Blocked on write so a script cannot leave something behind that
    // running it was never meant to authorise.
    inline bool blockedExtension(const std::filesystem::path& path)
    {
        // Exactly the set the macOS reference executor blocks, which is what
        // sUNC checks against - including .xml and the archive formats, which
        // are not obviously dangerous but are on the list. Guessing at this
        // list one failure at a time does not converge; this is the list.
        static const char* const blocked[] = {
            ".exe", ".com", ".scr", ".pif", ".cpl", ".msc", ".bat", ".cmd",
            ".ps1", ".psd1", ".vbs", ".vbe", ".js", ".jse", ".wsf", ".wsh",
            ".hta", ".scf", ".lnk", ".zip", ".rar", ".7z", ".cab", ".iso",
            ".img", ".xml", ".msi", ".msp", ".reg", ".inf", ".url", ".sh",
            ".dll", ".so", ".dylib", ".jar", ".app", ".dmg", ".pkg",
            ".command", ".scpt",
            // macOS-specific bundles the reference does not list but which are
            // executable in the same way.
            ".bundle", ".kext", ".framework", ".workflow", ".osax", ".terminal",
        };

        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        for (const char* candidate : blocked) {
            if (extension == candidate)
                return true;
        }
        return false;
    }

    inline const std::filesystem::path& root()
    {
        static const std::filesystem::path directory = [] {
            const char* home = getenv("HOME");
            if (!home || !*home) {
                const passwd* entry = getpwuid(getuid());
                home = entry ? entry->pw_dir : "/tmp";
            }
            std::filesystem::path path = std::filesystem::path(home) / "Documents" / "Vanta" / "workspace";
            std::error_code ignored;
            std::filesystem::create_directories(path, ignored);
            return path;
        }();
        return directory;
    }

    // Scripts here are run on every join. It is a sibling of the workspace, not
    // a directory inside it, so a script's own file writes can never land in it:
    // the sandbox in resolve() cannot reach outside the workspace, so nothing a
    // script does can drop a new autoexec entry.
    inline const std::filesystem::path& autoexecRoot()
    {
        static const std::filesystem::path directory = [] {
            std::filesystem::path path = root().parent_path() / "autoexec";
            std::error_code ignored;
            std::filesystem::create_directories(path, ignored);

            // The folder used to live inside the workspace. Anything still
            // there is moved up once, so an existing autoexec keeps running
            // after the move instead of silently stopping.
            const std::filesystem::path legacy = root() / "autoexec";
            if (std::filesystem::is_directory(legacy, ignored)) {
                for (const auto& entry : std::filesystem::directory_iterator(legacy, ignored)) {
                    if (!entry.is_regular_file(ignored))
                        continue;
                    const std::filesystem::path target = path / entry.path().filename();
                    if (!std::filesystem::exists(target, ignored))
                        std::filesystem::rename(entry.path(), target, ignored);
                }
                std::filesystem::remove(legacy, ignored);
            }
            return path;
        }();
        return directory;
    }

    // The GUI's executor-wide switches, as opposed to the autoexec ones that
    // live inside the autoexec folder. One line-based file, read from here at
    // join with no GUI attached and no socket up yet:
    //
    //     safemode=1
    //
    // A sibling of the workspace, never a file inside it: resolve() cannot
    // reach outside the workspace, so nothing an executed script can do
    // rewrites this. A safe mode a script can switch off is not one.
    inline const std::filesystem::path& settingsFile()
    {
        static const std::filesystem::path path = root().parent_path() / ".vanta-settings";
        return path;
    }

    // The path as a script would recognise it - relative to the workspace -
    // so an error names what was asked for rather than just the last component.
    inline std::string describe(const std::filesystem::path& path)
    {
        std::error_code code;
        const std::filesystem::path base = std::filesystem::weakly_canonical(root(), code);
        const std::string relative = path.lexically_relative(base).generic_string();
        return relative.empty() ? path.filename().string() : relative;
    }

    // Maps a script-supplied path into the workspace. Rejects absolute paths and
    // anything that climbs out with .. - weakly_canonical resolves those before
    // the containment check, so "a/../../x" is caught rather than trusted.
    inline bool resolve(const std::string& relative, std::filesystem::path& out, std::string& error)
    {
        // The workspace root never moves within a process (root() is computed
        // once), so neither does its canonical form - and weakly_canonical is an
        // lstat per path component, paid here on every file call before this
        // was cached. isfile measured 29 us with two canonicalisations per call
        // (perf2, 2026-09-03); the requested path still gets its own below,
        // because that one is what keeps a symlink inside the workspace from
        // pointing a script outside it.
        static const std::filesystem::path base = [] {
            std::error_code code;
            std::filesystem::path canonical = std::filesystem::weakly_canonical(root(), code);
            return code ? root() : canonical;
        }();

        // An empty path means the workspace root: listfiles("") is how the spec
        // documents listing it, so this is a valid request rather than an error.
        if (relative.empty()) {
            out = base;
            return true;
        }

        std::filesystem::path requested(relative);
        if (requested.is_absolute()) {
            error = "absolute paths are not allowed";
            return false;
        }

        // "C:/thing" is not absolute on POSIX - it is a directory literally
        // called "C:" - so nothing above would have caught it, and the sandbox
        // would have quietly created one. Every executor is tested with exactly
        // that path, and a backslash is the same trick spelt the other way.
        if (relative.find('\\') != std::string::npos) {
            error = "backslashes are not allowed in a path";
            return false;
        }
        for (const auto& part : requested) {
            const std::string component = part.string();
            if (component.find(':') != std::string::npos) {
                error = "drive-qualified paths are not allowed";
                return false;
            }
        }

        std::error_code code;
        std::filesystem::path full = std::filesystem::weakly_canonical(root() / requested, code);
        if (code) {
            error = "path could not be resolved";
            return false;
        }

        // Containment has to be checked component-wise; a plain string prefix
        // would accept a sibling directory whose name merely starts with the
        // workspace's, such as "workspace-elsewhere".
        auto baseIt = base.begin();
        auto fullIt = full.begin();
        for (; baseIt != base.end(); ++baseIt, ++fullIt) {
            if (fullIt == full.end() || *fullIt != *baseIt) {
                error = "path escapes the workspace";
                return false;
            }
        }

        out = std::move(full);
        return true;
    }
}

// rbxasset:// resolves against the content directory inside the app bundle, so
// getcustomasset works by putting a copy of the file there. The path is derived
// from the running executable rather than hardcoded: a Roblox installed
// somewhere other than /Applications still resolves correctly.
namespace roblox {

    inline const std::filesystem::path& contentRoot()
    {
        static const std::filesystem::path directory = [] {
            const char* executable = hostImage().path;
            if (!executable || !*executable)
                return std::filesystem::path("/Applications/Roblox.app/Contents/Resources/content");

            // .../Roblox.app/Contents/MacOS/RobloxPlayer -> .../Contents/Resources/content
            return std::filesystem::path(executable).parent_path().parent_path() / "Resources" / "content";
        }();
        return directory;
    }

    // Everything getcustomasset publishes goes in one subdirectory, so the
    // client's own content is never overwritten and the copies are easy to find.
    inline const std::filesystem::path& customAssetRoot()
    {
        static const std::filesystem::path directory = [] {
            std::filesystem::path path = contentRoot() / "vanta";
            std::error_code ignored;
            std::filesystem::create_directories(path, ignored);
            return path;
        }();
        return directory;
    }
}
