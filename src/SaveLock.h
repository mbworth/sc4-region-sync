/*
 * sc4-region-sync
 *
 * SaveLock.h - region-wide save lock shared by every instance on this host:
 * a named mutex, Local\SC4RegionSync.SaveLock.<region directory name>.
 * Windows-only, no game headers. Main thread only. See docs/PLAN-SAVE-LOCK.md.
 */

#pragma once
#include "Logger.h"
#include <cstdint>
#include <string>

namespace regionsync::savelock
{
	// The vendored Logger has no Warning level; Info is the one always written.
	constexpr LogLevel kWarning = LogLevel::Info;

	struct AcquireResult
	{
		bool held = false;
		uint32_t waitedMs = 0;
		bool abandoned = false;
		int depth = 0;
	};

	// Creates or opens the region's mutex; logs peer=1 when it already
	// existed. Reopening with the same name is a no-op.
	bool Open(const std::string& regionDirectoryName);
	// Releases a hold still taken, then closes the handle.
	void Close();
	bool IsOpen();

	// Depth 0 waits up to timeoutMs (0 ms while a previous wait timed out
	// and none has succeeded since); depth > 0 returns at once. Not held:
	// the caller goes ahead unlocked. who labels the log line.
	AcquireResult Acquire(uint32_t timeoutMs, const char* who);
	// Zero wait, no back-off. Not held: busy or no mutex.
	AcquireResult TryAcquire(const char* who);
	// Decrements; ReleaseMutex at depth 0. No-op when not held.
	void Release(const char* who);

	// Local wall clock, HH:mm:ss.fff.
	std::string Stamp();
	// CP_ACP to UTF-16 for mutex names; empty on failure.
	std::wstring WidenAcp(const std::string& s);

	class Guard
	{
	public:
		Guard(uint32_t timeoutMs, const char* who);
		// Zero wait (TryAcquire).
		explicit Guard(const char* who);
		~Guard();
		Guard(const Guard&) = delete;
		Guard& operator=(const Guard&) = delete;
		bool Held() const { return held; }
	private:
		const char* who;
		bool held;
	};
}
