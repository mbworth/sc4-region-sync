/*
 * sc4-region-sync launcher
 *
 * Resources.cpp - the resource line: how many more instances fit at full
 * speed. Formula from docs/PLAN-LAUNCHER.md, "Resource line".
 */

#include "Launcher.h"
#include <Psapi.h>
#include <algorithm>
#include <cstdio>

namespace
{
	constexpr uint64_t kMb = 1024ull * 1024ull;

	uint64_t ToU64(const FILETIME& ft)
	{
		return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
	}

	int PhysicalCores()
	{
		DWORD length = 0;
		GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
		std::vector<BYTE> buffer(length);
		auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
		if (length == 0 || !GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length))
		{
			return 1;
		}
		int count = 0;
		for (DWORD offset = 0; offset < length; ++count)
		{
			offset += reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset)->Size;
		}
		return std::max(count, 1);
	}

	int Room(int64_t available, int64_t reserve, uint64_t per)
	{
		const int64_t left = available - reserve;
		return left <= 0 || per == 0 ? 0 : static_cast<int>(static_cast<uint64_t>(left) / per);
	}
}

namespace launcher
{
	ResourceSample SampleResources(const Settings& settings, const std::vector<DWORD>& pids)
	{
		static const int physicalCores = PhysicalCores();
		static const int logicalCores = std::max<int>(1, static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
		static uint64_t largestWorkingSet = 0;
		static uint64_t largestPrivate = 0;
		static uint64_t lastIdle = 0, lastTotal = 0;

		for (const DWORD pid : pids)
		{
			const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (!process)
			{
				continue;
			}
			PROCESS_MEMORY_COUNTERS_EX pmc{};
			if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
			{
				largestWorkingSet = std::max<uint64_t>(largestWorkingSet, pmc.WorkingSetSize);
				largestPrivate = std::max<uint64_t>(largestPrivate, pmc.PrivateUsage);
			}
			CloseHandle(process);
		}

		MEMORYSTATUSEX mem{};
		mem.dwLength = sizeof(mem);
		GlobalMemoryStatusEx(&mem);

		const uint64_t need = std::max<uint64_t>(static_cast<uint64_t>(settings.perInstanceRamMb) * kMb, largestWorkingSet);
		const uint64_t commitNeed = std::max<uint64_t>(static_cast<uint64_t>(settings.perInstanceRamMb) * kMb, largestPrivate);
		const int64_t reserve = static_cast<int64_t>(settings.reserveRamMb) * static_cast<int64_t>(kMb);
		const int ramRoom = Room(static_cast<int64_t>(mem.ullAvailPhys), reserve, need);
		const int commitRoom = Room(static_cast<int64_t>(mem.ullAvailPageFile), reserve, commitNeed);

		// Idle over the last 5 s, in logical cores. Negative until two samples.
		double idleCores = -1.0;
		FILETIME idleFt{}, kernelFt{}, userFt{};
		if (GetSystemTimes(&idleFt, &kernelFt, &userFt))
		{
			const uint64_t idle = ToU64(idleFt);
			const uint64_t total = ToU64(kernelFt) + ToU64(userFt);  // kernel includes idle
			if (lastTotal != 0 && total > lastTotal)
			{
				idleCores = static_cast<double>(idle - lastIdle) / static_cast<double>(total - lastTotal) * logicalCores;
			}
			lastIdle = idle;
			lastTotal = total;
		}

		ResourceSample r;
		r.running = static_cast<int>(pids.size());
		r.memoryRoom = std::min(ramRoom, commitRoom);
		r.cpuRoom = std::max(0, physicalCores - settings.reserveCores - r.running);
		r.capRoom = std::max(0, settings.maxInstances - r.running);
		r.room = std::min({ r.memoryRoom, r.cpuRoom, r.capRoom });

		const bool idleLow = idleCores >= 0.0 && idleCores < 1.0;
		if (r.memoryRoom == 0)
		{
			r.colour = ResourceSample::Colour::Red;
		}
		else if (r.cpuRoom == 0 || idleLow)
		{
			r.colour = ResourceSample::Colour::Amber;
		}

		const wchar_t* binding = r.room == r.memoryRoom ? (ramRoom <= commitRoom ? L"RAM" : L"commit")
			: r.room == r.cpuRoom ? L"CPU cores" : L"MaxInstances";

		wchar_t idleText[32] = L"n/a";
		if (idleCores >= 0.0)
		{
			swprintf_s(idleText, L"%.1f", idleCores);
		}

		wchar_t text[256];
		swprintf_s(text, L"%d running, ~%d more at full speed (estimate), idle %s cores", r.running, r.room, idleText);
		r.text = text;

		wchar_t tip[512];
		swprintf_s(tip,
			L"Limited by %s.\r\nMemory room %d (RAM %d at %llu MB each, commit %d at %llu MB each)\r\n"
			L"CPU room %d (%d physical cores, %d reserved)\r\nMaxInstances room %d",
			binding, r.memoryRoom, ramRoom, need / kMb, commitRoom, commitNeed / kMb,
			r.cpuRoom, physicalCores, settings.reserveCores, r.capRoom);
		r.tooltip = tip;
		return r;
	}
}
