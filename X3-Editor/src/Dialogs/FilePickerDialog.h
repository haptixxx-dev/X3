#pragma once

#if defined(_WIN32)
    #include "Platform/Windows/Dialogs/FilePickerDialog.h"
#elif defined(__linux__)
    #include "Platform/Linux/Dialogs/FilePickerDialog.h"
#else
    #error "Unsupported platform for FilePickerDialog"
#endif
