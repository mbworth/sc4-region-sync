/*
 * sc4-region-sync
 *
 * Game641.h - globals of the SimCity 4 1.1.641 exe (fixed image base, no
 * relocations). Only the mirror should include this.
 */

#pragma once
#include <cstdint>

namespace regionsync::game641
{
	// "Drive on Right" property 0x49be9565, bound at 0x513247 to the byte at
	// 0x00b21cc1; nonzero = right-hand traffic (docs/research/dual-ferry-641.md).
	inline bool DriveOnRight()
	{
		return *reinterpret_cast<const volatile uint8_t*>(uintptr_t{ 0x00b21cc1 }) != 0;
	}
}
