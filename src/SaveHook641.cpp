/*
 * sc4-region-sync
 *
 * SaveHook641.cpp - see SaveHook641.h.
 */

#include "SaveHook641.h"
#include "SaveLock.h"
#include "cISC4App.h"
#include "cIGZString.h"
#include "Logger.h"
#include <Windows.h>
#include <cstdint>
#include <string>

namespace
{
	using regionsync::savelock::kWarning;

	constexpr uintptr_t kAppVtable = 0x00a86af0;
	constexpr size_t kSaveCityPathSlot = 15;       // byte offset 0x3c
	constexpr uintptr_t kSlotAddress = 0x00a86b2c;
	constexpr uintptr_t kOriginal = 0x00455e20;    // ret 8

	using SaveCityFn = bool(__thiscall*)(cISC4App*, const cIGZString&, bool);

	SaveCityFn original = nullptr;
	uint32_t lockTimeoutMs = 0;
	void** patchedSlot = nullptr;
	int saveDepth = 0;

	// Our side of a save. Never throws; the original is always called.
	class Scope
	{
	public:
		explicit Scope(bool fast)
		{
			const int depth = ++saveDepth;
			if (depth > 1)
			{
				try
				{
					Logger::GetInstance().WriteLineFormatted(kWarning, "SaveLock: %s save: warning, re-entered depth=%d fast=%d",
						regionsync::savelock::Stamp().c_str(), depth, fast ? 1 : 0);
				}
				catch (...)
				{
				}
			}
			try
			{
				held = regionsync::savelock::Acquire(lockTimeoutMs, "save").held;
			}
			catch (...)
			{
			}
		}

		~Scope()
		{
			try
			{
				if (held)
				{
					regionsync::savelock::Release("save");
				}
			}
			catch (...)
			{
			}
			--saveDepth;
		}

		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		bool held = false;
	};

	// ecx = this, edx unused, two stack args popped by the callee (ret 8).
	static bool __fastcall SaveCityWrapper(cISC4App* self, void* edx, const cIGZString& path, bool fast)
	{
		(void)edx;
		const Scope scope(fast);
		return original(self, path, fast);
	}

	void* WrapperAddress()
	{
		return reinterpret_cast<void*>(&SaveCityWrapper);
	}

	std::string ModuleOf(const void* address)
	{
		HMODULE module = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			static_cast<LPCWSTR>(address), &module))
		{
			return "<no module>";
		}
		wchar_t path[MAX_PATH]{};
		const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
		const int bytes = WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
		if (n == 0 || bytes <= 0)
		{
			return "<unknown>";
		}
		std::string out(static_cast<size_t>(bytes), '\0');
		WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(n), out.data(), bytes, nullptr, nullptr);
		return out;
	}
}

namespace regionsync::savehook641
{
	bool Install(cISC4App* pApp, uint32_t timeoutMs)
	{
		Logger& logger = Logger::GetInstance();
		if (!pApp)
		{
			logger.WriteLine(LogLevel::Error, "SaveHook: no App, not patched.");
			return false;
		}
		lockTimeoutMs = timeoutMs;
		if (patchedSlot)
		{
			return true;
		}

		void** vtbl = *reinterpret_cast<void***>(pApp);
		void** slot = &vtbl[kSaveCityPathSlot];
		void* current = *slot;
		logger.WriteLineFormatted(LogLevel::Info, "SaveHook: vtable=%p (want %08X) slot=%p (want %08X) value=%p (original %08X) wrapper=%p",
			static_cast<void*>(vtbl), static_cast<unsigned>(kAppVtable), static_cast<void*>(slot), static_cast<unsigned>(kSlotAddress),
			current, static_cast<unsigned>(kOriginal), WrapperAddress());

		if (reinterpret_cast<uintptr_t>(vtbl) != kAppVtable || reinterpret_cast<uintptr_t>(slot) != kSlotAddress)
		{
			logger.WriteLine(LogLevel::Info, "SaveHook: not the 641 App vtable, not patched.");
			return false;
		}
		if (current == WrapperAddress())
		{
			return true;
		}
		if (reinterpret_cast<uintptr_t>(current) != kOriginal)
		{
			logger.WriteLineFormatted(kWarning, "SaveHook: warning, slot already patched to %p in %s; chaining to it.", current, ModuleOf(current).c_str());
		}

		original = reinterpret_cast<SaveCityFn>(current);

		DWORD oldProtect = 0;
		const BOOL unprotected = VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect);
		const DWORD unprotectError = unprotected ? 0 : GetLastError();
		logger.WriteLineFormatted(LogLevel::Info, "SaveHook: VirtualProtect(RW) ok=%d oldProtect=0x%lx error=%lu", unprotected ? 1 : 0, oldProtect, unprotectError);
		if (!unprotected)
		{
			return false;
		}

		void* before = InterlockedCompareExchangePointer(slot, WrapperAddress(), current);
		DWORD ignored = 0;
		const BOOL restored = VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
		void* after = *slot;
		logger.WriteLineFormatted(LogLevel::Info, "SaveHook: slot before=%p after=%p VirtualProtect(restore 0x%lx) ok=%d",
			before, after, oldProtect, restored ? 1 : 0);
		logger.Flush();

		if (before != current)
		{
			logger.WriteLine(LogLevel::Info, "SaveHook: slot changed during the patch, not patched.");
			return false;
		}
		patchedSlot = slot;
		return true;
	}

	void Uninstall(bool log)
	{
		if (!patchedSlot)
		{
			return;
		}
		void** slot = patchedSlot;
		patchedSlot = nullptr;

		DWORD oldProtect = 0;
		if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
		{
			if (log)
			{
				Logger::GetInstance().WriteLineFormatted(LogLevel::Error, "SaveHook: uninstall VirtualProtect failed, error=%lu", GetLastError());
			}
			return;
		}
		void* before = InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(original), WrapperAddress());
		DWORD ignored = 0;
		VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);

		if (log)
		{
			Logger& logger = Logger::GetInstance();
			if (before == WrapperAddress())
			{
				logger.WriteLineFormatted(LogLevel::Info, "SaveHook: uninstalled, slot restored to %p", *slot);
			}
			else
			{
				logger.WriteLineFormatted(kWarning, "SaveHook: warning, slot holds %p (not ours), left alone", before);
			}
			logger.Flush();
		}
	}

	bool InSave()
	{
		return saveDepth > 0;
	}
}
