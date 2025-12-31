#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>

namespace X3
{

    using NativeWindowHandle = void*; // Ignored on macOS

    inline std::filesystem::path FilePickerDialog(
        const char* ext,
        const char* title,
        NativeWindowHandle owner = nullptr
    ) {
        // Build osascript command for file picker
        std::string cmd = "osascript -e 'POSIX path of (choose file";

        if (title && *title) {
            cmd += " with prompt \"";
            cmd += title;
            cmd += "\"";
        }

        if (ext && *ext) {
            // Extract file types - convert ".ext" or "*.ext" to just "ext"
            std::string extension = ext;
            if (extension.find("*.") == 0) {
                extension = extension.substr(2);
            } else if (extension[0] == '.') {
                extension = extension.substr(1);
            }

            // Only add type filter if not wildcard
            if (extension != "*" && extension != "*.*") {
                cmd += " of type {\"";
                cmd += extension;
                cmd += "\"}";
            }
        }

        cmd += ")' 2>/dev/null";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return {};
        }

        char buffer[4096];
        std::string result;
        while (fgets(buffer, sizeof(buffer), pipe)) {
            result += buffer;
        }
        pclose(pipe);

        if (!result.empty() && result.back() == '\n') {
            result.pop_back();
        }

        return result.empty() ? std::filesystem::path{} : std::filesystem::path(result);
    }

    inline std::filesystem::path SaveFileDialog(
        const char* ext,
        const char* title,
        const char* defaultName = nullptr,
        NativeWindowHandle owner = nullptr
    ) {
        // Build osascript command for save file picker
        std::string cmd = "osascript -e 'POSIX path of (choose file name";

        if (title && *title) {
            cmd += " with prompt \"";
            cmd += title;
            cmd += "\"";
        }

        if (defaultName && *defaultName) {
            cmd += " default name \"";
            cmd += defaultName;
            cmd += "\"";
        }

        cmd += ")' 2>/dev/null";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return {};
        }

        char buffer[4096];
        std::string result;
        while (fgets(buffer, sizeof(buffer), pipe)) {
            result += buffer;
        }
        pclose(pipe);

        if (!result.empty() && result.back() == '\n') {
            result.pop_back();
        }

        return result.empty() ? std::filesystem::path{} : std::filesystem::path(result);
    }
}
