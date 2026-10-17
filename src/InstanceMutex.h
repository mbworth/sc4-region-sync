/*
 * sc4-region-sync
 *
 * InstanceMutex.h - SimCity 4 refuses to start a second copy while a
 * named mutex ("SimCity 4 Mutex", observed 2026-10-17 in Process
 * Explorer) exists. Closing that handle in the running instance lets the
 * next instance create its own and start. See docs/OPEN-QUESTIONS.md Q5.
 */

#pragma once
#include <string>
#include <vector>

namespace regionsync
{
	struct NamedHandle
	{
		unsigned short handle;
		std::wstring type;
		std::wstring name;
	};

	// Named Mutant handles owned by this process whose name contains
	// nameSubstring (case-insensitive). Empty substring lists all of them.
	std::vector<NamedHandle> FindNamedMutexHandles(const std::wstring& nameSubstring);

	// Closes every handle returned by FindNamedMutexHandles. Returns how
	// many were closed.
	int CloseNamedMutexHandles(const std::wstring& nameSubstring);
}
