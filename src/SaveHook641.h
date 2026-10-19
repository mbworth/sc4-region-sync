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

class cISC4App;

namespace regionsync::savehook641
{
	// Each save then holds the save lock, waiting up to timeoutMs.
	bool Install(cISC4App* pApp, uint32_t timeoutMs);
	// Restores the slot if it still holds our wrapper. log=false for DLL
	// detach, where the Logger may be gone.
	void Uninstall(bool log);
	// True while a SaveCity call is inside the wrapper.
	bool InSave();
}
