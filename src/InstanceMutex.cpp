/*
 * sc4-region-sync
 *
 * InstanceMutex.cpp - see InstanceMutex.h.
 */

#include "InstanceMutex.h"
#include <Windows.h>
#include <winternl.h>
#include <algorithm>
#include <cwctype>
\n#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

namespace
{
	// Not in winternl.h. Layout for 32-bit processes.
	struct SYSTEM_HANDLE_ENTRY
	{
		USHORT ProcessId;
		USHORT CreatorBackTraceIndex;
		UCHAR ObjectTypeNumber;
		UCHAR Flags;
		USHORT Handle;
		PVOID Object;
		ACCESS_MASK GrantedAccess;
	};

	struct SYSTEM_HANDLE_INFORMATION_
	{
		ULONG Count;
		SYSTEM_HANDLE_ENTRY Handles[1];
	};

	constexpr SYSTEM_INFORMATION_CLASS kSystemHandleInformation = static_cast<SYSTEM_INFORMATION_CLASS>(16);
	constexpr OBJECT_INFORMATION_CLASS kObjectNameInformation = static_cast<OBJECT_INFORMATION_CLASS>(1);
	constexpr OBJECT_INFORMATION_CLASS kObjectTypeInformation = static_cast<OBJECT_INFORMATION_CLASS>(2);

	using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
	using NtQueryObjectFn = NTSTATUS(NTAPI*)(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);

	struct OBJECT_TYPE_INFORMATION_
	{
		UNICODE_STRING TypeName;
		// more fields follow; only TypeName is read
	};

	struct OBJECT_NAME_INFORMATION_
	{
		UNICODE_STRING Name;
	};

	std::wstring Lower(std::wstring s)
	{
		for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
		return s;
	}

	std::wstring QueryString(NtQueryObjectFn fn, HANDLE h, OBJECT_INFORMATION_CLASS cls)
	{
		std::vector<unsigned char> buffer(1024);
		ULONG needed = 0;
		NTSTATUS status = fn(h, cls, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
		if (status == STATUS_INFO_LENGTH_MISMATCH || static_cast<ULONG>(status) == 0xC0000023 /*STATUS_BUFFER_TOO_SMALL*/)
		{
			buffer.resize(needed + 64);
			status = fn(h, cls, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
		}
		if (status != 0)
		{
			return L"";
		}
		const UNICODE_STRING& us = *reinterpret_cast<const UNICODE_STRING*>(buffer.data());
		if (!us.Buffer || us.Length == 0)
		{
			return L"";
		}
		return std::wstring(us.Buffer, us.Length / sizeof(wchar_t));
	}
}

namespace regionsync
{
	std::vector<NamedHandle> FindNamedMutexHandles(const std::wstring& nameSubstring)
	{
		std::vector<NamedHandle> result;

		HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		if (!ntdll)
		{
			return result;
		}
		auto querySystem = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
		auto queryObject = reinterpret_cast<NtQueryObjectFn>(GetProcAddress(ntdll, "NtQueryObject"));
		if (!querySystem || !queryObject)
		{
			return result;
		}

		std::vector<unsigned char> buffer(1 << 20);
		ULONG needed = 0;
		NTSTATUS status;
		while ((status = querySystem(kSystemHandleInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed)) == STATUS_INFO_LENGTH_MISMATCH)
		{
			buffer.resize(buffer.size() * 2);
		}
		if (status != 0)
		{
			return result;
		}

		const auto* info = reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_*>(buffer.data());
		const USHORT ownPid = static_cast<USHORT>(GetCurrentProcessId());
		const std::wstring needle = Lower(nameSubstring);

		for (ULONG i = 0; i < info->Count; i++)
		{
			const SYSTEM_HANDLE_ENTRY& e = info->Handles[i];
			if (e.ProcessId != ownPid)
			{
				continue;
			}
			HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(e.Handle));

			// Type first: querying the name of some pipe handles can block,
			// mutants never do.
			std::wstring type = QueryString(queryObject, h, kObjectTypeInformation);
			if (type != L"Mutant")
			{
				continue;
			}
			std::wstring name = QueryString(queryObject, h, kObjectNameInformation);
			if (name.empty())
			{
				continue;
			}
			if (!needle.empty() && Lower(name).find(needle) == std::wstring::npos)
			{
				continue;
			}
			result.push_back(NamedHandle{ e.Handle, type, name });
		}

		return result;
	}

	int CloseNamedMutexHandles(const std::wstring& nameSubstring)
	{
		int closed = 0;
		for (const NamedHandle& nh : FindNamedMutexHandles(nameSubstring))
		{
			if (CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(nh.handle))))
			{
				closed++;
			}
		}
		return closed;
	}
}
