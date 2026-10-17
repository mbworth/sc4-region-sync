/*
 * sc4-region-sync
 *
 * SyncSettings.h - INI-backed settings. Lives next to the DLL as
 * SC4RegionSync.ini; every value has a safe default so the plugin runs
 * (in logging-only mode) with no INI at all.
 */

#pragma once
#include "Logger.h"
#include <filesystem>
#include <string>

namespace regionsync
{
	struct SyncSettings
	{
		// Master switch. When false the plugin only registers cheats and logs.
		bool enabled = true;

		// Automatic cadence, in sim months. 0 disables that action.
		int publishEveryMonths = 1;
		int pullEveryMonths = 1;
		int autoSaveEveryMonths = 0;

		// Publish/pull at city load and publish at shutdown.
		bool publishOnLoad = true;
		bool pullOnLoad = true;
		bool publishOnShutdown = true;

		// Call cISC4RegionalCity::UpdateCityCache(cISC4City*) before capturing.
		// Needed for the snapshot to reflect the live city rather than the
		// values from the last save. See docs/OPEN-QUESTIONS.md, Q1.
		bool refreshCacheBeforePublish = true;

		// Close the game's single-instance mutex ("SimCity 4 Mutex") at
		// startup so a second instance can launch. Off until Q5 confirms it
		// is safe; the RegionSyncUnlock cheat does the same on demand.
		bool releaseInstanceMutexOnStart = false;

		// Write pulled snapshots into the other tiles' regional records.
		// 0 = read the store and log, touch nothing (crash isolation).
		bool applyPulledSnapshots = true;

		// Where snapshots live. Empty means "<DLL folder>\RegionSync\<region dir>".
		// Both game instances share the install, so the default is already a
		// shared location. Set it explicitly if the instances use separate
		// installs.
		std::string storeDirectory;

		LogLevel logLevel = LogLevel::Info;

		void Load(const std::filesystem::path& iniPath);
		std::filesystem::path ResolveStoreDirectory(const std::string& regionDirectoryName) const;
	};
}
