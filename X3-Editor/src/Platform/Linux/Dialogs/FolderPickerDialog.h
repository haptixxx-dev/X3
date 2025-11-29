#pragma once

#include <filesystem>
#include <string>

namespace Laura
{

    using NativeWindowHandle = void*; // Ignored on Linux

    inline std::filesystem::path FolderPickerDialog(const std::string& title = "Select Folder", NativeWindowHandle owner = nullptr) {
        std::string cmd = "zenity --file-selection --directory --title=\"" + title + "\" 2>/dev/null";
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
