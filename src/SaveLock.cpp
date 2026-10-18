/*
 * sc4-region-sync
 *
 * SaveLock.cpp - see SaveLock.h.
 */

#include "SaveLock.h"
#include <Windows.h>
#include <cstdio>

using regionsync::savelock::kWarning;

namespace
{
	HANDLE mutex = nullptr;
	std::string openName;
	int depth = 0;
	bool timedOut = false;    // wait 0 ms until an Acquire succeeds
	ULONGLONG acquiredAt = 0;

	const char* Label(const char* who) { return who ? who : "?"; }

	// Never throws, so lock state stays consistent if logging fails.
	template <typename... Args>
	void Log(LogLevel level, bool flush, const char* format, Args... args)
	{
		try
		{
			Logger& logger = Logger::GetInstance();
			logger.WriteLineFormatted(level, format, args...);
			if (flush)
			{
				logger.Flush();
			}
		}
		catch (...)
		{
		}
	}
}

namespace regionsync::savelock
{
	std::wstring WidenAcp(const std::string& s)
	{
		const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
		if (n <= 0)
		{
			return {};
		}
		std::wstring out(static_cast<size_t>(n), L'\0');
		MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
		return out;
	}

	std::string Stamp()
	{
		FILETIME utc{}, local{};
		SYSTEMTIME st{};
		GetSystemTimePreciseAsFileTime(&utc);
		if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st))
		{
			return "??:??:??.???";
		}
		char buffer[16];
		snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
		return buffer;
	}

	bool Open(const std::string& regionDirectoryName)
	{
		const std::string name = "Local\\SC4RegionSync.SaveLock." + regionDirectoryName;
		if (mutex && name == openName)
		{
			return true;
		}
		Close();

		if (regionDirectoryName.empty() || regionDirectoryName.find('\\') != std::string::npos)
		{
			Log(LogLevel::Error, false, "SaveLock: %s bad region name '%s', not opened", Stamp().c_str(), regionDirectoryName.c_str());
			return false;
		}

		const std::wstring wide = WidenAcp(name);
		mutex = wide.empty() ? nullptr : CreateMutexW(nullptr, FALSE, wide.c_str());
		const DWORD error = GetLastError();
		if (!mutex)
		{
			Log(LogLevel::Error, false, "SaveLock: %s CreateMutexW '%s' failed, error=%lu; saves run unlocked", Stamp().c_str(), name.c_str(), error);
			return false;
		}
		openName = name;
		depth = 0;
		timedOut = false;
		Log(LogLevel::Info, true, "SaveLock: %s opened '%s' peer=%d", Stamp().c_str(), name.c_str(), error == ERROR_ALREADY_EXISTS ? 1 : 0);
		return true;
	}

	void Close()
	{
		if (!mutex)
		{
			return;
		}
		const int heldDepth = depth;
		if (depth > 0)
		{
			ReleaseMutex(mutex);
			depth = 0;
		}
		CloseHandle(mutex);
		mutex = nullptr;
		timedOut = false;
		Log(LogLevel::Info, true, "SaveLock: %s closed '%s' (released depth=%d)", Stamp().c_str(), openName.c_str(), heldDepth);
		openName.clear();
	}

	AcquireResult Acquire(uint32_t timeoutMs, const char* who)
	{
		AcquireResult r;

		if (!mutex)
		{
			Log(kWarning, false, "SaveLock: %s %s: warning, no mutex open, unlocked", Stamp().c_str(), Label(who));
			return r;
		}
		if (depth > 0)
		{
			r.held = true;
			r.depth = ++depth;
			Log(LogLevel::Info, true, "SaveLock: %s %s: acquired waited=0 ms depth=%d (already held)", Stamp().c_str(), Label(who), depth);
			return r;
		}

		const DWORD wait = timedOut ? 0 : timeoutMs;
		const ULONGLONG start = GetTickCount64();
		const DWORD result = WaitForSingleObject(mutex, wait);
		const DWORD err = GetLastError();
		const ULONGLONG now = GetTickCount64();
		r.waitedMs = static_cast<uint32_t>(now - start);

		if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)
		{
			r.held = true;
			r.abandoned = result == WAIT_ABANDONED;
			r.depth = depth = 1;
			timedOut = false;
			acquiredAt = now;
			Log(r.abandoned ? kWarning : LogLevel::Info, true, "SaveLock: %s %s: acquired%s waited=%u ms depth=1",
				Stamp().c_str(), Label(who), r.abandoned ? " (abandoned)" : "", r.waitedMs);
		}
		else if (result == WAIT_TIMEOUT && !timedOut)
		{
			timedOut = true;
			Log(kWarning, true, "SaveLock: %s %s: warning, timeout after %u ms, going ahead unlocked", Stamp().c_str(), Label(who), r.waitedMs);
		}
		else if (result == WAIT_TIMEOUT)
		{
			Log(kWarning, true, "SaveLock: %s %s: warning, unlocked (peer still holding since last timeout)", Stamp().c_str(), Label(who));
		}
		else
		{
			Log(kWarning, true, "SaveLock: %s %s: warning, wait failed result=0x%lx error=%lu, unlocked", Stamp().c_str(), Label(who), result, err);
		}
		return r;
	}

	void Release(const char* who)
	{
		if (!mutex || depth <= 0)
		{
			return;
		}
		if (--depth > 0)
		{
			Log(LogLevel::Info, true, "SaveLock: %s %s: released depth=%d (still held)", Stamp().c_str(), Label(who), depth);
			return;
		}
		const BOOL ok = ReleaseMutex(mutex);
		Log(LogLevel::Info, true, "SaveLock: %s %s: released held=%llu ms ok=%d",
			Stamp().c_str(), Label(who), static_cast<unsigned long long>(GetTickCount64() - acquiredAt), ok ? 1 : 0);
	}

	Guard::Guard(uint32_t timeoutMs, const char* who) : who(who), held(Acquire(timeoutMs, who).held) {}

	Guard::~Guard()
	{
		if (held)
		{
			try { Release(who); } catch (...) {}
		}
	}
}
