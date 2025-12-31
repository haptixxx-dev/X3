#pragma once

#include <filesystem>
#include <string>

namespace X3
{

    using NativeWindowHandle = void*; // Ignored on macOS

    inline std::filesystem::path FolderPickerDialog(const std::string& title = "Select Folder", NativeWindowHandle owner = nullptr) {
        std::string cmd = "osascript -e 'POSIX path of (choose folder";

        if (!title.empty()) {
            cmd += " with prompt \"" + title + "\"";
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
