#pragma once

#include <filesystem>
#include <string>
#include <windows.h>
#include <shobjidl.h> // IFileDialog

namespace Laura
{

    inline std::filesystem::path FolderPickerDialog(const std::string& title = "Select Folder", HWND owner = nullptr) {
		IFileDialog* pDialog = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
									  IID_IFileDialog, reinterpret_cast<void**>(&pDialog));
		if (FAILED(hr) || !pDialog) {
			return {};
		}
		// Tell the dialog to pick folders
		DWORD options;
		pDialog->GetOptions(&options);
		pDialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        std::wstring wtitle(title.begin(), title.end());
        pDialog->SetTitle(wtitle.c_str());

		// Show the dialog
		hr = pDialog->Show(owner);
		if (FAILED(hr)) {
			pDialog->Release();
            return {};
		}
		// Get the result
		IShellItem* pItem = nullptr;
		hr = pDialog->GetResult(&pItem);
		if (FAILED(hr) || !pItem) {
			pDialog->Release();
            return {};
		}
		PWSTR pszPath = nullptr;
		hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
        std::filesystem::path result{};

		if (SUCCEEDED(hr)) {
			result = std::filesystem::path(pszPath);
			CoTaskMemFree(pszPath);
		}
		pItem->Release();
		pDialog->Release();
		return result;
	}
}