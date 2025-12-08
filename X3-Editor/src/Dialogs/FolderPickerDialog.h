#pragma once

#if defined(_WIN32)
    #include "Platform/Windows/Dialogs/FolderPickerDialog.h"
#elif defined(__linux__)
    #include "Platform/Linux/Dialogs/FolderPickerDialog.h"
#else
    #error "Unsupported platform for FolderPickerDialog"
#endif
