/*
 * sc4-region-sync - a SimCity 4 DLL plugin that keeps the region in sync
 * across multiple running game instances.
 *
 * RegionSyncDllDirector.cpp - the plugin entry point.
 *
 * Phase 1 behavior (see docs/PHASES.md):
 *   - On city load, locate the region, this tile's regional record, and the
 *     neighbors simulator.
 *   - Every N sim-months, publish this city's summary to the shared store
 *     and pull every other tile's summary from the store into this
 *     instance's regional records.
 *   - Expose cheat-console commands so each step can be run and inspected
 *     by hand: RegionSyncDump, RegionSyncPublish, RegionSyncPull,
 *     RegionSyncReload, RegionSyncStatus.
 *
 * Built on the same skeleton as Null45's published DLL plugins so that the
 * conventions (logging, INI, version detection) match what the SC4 DLL
 * community already knows.
 */

#include "version.h"
#include "NeighborSnapshot.h"
#include "RegionalCityBridge.h"
#include "SyncSettings.h"
#include "InstanceMutex.h"
#include "RegionalCity641.h"

#include "cIGZCheatCodeManager.h"
#include "cIGZCOM.h"
#include "cIGZDate.h"
#include "cIGZFrameWork.h"
#include "cIGZMessage2Standard.h"
#include "cIGZMessageServer2.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC4Demand.h"
#include "cISC4DemandSimulator.h"
#include "SC4Percentage.h"
#include "cISC4NeighborConnection.h"
#include "cISC4NeighborDeal.h"
#include "cISC4NeighborsSimulator.h"
#include "cISC4Region.h"
#include "cISC4RegionalCity.h"
#include "cISC4Simulator.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "cRZMessage2COMDirector.h"
#include "GZServPtrs.h"
#include "SC4List.h"
#include "SC4String.h"
#include "SC4Vector.h"

#include "FileSystem.h"
#include "Logger.h"
#include "SC4NotificationDialog.h"
#include "SC4VersionDetection.h"

#include <Windows.h>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Message IDs. The first group is shared with Null45's plugins; MonthPassed
// comes from the gzcom-dll city hall sample and matches the simulator's
// AgentTypeSimNewMonth broadcast.
static constexpr uint32_t kMessageCheatIssued = 0x230E27AC;
static constexpr uint32_t kSC4MessagePostCityInit = 0x26D31EC1;
static constexpr uint32_t kSC4MessagePostCityInitComplete = 0xEA8AE29A;
static constexpr uint32_t kSC4MessagePostCityShutdown = 0x26D31EC3;
static constexpr uint32_t kSC4MessagePostRegionInit = 0xCBB5BB45;
static constexpr uint32_t kSC4MessagePreRegionShutdown = 0x8BB5BB46;
static constexpr uint32_t kSC4MessageSimNewMonth = 0x66956816;

// Director ID. Random 32-bit value, must not collide with other plugins.
static constexpr uint32_t kRegionSyncDllDirector = 0x7A3C9E51;

// Cheat IDs and strings.
static constexpr uint32_t kCheatDumpID = 0x7A3C9E52;
static constexpr uint32_t kCheatPublishID = 0x7A3C9E53;
static constexpr uint32_t kCheatPullID = 0x7A3C9E54;
static constexpr uint32_t kCheatReloadID = 0x7A3C9E55;
static constexpr uint32_t kCheatStatusID = 0x7A3C9E56;
static constexpr uint32_t kCheatSaveID = 0x7A3C9E57;
static constexpr uint32_t kCheatUnlockID = 0x7A3C9E58;
static constexpr uint32_t kCheatSetterTestID = 0x7A3C9E59;
static constexpr uint32_t kCheatProbeID = 0x7A3C9E5A;
static constexpr uint32_t kCheatDemandID = 0x7A3C9E5B;
static constexpr uint32_t kCheatFakeNeighborID = 0x7A3C9E5C;

static constexpr std::string_view kCheatDump = "RegionSyncDump";
static constexpr std::string_view kCheatPublish = "RegionSyncPublish";
static constexpr std::string_view kCheatPull = "RegionSyncPull";
static constexpr std::string_view kCheatReload = "RegionSyncReload";
static constexpr std::string_view kCheatStatus = "RegionSyncStatus";
static constexpr std::string_view kCheatSave = "RegionSyncSave";
static constexpr std::string_view kCheatUnlock = "RegionSyncUnlock";
static constexpr std::string_view kCheatSetterTest = "RegionSyncSetterTest";
static constexpr std::string_view kCheatProbe = "RegionSyncProbe";
static constexpr std::string_view kCheatDemand = "RegionSyncDemand";
static constexpr std::string_view kCheatFakeNeighbor = "RegionSyncFakeNeighbor";

namespace
{
	const char* ConnectionTypeName(cISC4NeighborsSimulator::ConnectionType type)
	{
		using CT = cISC4NeighborsSimulator::ConnectionType;
		switch (type)
		{
		case CT::None: return "None";
		case CT::Rail: return "Rail";
		case CT::Road: return "Road";
		case CT::Highway: return "Highway";
		case CT::PowerPole: return "PowerPole";
		case CT::Pipe: return "Pipe";
		case CT::Subway: return "Subway";
		case CT::LightRail: return "LightRail";
		case CT::Avenue: return "Avenue";
		case CT::Ferry: return "Ferry";
		case CT::DirtRoad: return "DirtRoad";
		case CT::Monorail: return "Monorail";
		case CT::GroundHighway: return "GroundHighway";
		default: return "Unknown";
		}
	}

	// With Setup-MultiInstance.ps1 every instance shares one Plugins folder,
	// so a log next to the DLL would be fought over by two processes. When
	// the game was started with -UserDir:"<dir>" the log goes there instead.
	std::filesystem::path LogFilePath()
	{
		std::wstring cmd = GetCommandLineW();
		std::wstring lower = cmd;
		for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));

		const std::wstring key = L"-userdir:";
		size_t pos = lower.find(key);
		if (pos != std::wstring::npos)
		{
			// Accept -UserDir:"C:\x\" and "-UserDir:C:\x\" (the latter is
			// what PowerShell's Start-Process produces).
			const bool quoteBefore = pos > 0 && cmd[pos - 1] == L'"';
			pos += key.size();
			const bool quoted = pos < cmd.size() && cmd[pos] == L'"';
			if (quoted) pos++;
			const size_t end = cmd.find((quoted || quoteBefore) ? L'"' : L' ', pos);
			std::wstring dir = cmd.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
			if (!dir.empty())
			{
				return std::filesystem::path(dir) / L"SC4RegionSync.log";
			}
		}
		return FileSystem::GetDllLogFilePath();
	}

	std::string GetRegionalCityName(cISC4RegionalCity* pCity)
	{
		cRZBaseString name;
		if (pCity)
		{
			pCity->GetCityName(name);
		}
		return name.ToChar();
	}
}

class RegionSyncDllDirector final : public cRZMessage2COMDirector
{
public:
	RegionSyncDllDirector()
		: pCheatCodeManager(nullptr),
		  pCity(nullptr),
		  pRegion(nullptr),
		  pOwnRegionalCity(nullptr),
		  pNeighborsSim(nullptr),
		  pSimulator(nullptr),
		  monthsSincePublish(0),
		  monthsSincePull(0),
		  monthsSinceSave(0),
		  publishCount(0),
		  pullCount(0),
		  instanceId(static_cast<uint32_t>(GetCurrentProcessId()))
	{
		Logger& logger = Logger::GetInstance();
		logger.Init(LogFilePath(), LogLevel::Info);
		logger.WriteLogFileHeader("SC4RegionSync v" PLUGIN_VERSION_STR);
		logger.Flush();
	}

	uint32_t GetDirectorID() const
	{
		return kRegionSyncDllDirector;
	}

	bool OnStart(cIGZCOM* pCOM)
	{
		const uint16_t gameVersion = SC4VersionDetection::GetGameVersion();

		if (gameVersion != 641)
		{
			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Error,
				"Requires game version 641, found %u. Plugin disabled.",
				gameVersion);
			return true;
		}

		settings.Load(FileSystem::GetDllIniFilePath());
		Logger::GetInstance().SetLogLevel(settings.logLevel);

		cIGZFrameWork* const pFramework = pCOM->FrameWork();
		if (pFramework->GetState() < cIGZFrameWork::kStatePreAppInit)
		{
			pFramework->AddHook(this);
		}
		else
		{
			PreAppInit();
		}
		return true;
	}

	bool PostAppInit()
	{
		Logger& logger = Logger::GetInstance();

		cISC4AppPtr pSC4App;
		if (pSC4App)
		{
			pCheatCodeManager = pSC4App->GetCheatCodeManager();
		}

		cIGZMessageServer2Ptr pMS2;
		if (!pMS2)
		{
			logger.WriteLine(LogLevel::Error, "Failed to get the message server.");
			return false;
		}

		const uint32_t messages[] =
		{
			kSC4MessagePostCityInit,
			kSC4MessagePostCityInitComplete,
			kSC4MessagePostCityShutdown,
			kSC4MessagePostRegionInit,
			kSC4MessagePreRegionShutdown,
			kSC4MessageSimNewMonth,
		};

		for (uint32_t id : messages)
		{
			if (!pMS2->AddNotification(this, id))
			{
				logger.WriteLineFormatted(LogLevel::Error, "Failed to subscribe to message 0x%08X.", id);
				return false;
			}
		}

		logger.WriteLineFormatted(
			LogLevel::Info,
			"Initialized. instanceId=%u enabled=%d publishEveryMonths=%d pullEveryMonths=%d autoSaveEveryMonths=%d",
			instanceId,
			settings.enabled ? 1 : 0,
			settings.publishEveryMonths,
			settings.pullEveryMonths,
			settings.autoSaveEveryMonths);

		logger.Flush();

		if (settings.releaseInstanceMutexOnStart)
		{
			Unlock("startup");
		}

		return true;
	}

	bool DoMessage(cIGZMessage2* pMsg)
	{
		cIGZMessage2Standard* pStandardMsg = static_cast<cIGZMessage2Standard*>(pMsg);

		switch (pStandardMsg->GetType())
		{
		case kMessageCheatIssued:
			ProcessCheat(pStandardMsg);
			break;
		case kSC4MessagePostRegionInit:
			PostRegionInit();
			break;
		case kSC4MessagePreRegionShutdown:
			PreRegionShutdown();
			break;
		case kSC4MessagePostCityInit:
			PostCityInit(pStandardMsg);
			break;
		case kSC4MessagePostCityInitComplete:
			PostCityInitComplete();
			break;
		case kSC4MessagePostCityShutdown:
			PostCityShutdown();
			break;
		case kSC4MessageSimNewMonth:
			OnNewMonth();
			break;
		}

		// The logger buffers; flush so the file is live and survives a crash.
		Logger::GetInstance().Flush();
		return true;
	}

private:
	// ---------------------------------------------------------------
	// Lifecycle
	// ---------------------------------------------------------------

	void PostRegionInit()
	{
		cISC4AppPtr pSC4App;
		pRegion = pSC4App ? pSC4App->GetRegion() : nullptr;

		if (pRegion)
		{
			SC4String* pDirName = pRegion->GetDirectoryName();
			regionDirectoryName = pDirName ? pDirName->ToChar() : "";
			storeDirectory = settings.ResolveStoreDirectory(regionDirectoryName);

			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Info,
				"Region loaded: '%s'. Store directory: %s",
				regionDirectoryName.c_str(),
				storeDirectory.string().c_str());
		}
	}

	void PreRegionShutdown()
	{
		pRegion = nullptr;
		regionDirectoryName.clear();
	}

	void PostCityInit(cIGZMessage2Standard* pStandardMsg)
	{
		pCity = static_cast<cISC4City*>(pStandardMsg->GetVoid1());

		if (pCheatCodeManager)
		{
			pCheatCodeManager->RegisterCheatCode(kCheatDumpID, cRZBaseString(kCheatDump));
			pCheatCodeManager->RegisterCheatCode(kCheatPublishID, cRZBaseString(kCheatPublish));
			pCheatCodeManager->RegisterCheatCode(kCheatPullID, cRZBaseString(kCheatPull));
			pCheatCodeManager->RegisterCheatCode(kCheatReloadID, cRZBaseString(kCheatReload));
			pCheatCodeManager->RegisterCheatCode(kCheatStatusID, cRZBaseString(kCheatStatus));
			pCheatCodeManager->RegisterCheatCode(kCheatSaveID, cRZBaseString(kCheatSave));
			pCheatCodeManager->RegisterCheatCode(kCheatUnlockID, cRZBaseString(kCheatUnlock));
			pCheatCodeManager->RegisterCheatCode(kCheatSetterTestID, cRZBaseString(kCheatSetterTest));
			pCheatCodeManager->RegisterCheatCode(kCheatProbeID, cRZBaseString(kCheatProbe));
			pCheatCodeManager->RegisterCheatCode(kCheatDemandID, cRZBaseString(kCheatDemand));
			pCheatCodeManager->RegisterCheatCode(kCheatFakeNeighborID, cRZBaseString(kCheatFakeNeighbor));
			pCheatCodeManager->AddNotification2(this, 0);
		}
	}

	void PostCityInitComplete()
	{
		Logger& logger = Logger::GetInstance();

		cISC4AppPtr pSC4App;
		if (!pSC4App || !pCity)
		{
			return;
		}

		pRegion = pSC4App->GetRegion();
		pOwnRegionalCity = pSC4App->GetRegionalCity();
		pNeighborsSim = pCity->GetNeighborsSimulator();
		pSimulator = pCity->GetSimulator();

		monthsSincePublish = 0;
		monthsSincePull = 0;
		monthsSinceSave = 0;

		if (pRegion && regionDirectoryName.empty())
		{
			// PostRegionInit can fire before our subscription in some paths.
			PostRegionInit();
		}

		int32_t x = -1, z = -1;
		if (pOwnRegionalCity)
		{
			pOwnRegionalCity->GetPosition(x, z);
		}

		logger.WriteLineFormatted(
			LogLevel::Info,
			"City loaded: '%s' at (%d,%d) established=%d neighborsSim=%p simulator=%p",
			GetRegionalCityName(pOwnRegionalCity).c_str(),
			x, z,
			pCity->GetEstablished() ? 1 : 0,
			static_cast<void*>(pNeighborsSim),
			static_cast<void*>(pSimulator));
		logger.Flush();

		DiscoverRegionalDemand();

		if (settings.enabled && settings.publishOnLoad)
		{
			logger.WriteLine(LogLevel::Info, "City load: publishing");
			logger.Flush();
			Publish("city load");
		}
		if (settings.enabled && settings.pullOnLoad)
		{
			logger.WriteLine(LogLevel::Info, "City load: pulling");
			logger.Flush();
			Pull("city load");
		}
		logger.WriteLine(LogLevel::Info, "City load: done");
	}

	// ---------------------------------------------------------------
	// Regional demand feed (Phase 1b)
	//
	// cSC4DemandSimulator adds the region's extrapolated per-type
	// population into the demand objects whose GetRegionUse() is 1
	// (AddToSupplyValue) or 2 (AddToDemandValue), but only as a delta when
	// its neighbor list changes, and once at city load (FUN_006a5cb0 in
	// 1.1.641). Records updated by a pull are therefore never seen. We do
	// the same bookkeeping: baseline sums at city load, delta after each
	// pull.
	// ---------------------------------------------------------------

	struct RegionalDemandEntry
	{
		uint32_t groupId;
		uint32_t demandIndex;
		uint32_t regionUse;
		cISC4Demand* pDemand;
		float lastSum;
	};

	float SumOtherTiles(uint32_t groupId)
	{
		if (!pRegion || !pOwnRegionalCity) return 0.0f;
		int32_t ox = 0, oz = 0;
		pOwnRegionalCity->GetPosition(ox, oz);
		SC4Vector<cISC4Region::cLocation> locations;
		pRegion->GetCityLocations(locations);
		float sum = 0.0f;
		for (size_t i = 0; i < locations.size(); i++)
		{
			const int32_t x = static_cast<int32_t>(locations[i].x);
			const int32_t z = static_cast<int32_t>(locations[i].z);
			if (x == ox && z == oz) continue;
			cISC4RegionalCity* p = regionsync::FindRegionalCity(pRegion, x, z);
			if (!p || !p->GetEstablished()) continue;
			sum += static_cast<float>(p->GetExtrapolatedPopulation(groupId));
		}
		return sum;
	}

	void DiscoverRegionalDemand()
	{
		Logger& logger = Logger::GetInstance();
		regionalDemand.clear();
		cISC4DemandSimulator* pDemandSim = pCity ? pCity->GetDemandSimulator() : nullptr;
		if (!pDemandSim)
		{
			logger.WriteLine(LogLevel::Info, "RegionalDemand: no demand simulator.");
			return;
		}
		// Demand index is "the upper bytes of the instance id"; 0x20000 is
		// the totals set (region use 0). Try the others.
		const uint32_t indices[] = { 0x00000, 0x10000, 0x20000, 0x30000, 0x40000, 0x50000, 0x60000, 0x70000, 0x80000 };
		for (uint32_t groupId : regionsync::kRCIGroupIds)
		{
			for (uint32_t idx : indices)
			{
				cISC4Demand* pDemand = pDemandSim->GetDemand(groupId, idx);
				if (!pDemand) continue;
				const uint32_t use = pDemand->GetRegionUse();
				if (use == 1 || use == 2)
				{
					const float base = SumOtherTiles(groupId);
					regionalDemand.push_back(RegionalDemandEntry{ groupId, idx, use, pDemand, base });
					logger.WriteLineFormatted(LogLevel::Info, "RegionalDemand: 0x%04X index 0x%05X regionUse=%u baseline regional pop=%.0f demand=%.1f supply=%.1f",
						groupId, idx, use, base, pDemand->QueryDemandValue(), pDemand->QuerySupplyValue());
				}
			}
		}
		logger.WriteLineFormatted(LogLevel::Info, "RegionalDemand: %zu region-using demand objects found.", regionalDemand.size());
		logger.Flush();
	}

	void ApplyRegionalDemandDelta(const char* reason)
	{
		if (!settings.applyRegionalDemand || regionalDemand.empty()) return;
		Logger& logger = Logger::GetInstance();
		for (RegionalDemandEntry& e : regionalDemand)
		{
			const float now = SumOtherTiles(e.groupId);
			const float delta = now - e.lastSum;
			if (std::fabs(delta) < 0.5f) continue;
			logger.WriteLineFormatted(LogLevel::Info, "RegionalDemand (%s): 0x%04X idx 0x%05X use=%u regional pop %.0f -> %.0f, %s(%.0f); before demand=%.1f supply=%.1f",
				reason, e.groupId, e.demandIndex, e.regionUse, e.lastSum, now,
				e.regionUse == 1 ? "AddToSupplyValue" : "AddToDemandValue", delta,
				e.pDemand->QueryDemandValue(), e.pDemand->QuerySupplyValue());
			logger.Flush();
			if (e.regionUse == 1) e.pDemand->AddToSupplyValue(delta);
			else e.pDemand->AddToDemandValue(delta);
			e.lastSum = now;
			logger.WriteLineFormatted(LogLevel::Info, "RegionalDemand (%s): after demand=%.1f supply=%.1f", reason, e.pDemand->QueryDemandValue(), e.pDemand->QuerySupplyValue());
			logger.Flush();
		}
	}

	void PostCityShutdown()
	{
		if (settings.enabled && settings.publishOnShutdown && pCity)
		{
			Publish("city shutdown");
		}

		pCity = nullptr;
		pOwnRegionalCity = nullptr;
		pNeighborsSim = nullptr;
		pSimulator = nullptr;
		regionalDemand.clear();

		if (pCheatCodeManager)
		{
			pCheatCodeManager->UnregisterCheatCode(kCheatDumpID);
			pCheatCodeManager->UnregisterCheatCode(kCheatPublishID);
			pCheatCodeManager->UnregisterCheatCode(kCheatPullID);
			pCheatCodeManager->UnregisterCheatCode(kCheatReloadID);
			pCheatCodeManager->UnregisterCheatCode(kCheatStatusID);
			pCheatCodeManager->UnregisterCheatCode(kCheatSaveID);
			pCheatCodeManager->UnregisterCheatCode(kCheatUnlockID);
			pCheatCodeManager->UnregisterCheatCode(kCheatSetterTestID);
			pCheatCodeManager->UnregisterCheatCode(kCheatProbeID);
			pCheatCodeManager->UnregisterCheatCode(kCheatDemandID);
			pCheatCodeManager->UnregisterCheatCode(kCheatFakeNeighborID);
			pCheatCodeManager->RemoveNotification2(this, 0);
		}
	}

	void OnNewMonth()
	{
		if (!settings.enabled || !pCity || !pCity->GetEstablished())
		{
			return;
		}

		monthsSincePublish++;
		monthsSincePull++;
		monthsSinceSave++;

		if (settings.publishEveryMonths > 0 && monthsSincePublish >= settings.publishEveryMonths)
		{
			Publish("month tick");
			monthsSincePublish = 0;
		}

		if (settings.pullEveryMonths > 0 && monthsSincePull >= settings.pullEveryMonths)
		{
			Pull("month tick");
			monthsSincePull = 0;
		}

		if (settings.autoSaveEveryMonths > 0 && monthsSinceSave >= settings.autoSaveEveryMonths)
		{
			Save("month tick");
			monthsSinceSave = 0;
		}
	}

	// ---------------------------------------------------------------
	// Core operations
	// ---------------------------------------------------------------

	// Publish: refresh this tile's regional record from the live city, then
	// write it to the shared store so other instances can pull it.
	void Publish(const char* reason)
	{
		Logger& logger = Logger::GetInstance();

		if (!pOwnRegionalCity || !pCity)
		{
			logger.WriteLine(LogLevel::Error, "Publish: no city loaded.");
			return;
		}

		if (settings.refreshCacheBeforePublish)
		{
			// This is what the game does on save to bring the regional
			// record up to date with the live city. If it has side effects
			// beyond the in-memory record, the Dump cheat before/after will
			// show it. See docs/OPEN-QUESTIONS.md, Q1.
			pOwnRegionalCity->UpdateCityCache(pCity);
		}

		regionsync::NeighborSnapshot snapshot;
		regionsync::CaptureRegionalCity(pOwnRegionalCity, pSimulator, snapshot);

		const std::filesystem::path path =
			storeDirectory / regionsync::SnapshotFileName(snapshot.regionX, snapshot.regionZ);

		std::string error;
		if (regionsync::WriteSnapshotFile(path, snapshot, error))
		{
			publishCount++;
			logger.WriteLineFormatted(
				LogLevel::Info,
				"Publish (%s): (%d,%d) '%s' pop=%d com=%d ind=%d simDate=%u-%02u-%02u -> %s",
				reason,
				snapshot.regionX, snapshot.regionZ,
				snapshot.cityName.c_str(),
				snapshot.population, snapshot.commercialJobs, snapshot.industrialJobs,
				snapshot.simYear, snapshot.simMonth, snapshot.simDay,
				path.string().c_str());
		}
		else
		{
			logger.WriteLineFormatted(LogLevel::Error, "Publish (%s) failed: %s", reason, error.c_str());
		}
	}

	// Pull: read every snapshot in the store except our own and apply it to
	// the matching regional record in this instance.
	void Pull(const char* reason)
	{
		Logger& logger = Logger::GetInstance();

		if (!pRegion || !pOwnRegionalCity)
		{
			logger.WriteLine(LogLevel::Error, "Pull: no region loaded.");
			return;
		}

		int32_t ownX = -1, ownZ = -1;
		pOwnRegionalCity->GetPosition(ownX, ownZ);

		std::error_code ec;
		if (!std::filesystem::exists(storeDirectory, ec))
		{
			logger.WriteLineFormatted(LogLevel::Info, "Pull (%s): store directory does not exist yet.", reason);
			return;
		}

		int filesSeen = 0;
		int applied = 0;
		int totalChanged = 0;

		for (const auto& entry : std::filesystem::directory_iterator(storeDirectory, ec))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".snapshot")
			{
				continue;
			}

			filesSeen++;

			regionsync::NeighborSnapshot snapshot;
			std::string error;
			if (!regionsync::ReadSnapshotFile(entry.path(), snapshot, error))
			{
				logger.WriteLineFormatted(LogLevel::Error, "Pull: %s", error.c_str());
				continue;
			}

			if (snapshot.regionX == ownX && snapshot.regionZ == ownZ)
			{
				continue; // our own tile
			}
			if (fakeNeighborHeld && snapshot.regionX == fakeNeighborX && snapshot.regionZ == fakeNeighborZ)
			{
				logger.WriteLineFormatted(LogLevel::Info, "Pull: (%d,%d) held by RegionSyncFakeNeighbor, skipped.", snapshot.regionX, snapshot.regionZ);
				continue;
			}

			cISC4RegionalCity* pTarget = regionsync::FindRegionalCity(pRegion, snapshot.regionX, snapshot.regionZ);
			if (!pTarget)
			{
				logger.WriteLineFormatted(
					LogLevel::Debug,
					"Pull: no regional record for (%d,%d), skipping.",
					snapshot.regionX, snapshot.regionZ);
				continue;
			}

			// Log before and after: these setters and MarkDealsDirty are
			// reverse-engineered vtable slots that had never been exercised
			// with real changes before 2026-10-17 (instance 2 crashed on
			// city load that day).
			logger.WriteLineFormatted(
				LogLevel::Info,
				"Pull: record for (%d,%d) '%s' found (record pop=%d, snapshot pop=%d). apply=%d",
				snapshot.regionX, snapshot.regionZ,
				snapshot.cityName.c_str(),
				pTarget->GetPopulation(),
				snapshot.population,
				settings.applyPulledSnapshots ? 1 : 0);
			logger.WriteLineFormatted(
				LogLevel::Info,
				"Pull: applyFields established=%d budget=%d income=%d commodities=%d population=%d markDealsDirty=%d",
				settings.applyEstablished ? 1 : 0, settings.applyBudget ? 1 : 0,
				settings.applyIncome ? 1 : 0, settings.applyCommodities ? 1 : 0,
				settings.applyPopulation ? 1 : 0, settings.markDealsDirty ? 1 : 0);
			logger.Flush();

			if (!settings.applyPulledSnapshots)
			{
				continue;
			}

			regionsync::ApplyOptions opt;
			opt.established = settings.applyEstablished;
			opt.budget = settings.applyBudget;
			opt.income = settings.applyIncome;
			opt.commodities = settings.applyCommodities;
			opt.population = settings.applyPopulation;

			const int changed = regionsync::ApplySnapshotToRegionalCity(snapshot, pTarget, opt);
			applied++;
			totalChanged += changed;

			logger.WriteLineFormatted(
				LogLevel::Info,
				"Pull: applied (%d,%d) changedFields=%d",
				snapshot.regionX, snapshot.regionZ, changed);
			logger.Flush();
		}

		if (totalChanged > 0)
		{
			ApplyRegionalDemandDelta("pull");
		}

		if (totalChanged > 0 && pNeighborsSim && settings.markDealsDirty)
		{
			// Ask the neighbors simulator to recompute deal state from the
			// updated records.
			logger.WriteLine(LogLevel::Info, "Pull: calling MarkDealsDirty");
			logger.Flush();
			pNeighborsSim->MarkDealsDirty();
			logger.WriteLine(LogLevel::Info, "Pull: MarkDealsDirty returned");
		}

		pullCount++;
		logger.WriteLineFormatted(
			LogLevel::Info,
			"Pull (%s): files=%d applied=%d changedFields=%d",
			reason, filesSeen, applied, totalChanged);
	}

	// Reload: ask the region to re-read neighbor records from disk. This is
	// the "can the neighbor load be re-run mid-game" experiment. It bypasses
	// our store entirely and uses the other instance's save file instead.
	void ReloadAdjacent()
	{
		Logger& logger = Logger::GetInstance();

		if (!pRegion || !pNeighborsSim || !pOwnRegionalCity)
		{
			logger.WriteLine(LogLevel::Error, "Reload: no region or neighbors simulator.");
			return;
		}

		// GetAdjacentCities' list elements crashed on first use (2026-10-17);
		// walk the region's tile list instead and keep only established
		// tiles other than ours. Adjacency is not checked.
		int32_t ownX = -1, ownZ = -1;
		pOwnRegionalCity->GetPosition(ownX, ownZ);
		SC4Vector<cISC4Region::cLocation> locations;
		pRegion->GetCityLocations(locations);
		std::vector<cISC4RegionalCity*> adjacent;
		for (size_t i = 0; i < locations.size(); i++)
		{
			const int32_t x = static_cast<int32_t>(locations[i].x);
			const int32_t z = static_cast<int32_t>(locations[i].z);
			if (x == ownX && z == ownZ) continue;
			cISC4RegionalCity* p = regionsync::FindRegionalCity(pRegion, x, z);
			if (p && p->GetEstablished()) adjacent.push_back(p);
		}

		int reloaded = 0;
		for (cISC4RegionalCity* pAdj : adjacent)
		{
			int32_t x = -1, z = -1;
			pAdj->GetPosition(x, z);
			const int32_t popBefore = pAdj->GetPopulation();

			cRZAutoRefCount<cISC4RegionalCity>* pSlot = pRegion->GetCity(x, z);
			if (!pSlot)
			{
				continue;
			}

			const bool ok = pRegion->ReloadCity(pSlot);
			cISC4RegionalCity* pAfter = regionsync::FindRegionalCity(pRegion, x, z);

			logger.WriteLineFormatted(
				LogLevel::Info,
				"Reload (%d,%d): ok=%d popBefore=%d popAfter=%d recordPtrChanged=%d",
				x, z,
				ok ? 1 : 0,
				popBefore,
				pAfter ? pAfter->GetPopulation() : -1,
				(pAfter != pAdj) ? 1 : 0);

			if (ok)
			{
				reloaded++;
			}
		}

		if (reloaded > 0)
		{
			pNeighborsSim->MarkDealsDirty();
		}

		logger.WriteLineFormatted(LogLevel::Info, "Reload: %d of %zu adjacent tiles reloaded.", reloaded, static_cast<size_t>(adjacent.size()));
	}

	void Save(const char* reason)
	{
		cISC4AppPtr pSC4App;
		if (pSC4App && pCity)
		{
			// Fast save, no notification popup.
			const bool ok = pSC4App->RequestSaveCity(false, true);
			Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "Save (%s): requested, ok=%d", reason, ok ? 1 : 0);
		}
	}

	// Dump: write everything we can see about the region and this tile's
	// neighbor state to the log. This is the primary research tool.
	void Dump()
	{
		Logger& logger = Logger::GetInstance();

		if (!pRegion)
		{
			logger.WriteLine(LogLevel::Error, "Dump: no region.");
			return;
		}

		logger.WriteLine(LogLevel::Info, "==== RegionSyncDump begin ====");

		// Every tile in the region
		SC4Vector<cISC4Region::cLocation> locations;
		pRegion->GetCityLocations(locations);
		logger.WriteLineFormatted(LogLevel::Info, "Region '%s': %zu tiles", regionDirectoryName.c_str(), static_cast<size_t>(locations.size()));

		for (size_t i = 0; i < locations.size(); i++)
		{
			const cISC4Region::cLocation& loc = locations[i];
			cISC4RegionalCity* pRC = regionsync::FindRegionalCity(pRegion, static_cast<int32_t>(loc.x), static_cast<int32_t>(loc.z));
			if (!pRC)
			{
				continue;
			}

			regionsync::NeighborSnapshot s;
			regionsync::CaptureRegionalCity(pRC, nullptr, s);

			logger.WriteLineFormatted(
				LogLevel::Info,
				"  tile (%u,%u) size=%u '%s' est=%d serial=%u pop=%d com=%d ind=%d wf=%.3f rating=%d budget=%.0f income=%.0f",
				loc.x, loc.z, static_cast<uint32_t>(loc.cityTileSize),
				s.cityName.c_str(), s.established ? 1 : 0, s.citySerial,
				s.population, s.commercialJobs, s.industrialJobs,
				s.workforcePercentage, static_cast<int32_t>(s.mayorRating),
				s.budget, s.income);

			for (size_t c = 0; c < regionsync::kCommoditySlots; c++)
			{
				const auto& cs = s.commodities[c];
				if (cs.exported != 0 || cs.imported != 0 || cs.produced != 0 || cs.demanded != 0 || cs.costPerUnit != 0)
				{
					logger.WriteLineFormatted(
						LogLevel::Info,
						"    commodity[%zu] exp=%.2f imp=%.2f prod=%.2f dem=%.2f cost=%.4f",
						c, cs.exported, cs.imported, cs.produced, cs.demanded, cs.costPerUnit);
				}
			}

			for (size_t g = 0; g < s.rciGroupCount; g++)
			{
				if (s.rciGroups[g].population != 0)
				{
					logger.WriteLineFormatted(LogLevel::Info, "    rci 0x%04X pop=%d", s.rciGroups[g].groupId, s.rciGroups[g].population);
				}
			}
		}

		// This tile's connections and deals
		if (pNeighborsSim)
		{
			SC4List<cISC4NeighborConnection*> connections;
			pNeighborsSim->GetAllConnections(connections);
			logger.WriteLineFormatted(LogLevel::Info, "Connections from this tile: %zu", static_cast<size_t>(connections.size()));

			for (cISC4NeighborConnection* pConn : connections)
			{
				if (!pConn)
				{
					continue;
				}
				int32_t cx = 0, cz = 0, ox = 0, oz = 0;
				pConn->GetConnectionCell(cx, cz);
				pConn->GetRegionCellOnOtherSideOfConnection(ox, oz);
				cISC4RegionalCity* pOther = pConn->GetCityOnOtherSideOfConnection();

				logger.WriteLineFormatted(
					LogLevel::Info,
					"  %s at cell (%d,%d) -> region cell (%d,%d) '%s' active=%d",
					ConnectionTypeName(pConn->GetType()),
					cx, cz, ox, oz,
					GetRegionalCityName(pOther).c_str(),
					pConn->GetIsActive() ? 1 : 0);
			}

			logger.WriteLineFormatted(
				LogLevel::Info,
				"Deal income/month=%lld expense/month=%lld",
				pNeighborsSim->GetTotalMonthlyNeighborDealIncome(),
				pNeighborsSim->GetTotalMonthlyNeighborDealExpense());
		}

		if (pOwnRegionalCity)
		{
			SC4List<cISC4NeighborDeal*> deals;
			pOwnRegionalCity->GetLocalDeals(deals);
			logger.WriteLineFormatted(LogLevel::Info, "Local deals: %zu", static_cast<size_t>(deals.size()));

			for (cISC4NeighborDeal* pDeal : deals)
			{
				if (!pDeal)
				{
					continue;
				}
				int32_t ox = 0, oz = 0;
				pDeal->GetRegionCellOnOtherSideOfConnection(ox, oz);
				logger.WriteLineFormatted(
					LogLevel::Info,
					"  deal type=%d amount=%d used=%d cost=%lld flags=0x%X other=(%d,%d) '%s'",
					pDeal->GetType(), pDeal->GetAmount(), pDeal->GetAmountUsed(),
					pDeal->GetCost(), pDeal->GetFlags(), ox, oz,
					GetRegionalCityName(pDeal->GetCityOnOtherSideOfConnection()).c_str());
			}
		}

		logger.WriteLine(LogLevel::Info, "==== RegionSyncDump end ====");
		SC4NotificationDialog::ShowDialog(cRZBaseString("Dump written to SC4RegionSync.log"), cRZBaseString("RegionSync"));
	}

	// Unlock: close the game's single-instance mutex so another instance
	// can start. Lists what it found first so the log shows the names.
	void Unlock(const char* reason)
	{
		Logger& logger = Logger::GetInstance();
		int closed = 0;
		try
		{
			for (const auto& h : regionsync::FindNamedMutexHandles(L""))
			{
				logger.WriteLineFormatted(LogLevel::Info, "Unlock (%s): mutex handle 0x%04X '%ls'", reason, h.handle, h.name.c_str());
			}
			closed = regionsync::CloseNamedMutexHandles(L"SimCity 4 Mutex");
		}
		catch (const std::exception& e)
		{
			logger.WriteLineFormatted(LogLevel::Error, "Unlock (%s) failed: %s", reason, e.what());
		}
		logger.WriteLineFormatted(LogLevel::Info, "Unlock (%s): closed %d handle(s) named 'SimCity 4 Mutex'.", reason, closed);
	}

	// SetterTest: cISC4RegionalCity setters crashed the game when called on
	// another tile's record during a pull (2026-10-17). First run showed
	// same-value writes on the own record pass; this version writes changed
	// values (then restores) and takes the neighbor from the region slot,
	// the same path Pull uses. One flushed log line per step.
	void SetterTest()
	{
		Logger& logger = Logger::GetInstance();
		if (!pOwnRegionalCity || !pRegion)
		{
			logger.WriteLine(LogLevel::Error, "SetterTest: no city.");
			return;
		}

		auto step = [&](const char* what, auto&& fn)
		{
			logger.WriteLineFormatted(LogLevel::Info, "SetterTest: %s ...", what);
			logger.Flush();
			fn();
			logger.WriteLineFormatted(LogLevel::Info, "SetterTest: %s ok", what);
			logger.Flush();
		};

		auto probe = [&](const char* who, cISC4RegionalCity* p)
		{
			using namespace regionsync::rc641;
			double v = 0.0;
			step((std::string(who) + " GetProduced(2)").c_str(), [&] { v = GetCommodity(p, kGetProduced, 2); logger.WriteLineFormatted(LogLevel::Info, "  = %.2f", v); });
			step((std::string(who) + " SetProduced(2, +1)").c_str(), [&] { SetCommodity(p, kSetProduced, 2, v + 1.0); logger.WriteLineFormatted(LogLevel::Info, "  readback = %.2f", GetCommodity(p, kGetProduced, 2)); });
			step((std::string(who) + " SetProduced(2, restore)").c_str(), [&] { SetCommodity(p, kSetProduced, 2, v); logger.WriteLineFormatted(LogLevel::Info, "  readback = %.2f", GetCommodity(p, kGetProduced, 2)); });

			double b = 0.0;
			step((std::string(who) + " GetBudget").c_str(), [&] { b = GetBudget(p); logger.WriteLineFormatted(LogLevel::Info, "  = %.0f", b); });
			step((std::string(who) + " SetBudget(+1)").c_str(), [&] { SetBudget(p, b + 1.0); logger.WriteLineFormatted(LogLevel::Info, "  readback = %.0f", GetBudget(p)); });
			step((std::string(who) + " SetBudget(restore)").c_str(), [&] { SetBudget(p, b); logger.WriteLineFormatted(LogLevel::Info, "  readback = %.0f", GetBudget(p)); });
		};

		probe("own", pOwnRegionalCity);

		// Neighbor via the region slot, exactly as Pull does.
		int32_t ox = 0, oz = 0;
		pOwnRegionalCity->GetPosition(ox, oz);
		cISC4RegionalCity* pOther = nullptr;
		SC4Vector<cISC4Region::cLocation> locations;
		pRegion->GetCityLocations(locations);
		for (size_t i = 0; i < locations.size() && !pOther; i++)
		{
			const int32_t x = static_cast<int32_t>(locations[i].x);
			const int32_t z = static_cast<int32_t>(locations[i].z);
			if (x == ox && z == oz) continue;
			cISC4RegionalCity* p = regionsync::FindRegionalCity(pRegion, x, z);
			if (p && p->GetEstablished()) pOther = p;
		}
		if (!pOther)
		{
			logger.WriteLine(LogLevel::Info, "SetterTest: no other established tile, neighbor steps skipped.");
			return;
		}

		logger.WriteLineFormatted(LogLevel::Info, "SetterTest: neighbor %p own %p", static_cast<void*>(pOther), static_cast<void*>(pOwnRegionalCity));
		logger.Flush();
		step("neighbor GetCityName", [&] { logger.WriteLineFormatted(LogLevel::Info, "  = '%s'", GetRegionalCityName(pOther).c_str()); });
		probe("neighbor", pOther);
		logger.WriteLine(LogLevel::Info, "SetterTest: all steps survived.");
		logger.Flush();
	}

	// Probe: getters only, through the corrected slots, so the slot 9..16
	// semantics read from the binary can be checked against the UI.
	void Probe()
	{
		using namespace regionsync::rc641;
		Logger& logger = Logger::GetInstance();
		cISC4RegionalCity* p = pOwnRegionalCity;
		if (!p)
		{
			logger.WriteLine(LogLevel::Error, "Probe: no city.");
			return;
		}
		logger.WriteLine(LogLevel::Info, "==== RegionSyncProbe begin ====");
		logger.Flush();
		logger.WriteLineFormatted(LogLevel::Info, "slot10 GetPopulation=%d slot11 GetCommercialJobs=%d slot12 GetIndustrialJobs=%d",
			GetPopulation(p), GetCommercialJobs(p), GetIndustrialJobs(p));
		logger.Flush();
		logger.WriteLineFormatted(LogLevel::Info, "slot14 GetMayorRating=%d slot15 GetDifficultyLevel=%d",
			static_cast<int32_t>(GetMayorRating(p)), GetDifficultyLevel(p));
		logger.Flush();
		const uint32_t types[] = { 0x1010, 0x1020, 0x1030, 0x3110, 0x3320, 0x4200, 0x4401 };
		for (uint32_t t : types)
		{
			logger.WriteLineFormatted(LogLevel::Info, "slot9 GetPopulation(0x%04X)=%d slot16 GetTaxRate(0x%04X)=%.3f",
				t, GetPopulation(p, t), t, GetTaxRate(p, t));
			logger.Flush();
		}
		const float* wf = static_cast<const float*>(GetWorkforce(p));
		if (wf)
		{
			logger.WriteLineFormatted(LogLevel::Info, "slot13 GetWorkforce ptr=%p floats=%.4f %.4f %.4f ints=%d %d %d",
				static_cast<const void*>(wf), wf[0], wf[1], wf[2],
				reinterpret_cast<const int32_t*>(wf)[0], reinterpret_cast<const int32_t*>(wf)[1], reinterpret_cast<const int32_t*>(wf)[2]);
			logger.Flush();
		}
		logger.WriteLineFormatted(LogLevel::Info, "slot50 GetBudget=%.2f slot52 GetIncome=%.2f", GetBudget(p), GetIncome(p));
		logger.Flush();
		for (int32_t c = 0; c < 4; c++)
		{
			logger.WriteLineFormatted(LogLevel::Info, "commodity %d exp=%.2f imp=%.2f prod=%.2f dem=%.2f cost=%.4f balance=%.2f",
				c, GetCommodity(p, kGetExported, c), GetCommodity(p, kGetImported, c), GetCommodity(p, kGetProduced, c),
				GetCommodity(p, kGetDemanded, c), GetCommodity(p, kGetCostPerUnit, c), GetCommodity(p, kGetCommodityBalance, c));
			logger.Flush();
		}
		logger.WriteLine(LogLevel::Info, "==== RegionSyncProbe end ====");
		logger.Flush();
	}

	// DemandDump: numeric regional demand readout per RCI group, the
	// instrument for Q2 (the demand graph is too coarse to read by eye).
	void DemandDump()
	{
		Logger& logger = Logger::GetInstance();
		cISC4DemandSimulator* pDemandSim = pCity ? pCity->GetDemandSimulator() : nullptr;
		if (!pDemandSim)
		{
			logger.WriteLine(LogLevel::Error, "Demand: no demand simulator.");
			return;
		}
		constexpr uint32_t kTotalsDemandIndex = 0x20000;
		logger.WriteLine(LogLevel::Info, "==== RegionSyncDemand begin ====");
		for (uint32_t groupId : regionsync::kRCIGroupIds)
		{
			cISC4Demand* pDemand = pDemandSim->GetDemand(groupId, kTotalsDemandIndex);
			if (!pDemand)
			{
				logger.WriteLineFormatted(LogLevel::Info, "  0x%04X: no demand object", groupId);
				continue;
			}
			const SC4Percentage* pCap = pDemand->GetDemandCap();
			logger.WriteLineFormatted(
				LogLevel::Info,
				"  0x%04X demand=%.1f active=%.1f supply=%.1f max=%.1f min=%.1f cap=%.4f regionUse=%u",
				groupId,
				pDemand->QueryDemandValue(), pDemand->QueryActiveDemandValue(), pDemand->QuerySupplyValue(),
				pDemand->QueryActiveDemandMax(), pDemand->QueryActiveDemandMin(),
				pCap ? pCap->percentage : -1.0f, pDemand->GetRegionUse());
		}
		for (const RegionalDemandEntry& e : regionalDemand)
		{
			const SC4Percentage* pCap = e.pDemand->GetDemandCap();
			logger.WriteLineFormatted(LogLevel::Info, "  regional 0x%04X idx 0x%05X use=%u demand=%.1f active=%.1f supply=%.1f cap=%.4f lastSum=%.0f",
				e.groupId, e.demandIndex, e.regionUse, e.pDemand->QueryDemandValue(), e.pDemand->QueryActiveDemandValue(),
				e.pDemand->QuerySupplyValue(), pCap ? pCap->percentage : -1.0f, e.lastSum);
		}
		logger.WriteLine(LogLevel::Info, "==== RegionSyncDemand end ====");
		logger.Flush();
	}

	// FakeNeighbor: toggle. First call writes a huge population into the
	// first other established tile's record and holds it against the
	// monthly pull; second call releases the hold (the next pull restores
	// the real values). RegionSyncDemand before and a month after tells
	// whether the demand simulator reads regional records live (Q2).
	void FakeNeighbor()
	{
		Logger& logger = Logger::GetInstance();
		if (fakeNeighborHeld)
		{
			fakeNeighborHeld = false;
			logger.WriteLineFormatted(LogLevel::Info, "FakeNeighbor: released (%d,%d); next pull restores it.", fakeNeighborX, fakeNeighborZ);
			logger.Flush();
			return;
		}
		if (!pRegion || !pOwnRegionalCity)
		{
			logger.WriteLine(LogLevel::Error, "FakeNeighbor: no region.");
			return;
		}
		int32_t ox = 0, oz = 0;
		pOwnRegionalCity->GetPosition(ox, oz);
		SC4Vector<cISC4Region::cLocation> locations;
		pRegion->GetCityLocations(locations);
		for (size_t i = 0; i < locations.size(); i++)
		{
			const int32_t x = static_cast<int32_t>(locations[i].x);
			const int32_t z = static_cast<int32_t>(locations[i].z);
			if (x == ox && z == oz) continue;
			cISC4RegionalCity* p = regionsync::FindRegionalCity(pRegion, x, z);
			if (!p || !p->GetEstablished()) continue;

			const int32_t pop = regionsync::rc641::GetPopulation(p);
			const int32_t fake = pop + 500000;
			regionsync::rc641::SetPopulation(p, fake);
			int32_t* pR1 = regionsync::rc641::FindPopulationByType(p, 0x1010);
			const int32_t r1 = pR1 ? *pR1 : -1;
			if (pR1) *pR1 = r1 + 500000;
			int32_t* pR1x = regionsync::rc641::FindExtrapolatedPopulationByType(p, 0x1010);
			if (pR1x) *pR1x = *pR1x + 500000;
			logger.WriteLineFormatted(LogLevel::Info, "FakeNeighbor: extrapolated R$ node %s; GetExtrapolatedPopulation(0x1010) now %d",
				pR1x ? "updated" : "absent", p->GetExtrapolatedPopulation(0x1010));
			regionsync::rc641::SetCommercialJobs(p, regionsync::rc641::GetCommercialJobs(p) + 200000);
			regionsync::rc641::SetIndustrialJobs(p, regionsync::rc641::GetIndustrialJobs(p) + 200000);
			fakeNeighborHeld = true; fakeNeighborX = x; fakeNeighborZ = z;
			logger.WriteLineFormatted(LogLevel::Info, "FakeNeighbor: (%d,%d) '%s' pop %d -> %d, R$ %d -> %d, jobs +200000 each. Held until typed again.",
				x, z, GetRegionalCityName(p).c_str(), pop, fake, r1, pR1 ? *pR1 : -1);
			logger.Flush();
			if (pNeighborsSim) pNeighborsSim->MarkDealsDirty();
			ApplyRegionalDemandDelta("fake neighbor");
			return;
		}
		logger.WriteLine(LogLevel::Info, "FakeNeighbor: no other established tile.");
	}

	void Status()
	{
		int32_t x = -1, z = -1;
		if (pOwnRegionalCity)
		{
			pOwnRegionalCity->GetPosition(x, z);
		}

		char buffer[512];
		snprintf(
			buffer, sizeof(buffer),
			"RegionSync v%s\ninstance=%u tile=(%d,%d)\nenabled=%d publishes=%d pulls=%d\nstore=%s",
			PLUGIN_VERSION_STR, instanceId, x, z,
			settings.enabled ? 1 : 0, publishCount, pullCount,
			storeDirectory.string().c_str());

		SC4NotificationDialog::ShowDialog(cRZBaseString(buffer), cRZBaseString("RegionSync"));
	}

	void ProcessCheat(cIGZMessage2Standard* pStandardMsg)
	{
		const uint32_t cheatID = static_cast<uint32_t>(pStandardMsg->GetData1());

		switch (cheatID)
		{
		case kCheatDumpID:    Dump(); break;
		case kCheatPublishID: Publish("cheat"); break;
		case kCheatPullID:    Pull("cheat"); break;
		case kCheatReloadID:  ReloadAdjacent(); break;
		case kCheatStatusID:  Status(); break;
		case kCheatSaveID:    Save("cheat"); break;
		case kCheatUnlockID:  Unlock("cheat"); break;
		case kCheatSetterTestID: SetterTest(); break;
		case kCheatProbeID: Probe(); break;
		case kCheatDemandID: DemandDump(); break;
		case kCheatFakeNeighborID: FakeNeighbor(); break;
		}
	}

	// ---------------------------------------------------------------
	// State
	// ---------------------------------------------------------------

	regionsync::SyncSettings settings;

	cIGZCheatCodeManager* pCheatCodeManager;
	cISC4City* pCity;
	cISC4Region* pRegion;
	cISC4RegionalCity* pOwnRegionalCity;
	cISC4NeighborsSimulator* pNeighborsSim;
	cISC4Simulator* pSimulator;

	std::string regionDirectoryName;
	std::filesystem::path storeDirectory;

	int monthsSincePublish;
	int monthsSincePull;
	int monthsSinceSave;
	int publishCount;
	int pullCount;
	uint32_t instanceId;

	std::vector<RegionalDemandEntry> regionalDemand;
	bool fakeNeighborHeld = false;
	int32_t fakeNeighborX = -1;
	int32_t fakeNeighborZ = -1;
};

cRZCOMDllDirector* RZGetCOMDllDirector()
{
	static RegionSyncDllDirector sDirector;
	return &sDirector;
}
