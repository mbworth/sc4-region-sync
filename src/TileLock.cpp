/*
 * sc4-region-sync
 *
 * TileLock.cpp - see TileLock.h.
 */

#include "TileLock.h"
#include "SaveLock.h"
#include <Windows.h>

using regionsync::savelock::Stamp;

namespace
{
	HANDLE mutex = nullptr;
	std::string openName;
	std::wstring conflictName;   // set after a conflicting Open, for PeerPresent

	template <typename... Args>
	void Log(LogLevel level, const char* format, Args... args)
	{
		try
		{
			Logger& logger = Logger::GetInstance();
			logger.WriteLineFormatted(level, format, args...);
			logger.Flush();
		}
		catch (...)
		{
		}
	}
}

namespace regionsync::tilelock
{
	bool Open(const std::string& regionDirectoryName, int32_t x, int32_t z)
	{
		Close();

		const std::string name = "Local\\SC4RegionSync.Tile." + regionDirectoryName + "." + std::to_string(x) + "_" + std::to_string(z);
		if (regionDirectoryName.empty() || regionDirectoryName.find('\\') != std::string::npos)
		{
			Log(LogLevel::Error, "TileLock: %s bad region name '%s', not opened", Stamp().c_str(), regionDirectoryName.c_str());
			return false;
		}

		const std::wstring wide = savelock::WidenAcp(name);
		HANDLE h = wide.empty() ? nullptr : CreateMutexW(nullptr, FALSE, wide.c_str());
		const DWORD error = GetLastError();
		if (!h)
		{
			Log(LogLevel::Error, "TileLock: %s CreateMutexW '%s' failed, error=%lu; no same-tile guard", Stamp().c_str(), name.c_str(), error);
			return false;
		}
		if (error == ERROR_ALREADY_EXISTS)
		{
			CloseHandle(h);
			conflictName = wide;
			Log(LogLevel::Info, "TileLock: %s '%s' already exists, peer=1", Stamp().c_str(), name.c_str());
			return true;
		}
		mutex = h;
		openName = name;
		Log(LogLevel::Info, "TileLock: %s opened '%s' peer=0", Stamp().c_str(), name.c_str());
		return false;
	}

	void Close()
	{
		conflictName.clear();
		if (!mutex)
		{
			return;
		}
		CloseHandle(mutex);
		mutex = nullptr;
		Log(LogLevel::Info, "TileLock: %s closed '%s'", Stamp().c_str(), openName.c_str());
		openName.clear();
	}

	bool PeerPresent()
	{
		if (conflictName.empty())
		{
			return false;
		}
		HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, conflictName.c_str());
		if (h)
		{
			CloseHandle(h);
			return true;
		}
		return GetLastError() != ERROR_FILE_NOT_FOUND;
	}
}
