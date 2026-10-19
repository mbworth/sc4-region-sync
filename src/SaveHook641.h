/*
 * sc4-region-sync
 *
 * SaveHook641.h - wraps cISC4App::SaveCity(path, fast) as it exists in
 * SimCity 4 1.1.641 by patching App vtable 0x00a86af0 slot 15 (byte 0x3c,
 * slot address 0x00a86b2c, routine 0x00455e20). Every save path goes
 * through that slot (docs/research/save-641.md). The patch is applied only
 * when the vtable and slot addresses match. Director only.
 */

#pragma once
#include <cstdint>
#include <functional>

class cISC4App;

namespace regionsync::savehook641
{
	// lock: each save holds the save lock, waiting up to timeoutMs. Without
	// it the wrapper only runs the callbacks.
	bool Install(cISC4App* pApp, bool lock, uint32_t timeoutMs);
	// Restores the slot if it still holds our wrapper. log=false for DLL
	// detach, where the Logger may be gone.
	void Uninstall(bool log);
	// True while a SaveCity call is inside the wrapper.
	bool InSave();
	// Outermost save only (depth 1). onSaving(path, ANSI; held) runs after the
	// lock attempt, before the original; onSaved(ok, held) after it returns.
	// held: the save lock is held. Exceptions are swallowed; the original
	// always runs.
	void SetCallbacks(std::function<void(const char*, bool)> onSaving, std::function<void(bool, bool)> onSaved);
}
