/*
 * sc4-region-sync
 *
 * RegionalCity641.h - raw vtable access to cSC4RegionalCity as it exists in
 * SimCity 4 1.1.641, for the slots where the vendored cISC4RegionalCity.h
 * is wrong. Read from the binary with Ghidra on 2026-10-17 (vtable at
 * 0x00a8fb20; see docs/OPEN-QUESTIONS.md Q2 and tools/ghidra/).
 *
 * What the header gets wrong:
 *   - slot 9 is GetPopulation(uint32_t type), not GetPopulation(void); the
 *     headline getters pop/com/ind/workforce/rating/difficulty are slots
 *     10..15 and GetTaxRate(type) is 16.
 *   - budget and income are doubles; the commodity getters return double
 *     and the setters take (int32_t, double). Calling them with float
 *     leaves the stack 4 bytes short and the stores are garbage.
 *
 * Only the director/bridge should include this. Vendored headers stay
 * untouched; this is the local wrapper the ground rules ask for.
 */

#pragma once
#include <cstddef>
#include <cstdint>

class cISC4RegionalCity;

namespace regionsync::rc641
{
	// Slot indices into the cISC4RegionalCity vtable (0 = QueryInterface).
	enum Slot : size_t
	{
		kGetPopulationByType = 9,   // int32 (uint32 type)      ret 4
		kGetPopulation = 10,        // int32 ()  [this+0x24]
		kGetCommercialJobs = 11,    // int32 ()  [this+0x28]
		kGetIndustrialJobs = 12,    // int32 ()  [this+0x2c]
		kGetWorkforce = 13,         // void* ()  -> this+0x30 (struct, first float = percentage?)
		kGetMayorRating = 14,       // int8  ()  [this+0x34]
		kGetDifficultyLevel = 15,   // int32 ()  [this+0x38]
		kGetTaxRate = 16,           // float (uint32 type)      ret 4
		kGetBudget = 50,            // double () [this+0x170]
		kSetBudget = 51,            // void (double)            ret 8
		kGetIncome = 52,            // double () [this+0x178]
		kSetIncome = 53,            // void (double)            ret 8
		kGetExported = 54,          // double (int32)           ret 4
		kSetExported = 55,          // void (int32, double)     ret 12
		kGetImported = 56,
		kSetImported = 57,
		kGetProduced = 58,
		kSetProduced = 59,
		kGetDemanded = 60,
		kSetDemanded = 61,
		kGetCostPerUnit = 62,
		kSetCostPerUnit = 63,
		kGetCommodityBalance = 64,  // double (int32)           ret 4
	};

	using GetI32Fn = int32_t(__thiscall*)(cISC4RegionalCity*);
	using GetI8Fn = int8_t(__thiscall*)(cISC4RegionalCity*);
	using GetPtrFn = void*(__thiscall*)(cISC4RegionalCity*);
	using GetI32ByTypeFn = int32_t(__thiscall*)(cISC4RegionalCity*, uint32_t);
	using GetFltByTypeFn = float(__thiscall*)(cISC4RegionalCity*, uint32_t);
	using GetDblFn = double(__thiscall*)(cISC4RegionalCity*);
	using SetDblFn = void(__thiscall*)(cISC4RegionalCity*, double);
	using GetDblByIdxFn = double(__thiscall*)(cISC4RegionalCity*, int32_t);
	using SetDblByIdxFn = void(__thiscall*)(cISC4RegionalCity*, int32_t, double);

	inline void** Vtable(cISC4RegionalCity* p)
	{
		return *reinterpret_cast<void***>(p);
	}

	template <typename Fn>
	Fn At(cISC4RegionalCity* p, Slot slot)
	{
		return reinterpret_cast<Fn>(Vtable(p)[slot]);
	}

	inline int32_t GetPopulation(cISC4RegionalCity* p) { return At<GetI32Fn>(p, kGetPopulation)(p); }
	inline int32_t GetCommercialJobs(cISC4RegionalCity* p) { return At<GetI32Fn>(p, kGetCommercialJobs)(p); }
	inline int32_t GetIndustrialJobs(cISC4RegionalCity* p) { return At<GetI32Fn>(p, kGetIndustrialJobs)(p); }
	inline int32_t GetPopulation(cISC4RegionalCity* p, uint32_t type) { return At<GetI32ByTypeFn>(p, kGetPopulationByType)(p, type); }
	inline void* GetWorkforce(cISC4RegionalCity* p) { return At<GetPtrFn>(p, kGetWorkforce)(p); }
	inline int8_t GetMayorRating(cISC4RegionalCity* p) { return At<GetI8Fn>(p, kGetMayorRating)(p); }
	inline int32_t GetDifficultyLevel(cISC4RegionalCity* p) { return At<GetI32Fn>(p, kGetDifficultyLevel)(p); }
	inline float GetTaxRate(cISC4RegionalCity* p, uint32_t type) { return At<GetFltByTypeFn>(p, kGetTaxRate)(p, type); }

	inline double GetBudget(cISC4RegionalCity* p) { return At<GetDblFn>(p, kGetBudget)(p); }
	inline void SetBudget(cISC4RegionalCity* p, double v) { At<SetDblFn>(p, kSetBudget)(p, v); }
	inline double GetIncome(cISC4RegionalCity* p) { return At<GetDblFn>(p, kGetIncome)(p); }
	inline void SetIncome(cISC4RegionalCity* p, double v) { At<SetDblFn>(p, kSetIncome)(p, v); }

	inline double GetCommodity(cISC4RegionalCity* p, Slot getter, int32_t commodity) { return At<GetDblByIdxFn>(p, getter)(p, commodity); }
	inline void SetCommodity(cISC4RegionalCity* p, Slot setter, int32_t commodity, double v) { At<SetDblByIdxFn>(p, setter)(p, commodity, v); }

	// Population and jobs have no setter on the interface. They are plain
	// int32 fields read by slots 10..12 (mov eax,[this+0x24/0x28/0x2c]).
	inline int32_t* FieldI32(cISC4RegionalCity* p, size_t offset)
	{
		return reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(p) + offset);
	}
	inline void SetPopulation(cISC4RegionalCity* p, int32_t v) { *FieldI32(p, 0x24) = v; }
	inline void SetCommercialJobs(cISC4RegionalCity* p, int32_t v) { *FieldI32(p, 0x28) = v; }
	inline void SetIndustrialJobs(cISC4RegionalCity* p, int32_t v) { *FieldI32(p, 0x2c) = v; }

	// Per-type population lives in a tree map at this+0x3c, as slot 9
	// (FUN_004a8500) walks it: header = *(this+0x3c), root = header+4,
	// node { +0x8 left, +0xc right, +0x10 key, +0x14 value }. Returns the
	// value slot for an existing key, or null; we never insert.
	inline int32_t* FindPopulationByType(cISC4RegionalCity* p, uint32_t type)
	{
		uint8_t* header = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(p) + 0x3c);
		if (!header)
		{
			return nullptr;
		}
		uint8_t* node = *reinterpret_cast<uint8_t**>(header + 4);
		uint8_t* candidate = header;
		while (node)
		{
			if (*reinterpret_cast<uint32_t*>(node + 0x10) < type)
			{
				node = *reinterpret_cast<uint8_t**>(node + 0xc);
			}
			else
			{
				candidate = node;
				node = *reinterpret_cast<uint8_t**>(node + 0x8);
			}
		}
		if (candidate == header || *reinterpret_cast<uint32_t*>(candidate + 0x10) != type)
		{
			return nullptr;
		}
		return reinterpret_cast<int32_t*>(candidate + 0x14);
	}
}
