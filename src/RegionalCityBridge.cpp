/*
 * sc4-region-sync
 *
 * RegionalCityBridge.cpp
 */

#include "RegionalCityBridge.h"
#include "RegionalCity641.h"
#include "cISC4Region.h"
#include "cISC4RegionalCity.h"
#include "cISC4Simulator.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "Logger.h"
#include <cmath>
#include <ctime>

namespace
{
	bool Differs(float a, float b)
	{
		return std::fabs(a - b) > 0.0001f;
	}

	float Finite(double v)
	{
		return std::isfinite(v) ? static_cast<float>(v) : 0.0f;
	}

	struct CommoditySlots
	{
		regionsync::rc641::Slot get;
		regionsync::rc641::Slot set;
		float regionsync::CommodityStats::* field;
		const char* name;
	};

	constexpr CommoditySlots kCommodityAccess[] =
	{
		{ regionsync::rc641::kGetExported, regionsync::rc641::kSetExported, &regionsync::CommodityStats::exported, "SetExported" },
		{ regionsync::rc641::kGetImported, regionsync::rc641::kSetImported, &regionsync::CommodityStats::imported, "SetImported" },
		{ regionsync::rc641::kGetProduced, regionsync::rc641::kSetProduced, &regionsync::CommodityStats::produced, "SetProduced" },
		{ regionsync::rc641::kGetDemanded, regionsync::rc641::kSetDemanded, &regionsync::CommodityStats::demanded, "SetDemanded" },
		{ regionsync::rc641::kGetCostPerUnit, regionsync::rc641::kSetCostPerUnit, &regionsync::CommodityStats::costPerUnit, "SetCostPerUnit" },
	};
}

namespace regionsync
{
	void CaptureRegionalCity(cISC4RegionalCity* pRegionalCity, cISC4Simulator* pSimulator, NeighborSnapshot& out)
	{
		out = NeighborSnapshot{};

		if (!pRegionalCity)
		{
			return;
		}

		pRegionalCity->GetPosition(out.regionX, out.regionZ);
		pRegionalCity->GetCitySize(out.sizeX, out.sizeZ);
		out.citySerial = pRegionalCity->GetCitySerialNumber();
		out.established = pRegionalCity->GetEstablished();

		cRZBaseString name;
		pRegionalCity->GetCityName(name);
		out.cityName = name.ToChar();

		out.writtenUnixTime = static_cast<int64_t>(std::time(nullptr));

		if (pSimulator)
		{
			out.simDateNumber = pSimulator->GetSimDateNumber();

			uint32_t year = 0, month = 0, day = 0, dayOfYear = 0, weekDay = 0;
			pSimulator->GetSimDate(&year, &month, &day, &dayOfYear, &weekDay);
			out.simYear = year;
			out.simMonth = month;
			out.simDay = day;
		}

		// Summary fields through the corrected slots (see RegionalCity641.h).
		out.population = rc641::GetPopulation(pRegionalCity);
		out.commercialJobs = rc641::GetCommercialJobs(pRegionalCity);
		out.industrialJobs = rc641::GetIndustrialJobs(pRegionalCity);
		const float* pWorkforce = static_cast<const float*>(rc641::GetWorkforce(pRegionalCity));
		out.workforcePercentage = pWorkforce ? Finite(*pWorkforce) : 0.0f;
		out.mayorRating = rc641::GetMayorRating(pRegionalCity);
		out.budget = Finite(rc641::GetBudget(pRegionalCity));
		out.income = Finite(rc641::GetIncome(pRegionalCity));

		for (size_t i = 0; i < kCommoditySlots; i++)
		{
			const int32_t commodity = static_cast<int32_t>(i);
			for (const CommoditySlots& a : kCommodityAccess)
			{
				out.commodities[i].*(a.field) = Finite(rc641::GetCommodity(pRegionalCity, a.get, commodity));
			}
		}

		out.rciGroupCount = 0;
		for (uint32_t groupId : kRCIGroupIds)
		{
			if (out.rciGroupCount >= kRCIGroupSlots)
			{
				break;
			}
			RCIGroupPopulation& g = out.rciGroups[out.rciGroupCount++];
			g.groupId = groupId;
			g.population = rc641::GetPopulation(pRegionalCity, groupId);
		}
	}

	int ApplySnapshotToRegionalCity(const NeighborSnapshot& s, cISC4RegionalCity* pRegionalCity, const ApplyOptions& opt)
	{
		if (!pRegionalCity)
		{
			return 0;
		}

		Logger& logger = Logger::GetInstance();
		int changed = 0;

		// Population and jobs: no setter on the interface, written straight
		// into the record's fields (see RegionalCity641.h).
		if (opt.population)
		{
			struct Total { const char* name; int32_t current; int32_t target; void (*set)(cISC4RegionalCity*, int32_t); };
			const Total totals[] =
			{
				{ "population", rc641::GetPopulation(pRegionalCity), s.population, &rc641::SetPopulation },
				{ "commercialJobs", rc641::GetCommercialJobs(pRegionalCity), s.commercialJobs, &rc641::SetCommercialJobs },
				{ "industrialJobs", rc641::GetIndustrialJobs(pRegionalCity), s.industrialJobs, &rc641::SetIndustrialJobs },
			};
			for (const Total& t : totals)
			{
				if (t.current == t.target)
				{
					continue;
				}
				logger.WriteLineFormatted(LogLevel::Info, "Apply: %s %d -> %d", t.name, t.current, t.target);
				logger.Flush();
				t.set(pRegionalCity, t.target);
				changed++;
			}

			for (size_t i = 0; i < s.rciGroupCount && i < kRCIGroupSlots; i++)
			{
				const RCIGroupPopulation& g = s.rciGroups[i];
				int32_t* pValue = rc641::FindPopulationByType(pRegionalCity, g.groupId);
				if (!pValue)
				{
					if (g.population != 0)
					{
						logger.WriteLineFormatted(LogLevel::Debug, "Apply: rci 0x%04X has no node in the record, skipped (snapshot %d)", g.groupId, g.population);
					}
					continue;
				}
				if (*pValue == g.population)
				{
					continue;
				}
				logger.WriteLineFormatted(LogLevel::Info, "Apply: rci 0x%04X %d -> %d", g.groupId, *pValue, g.population);
				logger.Flush();
				*pValue = g.population;
				changed++;
			}
		}

		if (opt.established && pRegionalCity->GetEstablished() != s.established)
		{
			logger.WriteLineFormatted(LogLevel::Info, "Apply: SetEstablished(%d)", s.established ? 1 : 0);
			logger.Flush();
			pRegionalCity->SetEstablished(s.established);
			changed++;
		}

		if (opt.budget && Differs(Finite(rc641::GetBudget(pRegionalCity)), s.budget))
		{
			logger.WriteLineFormatted(LogLevel::Info, "Apply: SetBudget(%.0f)", s.budget);
			logger.Flush();
			rc641::SetBudget(pRegionalCity, s.budget);
			changed++;
		}

		if (opt.income && Differs(Finite(rc641::GetIncome(pRegionalCity)), s.income))
		{
			logger.WriteLineFormatted(LogLevel::Info, "Apply: SetIncome(%.0f)", s.income);
			logger.Flush();
			rc641::SetIncome(pRegionalCity, s.income);
			changed++;
		}

		for (size_t i = 0; opt.commodities && i < kApplyCommoditySlots; i++)
		{
			const int32_t commodity = static_cast<int32_t>(i);
			const CommodityStats& c = s.commodities[i];

			for (const CommoditySlots& a : kCommodityAccess)
			{
				const float current = Finite(rc641::GetCommodity(pRegionalCity, a.get, commodity));
				const float target = c.*(a.field);
				if (!Differs(current, target))
				{
					continue;
				}
				logger.WriteLineFormatted(LogLevel::Info, "Apply: %s(%d, %.2f) was %.2f", a.name, commodity, target, current);
				logger.Flush();
				rc641::SetCommodity(pRegionalCity, a.set, commodity, target);
				changed++;
			}
		}

		logger.WriteLineFormatted(LogLevel::Info, "Apply: done, changed=%d", changed);
		logger.Flush();
		return changed;
	}

	cISC4RegionalCity* FindRegionalCity(cISC4Region* pRegion, int32_t regionX, int32_t regionZ)
	{
		if (!pRegion || regionX < 0 || regionZ < 0)
		{
			return nullptr;
		}

		// GetCity returns a pointer to the region's own ref-counted slot.
		// We do not add or release a reference on it.
		cRZAutoRefCount<cISC4RegionalCity>* pSlot = pRegion->GetCity(static_cast<uint32_t>(regionX), static_cast<uint32_t>(regionZ));
		if (!pSlot)
		{
			return nullptr;
		}

		return *reinterpret_cast<cISC4RegionalCity**>(pSlot);
	}
}
