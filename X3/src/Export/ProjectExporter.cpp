#include "Export/ProjectExporter.h"
#include "Project/ProjectManager.h"
#include "Project/Assets/AssetManager.h"
#include "EngineCfg.h"

namespace Laura
{

    bool ExportProject(
			const std::string& projectName,
			const std::filesystem::path dstfolderpath, // folder into which the exported project folder is created
			const std::filesystem::path projectFolderPath,
			const ExportSettings& exportSettings) {

		// Recursively copy all files and subdirectories from `src` into `dst`,
		// preserving the directory structure. Overwrites files if they already exist.
		//   src = "C:/data", dst = "D:/backup"
		//   "C:/data/img/cat.png" -> "D:/backup/img/cat.png"
		auto copyInto = [](const std::filesystem::path& dst, const std::filesystem::path& src) {
			if (!std::filesystem::exists(src))			{ throw std::runtime_error("Source directory does not exist: " + src.string()); }
			if (!std::filesystem::is_directory(src))	{ throw std::runtime_error("Source path is not a directory: " + src.string()); }
			for (const auto& entry : std::filesystem::recursive_directory_iterator(src)) {
				std::filesystem::path relPath = std::filesystem::relative(entry.path(), src);
				std::filesystem::path dest = dst / relPath;
				if (entry.is_directory()) {
					std::filesystem::create_directories(dest);
				} else {
					std::filesystem::create_directories(dest.parent_path());
					std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::overwrite_existing);
				}
			}
		};

		// ensure projectFolderPath contains .lrproj file
		std::filesystem::path srcProjectFilePath = projectFolderPath / (projectFolderPath.filename().string() + PROJECT_FILE_EXTENSION);
		if (!std::filesystem::exists(srcProjectFilePath)) {
			LOG_ENGINE_WARN("ExportProject: {0} does not contain project file {1}", projectFolderPath.string(), srcProjectFilePath.string());
			return false;
		}

		std::filesystem::path exportProjectFolderPath = dstfolderpath / projectName;
		if (std::filesystem::exists(exportProjectFolderPath)) {
			LOG_ENGINE_WARN("ExportProject: export folder already exists {0}", exportProjectFolderPath.string());
			return false;
		}

		auto absProjectPath = std::filesystem::weakly_canonical(projectFolderPath);
		auto absExportPath  = std::filesystem::weakly_canonical(exportProjectFolderPath);

		if (std::mismatch(absProjectPath.begin(), absProjectPath.end(),
						  absExportPath.begin()).first == absProjectPath.end()) {
			LOG_ENGINE_ERROR("Export folder cannot be inside the project folder!\nProject: {0}\nExport: {1}",
							 absProjectPath.string(), absExportPath.string());
			return false;
		}

        try {
			// create the export folder
            std::filesystem::create_directories(exportProjectFolderPath);

            // Copy source folder (containing .lrproj and assets)
            copyInto(exportProjectFolderPath, projectFolderPath);

			// Copy all assets and update .lrmeta files to point to the new paths
			for (const auto& entry : std::filesystem::recursive_directory_iterator(projectFolderPath)) {
				if (entry.path().extension() == ASSET_META_FILE_EXTENSION) {
					std::optional<AssetMetaFile> metaFileOpt = LoadMetaFile(entry.path());
					if (!metaFileOpt.has_value()) {
						LOG_ENGINE_WARN("ExportProject: failed to load meta file {0}, skipping", entry.path().string());
						continue;
					}

					AssetMetaFile& metaFile = metaFileOpt.value();

					// Check if the source asset file exists
					if (!std::filesystem::exists(metaFile.sourcePath)) {
						LOG_ENGINE_WARN("ExportProject: source asset file does not exist {0}, skipping meta file {1}",
							metaFile.sourcePath.string(), entry.path().string());
						continue;
					}

					// Destination path for the asset file
					std::filesystem::path assetFilename = metaFile.sourcePath.filename();
					std::filesystem::path exportAssetPath = exportProjectFolderPath / assetFilename;

					// Copy the asset file
					try {
						std::filesystem::copy_file(metaFile.sourcePath, exportAssetPath,
												   std::filesystem::copy_options::overwrite_existing);
					} catch (const std::exception& e) {
						LOG_ENGINE_ERROR("ExportProject: failed to copy asset file {0} to {1}: {2}",
							metaFile.sourcePath.string(), exportAssetPath.string(), e.what());
						continue;
					}

					// Update the meta file to point to the new asset path
					metaFile.sourcePath = exportAssetPath;

					// Save the updated meta file in the export folder
					// Destination path for the metafile
					std::filesystem::path metaFileFilename = entry.path().filename();
					std::filesystem::path exportMetaFilePath = exportProjectFolderPath / metaFileFilename;
					if (!SaveMetaFile(exportMetaFilePath, metaFile)) {
						LOG_ENGINE_ERROR("ExportProject: failed to save updated meta file {0}", exportMetaFilePath.string());
						// Continue processing other files even if one fails
					}
				}
			}

            // Rename the .lrproj
            std::filesystem::path oldProjFile = exportProjectFolderPath / srcProjectFilePath.filename();
            std::filesystem::path newProjFile = exportProjectFolderPath / (projectName + PROJECT_FILE_EXTENSION);
            std::filesystem::rename(oldProjFile, newProjFile);

            // Serialize export settings (creates ExportSettings.yaml)
            SerializeExportSettingsYaml(exportProjectFolderPath, exportSettings);

            // Copy runtime from a known location
            std::filesystem::path runtimePath = EngineCfg::EXECUTABLE_DIR / "runtime";
            if (!std::filesystem::exists(runtimePath)) {
                LOG_ENGINE_ERROR("ExportProject: runtime directory not found at {0}", runtimePath.string());
                throw std::runtime_error("Runtime directory not found");
            }
            copyInto(exportProjectFolderPath, runtimePath);

			
			// Rename the runtime executable file
			#if defined(_WIN32)
			std::filesystem::rename(
				exportProjectFolderPath / "LauraRuntime.exe", // hardcoded string (not ideal)
				exportProjectFolderPath / (projectName + ".exe")
			);
			#else
			std::filesystem::rename(
				exportProjectFolderPath / "LauraRuntime",
				exportProjectFolderPath / projectName
			);
			#endif

			#ifdef BUILD_INSTALL
			// Copy engine resources into export folder.
			// Notes:
			//  - In non-shipping builds, runtime uses the source tree resource path instead.
			//  - For shipping, resources must be placed next to the runtime executable.
			//  - Since editor and runtime share engine resource paths, both expect them
			//    in the same relative location ("engine_res" beside the executable).
			copyInto(exportProjectFolderPath / "engine_res", EngineCfg::RESOURCES_PATH); 
			#endif
			
            return true;
        }
        catch (const std::exception& e) {
			// clean up export folder on failiure
			std::filesystem::remove(exportProjectFolderPath);
			LOG_ENGINE_ERROR("ExportProject: {0}", e.what());
            return false;
        }
    }
}