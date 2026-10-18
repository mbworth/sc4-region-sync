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
		bool publishOnShutdown = false;

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

		// Which record fields a pull writes. Comma list of
		// established, budget, income, commodities, population.
		bool applyEstablished = true;
		bool applyBudget = true;
		bool applyIncome = true;
		bool applyCommodities = true;
		bool applyPopulation = true;

		// Call cISC4NeighborsSimulator::MarkDealsDirty after a pull changed
		// something.
		bool markDealsDirty = true;

		// Feed regional population deltas into the demand simulator after a
		// pull (Phase 1b). See the director's ApplyRegionalDemandDelta.
		bool applyRegionalDemand = true;

		// Regional sum for that feed: false = every established tile,
		// true = GetAllConnectedCities (what the game's fold uses).
		bool regionalSumConnected = false;

		// Mirror other instances' connections and deals into this city on
		// each pull (Phase 2 slice 1). Off until Q9 is recorded; the
		// RegionSyncMirror cheat runs the same steps on demand.
		bool syncConnections = true;
		bool syncDeals = true;

		// Lay the edge network stub under mirrored connections on the timed
		// path. Off until Q10 is observed; RegionSyncMirror always lays.
		bool layEdgeNetwork = true;

		// Remove connections and deals a live publisher stopped listing
		// (docs/PLAN-REMOVAL.md). Off until Q11 is observed.
		bool syncRemovals = true;

		// Drop another instance's snapshot when its writer process has exited
		// (the Exit button sends no shutdown). Same machine only.
		bool checkWriterLiveness = true;

		// Hold a region-wide named mutex across every save (docs/PLAN-SAVE-LOCK.md).
		// Cooperative: an instance with 0 does not take it.
		bool saveLock = true;
		// Longest a save waits for the peer; then it goes ahead unlocked. 0 = never wait.
		int saveLockTimeoutMs = 15000;

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
