/*
 * sc4-region-sync
 *
 * SyncSettings.cpp
 */

#include "SyncSettings.h"
#include "FileSystem.h"
#include "IniReader.h"
#include <fstream>
#include <algorithm>
#include <cctype>

namespace
{
	bool ParseBool(const std::string& value, bool fallback)
	{
		if (value.empty())
		{
			return fallback;
		}
		std::string v = value;
		std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
		if (v == "0" || v == "false" || v == "no" || v == "off") return false;
		return fallback;
	}

	int ParseInt(const std::string& value, int fallback)
	{
		if (value.empty())
		{
			return fallback;
		}
		try
		{
			return std::stoi(value);
		}
		catch (...)
		{
			return fallback;
		}
	}

	LogLevel ParseLogLevel(const std::string& value, LogLevel fallback)
	{
		std::string v = value;
		std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (v == "info") return LogLevel::Info;
		if (v == "error") return LogLevel::Error;
		if (v == "debug") return LogLevel::Debug;
		if (v == "trace") return LogLevel::Trace;
		return fallback;
	}
}

namespace regionsync
{
	void SyncSettings::Load(const std::filesystem::path& iniPath)
	{
		std::ifstream stream(iniPath, std::ifstream::in);
		if (!stream)
		{
			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Info,
				"No settings file at %s, using defaults.",
				iniPath.string().c_str());
			return;
		}

		try
		{
			IniReader reader(stream);

			auto sync = reader.get_section_optional("Sync");
			if (sync)
			{
				enabled = ParseBool(sync->get_value("Enabled", ""), enabled);
				publishEveryMonths = ParseInt(sync->get_value("PublishEveryMonths", ""), publishEveryMonths);
				pullEveryMonths = ParseInt(sync->get_value("PullEveryMonths", ""), pullEveryMonths);
				autoSaveEveryMonths = ParseInt(sync->get_value("AutoSaveEveryMonths", ""), autoSaveEveryMonths);
				publishOnLoad = ParseBool(sync->get_value("PublishOnLoad", ""), publishOnLoad);
				pullOnLoad = ParseBool(sync->get_value("PullOnLoad", ""), pullOnLoad);
				publishOnShutdown = ParseBool(sync->get_value("PublishOnShutdown", ""), publishOnShutdown);
				refreshCacheBeforePublish = ParseBool(sync->get_value("RefreshCacheBeforePublish", ""), refreshCacheBeforePublish);
				storeDirectory = sync->get_value("StoreDirectory", "");
				releaseInstanceMutexOnStart = ParseBool(sync->get_value("ReleaseInstanceMutexOnStart", ""), releaseInstanceMutexOnStart);
				applyPulledSnapshots = ParseBool(sync->get_value("ApplyPulledSnapshots", ""), applyPulledSnapshots);

				const std::string fields = sync->get_value("ApplyFields", "");
				if (!fields.empty())
				{
					std::string f = fields;
					std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					applyEstablished = f.find("established") != std::string::npos;
					applyBudget = f.find("budget") != std::string::npos;
					applyIncome = f.find("income") != std::string::npos;
					applyCommodities = f.find("commodities") != std::string::npos;
					applyPopulation = f.find("population") != std::string::npos;
				}
				markDealsDirty = ParseBool(sync->get_value("MarkDealsDirty", ""), markDealsDirty);
				applyRegionalDemand = ParseBool(sync->get_value("ApplyRegionalDemand", ""), applyRegionalDemand);
				freezeExtrapolation = ParseBool(sync->get_value("FreezeExtrapolation", ""), freezeExtrapolation);
				{
					std::string set = sync->get_value("RegionalSumSet", "");
					std::transform(set.begin(), set.end(), set.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (set == "connected") regionalSumConnected = true;
					else if (set == "all") regionalSumConnected = false;
				}
				syncConnections = ParseBool(sync->get_value("SyncConnections", ""), syncConnections);
				syncDeals = ParseBool(sync->get_value("SyncDeals", ""), syncDeals);
				layEdgeNetwork = ParseBool(sync->get_value("LayEdgeNetwork", ""), layEdgeNetwork);
				syncRemovals = ParseBool(sync->get_value("SyncRemovals", ""), syncRemovals);
				checkWriterLiveness = ParseBool(sync->get_value("CheckWriterLiveness", ""), checkWriterLiveness);
				saveLock = ParseBool(sync->get_value("SaveLock", ""), saveLock);
				saveLockTimeoutMs = (std::max)(0, ParseInt(sync->get_value("SaveLockTimeoutMs", ""), saveLockTimeoutMs));
				pullOnStoreChange = ParseBool(sync->get_value("PullOnStoreChange", ""), pullOnStoreChange);
				reloadOnPeerSave = ParseBool(sync->get_value("ReloadOnPeerSave", ""), reloadOnPeerSave);
			}

			auto logging = reader.get_section_optional("Logging");
			if (logging)
			{
				logLevel = ParseLogLevel(logging->get_value("LogLevel", ""), logLevel);
			}
		}
		catch (const std::exception& e)
		{
			Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "Failed to read settings: %s", e.what());
		}
	}

	std::filesystem::path SyncSettings::ResolveStoreDirectory(const std::string& regionDirectoryName) const
	{
		std::filesystem::path base;

		if (!storeDirectory.empty())
		{
			base = storeDirectory;
		}
		else
		{
			base = FileSystem::GetDllModulePath().parent_path() / "RegionSync";
		}

		if (!regionDirectoryName.empty())
		{
			base /= regionDirectoryName;
		}

		return base;
	}
}
