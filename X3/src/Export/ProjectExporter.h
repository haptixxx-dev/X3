#pragma once

#include "lrpch.h"
#include "Export/ExportSettings.h"

namespace X3
{

	bool ExportProject(
		const std::string& projectName,
		const std::filesystem::path dstfolderpath,
		const std::filesystem::path srcfilepath,
		const ExportSettings& exportSettings);
}