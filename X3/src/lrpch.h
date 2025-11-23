#pragma once

#include <memory>
#include <utility>
#include <functional>
#include <cstdint>

#include <iostream>
#include <sstream>
#include <ostream>
#include <fstream>

#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <span>

// GLM headers
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>

// Additional STL headers commonly used
#include <algorithm>
#include <filesystem>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>

// Engine core headers that are frequently used
#include "Core/Log.h"

#ifdef LR_PLATFORM_WINDOWS
	#define NOMINMAX // Prevent windows.h from defining min and max macros that conflict with std::min and std::max
	#include <Windows.h>
#endif

