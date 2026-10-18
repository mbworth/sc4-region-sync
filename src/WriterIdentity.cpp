/*
 * sc4-region-sync
 *
 * WriterIdentity.cpp
 */

#include "WriterIdentity.h"
#include <Windows.h>

namespace
{
	uint64_t ToU64(const FILETIME& ft)
	{
		return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
	}

	uint64_t CreationTime(HANDLE process)
	{
		FILETIME created{}, exited{}, kernel{}, user{};
		return GetProcessTimes(process, &created, &exited, &kernel, &user) ? ToU64(created) : 0;
	}

	std::string ComputerName()
	{
		wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
		DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
		if (!GetComputerNameW(name, &length) || length == 0)
		{
			return {};
		}
		const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
		if (bytes <= 0)
		{
			return {};
		}
		std::string out(static_cast<size_t>(bytes), '\0');
		WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(length), out.data(), bytes, nullptr, nullptr);
		return out;
	}

	class ProcessHandle
	{
	public:
		explicit ProcessHandle(HANDLE h) : handle(h) {}
		~ProcessHandle() { if (handle) CloseHandle(handle); }
		ProcessHandle(const ProcessHandle&) = delete;
		ProcessHandle& operator=(const ProcessHandle&) = delete;
		HANDLE get() const { return handle; }
	private:
		HANDLE handle;
	};
}

namespace regionsync::writer
{
	WriterIdentity Current()
	{
		WriterIdentity id;
		id.host = ComputerName();
		id.pid = GetCurrentProcessId();
		id.start = CreationTime(GetCurrentProcess());
		return id;
	}

	Liveness IsAlive(const WriterIdentity& id)
	{
		static const std::string ownHost = ComputerName();
		if (!id.Known() || ownHost.empty() || id.host != ownHost)
		{
			return Liveness::Unknown;
		}

		const ProcessHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id.pid));
		if (!process.get())
		{
			// No such pid. Access denied and the rest mean it may exist.
			return GetLastError() == ERROR_INVALID_PARAMETER ? Liveness::Dead : Liveness::Unknown;
		}

		const uint64_t created = CreationTime(process.get());
		if (created == 0)
		{
			return Liveness::Unknown;
		}
		if (created != id.start)
		{
			return Liveness::Dead;  // pid reused by a newer process
		}

		DWORD exitCode = 0;
		if (!GetExitCodeProcess(process.get(), &exitCode))
		{
			return Liveness::Unknown;
		}
		return exitCode == STILL_ACTIVE ? Liveness::Alive : Liveness::Dead;
	}
}
