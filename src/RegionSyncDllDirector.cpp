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

#include "cIGZCheatCodeManager.h"
#include "cIGZCOM.h"
#include "cIGZDate.h"
#include "cIGZFrameWork.h"
#include "cIGZMessage2Standard.h"
#include "cIGZMessageServer2.h"
#include "cISC4App.h"
#include "cISC4City.h"
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

static constexpr std::string_view kCheatDump = "RegionSyncDump";
static constexpr std::string_view kCheatPublish = "RegionSyncPublish";
static constexpr std::string_view kCheatPull = "RegionSyncPull";
static constexpr std::string_view kCheatReload = "RegionSyncReload";
static constexpr std::string_view kCheatStatus = "RegionSyncStatus";
static constexpr std::string_view kCheatSave = "RegionSyncSave";

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
			pos += key.size();
			const bool quoted = pos < cmd.size() && cmd[pos] == L'"';
			if (quoted) pos++;
			const size_t end = cmd.find(quoted ? L'"' : L' ', pos);
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

		if (settings.enabled && settings.publishOnLoad)
		{
			Publish("city load");
		}
		if (settings.enabled && settings.pullOnLoad)
		{
			Pull("city load");
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

		if (pCheatCodeManager)
		{
			pCheatCodeManager->UnregisterCheatCode(kCheatDumpID);
			pCheatCodeManager->UnregisterCheatCode(kCheatPublishID);
			pCheatCodeManager->UnregisterCheatCode(kCheatPullID);
			pCheatCodeManager->UnregisterCheatCode(kCheatReloadID);
			pCheatCodeManager->UnregisterCheatCode(kCheatStatusID);
			pCheatCodeManager->UnregisterCheatCode(kCheatSaveID);
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

			cISC4RegionalCity* pTarget = regionsync::FindRegionalCity(pRegion, snapshot.regionX, snapshot.regionZ);
			if (!pTarget)
			{
				logger.WriteLineFormatted(
					LogLevel::Debug,
					"Pull: no regional record for (%d,%d), skipping.",
					snapshot.regionX, snapshot.regionZ);
				continue;
			}

			const int changed = regionsync::ApplySnapshotToRegionalCity(snapshot, pTarget);
			applied++;
			totalChanged += changed;

			logger.WriteLineFormatted(
				LogLevel::Debug,
				"Pull: applied (%d,%d) '%s' changedFields=%d (snapshot pop=%d, record pop=%d)",
				snapshot.regionX, snapshot.regionZ,
				snapshot.cityName.c_str(),
				changed,
				snapshot.population,
				pTarget->GetPopulation());
		}

		if (totalChanged > 0 && pNeighborsSim)
		{
			// Ask the neighbors simulator to recompute deal state from the
			// updated records.
			pNeighborsSim->MarkDealsDirty();
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

		if (!pRegion || !pNeighborsSim)
		{
			logger.WriteLine(LogLevel::Error, "Reload: no region or neighbors simulator.");
			return;
		}

		SC4List<cISC4RegionalCity*> adjacent;
		pNeighborsSim->GetAdjacentCities(adjacent, true);

		int reloaded = 0;
		for (cISC4RegionalCity* pAdj : adjacent)
		{
			if (!pAdj)
			{
				continue;
			}

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
};

cRZCOMDllDirector* RZGetCOMDllDirector()
{
	static RegionSyncDllDirector sDirector;
	return &sDirector;
}
