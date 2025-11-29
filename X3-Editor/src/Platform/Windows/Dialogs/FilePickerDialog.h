#pragma once

#include <filesystem>
#include <string>
#include <commdlg.h> // OPENFILENAMEA
#include <format>

namespace Laura
{

    inline std::filesystem::path FilePickerDialog(const char* ext, const char* title, HWND owner = nullptr) {
        char buff[MAX_PATH] = {};

        std::string filter = std::format("{} Files", ext);
		filter.push_back('\0');
		filter += std::format(" * {}", ext);
		filter.push_back('\0');
		filter.push_back('\0');

        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = owner;
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrTitle  = title;
        ofn.nMaxFile    = static_cast<DWORD>(sizeof(buff));
        ofn.lpstrFile   = buff;
        ofn.Flags       = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileNameA(&ofn)) {
            return std::filesystem::path(buff);
        }
        return {}; // empty path if cancelled
    }

    inline std::filesystem::path SaveFileDialog(const char* ext, const char* title, const char* defaultName = nullptr, HWND owner = nullptr) {
        // Use Unicode version for proper Unicode support and longer paths
        const int MAX_UNICODE_PATH = 32768; // Much longer than MAX_PATH
        std::vector<wchar_t> buff(MAX_UNICODE_PATH, L'\0');

        // Build filter string in Unicode
        std::string filterStr;
        if (ext && strlen(ext) > 0) {
            // Format: "Description\0*.ext\0\0"
            std::string extension = ext;

            // Ensure extension starts with "*."
            if (extension.find("*.") != 0) {
                if (extension[0] == '.') {
                    extension = "*" + extension;  // ".lrproj" -> "*.lrproj"
                } else {
                    extension = "*." + extension; // "lrproj" -> "*.lrproj"
                }
            }

            filterStr = std::format("{} Files", ext);  // Use original ext for description
            filterStr.push_back('\0');     // Null terminate description
            filterStr += extension;        // Add the extension pattern (e.g., "*.lrproj")
            filterStr.push_back('\0');     // Null terminate pattern
        } else {
            // No filter - show all files
            filterStr = "All Files";
            filterStr.push_back('\0');     // Null terminate description
            filterStr += "*.*";            // Add the all files pattern
            filterStr.push_back('\0');     // Null terminate pattern
        }
        filterStr.push_back('\0');        // Double null terminator

        // Convert filter to wide string
        int filterSize = MultiByteToWideChar(CP_UTF8, 0, filterStr.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> wfilter(filterSize > 0 ? filterSize : 1, L'\0');
        if (filterSize > 0) {
            MultiByteToWideChar(CP_UTF8, 0, filterStr.c_str(), -1, wfilter.data(), filterSize);
        }

        // Convert title to wide string
        std::vector<wchar_t> wtitle;
        if (title && strlen(title) > 0) {
            int titleSize = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
            if (titleSize > 0) {
                wtitle.resize(titleSize, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle.data(), titleSize);
            }
        }

        // Convert default name to wide string and copy to buffer
        if (defaultName && strlen(defaultName) > 0) {
            int nameSize = MultiByteToWideChar(CP_UTF8, 0, defaultName, -1, nullptr, 0);
            if (nameSize > 0 && nameSize < buff.size()) {
                std::vector<wchar_t> wdefaultName(nameSize, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, defaultName, -1, wdefaultName.data(), nameSize);
                std::copy(wdefaultName.begin(), wdefaultName.end(), buff.begin());
            }
        }

        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = owner;
        ofn.lpstrFilter = wfilter.data();
        ofn.lpstrTitle  = wtitle.empty() ? nullptr : wtitle.data();
        ofn.nMaxFile    = static_cast<DWORD>(buff.size());
        ofn.lpstrFile   = buff.data();
        ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

        if (GetSaveFileNameW(&ofn)) {
            return std::filesystem::path(buff.data());
        }
        return {}; // empty path if cancelled
    }
}