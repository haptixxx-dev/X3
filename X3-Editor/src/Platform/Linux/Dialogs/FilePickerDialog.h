#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>

namespace Laura
{

    using NativeWindowHandle = void*; // Ignored on Linux

    inline std::filesystem::path FilePickerDialog(
        const char* ext,
        const char* title,
        NativeWindowHandle owner = nullptr
    ) {
        // Build Zenity command
        std::string cmd = "zenity --file-selection --title=\"";
        cmd += title ? title : "Select File";
        cmd += "\"";

        if (ext && *ext) {
            std::string pattern = ext;
            if (pattern.rfind("*.") != 0) {
                if (pattern[0] == '.') {
                    pattern = "*" + pattern;
                } else {
                    pattern = "*." + pattern;
                }
            }
            cmd += " --file-filter=\"*";
            cmd += pattern.substr(1);
            cmd += "\"";
        }

        cmd += " 2>/dev/null"; // suppress warnings

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
        // Build Zenity command
        std::string cmd = "zenity --file-selection --save --confirm-overwrite --title=\"";
        cmd += title ? title : "Save File";
        cmd += "\"";

        if (ext && *ext) {
            std::string pattern = ext;
            if (pattern.rfind("*.") != 0) {
                if (pattern[0] == '.') {
                    pattern = "*" + pattern;
                } else {
                    pattern = "*." + pattern;
                }   
            }
            cmd += " --file-filter=\"*";
            cmd += pattern.substr(1);
            cmd += "\"";
        }

        if (defaultName && *defaultName) {
            cmd += " --filename=\"";
            cmd += defaultName;
            cmd += "\"";
        }

        cmd += " 2>/dev/null"; // suppress warnings

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
