//
//  dialogs.hpp
//  vanta-base
//
//  openfiledialog / openfilesdialog / openfolderdialog / savefiledialog.
//  The panels themselves are AppKit's, reached through osascript rather than by
//  linking AppKit and hopping to the main thread - NSOpenPanel has to run on
//  the main run loop, and the Lua thread asking for it is not it.
//
//  Anything a dialog hands back is outside the workspace by definition, so the
//  file objects built on top of these read and write through their own raw
//  helpers. Only paths the user actually picked - and what lives under a picked
//  folder - are reachable that way; the sandbox still holds for every path a
//  script names itself.
//

#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace dialogs {

    inline std::mutex mutex;
    inline std::unordered_set<std::string> granted;

    inline void grant(const std::string& path)
    {
        std::lock_guard<std::mutex> lock(mutex);
        granted.insert(path);
    }

    // A folder the user picked carries its subtree with it, which is what makes
    // folder:List() and folder:MakeFolder() usable at all.
    inline bool allowed(const std::string& path)
    {
        std::error_code code;
        const std::filesystem::path candidate = std::filesystem::weakly_canonical(path, code);
        if (code)
            return false;

        std::lock_guard<std::mutex> lock(mutex);
        for (const std::string& base : granted) {
            const std::filesystem::path root = std::filesystem::weakly_canonical(base, code);
            if (code)
                continue;
            auto rootIt = root.begin();
            auto candidateIt = candidate.begin();
            bool contained = true;
            for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
                if (candidateIt == candidate.end() || *candidateIt != *rootIt) {
                    contained = false;
                    break;
                }
            }
            if (contained)
                return true;
        }
        return false;
    }

    inline std::string escape(const std::string& text)
    {
        std::string out;
        for (char c : text) {
            if (c == '\\' || c == '"')
                out += '\\';
            out += c;
        }
        return out;
    }

    inline std::vector<std::string> run(const std::string& script)
    {
        std::string command = "/usr/bin/osascript 2>/dev/null <<'VANTA_OSA_EOF'\n" + script + "\nVANTA_OSA_EOF\n";
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe)
            return {};

        std::string output;
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe))
            output += buffer;
        pclose(pipe);

        std::vector<std::string> paths;
        size_t start = 0;
        while (start < output.size()) {
            size_t end = output.find('\n', start);
            if (end == std::string::npos)
                end = output.size();
            std::string line = output.substr(start, end - start);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (!line.empty()) {
                grant(line);
                paths.push_back(line);
            }
            start = end + 1;
        }
        return paths;
    }

    inline std::string ofType(const std::vector<std::string>& extensions)
    {
        if (extensions.empty())
            return "";
        std::string list = " of type {";
        for (size_t i = 0; i < extensions.size(); i++) {
            if (i)
                list += ", ";
            list += "\"" + escape(extensions[i]) + "\"";
        }
        return list + "}";
    }

    inline std::string location(const std::string& defaultPath)
    {
        if (defaultPath.empty())
            return "";
        return " default location POSIX file \"" + escape(defaultPath) + "\"";
    }

    inline std::vector<std::string> openFile(const std::string& title, const std::string& defaultPath,
                                             const std::vector<std::string>& extensions, bool multiple)
    {
        std::string script =
            "set picked to choose file with prompt \"" + escape(title.empty() ? "Select a file" : title) + "\"" +
            ofType(extensions) + location(defaultPath) +
            (multiple ? " with multiple selections allowed" : "") + "\n"
            "if class of picked is not list then set picked to {picked}\n"
            "set out to \"\"\n"
            "repeat with item_ in picked\n"
            "  set out to out & POSIX path of item_ & linefeed\n"
            "end repeat\n"
            "return out";
        return run(script);
    }

    inline std::vector<std::string> openFolder(const std::string& title, const std::string& defaultPath)
    {
        std::string script =
            "set picked to choose folder with prompt \"" + escape(title.empty() ? "Select a folder" : title) + "\"" +
            location(defaultPath) + "\n"
            "return POSIX path of picked";
        return run(script);
    }

    inline std::vector<std::string> saveFile(const std::string& title, const std::string& defaultPath,
                                             const std::string& suggestedName)
    {
        std::string script =
            "set picked to choose file name with prompt \"" + escape(title.empty() ? "Save file" : title) + "\"" +
            (suggestedName.empty() ? "" : " default name \"" + escape(suggestedName) + "\"") +
            location(defaultPath) + "\n"
            "return POSIX path of picked";
        std::vector<std::string> picked = run(script);
        // The Lua side may append a default extension to what came back, so the
        // directory the user chose is granted too - otherwise the very file the
        // dialog was opened to create would be unwritable.
        for (const std::string& path : picked)
            grant(std::filesystem::path(path).parent_path().string());
        return picked;
    }
}
