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
 * Phase 2 slice 1 (docs/PLAN-PHASE2.md): snapshots carry connections and
 * deals; RegionSyncMirror (or SyncConnections/SyncDeals, on the SimNewDay
 * after a pull) creates the other instances' ones here.
 *
 * Built on the same skeleton as Null45's published DLL plugins so that the
 * conventions (logging, INI, version detection) match what the SC4 DLL
 * community already knows.
 */

#include "version.h"
#include "NeighborSnapshot.h"
#include "RegionalCityBridge.h"
#include "NeighborMirror.h"
#include "SyncSettings.h"
#include "InstanceMutex.h"
#include "RegionalCity641.h"
#include "NeighborsSim641.h"
#include "WriterIdentity.h"
#include "SaveLock.h"
#include "SaveHook641.h"
#include "InstanceStatus.h"
#include "TileLock.h"
#include "IdleService.h"

#include "cIGZCheatCodeManager.h"
#include "cIGZCOM.h"
#include "cIGZDate.h"
#include "cIGZFrameWork.h"
#include "cIGZMessage2Standard.h"
#include "cIGZMessageServer2.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC4Demand.h"
#include "cISC4NeighborConnection.h"
#include "cISC4NeighborDeal.h"
#include "cISC4DemandSimulator.h"
#include "SC4List.h"
#include "SC4Percentage.h"
#include "cISC4NeighborsSimulator.h"
#include "cISC4Region.h"
#include "cISC4RegionalCity.h"
#include "cISC4Simulator.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "cRZMessage2COMDirector.h"
#include "GZServPtrs.h"
#include "SC4String.h"
#include "SC4Vector.h"

#include "FileSystem.h"
#include "Logger.h"
#include "SC4NotificationDialog.h"
#include "SC4VersionDetection.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
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
static constexpr uint32_t kSC4MessageSimNewDay = 0x66956814;

// Neighbors simulator messages (docs/research/*-641.md). Connection ones
// carry data1=type, data2=x, data3=z; deal status carries data1=reason.
static constexpr uint32_t kMessageConnectionCreated = 0x8A0D15DA;
static constexpr uint32_t kMessageConnectionDeleted = 0xEA0D163E;
static constexpr uint32_t kMessageDealStatus = 0xEA418D8F;
// Posted after the record apply pass at load (connections-641.md §2).
static constexpr uint32_t kMessageConnectionsReady = 0x6AC284F3;

// Director ID. Random 32-bit value, must not collide with other plugins.
static constexpr uint32_t kRegionSyncDllDirector = 0x7A3C9E51;

// Idle service ID (Q17), random.
static constexpr uint32_t kIdleServiceID = 0x51CDBF59;
// Quiet period after a region-folder signal.
static constexpr ULONGLONG kWatchQuietMs = 750;

// Cheat IDs and strings.
static constexpr uint32_t kCheatDumpID = 0x7A3C9E52;
static constexpr uint32_t kCheatPublishID = 0x7A3C9E53;
static constexpr uint32_t kCheatPullID = 0x7A3C9E54;
static constexpr uint32_t kCheatReloadID = 0x7A3C9E55;
static constexpr uint32_t kCheatStatusID = 0x7A3C9E56;
static constexpr uint32_t kCheatSaveID = 0x7A3C9E57;
static constexpr uint32_t kCheatRedrawID = 0x6B2E91D7;
static constexpr uint32_t kCheatUnlockID = 0x7A3C9E58;
static constexpr uint32_t kCheatSetterTestID = 0x7A3C9E59;
static constexpr uint32_t kCheatProbeID = 0x7A3C9E5A;
static constexpr uint32_t kCheatDemandID = 0x7A3C9E5B;
static constexpr uint32_t kCheatFakeNeighborID = 0x7A3C9E5C;
// Outside the full 0x7A3C9E5x block. Drawn from /dev/urandom; checked against this repo only (Null45's published ids were not reachable offline).
static constexpr uint32_t kCheatMirrorID = 0x1FDCE6B8;
// Same: /dev/urandom, outside the 0x7A3C9E5x block, checked against this repo only.
static constexpr uint32_t kCheatOfferID = 0x343F73E3;
// Outside the 0x7A3C9E5x block; no hit in this repo or on GitHub code search (owner 0xC0000054).
static constexpr uint32_t kCheatHoldLockID = 0x4C9D27B1;

static constexpr std::string_view kCheatDump = "RegionSyncDump";
static constexpr std::string_view kCheatPublish = "RegionSyncPublish";
static constexpr std::string_view kCheatPull = "RegionSyncPull";
static constexpr std::string_view kCheatReload = "RegionSyncReload";
static constexpr std::string_view kCheatStatus = "RegionSyncStatus";
static constexpr std::string_view kCheatSave = "RegionSyncSave";
static constexpr std::string_view kCheatRedraw = "RegionSyncRedraw";
static constexpr std::string_view kCheatUnlock = "RegionSyncUnlock";
static constexpr std::string_view kCheatSetterTest = "RegionSyncSetterTest";
static constexpr std::string_view kCheatProbe = "RegionSyncProbe";
static constexpr std::string_view kCheatDemand = "RegionSyncDemand";
static constexpr std::string_view kCheatFakeNeighbor = "RegionSyncFakeNeighbor";
static constexpr std::string_view kCheatMirror = "RegionSyncMirror";
static constexpr std::string_view kCheatOffer = "RegionSyncOffer";
static constexpr std::string_view kCheatHoldLock = "RegionSyncHoldLock";

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

	// The -UserDir:"<dir>" argument as given; empty without one.
	std::wstring UserDirFromCommandLine()
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
			return cmd.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
		}
		return {};
	}

	// With Setup-MultiInstance.ps1 every instance shares one Plugins folder,
	// so a log next to the DLL would be fought over by two processes. When
	// the game was started with -UserDir:"<dir>" the log goes there instead.
	std::filesystem::path LogFilePath()
	{
		const std::wstring dir = UserDirFromCommandLine();
		return dir.empty() ? FileSystem::GetDllLogFilePath() : std::filesystem::path(dir) / L"SC4RegionSync.log";
	}

	std::string ToUtf8(const std::wstring& s)
	{
		const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
		if (n <= 0)
		{
			return {};
		}
		std::string out(static_cast<size_t>(n), '\0');
		WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
		return out;
	}

	// FILETIME (UTC) as local "YYYY-MM-DD HH:MM:SS".
	std::string FormatFileTime(uint64_t value)
	{
		FILETIME utc{ static_cast<DWORD>(value), static_cast<DWORD>(value >> 32) }, local{};
		SYSTEMTIME st{};
		if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st))
		{
			return "?";
		}
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		return buffer;
	}

	// Region and city names come from the game in the ANSI code page; the status file is UTF-8.
	std::string AcpToUtf8(const std::string& s)
	{
		return ToUtf8(regionsync::savelock::WidenAcp(s));
	}

	// Basename of an ANSI save path, UTF-8.
	std::string SaveFileBasename(const char* acpPath)
	{
		return acpPath ? ToUtf8(std::filesystem::path(regionsync::savelock::WidenAcp(acpPath)).filename().wstring()) : std::string();
	}

	bool IsStatusFile(const std::filesystem::path& p)
	{
		return p.extension() == ".status" || (p.extension() == ".tmp" && p.stem().extension() == ".status");
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

	// Change-notification handle, polled from the game thread (Q17).
	struct ChangeWatch
	{
		const char* who;
		HANDLE handle = INVALID_HANDLE_VALUE;
		std::filesystem::path path;

		bool IsOpen() const { return handle != INVALID_HANDLE_VALUE; }

		// Idempotent for the same path; another path closes the old handle first.
		void Open(const std::filesystem::path& dir, DWORD filter)
		{
			if (IsOpen() && dir == path)
			{
				return;
			}
			Close();
			handle = FindFirstChangeNotificationW(dir.c_str(), FALSE, filter);
			if (!IsOpen())
			{
				Logger::GetInstance().WriteLineFormatted(regionsync::savelock::kWarning, "%s: warning, cannot watch %s, error=%lu",
					who, dir.string().c_str(), GetLastError());
				return;
			}
			path = dir;
			Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "%s: opened %s", who, path.string().c_str());
		}

		void Close()
		{
			if (IsOpen())
			{
				FindCloseChangeNotification(handle);
				handle = INVALID_HANDLE_VALUE;
				Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "%s: closed %s", who, path.string().c_str());
			}
			path.clear();
		}

		// True once per signal; re-arms.
		bool Signalled()
		{
			if (!IsOpen() || WaitForSingleObject(handle, 0) != WAIT_OBJECT_0)
			{
				return false;
			}
			if (!FindNextChangeNotification(handle))
			{
				Logger::GetInstance().WriteLineFormatted(regionsync::savelock::kWarning, "%s: warning, FindNextChangeNotification failed, error=%lu; watch closed",
					who, GetLastError());
				Close();
			}
			return true;
		}
	};
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

	~RegionSyncDllDirector()
	{
		regionsync::savehook641::Uninstall(false);
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
		if (const std::wstring userDir = UserDirFromCommandLine(); !userDir.empty())
		{
			settings.LoadLocal(std::filesystem::path(userDir) / L"SC4RegionSync.local.ini");
		}
		regionsync::writer::SetHostAlias(settings.hostAlias);
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

		bool hookInstalled = false;
		if (pSC4App)
		{
			try
			{
				regionsync::savehook641::SetCallbacks(
					[this](const char* path, bool held) { OnSaving(path, held); },
					[this](bool ok, bool held) { OnSaved(ok, held); });
				hookInstalled = regionsync::savehook641::Install(pSC4App, settings.saveLock, LockTimeoutMs());
			}
			catch (const std::exception& e)
			{
				logger.WriteLineFormatted(LogLevel::Error, "SaveHook: install threw: %s", e.what());
			}
			catch (...)
			{
				logger.WriteLine(LogLevel::Error, "SaveHook: install threw a non-standard exception.");
			}
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
			kSC4MessageSimNewDay,
			kMessageConnectionCreated,
			kMessageConnectionDeleted,
			kMessageDealStatus,
			kMessageConnectionsReady,
		};

		for (uint32_t id : messages)
		{
			if (!pMS2->AddNotification(this, id))
			{
				logger.WriteLineFormatted(LogLevel::Error, "Failed to subscribe to message 0x%08X.", id);
				return false;
			}
		}

		if (settings.reloadOnPeerSave)
		{
			AddIdleService();
		}

		logger.WriteLineFormatted(
			LogLevel::Info,
			"Initialized. instanceId=%u enabled=%d publishEveryMonths=%d pullEveryMonths=%d autoSaveEveryMonths=%d syncConnections=%d syncDeals=%d layEdgeNetwork=%d syncRemovals=%d saveLock=%d saveLockTimeoutMs=%d hook=%d pullOnStoreChange=%d reloadOnPeerSave=%d%s",
			instanceId,
			settings.enabled ? 1 : 0,
			settings.publishEveryMonths,
			settings.pullEveryMonths,
			settings.autoSaveEveryMonths,
			settings.syncConnections ? 1 : 0,
			settings.syncDeals ? 1 : 0,
			settings.layEdgeNetwork ? 1 : 0,
			settings.syncRemovals ? 1 : 0,
			settings.saveLock ? 1 : 0,
			settings.saveLockTimeoutMs,
			hookInstalled ? 1 : 0,
			settings.pullOnStoreChange ? 1 : 0,
			settings.reloadOnPeerSave ? 1 : 0,
			settings.hostAlias.empty() ? "" : (" hostAlias=" + settings.hostAlias).c_str());

		ownWriter = regionsync::writer::Current();
		logger.WriteLineFormatted(LogLevel::Info, "Writer: host=%s pid=%u start=%llu checkWriterLiveness=%d",
			ownWriter.host.c_str(), ownWriter.pid, static_cast<unsigned long long>(ownWriter.start), settings.checkWriterLiveness ? 1 : 0);

		logger.Flush();

		if (settings.releaseInstanceMutexOnStart)
		{
			Unlock("startup");
		}

		// Last, so a present status file means the next exe can start.
		try
		{
			InitStatus();
		}
		catch (const std::exception& e)
		{
			logger.WriteLineFormatted(LogLevel::Error, "Status: init threw: %s", e.what());
		}
		catch (...)
		{
			logger.WriteLine(LogLevel::Error, "Status: init threw a non-standard exception.");
		}
		logger.Flush();

		return true;
	}

	bool PreAppShutdown()
	{
		// The Exit button sends no PostCityShutdown; release the hold here.
		try
		{
			RemoveIdleService();
			UnregisterReloadCheat();
			storeWatch.Close();
			regionWatch.Close();
			regionsync::savehook641::Uninstall(true);
			if (holdLock)
			{
				holdLock = false;
				Logger::GetInstance().WriteLine(LogLevel::Info, "HoldLock: released at app shutdown.");
			}
			regionsync::savelock::Close();
			regionsync::tilelock::Close();
			if (!statusPath.empty())
			{
				std::error_code ec;
				const bool removed = std::filesystem::remove(statusPath, ec);
				Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "Status: %s %s", statusPath.string().c_str(), removed ? "removed" : "not removed");
				statusPath.clear();   // no later SetStatus recreates it
			}
			Logger::GetInstance().Flush();
		}
		catch (...)
		{
		}
		return true;
	}

	bool DoMessage(cIGZMessage2* pMsg)
	{
		cIGZMessage2Standard* pStandardMsg = static_cast<cIGZMessage2Standard*>(pMsg);

		switch (pStandardMsg->GetType())
		{
		case kMessageCheatIssued:
			try
			{
				ProcessCheat(pStandardMsg);
			}
			catch (const std::exception& e)
			{
				Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "Cheat 0x%08X threw: %s", static_cast<uint32_t>(pStandardMsg->GetData1()), e.what());
			}
			catch (...)
			{
				Logger::GetInstance().WriteLine(LogLevel::Error, "Cheat threw a non-standard exception.");
			}
			break;
		case kSC4MessagePostRegionInit:
			try
			{
				PostRegionInit(true);
			}
			catch (const std::exception& e)
			{
				Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "PostRegionInit threw: %s", e.what());
			}
			catch (...)
			{
				Logger::GetInstance().WriteLine(LogLevel::Error, "PostRegionInit threw a non-standard exception.");
			}
			break;
		case kSC4MessagePreRegionShutdown:
			try
			{
				PreRegionShutdown();
			}
			catch (const std::exception& e)
			{
				Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "PreRegionShutdown threw: %s", e.what());
			}
			catch (...)
			{
				Logger::GetInstance().WriteLine(LogLevel::Error, "PreRegionShutdown threw a non-standard exception.");
			}
			break;
		case kSC4MessagePostCityInit:
			PostCityInit(pStandardMsg);
			break;
		case kSC4MessagePostCityInitComplete:
			try
			{
				PostCityInitComplete();
			}
			catch (const std::exception& e)
			{
				Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "PostCityInitComplete threw: %s", e.what());
			}
			catch (...)
			{
				Logger::GetInstance().WriteLine(LogLevel::Error, "PostCityInitComplete threw a non-standard exception.");
			}
			break;
		case kSC4MessagePostCityShutdown:
			try
			{
				PostCityShutdown();
			}
			catch (const std::exception& e)
			{
				Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "PostCityShutdown threw: %s", e.what());
			}
			catch (...)
			{
				Logger::GetInstance().WriteLine(LogLevel::Error, "PostCityShutdown threw a non-standard exception.");
			}
			break;
		case kSC4MessageSimNewMonth:
			OnNewMonth();
			break;
		case kSC4MessageSimNewDay:
			try
			{
				OnNewDay();
			}
			catch (const std::exception& e)
			{
				Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "SimNewDay threw: %s", e.what());
			}
			catch (...)
			{
				Logger::GetInstance().WriteLine(LogLevel::Error, "SimNewDay threw a non-standard exception.");
			}
			break;
		case kMessageConnectionsReady:
			if (!connectionsReady)
			{
				connectionsReady = true;
				publishDirty = true;
				Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "Event 0x%08X (connections ready) first this session; lists published as complete from now on.", kMessageConnectionsReady);
			}
			break;
		case kMessageConnectionCreated:
		case kMessageConnectionDeleted:
		case kMessageDealStatus:
			try
			{
				OnNeighborEvent(pStandardMsg);
			}
			catch (const std::exception& e)
			{
				Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "Message 0x%08X threw: %s", pStandardMsg->GetType(), e.what());
			}
			catch (...)
			{
				Logger::GetInstance().WriteLine(LogLevel::Error, "Neighbor message threw a non-standard exception.");
			}
			break;
		}

		// The logger buffers; flush so the file is live and survives a crash.
		Logger::GetInstance().Flush();
		return true;
	}

private:
	using TileKey = std::pair<int32_t, int32_t>;

	// A tile's save file as the region watch last saw it.
	struct TileProbe
	{
		uintmax_t size = 0;
		std::filesystem::file_time_type time{};
		bool operator==(const TileProbe&) const = default;
	};
	using TileProbes = std::map<TileKey, TileProbe>;

	// ---------------------------------------------------------------
	// Lifecycle
	// ---------------------------------------------------------------

	// fromMessage: false for the late PostCityInitComplete call.
	void PostRegionInit(bool fromMessage)
	{
		cISC4AppPtr pSC4App;
		pRegion = pSC4App ? pSC4App->GetRegion() : nullptr;

		if (pRegion)
		{
			SC4String* pDirName = pRegion->GetDirectoryName();
			regionDirectoryName = pDirName ? pDirName->ToChar() : "";
			// "_" names are reserved for _instances and _saves: no store for such a region.
			if (!regionDirectoryName.empty() && regionDirectoryName[0] == '_')
			{
				storeDirectory.clear();
				Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "Region '%s': name starts with '_', no store; publish, pull and save count are off.",
					regionDirectoryName.c_str());
			}
			else
			{
				storeDirectory = settings.ResolveStoreDirectory(regionDirectoryName);
			}

			Logger::GetInstance().WriteLineFormatted(
				LogLevel::Info,
				"Region loaded: '%s'. Store directory: %s",
				regionDirectoryName.c_str(),
				storeDirectory.string().c_str());

			if (settings.saveLock)
			{
				regionsync::savelock::Open(regionDirectoryName);
			}

			status.region = AcpToUtf8(regionDirectoryName);
			SetStatus("region", "region init");

			if (settings.enabled && settings.pullOnStoreChange && !storeDirectory.empty())
			{
				std::error_code ec;
				std::filesystem::create_directories(storeDirectory, ec);
				storeWatch.Open(storeDirectory, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
			}

			if (settings.reloadOnPeerSave)
			{
				OpenRegionWatch(pSC4App);
			}
		}

		RegisterReloadCheat();

		if (fromMessage)
		{
			regionViewReady = true;
			SeedRegionSeen();
			Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "RegionWatch: PostRegionInit message, regionViewReady=1 watch=%d",
				regionWatch.IsOpen() ? 1 : 0);
		}
	}

	void PreRegionShutdown()
	{
		regionViewReady = false;
		regionPending = false;
		storeWatch.Close();
		regionWatch.Close();
		storeSeen.clear();
		UnregisterReloadCheat();
		holdLock = false;
		regionsync::savelock::Close();
		pRegion = nullptr;
		regionDirectoryName.clear();
		SetStatus("starting", "region shutdown");
	}

	// Per-city-session state; cleared on city init and shutdown.
	void ResetSessionState()
	{
		mirror.Reset();
		mirrorPending.clear();
		publishDirty = false;
		connectionsReady = false;
		loadedSaveFile.clear();
		pendingSave.reset();
	}

	void PostCityInit(cIGZMessage2Standard* pStandardMsg)
	{
		pCity = static_cast<cISC4City*>(pStandardMsg->GetVoid1());
		ResetSessionState();
		FILETIME now{};
		GetSystemTimeAsFileTime(&now);
		cityLoadedAt = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
		storeSeen.clear();
		regionViewReady = false;
		regionPending = false;

		if (pCheatCodeManager)
		{
			pCheatCodeManager->RegisterCheatCode(kCheatDumpID, cRZBaseString(kCheatDump));
			pCheatCodeManager->RegisterCheatCode(kCheatPublishID, cRZBaseString(kCheatPublish));
			pCheatCodeManager->RegisterCheatCode(kCheatPullID, cRZBaseString(kCheatPull));
			pCheatCodeManager->RegisterCheatCode(kCheatStatusID, cRZBaseString(kCheatStatus));
			pCheatCodeManager->RegisterCheatCode(kCheatSaveID, cRZBaseString(kCheatSave));
			pCheatCodeManager->RegisterCheatCode(kCheatUnlockID, cRZBaseString(kCheatUnlock));
			pCheatCodeManager->RegisterCheatCode(kCheatSetterTestID, cRZBaseString(kCheatSetterTest));
			pCheatCodeManager->RegisterCheatCode(kCheatProbeID, cRZBaseString(kCheatProbe));
			pCheatCodeManager->RegisterCheatCode(kCheatDemandID, cRZBaseString(kCheatDemand));
			pCheatCodeManager->RegisterCheatCode(kCheatFakeNeighborID, cRZBaseString(kCheatFakeNeighbor));
			pCheatCodeManager->RegisterCheatCode(kCheatMirrorID, cRZBaseString(kCheatMirror));
			pCheatCodeManager->RegisterCheatCode(kCheatOfferID, cRZBaseString(kCheatOffer));
			pCheatCodeManager->RegisterCheatCode(kCheatHoldLockID, cRZBaseString(kCheatHoldLock));
		}
		RegisterReloadCheat();
	}

	// RegionSyncReload and RegionSyncRedraw both run in the region view, so they
	// and the cheat notification outlive the city cheats.
	void RegisterReloadCheat()
	{
		if (!pCheatCodeManager || reloadCheat)
		{
			return;
		}
		pCheatCodeManager->RegisterCheatCode(kCheatReloadID, cRZBaseString(kCheatReload));
		pCheatCodeManager->RegisterCheatCode(kCheatRedrawID, cRZBaseString(kCheatRedraw));
		pCheatCodeManager->AddNotification2(this, 0);
		reloadCheat = true;
	}

	void UnregisterReloadCheat()
	{
		if (!pCheatCodeManager || !reloadCheat)
		{
			return;
		}
		pCheatCodeManager->UnregisterCheatCode(kCheatReloadID);
		pCheatCodeManager->UnregisterCheatCode(kCheatRedrawID);
		pCheatCodeManager->RemoveNotification2(this, 0);
		reloadCheat = false;
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
			PostRegionInit(false);
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

		// Tile guard first, then status, then the load publish (which it may refuse).
		const std::string cityName = GetRegionalCityName(pOwnRegionalCity);
		if (conflict && guardCity == pCity && guardX == x && guardZ == z)
		{
			logger.WriteLineFormatted(LogLevel::Info, "TileLock: city load again for (%d,%d), conflict kept, Open skipped.", x, z);
		}
		else if (!pOwnRegionalCity || regionDirectoryName.empty())
		{
			regionsync::tilelock::Close();
			conflict = false;
			logger.WriteLine(LogLevel::Info, "TileLock: guard skipped (no regional city or region name).");
		}
		else
		{
			conflict = regionsync::tilelock::Open(regionDirectoryName, x, z);
			conflictHolderGone = false;
			guardCity = pCity;
			guardX = x;
			guardZ = z;
			if (conflict)
			{
				LogConflict(x, z, cityName);
			}
		}
		if (pOwnRegionalCity)
		{
			cRZBaseString savePath;
			pOwnRegionalCity->GetCitySaveFilePath(savePath);
			loadedSaveFile = SaveFileBasename(savePath.ToChar());
			logger.WriteLineFormatted(LogLevel::Info, "SaveCount: (%d,%d) loaded from '%s'", x, z, loadedSaveFile.c_str());
		}
		status.regionX = x;
		status.regionZ = z;
		status.cityName = AcpToUtf8(cityName);
		status.conflict = conflict;
		SetStatus("city", "city load");

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
	// cSC4DemandSimulator folds the region's extrapolated per-type
	// population into GetDemand(group, 0x10000) for each group whose
	// index-0 object has region use 1 (AddToSupplyValue) or 2
	// (AddToDemandValue), at Init and on connection messages only
	// (0x6a5cb0 in 1.1.641, docs/research/population-641.md). Records a
	// pull changes are never re-read, so each pull feeds the change in the
	// regional sum, measured before and after its own writes, into that
	// target. No baseline is carried: the game's own extrapolation and
	// re-sums between pulls already landed in the target.
	// ---------------------------------------------------------------

	static constexpr uint32_t kRegionalTargetIndex = 0x10000;

	struct RegionalDemandEntry
	{
		uint32_t groupId;
		uint32_t regionUse;
		cISC4Demand* pLocal;   // index 0: what the record's +0x3c is written from
		cISC4Demand* pTarget;  // index 0x10000: where the game folds regional population; may be null
	};

	// Per-entry sums of GetExtrapolatedPopulation, index-aligned with regionalDemand.
	using RegionalSums = std::vector<float>;

	void AddTile(RegionalSums& sums, cISC4RegionalCity* p) const
	{
		for (size_t i = 0; i < regionalDemand.size(); i++)
		{
			sums[i] += static_cast<float>(p->GetExtrapolatedPopulation(regionalDemand[i].groupId));
		}
	}

	// Every established tile but ours.
	RegionalSums SumOtherTiles() const
	{
		RegionalSums sums(regionalDemand.size(), 0.0f);
		if (!pRegion || !pOwnRegionalCity) return sums;
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
			if (p && p->GetEstablished()) AddTile(sums, p);
		}
		return sums;
	}

	// The set the game's fold uses: transitive closure over connections,
	// established only, self excluded. List elements are AddRef'd by the
	// game and released by the SC4List destructor.
	RegionalSums SumConnectedTiles() const
	{
		RegionalSums sums(regionalDemand.size(), 0.0f);
		if (!pNeighborsSim) return sums;
		SC4List<cISC4RegionalCity*> list;
		pNeighborsSim->GetAllConnectedCities(list, false);
		for (cISC4RegionalCity* p : list)
		{
			if (p) AddTile(sums, p);
		}
		return sums;
	}

	RegionalSums SumRegional() const
	{
		return settings.regionalSumConnected ? SumConnectedTiles() : SumOtherTiles();
	}

	void LogConnectedCities(const char* who) const
	{
		Logger& logger = Logger::GetInstance();
		if (!pNeighborsSim)
		{
			logger.WriteLineFormatted(LogLevel::Info, "%s: connected cities: no neighbors simulator.", who);
			return;
		}
		try
		{
			SC4List<cISC4RegionalCity*> list;
			pNeighborsSim->GetAllConnectedCities(list, false);
			std::string positions;
			size_t count = 0;
			for (cISC4RegionalCity* p : list)
			{
				count++;
				char buf[48];
				if (p)
				{
					int32_t x = -1, z = -1;
					p->GetPosition(x, z);
					snprintf(buf, sizeof(buf), " (%d,%d)", x, z);
				}
				else
				{
					snprintf(buf, sizeof(buf), " null");
				}
				positions += buf;
			}
			logger.WriteLineFormatted(LogLevel::Info, "%s: connected cities (GetAllConnectedCities) count=%zu:%s", who, count, positions.c_str());
		}
		catch (const std::exception& e)
		{
			logger.WriteLineFormatted(LogLevel::Error, "%s: GetAllConnectedCities threw: %s", who, e.what());
		}
		catch (...)
		{
			logger.WriteLineFormatted(LogLevel::Error, "%s: GetAllConnectedCities threw a non-standard exception.", who);
		}
		logger.Flush();
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
		for (uint32_t groupId : regionsync::kRCIGroupIds)
		{
			cISC4Demand* pLocal = pDemandSim->GetDemand(groupId, 0);
			if (!pLocal) continue;
			const uint32_t use = pLocal->GetRegionUse();
			if (use == 1 || use == 2)
			{
				regionalDemand.push_back(RegionalDemandEntry{ groupId, use, pLocal, pDemandSim->GetDemand(groupId, kRegionalTargetIndex) });
			}
		}

		const RegionalSums all = SumOtherTiles();
		RegionalSums connected(regionalDemand.size(), 0.0f);
		// GetAllConnectedCities has not run in the game yet; at load it is
		// only called when the setting asks for it. RegionSyncDemand always
		// calls it, which is the experiment.
		if (settings.regionalSumConnected)
		{
			LogConnectedCities("RegionalDemand");
			connected = SumConnectedTiles();
		}
		else
		{
			logger.WriteLine(LogLevel::Info, "RegionalDemand: sum set=all; connected set not queried at load (RegionSyncDemand logs it).");
		}
		// Load check: target supply should equal sumConnected (the game's Init sets it absolutely).
		for (size_t i = 0; i < regionalDemand.size(); i++)
		{
			const RegionalDemandEntry& e = regionalDemand[i];
			char target[32];
			if (e.pTarget) snprintf(target, sizeof(target), "%.1f", e.pTarget->QuerySupplyValue());
			else snprintf(target, sizeof(target), "absent");
			logger.WriteLineFormatted(LogLevel::Info, "RegionalDemand: 0x%04X use=%u local supply=%.1f target supply=%s sumConnected=%.0f sumAll=%.0f",
				e.groupId, e.regionUse, e.pLocal->QuerySupplyValue(), target, connected[i], all[i]);
		}
		logger.WriteLineFormatted(LogLevel::Info, "RegionalDemand: %zu region-using groups, sum set=%s.",
			regionalDemand.size(), settings.regionalSumConnected ? "connected" : "all");
		logger.Flush();
	}

	void FeedRegionalDelta(const char* reason, const RegionalSums& before, const RegionalSums& after)
	{
		Logger& logger = Logger::GetInstance();
		for (size_t i = 0; i < regionalDemand.size() && i < before.size() && i < after.size(); i++)
		{
			const RegionalDemandEntry& e = regionalDemand[i];
			const float delta = after[i] - before[i];
			if (!e.pTarget || std::fabs(delta) < 0.5f) continue;
			logger.WriteLineFormatted(LogLevel::Info, "RegionalDemand (%s): 0x%04X regional %.0f -> %.0f, %s(%.0f); target before demand=%.1f supply=%.1f",
				reason, e.groupId, before[i], after[i],
				e.regionUse == 1 ? "AddToSupplyValue" : "AddToDemandValue", delta,
				e.pTarget->QueryDemandValue(), e.pTarget->QuerySupplyValue());
			logger.Flush();
			if (e.regionUse == 1) e.pTarget->AddToSupplyValue(delta);
			else e.pTarget->AddToDemandValue(delta);
			logger.WriteLineFormatted(LogLevel::Info, "RegionalDemand (%s): 0x%04X target after demand=%.1f supply=%.1f",
				reason, e.groupId, e.pTarget->QueryDemandValue(), e.pTarget->QuerySupplyValue());
			logger.Flush();
		}
	}

	bool FeedsRegionalDemand() const { return settings.applyRegionalDemand && !regionalDemand.empty(); }

	void PostCityShutdown()
	{
		// In conflict the snapshot belongs to the first opener.
		if (settings.enabled && pCity && pOwnRegionalCity && !ConflictRefuses("Shutdown", "snapshot"))
		{
			if (settings.publishOnShutdown)
			{
				Publish("city shutdown", false);
			}
			else
			{
				// A snapshot that outlives its writer is stale by definition:
				// the next instance to load this region would pull it over
				// the saved record (observed 2026-10-17, Q2 notes).
				int32_t x = 0, z = 0;
				pOwnRegionalCity->GetPosition(x, z);
				std::error_code ec;
				const std::filesystem::path path = storeDirectory / regionsync::SnapshotFileName(x, z);
				const bool removed = !storeDirectory.empty() && std::filesystem::remove(path, ec);
				Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "Shutdown: snapshot %s %s", path.string().c_str(), removed ? "removed" : "not present");
			}
		}

		if (holdLock)
		{
			holdLock = false;
			regionsync::savelock::Release("hold");
			Logger::GetInstance().WriteLine(LogLevel::Info, "HoldLock: released at city shutdown.");
		}

		regionsync::tilelock::Close();
		conflict = false;
		conflictHolderGone = false;
		guardCity = nullptr;
		SetStatus("region", "city shutdown");

		pCity = nullptr;
		pOwnRegionalCity = nullptr;
		pNeighborsSim = nullptr;
		pSimulator = nullptr;
		regionalDemand.clear();
		ResetSessionState();

		// RegionSyncReload stays registered for the region view.
		if (pCheatCodeManager)
		{
			pCheatCodeManager->UnregisterCheatCode(kCheatDumpID);
			pCheatCodeManager->UnregisterCheatCode(kCheatPublishID);
			pCheatCodeManager->UnregisterCheatCode(kCheatPullID);
			pCheatCodeManager->UnregisterCheatCode(kCheatStatusID);
			pCheatCodeManager->UnregisterCheatCode(kCheatSaveID);
			pCheatCodeManager->UnregisterCheatCode(kCheatUnlockID);
			pCheatCodeManager->UnregisterCheatCode(kCheatSetterTestID);
			pCheatCodeManager->UnregisterCheatCode(kCheatProbeID);
			pCheatCodeManager->UnregisterCheatCode(kCheatDemandID);
			pCheatCodeManager->UnregisterCheatCode(kCheatFakeNeighborID);
			pCheatCodeManager->UnregisterCheatCode(kCheatMirrorID);
			pCheatCodeManager->UnregisterCheatCode(kCheatOfferID);
			pCheatCodeManager->UnregisterCheatCode(kCheatHoldLockID);
		}

		regionViewReady = false;
		regionPending = false;
		Logger::GetInstance().WriteLine(LogLevel::Info, "RegionWatch: PostCityShutdown, regionViewReady=0 until a PostRegionInit message.");
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

	// Connection and deal messages only mark state; the work happens on the
	// next SimNewDay, outside whatever game call posted or sent them.
	void OnNeighborEvent(cIGZMessage2Standard* pMsg)
	{
		const uint32_t type = pMsg->GetType();
		const intptr_t d1 = pMsg->GetData1();
		bool dirty = true;
		if (type == kMessageDealStatus)
		{
			// Reasons 1..7 change a deal's existence or state; 0 (amount changed) and 8 (retry) do not in slice 1.
			dirty = d1 >= 1 && d1 <= 7;
		}
		publishDirty = publishDirty || dirty;
		const bool amountOnly = type == kMessageDealStatus && d1 == 0;
		Logger::GetInstance().WriteLineFormatted(amountOnly ? LogLevel::Debug : LogLevel::Info, "Event 0x%08X data1=%d data2=%d data3=%d dirty=%d",
			type, static_cast<int32_t>(d1), static_cast<int32_t>(pMsg->GetData2()), static_cast<int32_t>(pMsg->GetData3()), dirty ? 1 : 0);
	}

	void OnNewDay()
	{
		Logger& logger = Logger::GetInstance();
		if (!sawNewDay)
		{
			sawNewDay = true;
			logger.WriteLineFormatted(LogLevel::Info, "SimNewDay 0x%08X received (first this session).", kSC4MessageSimNewDay);
		}

		if (settings.enabled && settings.pullOnStoreChange && !inPull && pCity && pOwnRegionalCity)
		{
			try
			{
				if (storeWatch.Signalled())
				{
					if (PeerSnapshotChanged())
					{
						Pull("store change");
					}
					else
					{
						logger.WriteLine(LogLevel::Debug, "StoreWatch: signal, no peer change");
					}
				}
			}
			catch (const std::exception& e)
			{
				logger.WriteLineFormatted(LogLevel::Error, "StoreWatch: threw: %s", e.what());
			}
			catch (...)
			{
				logger.WriteLine(LogLevel::Error, "StoreWatch: threw a non-standard exception.");
			}
		}

		if (publishDirty && !inPull && settings.enabled && pCity)
		{
			Publish("event");
		}

		// After the publish check, so events the mirror raises publish tomorrow.
		if (!mirrorPending.empty() && !inPull)
		{
			InPullScope scope(inPull);
			std::vector<regionsync::NeighborSnapshot> pending;
			pending.swap(mirrorPending);
			for (const regionsync::NeighborSnapshot& snapshot : pending)
			{
				MirrorSnapshot(snapshot, false);
			}
		}
	}

	// ---------------------------------------------------------------
	// Change notification (Q17, docs/PLAN-WATCH.md)
	// ---------------------------------------------------------------

	// Peer snapshot file name -> last write time. Our own tile is left out.
	std::map<std::string, std::filesystem::file_time_type> ScanPeerSnapshots() const
	{
		std::map<std::string, std::filesystem::file_time_type> times;
		if (!pOwnRegionalCity)
		{
			return times;
		}
		int32_t x = -1, z = -1;
		pOwnRegionalCity->GetPosition(x, z);
		const std::string own = regionsync::SnapshotFileName(x, z);
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(storeDirectory, ec))
		{
			const std::string name = entry.path().filename().string();
			std::error_code fileEc;
			if (!entry.is_regular_file(fileEc) || entry.path().extension() != ".snapshot" || name == own)
			{
				continue;
			}
			std::error_code timeEc;
			const auto time = entry.last_write_time(timeEc);
			if (!timeEc)
			{
				times[name] = time;
			}
		}
		return times;
	}

	bool PeerSnapshotChanged() const
	{
		for (const auto& [name, time] : ScanPeerSnapshots())
		{
			const auto seen = storeSeen.find(name);
			if (seen == storeSeen.end() || seen->second != time)
			{
				return true;
			}
		}
		return false;
	}

	void AddIdleService()
	{
		Logger& logger = Logger::GetInstance();
		try
		{
			cIGZFrameWork* pFramework = RZGetFrameWork();
			if (!pFramework)
			{
				logger.WriteLine(LogLevel::Error, "IdleService: no framework.");
				return;
			}
			idleService.SetCallback([this] { OnIdle(); });
			idleAdded = pFramework->AddSystemService(&idleService);
			onIdleAdded = idleAdded && pFramework->AddToOnIdle(&idleService);
			logger.WriteLineFormatted(LogLevel::Info, "IdleService: added=%d onIdle=%d id=0x%08X",
				idleAdded ? 1 : 0, onIdleAdded ? 1 : 0, kIdleServiceID);
		}
		catch (const std::exception& e)
		{
			logger.WriteLineFormatted(LogLevel::Error, "IdleService: add threw: %s", e.what());
		}
		catch (...)
		{
			logger.WriteLine(LogLevel::Error, "IdleService: add threw a non-standard exception.");
		}
	}

	void RemoveIdleService()
	{
		idleService.SetCallback(nullptr);
		if (!idleAdded)
		{
			return;
		}
		cIGZFrameWork* pFramework = RZGetFrameWork();
		const bool fromIdle = onIdleAdded && pFramework && pFramework->RemoveFromOnIdle(&idleService);
		const bool removed = pFramework && pFramework->RemoveSystemService(&idleService);
		onIdleAdded = false;
		idleAdded = false;
		Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "IdleService: removed onIdle=%d service=%d", fromIdle ? 1 : 0, removed ? 1 : 0);
	}

	// Watch the region's save folder (Q17 slice B).
	void OpenRegionWatch(cISC4App* pSC4App)
	{
		cRZBaseString regionsDir;
		const bool ok = pSC4App->GetRegionsDirectory(regionsDir);
		regionFolder = std::filesystem::path(regionsync::savelock::WidenAcp(regionsDir.ToChar()))
			/ regionsync::savelock::WidenAcp(regionDirectoryName);
		std::error_code ec;
		const bool regionIni = std::filesystem::exists(regionFolder / "region.ini", ec);
		Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "RegionsDirectory: ok=%d '%s' folder '%s' regionIni=%d",
			ok ? 1 : 0, regionsDir.ToChar(), regionFolder.string().c_str(), regionIni ? 1 : 0);
		if (ok)
		{
			regionWatch.Open(regionFolder, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
		}
	}

	// Save-file size and mtime per established tile.
	TileProbes ProbeTiles()
	{
		Logger& logger = Logger::GetInstance();
		TileProbes probes;
		for (const TileKey& key : EstablishedTiles(pRegion))
		{
			cISC4RegionalCity* p = regionsync::FindRegionalCity(pRegion, key.first, key.second);
			if (!p)
			{
				continue;
			}
			cRZBaseString savePath;
			p->GetCitySaveFilePath(savePath);
			const bool firstTime = regionPathLogged.insert(key).second;
			if (savePath.Strlen() == 0)
			{
				if (firstTime)
				{
					logger.WriteLineFormatted(LogLevel::Info, "RegionWatch: (%d,%d) has no save path, skipped.", key.first, key.second);
				}
				continue;
			}
			std::filesystem::path file(regionsync::savelock::WidenAcp(savePath.ToChar()));
			if (file.is_relative())
			{
				file = regionFolder / file;
			}
			if (firstTime)
			{
				logger.WriteLineFormatted(LogLevel::Info, "RegionWatch: (%d,%d) save path '%s' -> %s",
					key.first, key.second, savePath.ToChar(), file.string().c_str());
			}
			std::error_code sizeEc, timeEc;
			const uintmax_t size = std::filesystem::file_size(file, sizeEc);
			const auto time = std::filesystem::last_write_time(file, timeEc);
			if (!sizeEc && !timeEc)
			{
				probes[key] = TileProbe{ size, time };
			}
		}
		return probes;
	}

	// Region view entered: what is on disk now is what the game just loaded.
	void SeedRegionSeen()
	{
		regionPending = false;
		regionBusyLogged = false;
		regionLastProbe.clear();
		if (!regionWatch.IsOpen() || !pRegion)
		{
			regionSeen.clear();
			return;
		}
		regionWatch.Signalled();
		regionSeen = ProbeTiles();
		Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "RegionWatch: seeded %zu tile(s).", regionSeen.size());
	}

	// Reload tiles a peer saved while we show the region view (Q17 step 4).
	void OnIdle()
	{
		if (inIdle)
		{
			return;
		}
		InPullScope idleScope(inIdle);

		if (regionsync::savehook641::InSave() || !regionViewReady || !pRegion)
		{
			return;
		}
		cISC4AppPtr pSC4App;
		if (!pSC4App || pSC4App->GetRegion() != pRegion || pSC4App->GetCity())
		{
			return;
		}

		const ULONGLONG now = GetTickCount64();
		if (regionWatch.Signalled())
		{
			regionSignalTick = now;
			regionPending = true;
		}
		if (!regionPending || now - regionSignalTick < kWatchQuietMs)
		{
			return;
		}

		Logger& logger = Logger::GetInstance();
		if (settings.saveLock && !regionsync::savelock::IsOpen())
		{
			if (!regionNoMutexLogged)
			{
				regionNoMutexLogged = true;
				logger.WriteLine(LogLevel::Info, "RegionWatch: no save mutex, region reload disabled");
				logger.Flush();
			}
			regionPending = false;
			return;
		}

		// Due: changed since seen, equal to a probe at least kWatchQuietMs old, non-empty.
		const TileProbes probes = ProbeTiles();
		const bool lastIsOld = now - regionLastProbeTick >= kWatchQuietMs;
		std::vector<TileKey> due;
		size_t changed = 0;
		for (const auto& [key, probe] : probes)
		{
			const auto seen = regionSeen.find(key);
			if (seen != regionSeen.end() && seen->second == probe)
			{
				continue;
			}
			changed++;
			const auto last = regionLastProbe.find(key);
			if (probe.size > 0 && lastIsOld && last != regionLastProbe.end() && last->second == probe)
			{
				due.push_back(key);
			}
		}
		regionLastProbe = probes;
		regionLastProbeTick = now;
		if (changed == 0)
		{
			regionPending = false;
			regionBusyLogged = false;
			return;
		}
		if (due.empty())
		{
			regionSignalTick = now;
			return;
		}

		if (!regionBusyLogged)
		{
			logger.WriteLineFormatted(LogLevel::Info, "RegionWatch: %zu tile(s) due", due.size());
			logger.Flush();
		}
		const int reloaded = ReloadTiles(pRegion, due, false);
		if (reloaded < 0)
		{
			if (!regionBusyLogged)
			{
				regionBusyLogged = true;
				logger.WriteLineFormatted(LogLevel::Info, "RegionWatch: busy, %zu tile(s) due, retrying on idle.", due.size());
				logger.Flush();
			}
			return;
		}
		for (const TileKey& key : due)
		{
			regionSeen[key] = probes.at(key);
		}
		regionPending = due.size() < changed;
		regionBusyLogged = false;
		logger.WriteLineFormatted(LogLevel::Info, "RegionWatch: reloaded %d of %zu (recordPtrChanged)", reloaded, due.size());
		logger.Flush();
	}

	// ---------------------------------------------------------------
	// Instance status file and same-tile guard (docs/PLAN-LAUNCHER.md)
	// ---------------------------------------------------------------

	void InitStatus()
	{
		statusDirectory = settings.ResolveStoreDirectory("") / "_instances";
		ReapDeadStatusFiles();

		std::string userDir = ToUtf8(UserDirFromCommandLine());
		while (!userDir.empty() && (userDir.back() == '\\' || userDir.back() == '/'))
		{
			userDir.pop_back();
		}
		status.writer = ownWriter;
		status.userDir = userDir;
		statusPath = statusDirectory / regionsync::InstanceStatusFileName(ownWriter.pid, ownWriter.start);
		SetStatus("starting", "startup");
	}

	// Deletes *.status and *.status.tmp that pass IsReapable and whose writer is Dead.
	void ReapDeadStatusFiles()
	{
		Logger& logger = Logger::GetInstance();
		std::vector<std::filesystem::path> dead;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(statusDirectory, ec))
		{
			if (!IsStatusFile(entry.path()))
			{
				continue;
			}
			regionsync::InstanceStatus s;
			std::string error;
			if (regionsync::ReadInstanceStatus(entry.path(), s, error) && regionsync::IsReapable(entry.path().filename().string(), s, ownWriter.host)
				&& SafeIsAlive(s.writer) == regionsync::writer::Liveness::Dead)
			{
				dead.push_back(entry.path());
			}
		}
		for (const std::filesystem::path& path : dead)
		{
			std::error_code removeEc;
			const bool removed = std::filesystem::remove(path, removeEc);
			logger.WriteLineFormatted(LogLevel::Info, "Status: dead instance file %s %s", path.string().c_str(), removed ? "removed" : "not removed");
		}
	}

	// Leaving city clears the city fields; leaving the region clears the region too.
	void SetStatus(const char* state, const char* event)
	{
		status.state = state;
		if (status.state != "city")
		{
			status.regionX = -1;
			status.regionZ = -1;
			status.cityName.clear();
			status.conflict = false;
		}
		if (status.state == "starting")
		{
			status.region.clear();
		}
		WriteStatus(event);
	}

	// Never throws. Error once per failure episode, Debug until a write succeeds.
	void WriteStatus(const char* event)
	{
		if (statusPath.empty())
		{
			return;
		}
		try
		{
			Logger& logger = Logger::GetInstance();
			status.writtenUnixTime = static_cast<int64_t>(std::time(nullptr));
			std::string error;
			if (regionsync::WriteInstanceStatus(statusPath, status, error))
			{
				statusWriteFailed = false;
				logger.WriteLineFormatted(LogLevel::Info, "Status (%s): state=%s region='%s' conflict=%d -> %s",
					event, status.state.c_str(), status.region.c_str(), status.conflict ? 1 : 0, statusPath.string().c_str());
				return;
			}
			logger.WriteLineFormatted(statusWriteFailed ? LogLevel::Debug : LogLevel::Error, "Status (%s) not written, running without it: %s", event, error.c_str());
			statusWriteFailed = true;
		}
		catch (...)
		{
			statusWriteFailed = true;
		}
	}

	// Another live instance whose status shows this tile open; unknown when none.
	regionsync::WriterIdentity FindTileHolder(int32_t x, int32_t z) const
	{
		const std::string region = AcpToUtf8(regionDirectoryName);
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(statusDirectory, ec))
		{
			if (entry.path().extension() != ".status")
			{
				continue;
			}
			regionsync::InstanceStatus s;
			std::string error;
			if (regionsync::ReadInstanceStatus(entry.path(), s, error) && s.writer.Known() && !regionsync::SameWriter(s.writer, ownWriter)
				&& s.state == "city" && !s.conflict && s.region == region && s.regionX == x && s.regionZ == z
				&& SafeIsAlive(s.writer) != regionsync::writer::Liveness::Dead)
			{
				return s.writer;
			}
		}
		return {};
	}

	// ---------------------------------------------------------------
	// Save counter (docs/PLAN-LAN.md P2). Runs inside the save hook.
	// ---------------------------------------------------------------

	void OnSaving(const char* path, bool held)
	{
		Logger& logger = Logger::GetInstance();
		pendingSave.reset();
		try
		{
			if (!pOwnRegionalCity || regionDirectoryName.empty() || storeDirectory.empty())
			{
				logger.WriteLine(LogLevel::Info, "SaveCount: no city, region or store, not counted.");
				return;
			}
			PendingSave p;
			pOwnRegionalCity->GetPosition(p.x, p.z);
			p.path = settings.ResolveStoreDirectory("") / "_saves" / regionDirectoryName / regionsync::SaveCounterFileName(p.x, p.z);

			regionsync::SaveCounter previous;
			std::string error;
			if (!regionsync::ReadSaveCounter(p.path, previous, error))
			{
				SaveCountFailed(p.x, p.z, error);
				return;
			}

			regionsync::SaveCounter& c = p.counter;
			c.count = previous.count + 1;
			c.file = SaveFileBasename(path);
			c.prevFile = loadedSaveFile != c.file ? loadedSaveFile : std::string();
			c.lastSaveUnix = static_cast<int64_t>(std::time(nullptr));
			c.simDateNumber = pSimulator ? pSimulator->GetSimDateNumber() : 0;
			c.population = regionsync::rc641::GetPopulation(pOwnRegionalCity);
			c.cityName = AcpToUtf8(GetRegionalCityName(pOwnRegionalCity));
			c.writer = ownWriter;

			if (!regionsync::WriteFileAtomic(p.path, regionsync::FormatSaveCounter(c), error))
			{
				SaveCountFailed(p.x, p.z, error);
				return;
			}
			logger.WriteLineFormatted(LogLevel::Info, "SaveCount: (%d,%d) count=%u file='%s' prevFile='%s'%s saving -> %s",
				p.x, p.z, c.count, c.file.c_str(), c.prevFile.c_str(), held ? "" : " unlocked=1", p.path.string().c_str());
			pendingSave = std::move(p);
		}
		catch (const std::exception& e)
		{
			logger.WriteLineFormatted(LogLevel::Error, "SaveCount: saving threw: %s", e.what());
		}
		catch (...)
		{
			logger.WriteLine(LogLevel::Error, "SaveCount: saving threw a non-standard exception.");
		}
	}

	void OnSaved(bool ok, bool held)
	{
		Logger& logger = Logger::GetInstance();
		try
		{
			if (!pendingSave)
			{
				return;
			}
			PendingSave p = std::move(*pendingSave);
			pendingSave.reset();
			p.counter.lastOk = ok ? 1 : 0;
			std::string error;
			if (!regionsync::WriteFileAtomic(p.path, regionsync::FormatSaveCounter(p.counter), error))
			{
				SaveCountFailed(p.x, p.z, error);
				return;
			}
			logger.WriteLineFormatted(LogLevel::Info, "SaveCount: (%d,%d) count=%u file='%s' ok=%d%s",
				p.x, p.z, p.counter.count, p.counter.file.c_str(), ok ? 1 : 0, held ? "" : " unlocked=1");
		}
		catch (const std::exception& e)
		{
			logger.WriteLineFormatted(LogLevel::Error, "SaveCount: saved threw: %s", e.what());
		}
		catch (...)
		{
			logger.WriteLine(LogLevel::Error, "SaveCount: saved threw a non-standard exception.");
		}
	}

	// Sticky per tile for the process: the launcher treats these tiles as not sent.
	void SaveCountFailed(int32_t x, int32_t z, const std::string& error)
	{
		Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "SaveCount: (%d,%d) not written: %s", x, z, error.c_str());
		auto& tiles = status.saveCountFailedTiles;
		if (std::find(tiles.begin(), tiles.end(), std::make_pair(x, z)) == tiles.end())
		{
			tiles.emplace_back(x, z);
			WriteStatus("save count failed");
		}
	}

	// The once-per-load Error line. Holder pid from its status file, else the
	// snapshot writer. 'since' from the snapshot when its writer is that pid.
	void LogConflict(int32_t x, int32_t z, const std::string& cityName)
	{
		regionsync::WriterIdentity holder;
		std::string since = "unknown";
		try
		{
			holder = FindTileHolder(x, z);
			regionsync::NeighborSnapshot snapshot;
			std::string error;
			if (regionsync::ReadSnapshotFile(storeDirectory / regionsync::SnapshotFileName(x, z), snapshot, error) && snapshot.writer.Known()
				&& !regionsync::SameWriter(snapshot.writer, ownWriter))
			{
				if (!holder.Known() && SafeIsAlive(snapshot.writer) != regionsync::writer::Liveness::Dead)
				{
					holder = snapshot.writer;
				}
				if (regionsync::SameWriter(snapshot.writer, holder) && snapshot.cityLoadedAt != 0)
				{
					since = FormatFileTime(snapshot.cityLoadedAt);
				}
			}
		}
		catch (...)
		{
		}
		const uint32_t pid = holder.pid;
		const std::string pidText = pid ? std::to_string(pid) : "unknown";
		Logger::GetInstance().WriteLineFormatted(LogLevel::Error,
			"Conflict: tile (%d,%d) '%s' is open in pid %s since %s. No publish, pull, mirror or plugin save for this city until it closes.",
			x, z, cityName.c_str(), pidText.c_str(), since.c_str());
		Logger::GetInstance().Flush();
	}

	// True (and logged at Debug) while this city is a duplicate. Also notes
	// once when the holder's mutex is gone: our copy is stale, we stay dark.
	bool ConflictRefuses(const char* what, const char* reason)
	{
		if (!conflict)
		{
			return false;
		}
		Logger& logger = Logger::GetInstance();
		logger.WriteLineFormatted(LogLevel::Debug, "%s (%s): refused, tile conflict.", what, reason);
		if (!conflictHolderGone && !regionsync::tilelock::PeerPresent())
		{
			conflictHolderGone = true;
			logger.WriteLine(LogLevel::Info, "Conflict: the tile's holder went away; staying dark, this copy is stale. Close the city to clear.");
		}
		return true;
	}

	// ---------------------------------------------------------------
	// Core operations
	// ---------------------------------------------------------------

	// Publish: refresh this tile's regional record from the live city, then
	// write it to the shared store so other instances can pull it. Per-group
	// values go out raw: +0x3c is index-0 supply, which holds no regional share.
	// listsComplete=false: a missing entry must not read as a removal (shutdown).
	void Publish(const char* reason, bool listsComplete = true)
	{
		Logger& logger = Logger::GetInstance();

		if (!pOwnRegionalCity || !pCity)
		{
			logger.WriteLine(LogLevel::Error, "Publish: no city loaded.");
			return;
		}
		publishDirty = false;
		if (ConflictRefuses("Publish", reason))
		{
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
		try
		{
			regionsync::CaptureRegionalCity(pOwnRegionalCity, pSimulator, snapshot);
			regionsync::CaptureConnections(pNeighborsSim, snapshot);
			regionsync::CaptureDeals(pNeighborsSim, pSimulator, snapshot);
		}
		catch (const std::exception& e)
		{
			logger.WriteLineFormatted(LogLevel::Error, "Publish (%s): capture threw: %s", reason, e.what());
			return;
		}
		catch (...)
		{
			logger.WriteLineFormatted(LogLevel::Error, "Publish (%s): capture threw a non-standard exception.", reason);
			return;
		}
		snapshot.writer = ownWriter;
		snapshot.cityLoadedAt = cityLoadedAt;
		// The load-time publish runs before vanilla's own reconcile.
		if (!connectionsReady || !listsComplete)
		{
			if (snapshot.connectionsComplete || snapshot.dealsComplete)
			{
				logger.WriteLineFormatted(LogLevel::Info, "Publish (%s): lists published as incomplete (connections ready 0x%08X seen=%d).",
					reason, kMessageConnectionsReady, connectionsReady ? 1 : 0);
			}
			snapshot.connectionsComplete = false;
			snapshot.dealsComplete = false;
		}

		if (storeDirectory.empty())
		{
			logger.WriteLineFormatted(LogLevel::Info, "Publish (%s): no store directory, skipped.", reason);
			return;
		}
		const std::filesystem::path path =
			storeDirectory / regionsync::SnapshotFileName(snapshot.regionX, snapshot.regionZ);

		std::string error;
		if (regionsync::WriteSnapshotFile(path, snapshot, error))
		{
			publishCount++;
			logger.WriteLineFormatted(
				LogLevel::Info,
				"Publish (%s): (%d,%d) '%s' pop=%d com=%d ind=%d connections=%zu deals=%zu simDate=%u-%02u-%02u -> %s",
				reason,
				snapshot.regionX, snapshot.regionZ,
				snapshot.cityName.c_str(),
				snapshot.population, snapshot.commercialJobs, snapshot.industrialJobs,
				snapshot.connections.size(), snapshot.deals.size(),
				snapshot.simYear, snapshot.simMonth, snapshot.simDay,
				path.string().c_str());
		}
		else
		{
			logger.WriteLineFormatted(LogLevel::Error, "Publish (%s) failed: %s", reason, error.c_str());
		}
	}

	// Exceptions read as Unknown: a liveness failure must not drop a file.
	// Our host: the pid check. Another host: its mirrored status file.
	regionsync::writer::Liveness SafeIsAlive(const regionsync::WriterIdentity& id) const
	{
		try
		{
			if (id.Known() && !ownWriter.host.empty() && !regionsync::SameHost(id.host, ownWriter.host))
			{
				return ForeignLiveness(id);
			}
			return regionsync::writer::IsAlive(id);
		}
		catch (...)
		{
			return regionsync::writer::Liveness::Unknown;
		}
	}

	// Alive when _instances\<pid>-<start>.status names the same writer, Dead
	// when absent, Unknown on a read error or a mismatch. Logged on change.
	regionsync::writer::Liveness ForeignLiveness(const regionsync::WriterIdentity& id) const
	{
		using L = regionsync::writer::Liveness;
		L result = L::Unknown;
		const char* why = "no status directory";
		if (!statusDirectory.empty())
		{
			const std::filesystem::path path = statusDirectory / regionsync::InstanceStatusFileName(id.pid, id.start);
			std::error_code ec;
			const bool exists = std::filesystem::exists(path, ec);
			regionsync::InstanceStatus s;
			std::string error;
			if (ec)
			{
				why = "exists failed";
			}
			else if (!exists)
			{
				result = L::Dead;
				why = "no status file";
			}
			else if (!regionsync::ReadInstanceStatus(path, s, error))
			{
				why = "read failed";
			}
			else if (regionsync::SameWriter(s.writer, id))
			{
				result = L::Alive;
				why = "status file";
			}
			else
			{
				why = "status file names another writer";
			}
		}
		const std::string key = id.host + "/" + std::to_string(id.pid) + "/" + std::to_string(id.start);
		if (const auto it = foreignLiveness.find(key); it == foreignLiveness.end() || it->second != result)
		{
			foreignLiveness[key] = result;
			Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "Liveness: host=%s pid=%u start=%llu -> %s (%s)",
				id.host.c_str(), id.pid, static_cast<unsigned long long>(id.start),
				result == L::Alive ? "alive" : result == L::Dead ? "dead" : "unknown", why);
		}
		return result;
	}

	regionsync::writer::Liveness WriterLiveness(const regionsync::NeighborSnapshot& snapshot) const
	{
		return settings.checkWriterLiveness ? SafeIsAlive(snapshot.writer) : regionsync::writer::Liveness::Unknown;
	}

	// Every snapshot in the store except our own tile, a held fake neighbor,
	// and snapshots whose writer process is gone (removed from the store).
	// deadTiles: tiles whose snapshot was found Dead (removed or remove failed).
	std::vector<regionsync::NeighborSnapshot> ReadOtherSnapshots(const char* who, int& filesSeen, int& dead, std::set<std::pair<int32_t, int32_t>>& deadTiles)
	{
		Logger& logger = Logger::GetInstance();
		std::vector<regionsync::NeighborSnapshot> result;
		struct Gone { std::filesystem::path path; std::filesystem::file_time_type seenAt; regionsync::NeighborSnapshot snapshot; };
		std::vector<Gone> gone;
		filesSeen = 0;
		dead = 0;

		int32_t ownX = -1, ownZ = -1;
		pOwnRegionalCity->GetPosition(ownX, ownZ);

		std::error_code ec;
		if (!std::filesystem::exists(storeDirectory, ec))
		{
			logger.WriteLineFormatted(LogLevel::Info, "%s: store directory does not exist yet.", who);
			return result;
		}

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
				logger.WriteLineFormatted(LogLevel::Error, "%s: %s", who, error.c_str());
				continue;
			}
			if (!error.empty())
			{
				logger.WriteLineFormatted(LogLevel::Info, "%s: %s: %s", who, entry.path().filename().string().c_str(), error.c_str());
			}

			if (snapshot.regionX == ownX && snapshot.regionZ == ownZ)
			{
				continue; // our own tile
			}
			if (fakeNeighborHeld && snapshot.regionX == fakeNeighborX && snapshot.regionZ == fakeNeighborZ)
			{
				logger.WriteLineFormatted(LogLevel::Info, "%s: (%d,%d) held by RegionSyncFakeNeighbor, skipped.", who, snapshot.regionX, snapshot.regionZ);
				continue;
			}
			if (WriterLiveness(snapshot) == regionsync::writer::Liveness::Dead)
			{
				std::error_code timeEc;
				gone.push_back({ entry.path(), entry.last_write_time(timeEc), std::move(snapshot) });
				continue;
			}
			result.push_back(std::move(snapshot));
		}

		// Removed after the scan, not while iterating the directory.
		for (const auto& [path, seenAt, snapshot] : gone)
		{
			dead++;
			// A relaunched instance may have replaced the file since the scan.
			std::error_code removeEc;
			if (std::filesystem::last_write_time(path, removeEc) != seenAt || removeEc)
			{
				logger.WriteLineFormatted(LogLevel::Info, "%s: snapshot (%d,%d) rewritten since the scan, kept.", who, snapshot.regionX, snapshot.regionZ);
				continue;
			}
			deadTiles.insert({ snapshot.regionX, snapshot.regionZ });
			const bool removed = std::filesystem::remove(path, removeEc);
			if (removeEc || !removed)
			{
				logger.WriteLineFormatted(LogLevel::Error, "%s: snapshot (%d,%d) '%s' writer pid %u on host %s is gone, remove failed: %s",
					who, snapshot.regionX, snapshot.regionZ, snapshot.cityName.c_str(), snapshot.writer.pid, snapshot.writer.host.c_str(), removeEc.message().c_str());
			}
			else
			{
				logger.WriteLineFormatted(LogLevel::Info, "%s: snapshot (%d,%d) '%s' writer pid %u on host %s is gone, removed",
					who, snapshot.regionX, snapshot.regionZ, snapshot.cityName.c_str(), snapshot.writer.pid, snapshot.writer.host.c_str());
			}
		}
		return result;
	}

	// Sets inPull for a scope. Guards nested sends: a SimNewDay delivered
	// inside one of our game calls must not publish or re-enter the mirror.
	struct InPullScope
	{
		bool& flag;
		explicit InPullScope(bool& f) : flag(f) { flag = true; }
		~InPullScope() { flag = false; }
	};

	// cheat (RegionSyncMirror): every pass on, ignoring SyncConnections, SyncDeals,
	// LayEdgeNetwork and SyncRemovals. Record injection is on for both (Q9, 2026-10-18).
	void MirrorSnapshot(const regionsync::NeighborSnapshot& snapshot, bool cheat)
	{
		if (ConflictRefuses("Mirror", cheat ? "cheat" : "pending"))
		{
			return;
		}
		Logger& logger = Logger::GetInstance();
		const bool connections = cheat || settings.syncConnections;
		const bool deals = cheat || settings.syncDeals;
		const bool alive = WriterLiveness(snapshot) == regionsync::writer::Liveness::Alive;
		const regionsync::MirrorContext ctx{
			.pRegion = pRegion,
			.pOwnRegionalCity = pOwnRegionalCity,
			.pNeighborsSim = pNeighborsSim,
			.pSimulator = pSimulator,
			.pCOM = GZCOM(),
			.allowRecordInjection = true,
			.ownLoadedAt = cityLoadedAt,
			.pCity = pCity,
			.layEdgeNetwork = cheat || settings.layEdgeNetwork,
			.publisherAlive = alive,
			.syncRemovals = cheat || settings.syncRemovals,
			.cheat = cheat,
		};
		regionsync::ConnectionMirrorResult connectionResult;
		regionsync::DealMirrorResult dealResult;
		try
		{
			if (connections)
			{
				connectionResult = mirror.MirrorConnections(ctx, snapshot);
			}
			if (deals)
			{
				dealResult = mirror.MirrorDeals(ctx, snapshot);
			}
		}
		catch (const std::exception& e)
		{
			logger.WriteLineFormatted(LogLevel::Error, "Mirror (%d,%d) threw: %s", snapshot.regionX, snapshot.regionZ, e.what());
		}
		catch (...)
		{
			logger.WriteLineFormatted(LogLevel::Error, "Mirror (%d,%d) threw a non-standard exception.", snapshot.regionX, snapshot.regionZ);
		}
		logger.WriteLineFormatted(LogLevel::Info, "Mirror (%d,%d): created connections=%d recordConnections=%d laid=%d removed=%d deals=%d updatedDeals=%d cancelledDeals=%d alive=%d",
			snapshot.regionX, snapshot.regionZ, connectionResult.created, connectionResult.injected, connectionResult.laid, connectionResult.removed,
			dealResult.created, dealResult.updated, dealResult.cancelled, alive ? 1 : 0);
	}

	// Tiles with mirror history whose snapshot was Dead or is gone from the store.
	// A file that exists but was not read (parse failure, held) keeps its history.
	void ForgetAbsentPublishers(const std::vector<regionsync::NeighborSnapshot>& present, const std::set<std::pair<int32_t, int32_t>>& deadTiles)
	{
		for (const auto& [x, z] : mirror.Publishers())
		{
			const bool found = std::any_of(present.begin(), present.end(), [x, z](const regionsync::NeighborSnapshot& s) { return s.regionX == x && s.regionZ == z; });
			if (found) continue;
			std::error_code ec;
			const bool exists = std::filesystem::exists(storeDirectory / regionsync::SnapshotFileName(x, z), ec);
			if (deadTiles.count({ x, z }) || (!exists && !ec))
			{
				mirror.ForgetPublisher(x, z);
			}
			else
			{
				Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "Mirror (%d,%d): snapshot file present but not read, history kept.", x, z);
			}
		}
	}

	// Pull: read every snapshot in the store except our own and apply it to
	// the matching regional record in this instance.
	void Pull(const char* reason)
	{
		Logger& logger = Logger::GetInstance();
		// Before any read, so a peer write during this pull signals a new change.
		storeSeen = ScanPeerSnapshots();

		if (!pRegion || !pOwnRegionalCity)
		{
			logger.WriteLine(LogLevel::Error, "Pull: no region loaded.");
			return;
		}
		if (ConflictRefuses("Pull", reason))
		{
			return;
		}

		InPullScope scope(inPull);

		int filesSeen = 0;
		int dead = 0;
		int applied = 0;
		int totalChanged = 0;
		std::vector<regionsync::NeighborSnapshot> pulled;
		const bool feed = FeedsRegionalDemand();
		const RegionalSums sumsBefore = feed ? SumRegional() : RegionalSums{};

		std::set<std::pair<int32_t, int32_t>> deadTiles;
		std::vector<regionsync::NeighborSnapshot> others = ReadOtherSnapshots("Pull", filesSeen, dead, deadTiles);
		ForgetAbsentPublishers(others, deadTiles);
		for (regionsync::NeighborSnapshot& snapshot : others)
		{
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
				"Pull: applyFields established=%d budget=%d income=%d commodities=%d population=%d markDealsDirty=%d freezeExtrapolation=%d",
				settings.applyEstablished ? 1 : 0, settings.applyBudget ? 1 : 0,
				settings.applyIncome ? 1 : 0, settings.applyCommodities ? 1 : 0,
				settings.applyPopulation ? 1 : 0, settings.markDealsDirty ? 1 : 0, settings.freezeExtrapolation ? 1 : 0);
			logger.Flush();

			if (!settings.applyPulledSnapshots)
			{
				pulled.push_back(std::move(snapshot));
				continue;
			}

			regionsync::ApplyOptions opt;
			opt.established = settings.applyEstablished;
			opt.budget = settings.applyBudget;
			opt.income = settings.applyIncome;
			opt.commodities = settings.applyCommodities;
			opt.population = settings.applyPopulation;
			opt.extrapolationCap = settings.freezeExtrapolation;

			const int changed = regionsync::ApplySnapshotToRegionalCity(snapshot, pTarget, opt);
			applied++;
			totalChanged += changed;

			logger.WriteLineFormatted(
				LogLevel::Info,
				"Pull: applied (%d,%d) changedFields=%d",
				snapshot.regionX, snapshot.regionZ, changed);
			logger.Flush();
			pulled.push_back(std::move(snapshot));
		}

		if (feed && totalChanged > 0)
		{
			FeedRegionalDelta("pull", sumsBefore, SumRegional());
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

		// Mirrored on the next SimNewDay, outside this game call.
		if (settings.syncConnections || settings.syncDeals)
		{
			mirrorPending = std::move(pulled);
		}

		pullCount++;
		logger.WriteLineFormatted(
			LogLevel::Info,
			"Pull (%s): files=%d dead=%d applied=%d changedFields=%d mirrorPending=%zu",
			reason, filesSeen, dead, applied, totalChanged, mirrorPending.size());
	}

	// Mirror cheat: the subscriber steps for every snapshot in the store,
	// regardless of SyncConnections/SyncDeals. Records are not applied.
	void MirrorAll()
	{
		Logger& logger = Logger::GetInstance();
		if (!pRegion || !pOwnRegionalCity || !pNeighborsSim)
		{
			logger.WriteLine(LogLevel::Error, "Mirror: no city loaded.");
			return;
		}

		InPullScope scope(inPull);
		int filesSeen = 0;
		int dead = 0;
		std::set<std::pair<int32_t, int32_t>> deadTiles;
		const std::vector<regionsync::NeighborSnapshot> snapshots = ReadOtherSnapshots("Mirror", filesSeen, dead, deadTiles);
		ForgetAbsentPublishers(snapshots, deadTiles);
		logger.WriteLineFormatted(LogLevel::Info, "==== RegionSyncMirror begin: files=%d dead=%d others=%zu ====", filesSeen, dead, snapshots.size());
		logger.Flush();
		for (const regionsync::NeighborSnapshot& snapshot : snapshots)
		{
			MirrorSnapshot(snapshot, true);
		}
		logger.WriteLine(LogLevel::Info, "==== RegionSyncMirror end ====");
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
		std::vector<TileKey> tiles = EstablishedTiles(pRegion);
		std::erase(tiles, TileKey{ ownX, ownZ });

		const int reloaded = ReloadTiles(pRegion, tiles, true);
		if (reloaded > 0)
		{
			pNeighborsSim->MarkDealsDirty();
		}

		logger.WriteLineFormatted(LogLevel::Info, "Reload: reloaded %d of %zu adjacent tiles (recordPtrChanged).", reloaded, tiles.size());
	}

	// ok is not a success flag (Q3); recordPtrChanged is.
	bool ReloadTile(cISC4Region* region, int32_t x, int32_t z)
	{
		cISC4RegionalCity* pBefore = regionsync::FindRegionalCity(region, x, z);
		const int32_t popBefore = pBefore ? pBefore->GetPopulation() : -1;
		const std::string nameBefore = GetRegionalCityName(pBefore);

		cRZAutoRefCount<cISC4RegionalCity>* pSlot = region->GetCity(x, z);
		if (!pSlot)
		{
			return false;
		}

		const bool ok = region->ReloadCity(pSlot);
		cISC4RegionalCity* pAfter = regionsync::FindRegionalCity(region, x, z);
		const bool changed = pAfter != pBefore;

		Logger::GetInstance().WriteLineFormatted(
			LogLevel::Info,
			"Reload (%d,%d): ok=%d popBefore=%d popAfter=%d recordPtrChanged=%d name='%s'->'%s'",
			x, z,
			ok ? 1 : 0,
			popBefore,
			pAfter ? pAfter->GetPopulation() : -1,
			changed ? 1 : 0,
			nameBefore.c_str(),
			GetRegionalCityName(pAfter).c_str());
		return changed;
	}

	// Under the save lock: blocking (wait, the in-city cheat) or zero wait.
	// Returns tiles whose record changed, or -1 when the zero wait was busy.
	int ReloadTiles(cISC4Region* region, const std::vector<TileKey>& tiles, bool wait)
	{
		std::optional<regionsync::savelock::Guard> lock;
		if (settings.saveLock)
		{
			if (wait)
			{
				lock.emplace(LockTimeoutMs(), "reload");
			}
			else
			{
				lock.emplace("watch");
				if (!lock->Held())
				{
					return -1;
				}
			}
		}
		int changed = 0;
		for (const auto& [x, z] : tiles)
		{
			if (ReloadTile(region, x, z))
			{
				changed++;
			}
		}
		return changed;
	}

	std::vector<TileKey> EstablishedTiles(cISC4Region* region) const
	{
		std::vector<TileKey> tiles;
		SC4Vector<cISC4Region::cLocation> locations;
		region->GetCityLocations(locations);
		for (size_t i = 0; i < locations.size(); i++)
		{
			const int32_t x = static_cast<int32_t>(locations[i].x);
			const int32_t z = static_cast<int32_t>(locations[i].z);
			cISC4RegionalCity* p = regionsync::FindRegionalCity(region, x, z);
			if (p && p->GetEstablished())
			{
				tiles.push_back({ x, z });
			}
		}
		return tiles;
	}

	// Adjacent tiles with a city open, else every established tile (Q17 step 3).
	void ReloadCheat()
	{
		if (pCity)
		{
			ReloadAdjacent();
			return;
		}
		Logger& logger = Logger::GetInstance();
		cISC4AppPtr pSC4App;
		cISC4Region* region = pSC4App ? pSC4App->GetRegion() : nullptr;
		logger.WriteLineFormatted(LogLevel::Info, "Reload: cheat fired in the region view. region=%p (ours %p) regionViewReady=%d",
			static_cast<void*>(region), static_cast<void*>(pRegion), regionViewReady ? 1 : 0);
		logger.Flush();
		if (!region || pSC4App->GetCity() || regionsync::savehook641::InSave())
		{
			logger.WriteLine(LogLevel::Info, "Reload: no region, a city is open, or a save is running; nothing reloaded.");
			return;
		}
		if (settings.saveLock && !regionsync::savelock::IsOpen())
		{
			logger.WriteLine(LogLevel::Info, "Reload: no save mutex, nothing reloaded.");
			return;
		}

		const std::vector<TileKey> tiles = EstablishedTiles(region);
		const int reloaded = ReloadTiles(region, tiles, false);
		if (reloaded < 0)
		{
			logger.WriteLine(LogLevel::Info, "Reload: save lock busy, retry the cheat.");
			return;
		}
		logger.WriteLineFormatted(LogLevel::Info, "Reload: reloaded %d of %zu established tiles (recordPtrChanged, region view).", reloaded, tiles.size());
	}

	// Region view only: the game's own region screen rebuild reloads tile pictures.
	void RedrawCheat()
	{
		Logger& logger = Logger::GetInstance();
		if (pCity)
		{
			logger.WriteLine(LogLevel::Info, "Redraw: a city is open; use it from the region view.");
			return;
		}
		cISC4AppPtr pSC4App;
		cISC4Region* region = pSC4App ? pSC4App->GetRegion() : nullptr;
		logger.WriteLineFormatted(LogLevel::Info, "Redraw: cheat fired in the region view. region=%p regionViewReady=%d",
			static_cast<void*>(region), regionViewReady ? 1 : 0);
		logger.Flush();
		if (!region || pSC4App->GetCity() || regionsync::savehook641::InSave())
		{
			logger.WriteLine(LogLevel::Info, "Redraw: no region, a city is open, or a save is running; nothing done.");
			return;
		}
		try
		{
			const bool ok = pSC4App->RequestGoToRegionView(false);
			logger.WriteLineFormatted(LogLevel::Info, "Redraw: RequestGoToRegionView(false) returned %d", ok ? 1 : 0);
			logger.Flush();
		}
		catch (...)
		{
			logger.WriteLine(LogLevel::Error, "Redraw: exception in RequestGoToRegionView.");
		}
	}

	// Plugin saves only; a save of a conflicting city would overwrite the first opener's.
	void Save(const char* reason)
	{
		if (ConflictRefuses("Save", reason))
		{
			return;
		}
		cISC4AppPtr pSC4App;
		if (pSC4App && pCity)
		{
			// Fast save, no notification popup.
			const bool ok = pSC4App->RequestSaveCity(false, true);
			Logger::GetInstance().WriteLineFormatted(LogLevel::Info, "Save (%s): requested, ok=%d", reason, ok ? 1 : 0);
		}
	}

	uint32_t LockTimeoutMs() const { return static_cast<uint32_t>(settings.saveLockTimeoutMs); }

	// Toggle: hold the save lock until the next use or city shutdown, so the
	// peer's saves can be watched waiting. Diagnostic only.
	void HoldLock()
	{
		Logger& logger = Logger::GetInstance();
		if (holdLock)
		{
			holdLock = false;
			regionsync::savelock::Release("hold");
			logger.WriteLine(LogLevel::Info, "HoldLock: off.");
			return;
		}
		if (!settings.saveLock)
		{
			logger.WriteLine(LogLevel::Info, "HoldLock: SaveLock=0, nothing to hold.");
			return;
		}
		holdLock = regionsync::savelock::Acquire(LockTimeoutMs(), "hold").held;
		logger.WriteLine(LogLevel::Info, holdLock ? "HoldLock: on." : "HoldLock: not taken.");
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

		// This tile's connections and deals, as a publish would capture them.
		if (pNeighborsSim)
		{
			regionsync::NeighborSnapshot own;
			regionsync::CaptureConnections(pNeighborsSim, own);
			regionsync::CaptureDeals(pNeighborsSim, pSimulator, own, true);

			logger.WriteLineFormatted(LogLevel::Info, "Connections from this tile: %zu", own.connections.size());
			for (const regionsync::ConnectionRecord& c : own.connections)
			{
				logger.WriteLineFormatted(LogLevel::Info, "  %s (%u) inner=(%d,%d) cell=(%d,%d) otherTile=(%d,%d) active=%d",
					ConnectionTypeName(static_cast<cISC4NeighborsSimulator::ConnectionType>(c.type)), c.type,
					c.innerX, c.innerZ, c.cellX, c.cellZ, c.otherTileX, c.otherTileZ, c.active ? 1 : 0);
			}

			logger.WriteLineFormatted(LogLevel::Info, "Deals (FindDealsAll): %zu", own.deals.size());
			for (const regionsync::DealRecord& d : own.deals)
			{
				logger.WriteLineFormatted(LogLevel::Info, "  deal type=0x%08X otherTile=(%d,%d) amount=%d amountUsed=%d cost=%lld flags=0x%X expiresIn=%d",
					d.type, d.otherTileX, d.otherTileZ, d.amount, d.amountUsed, static_cast<long long>(d.cost), d.flags, d.expiresIn);
			}

			logger.WriteLineFormatted(
				LogLevel::Info,
				"Deal income/month=%lld expense/month=%lld",
				pNeighborsSim->GetTotalMonthlyNeighborDealIncome(),
				pNeighborsSim->GetTotalMonthlyNeighborDealExpense());
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
		// 0x00000 feeds the record's +0x3c, 0x10000 holds the regional fold, 0x20000 the totals.
		const uint32_t indices[] = { 0x00000, kRegionalTargetIndex, 0x20000 };
		logger.WriteLine(LogLevel::Info, "==== RegionSyncDemand begin ====");
		for (uint32_t groupId : regionsync::kRCIGroupIds)
		{
			for (uint32_t idx : indices)
			{
				cISC4Demand* pDemand = pDemandSim->GetDemand(groupId, idx);
				if (!pDemand)
				{
					logger.WriteLineFormatted(LogLevel::Info, "  0x%04X idx 0x%05X: absent", groupId, idx);
					continue;
				}
				const SC4Percentage* pCap = pDemand->GetDemandCap();
				logger.WriteLineFormatted(
					LogLevel::Info,
					"  0x%04X idx 0x%05X demand=%.1f active=%.1f supply=%.1f max=%.1f min=%.1f cap=%.4f regionUse=%u",
					groupId, idx,
					pDemand->QueryDemandValue(), pDemand->QueryActiveDemandValue(), pDemand->QuerySupplyValue(),
					pDemand->QueryActiveDemandMax(), pDemand->QueryActiveDemandMin(),
					pCap ? pCap->percentage : -1.0f, pDemand->GetRegionUse());
			}
		}
		LogConnectedCities("RegionSyncDemand");
		const RegionalSums all = SumOtherTiles();
		const RegionalSums connected = SumConnectedTiles();
		for (size_t i = 0; i < regionalDemand.size(); i++)
		{
			logger.WriteLineFormatted(LogLevel::Info, "  regional 0x%04X use=%u sumConnected=%.0f sumAll=%.0f",
				regionalDemand[i].groupId, regionalDemand[i].regionUse, connected[i], all[i]);
		}
		logger.WriteLine(LogLevel::Info, "==== RegionSyncDemand end ====");
		logger.Flush();
	}

	static void LogRecordStats(const char* who, cISC4RegionalCity* p)
	{
		using namespace regionsync::rc641;
		Logger& logger = Logger::GetInstance();
		logger.WriteLineFormatted(LogLevel::Info, "%s budget=%.0f", who, GetBudget(p));
		for (int32_t c = 1; c <= 3; c++)
		{
			logger.WriteLineFormatted(LogLevel::Info, "%s commodity %d prod=%.2f dem=%.2f imp=%.2f exp=%.2f",
				who, c, GetCommodity(p, kGetProduced, c), GetCommodity(p, kGetDemanded, c),
				GetCommodity(p, kGetImported, c), GetCommodity(p, kGetExported, c));
		}
		logger.Flush();
	}

	// Offer: why ProposeDeals does or does not produce an offer. Mirrors its
	// per-neighbor, per-type checks, then calls it with addToSim=false.
	void OfferDiagnostic()
	{
		Logger& logger = Logger::GetInstance();
		if (!pCity || !pNeighborsSim || !pOwnRegionalCity)
		{
			logger.WriteLine(LogLevel::Error, "Offer: no city loaded.");
			return;
		}

		logger.WriteLine(LogLevel::Info, "==== RegionSyncOffer begin ====");
		logger.Flush();
		try
		{
			namespace ns = regionsync::ns641;
			pNeighborsSim->UpdateCommodityStats(pCity);
			LogRecordStats("Offer: own", pOwnRegionalCity);

			SC4List<cISC4RegionalCity*> adjacent;
			pNeighborsSim->GetAdjacentCities(adjacent, false);
			logger.WriteLineFormatted(LogLevel::Info, "Offer: adjacent established=%zu", adjacent.size());
			logger.Flush();
			for (cISC4RegionalCity* pOther : adjacent)
			{
				if (!pOther) continue;
				int32_t x = -1, z = -1;
				pOther->GetPosition(x, z);
				char who[96];
				snprintf(who, sizeof(who), "Offer: (%d,%d) '%s'", x, z, GetRegionalCityName(pOther).c_str());
				LogRecordStats(who, pOther);
				for (int32_t type = regionsync::kDealTypeFirst; type <= static_cast<int32_t>(regionsync::kDealTypeLast); type++)
				{
					cISC4NeighborConnection* pConn = pNeighborsSim->FindConnectionSuitableForDeal(type, pOther);
					SC4List<cISC4NeighborDeal*> existing;
					ns::FindDeals(pNeighborsSim, type, pOther, existing);
					const double maxAmount = ns::GetDealMaxAmount(pNeighborsSim, type, pOther);
					const double cost = ns::GetDealCost(pNeighborsSim, type, maxAmount, pOther);
					logger.WriteLineFormatted(LogLevel::Info, "%s type=0x%08X conn=%p active=%d existing=%zu max=%.2f costAtMax=%.2f (gate uses cost at the tuning minimum)",
						who, static_cast<uint32_t>(type), static_cast<void*>(pConn), pConn ? (pConn->GetIsActive() ? 1 : 0) : -1,
						existing.size(), maxAmount, cost);
					logger.Flush();
				}
			}

			// 0x6d0520 AddRefs each element into the list; the SC4List destructor releases.
			SC4List<cISC4NeighborDeal*> proposed;
			const bool ok = ns::ProposeDeals(pNeighborsSim, proposed, false);
			logger.WriteLineFormatted(LogLevel::Info, "Offer: ProposeDeals(false) ret=%d count=%zu", ok ? 1 : 0, proposed.size());
			for (cISC4NeighborDeal* pDeal : proposed)
			{
				if (!pDeal) continue;
				int32_t x = -1, z = -1;
				if (cISC4RegionalCity* pOther = pDeal->GetCityOnOtherSideOfConnection()) pOther->GetPosition(x, z);
				logger.WriteLineFormatted(LogLevel::Info, "Offer:   deal type=0x%08X other=(%d,%d) amount=%d cost=%lld flags=0x%X",
					static_cast<uint32_t>(pDeal->GetType()), x, z, pDeal->GetAmount(),
					static_cast<long long>(pDeal->GetCost()), static_cast<uint32_t>(pDeal->GetFlags()));
			}
		}
		catch (const std::exception& e)
		{
			logger.WriteLineFormatted(LogLevel::Error, "Offer: threw: %s", e.what());
		}
		catch (...)
		{
			logger.WriteLine(LogLevel::Error, "Offer: threw a non-standard exception.");
		}
		logger.WriteLine(LogLevel::Info, "==== RegionSyncOffer end ====");
		logger.Flush();
	}

	// FakeNeighbor: toggle. First call writes a huge population into the
	// first other established tile's record and holds it against the
	// monthly pull; second call restores the saved values and feeds the
	// negative delta to the demand simulator. RegionSyncDemand before and a month after tells
	// whether the demand simulator reads regional records live (Q2).
	void FakeNeighbor()
	{
		Logger& logger = Logger::GetInstance();
		const bool feed = FeedsRegionalDemand();
		const RegionalSums sumsBefore = feed ? SumRegional() : RegionalSums{};
		if (fakeNeighborHeld)
		{
			fakeNeighborHeld = false;
			cISC4RegionalCity* p = pRegion ? regionsync::FindRegionalCity(pRegion, fakeNeighborX, fakeNeighborZ) : nullptr;
			if (p)
			{
				regionsync::rc641::SetPopulation(p, fakeSaved.pop);
				regionsync::rc641::SetCommercialJobs(p, fakeSaved.com);
				regionsync::rc641::SetIndustrialJobs(p, fakeSaved.ind);
				if (int32_t* v = regionsync::rc641::FindPopulationByType(p, 0x1010)) *v = fakeSaved.r1;
				if (int32_t* v = regionsync::rc641::FindExtrapolatedPopulationByType(p, 0x1010)) *v = fakeSaved.r1x;
			}
			logger.WriteLineFormatted(LogLevel::Info, "FakeNeighbor: released (%d,%d); restored pop=%d R$=%d (extrapolated %d).",
				fakeNeighborX, fakeNeighborZ, fakeSaved.pop, fakeSaved.r1, fakeSaved.r1x);
			logger.Flush();
			if (pNeighborsSim) pNeighborsSim->MarkDealsDirty();
			if (feed) FeedRegionalDelta("fake neighbor release", sumsBefore, SumRegional());
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
			int32_t* pR1 = regionsync::rc641::FindPopulationByType(p, 0x1010);
			const int32_t r1 = pR1 ? *pR1 : -1;
			int32_t* pR1x = regionsync::rc641::FindExtrapolatedPopulationByType(p, 0x1010);
			fakeSaved = FakeSaved{ pop, regionsync::rc641::GetCommercialJobs(p), regionsync::rc641::GetIndustrialJobs(p), r1, pR1x ? *pR1x : -1 };
			regionsync::rc641::SetPopulation(p, fake);
			if (pR1) *pR1 = r1 + 500000;
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
			if (feed) FeedRegionalDelta("fake neighbor", sumsBefore, SumRegional());
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
		case kCheatReloadID:  ReloadCheat(); break;
		case kCheatRedrawID:  RedrawCheat(); break;
		case kCheatStatusID:  Status(); break;
		case kCheatSaveID:    Save("cheat"); break;
		case kCheatUnlockID:  Unlock("cheat"); break;
		case kCheatSetterTestID: SetterTest(); break;
		case kCheatProbeID: Probe(); break;
		case kCheatDemandID: DemandDump(); break;
		case kCheatFakeNeighborID: FakeNeighbor(); break;
		case kCheatMirrorID: MirrorAll(); break;
		case kCheatOfferID: OfferDiagnostic(); break;
		case kCheatHoldLockID: HoldLock(); break;
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
	regionsync::WriterIdentity ownWriter;
	uint64_t cityLoadedAt = 0;

	std::vector<RegionalDemandEntry> regionalDemand;
	bool fakeNeighborHeld = false;
	int32_t fakeNeighborX = -1;
	int32_t fakeNeighborZ = -1;
	struct FakeSaved { int32_t pop, com, ind, r1, r1x; };
	FakeSaved fakeSaved{};

	regionsync::NeighborMirror mirror;
	std::vector<regionsync::NeighborSnapshot> mirrorPending;
	bool publishDirty = false;
	bool connectionsReady = false;   // 0x6AC284F3 arrived this city session
	bool inPull = false;
	bool sawNewDay = false;
	bool holdLock = false;   // RegionSyncHoldLock taken

	std::filesystem::path statusDirectory;
	std::filesystem::path statusPath;   // empty until PostAppInit writes it
	regionsync::InstanceStatus status;
	bool statusWriteFailed = false;
	bool conflict = false;              // another instance has this tile open
	bool conflictHolderGone = false;
	cISC4City* guardCity = nullptr;     // city and tile the guard last ran for
	int32_t guardX = -1;
	int32_t guardZ = -1;

	bool reloadCheat = false;   // RegionSyncReload and the cheat notification registered

	// Save counter (docs/PLAN-LAN.md P2).
	std::string loadedSaveFile;   // basename at city load, UTF-8
	struct PendingSave { std::filesystem::path path; int32_t x = -1, z = -1; regionsync::SaveCounter counter; };
	std::optional<PendingSave> pendingSave;
	mutable std::map<std::string, regionsync::writer::Liveness> foreignLiveness;

	// Q17. Slice A: store watch; slice B: region watch on idle.
	ChangeWatch storeWatch{ "StoreWatch" };
	std::map<std::string, std::filesystem::file_time_type> storeSeen;
	regionsync::IdleService idleService{ kIdleServiceID };
	bool idleAdded = false;
	bool onIdleAdded = false;
	ChangeWatch regionWatch{ "RegionWatch" };
	std::filesystem::path regionFolder;
	bool regionViewReady = false;   // set only by the PostRegionInit message
	bool regionPending = false;
	bool regionBusyLogged = false;
	ULONGLONG regionSignalTick = 0;
	TileProbes regionSeen;
	TileProbes regionLastProbe;
	ULONGLONG regionLastProbeTick = 0;
	bool regionNoMutexLogged = false;
	bool inIdle = false;
	std::set<TileKey> regionPathLogged;
};

cRZCOMDllDirector* RZGetCOMDllDirector()
{
	static RegionSyncDllDirector sDirector;
	return &sDirector;
}
