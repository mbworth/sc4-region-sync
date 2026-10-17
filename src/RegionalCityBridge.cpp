/*
 * sc4-region-sync
 *
 * RegionalCityBridge.cpp
 */

#include "RegionalCityBridge.h"
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

	// The game returns NaN for some fields (GetWorkforcePercentage on every
	// tile, observed 2026-10-17). Keep the snapshot file finite.
	float Finite(float v)
	{
		return std::isfinite(v) ? v : 0.0f;
	}
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

		out.population = pRegionalCity->GetPopulation();
		out.commercialJobs = pRegionalCity->GetCommercialJobs();
		out.industrialJobs = pRegionalCity->GetIndustrialJobs();
		out.workforcePercentage = Finite(pRegionalCity->GetWorkforcePercentage());
		out.mayorRating = pRegionalCity->GetMayorRating();
		out.budget = Finite(pRegionalCity->GetBudget());
		out.income = Finite(pRegionalCity->GetIncome());

		for (size_t i = 0; i < kCommoditySlots; i++)
		{
			const int32_t commodity = static_cast<int32_t>(i);
			CommodityStats& c = out.commodities[i];
			c.exported = Finite(pRegionalCity->GetExported(commodity));
			c.imported = Finite(pRegionalCity->GetImported(commodity));
			c.produced = Finite(pRegionalCity->GetProduced(commodity));
			c.demanded = Finite(pRegionalCity->GetDemanded(commodity));
			c.costPerUnit = Finite(pRegionalCity->GetCostPerUnit(commodity));
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
			g.population = pRegionalCity->GetPopulation(groupId);
		}
	}

	int ApplySnapshotToRegionalCity(const NeighborSnapshot& s, cISC4RegionalCity* pRegionalCity)
	{
		if (!pRegionalCity)
		{
			return 0;
		}

		int changed = 0;

		// Phase 1 deliberately only writes the fields that have a declared
		// setter on cISC4RegionalCity. There is no public setter for the
		// headline population/jobs numbers (they come from UpdateCityCache
		// when the live city saves), so those are reported but not applied
		// until we confirm how the demand simulator reads them. See
		// docs/OPEN-QUESTIONS.md, Q2.

		Logger& logger = Logger::GetInstance();

		if (pRegionalCity->GetEstablished() != s.established)
		{
			logger.WriteLineFormatted(LogLevel::Info, "Apply: SetEstablished(%d)", s.established ? 1 : 0);
			pRegionalCity->SetEstablished(s.established);
			changed++;
		}

		if (Differs(pRegionalCity->GetBudget(), s.budget))
		{
			logger.WriteLineFormatted(LogLevel::Info, "Apply: SetBudget(%.0f)", s.budget);
			pRegionalCity->SetBudget(s.budget);
			changed++;
		}

		if (Differs(pRegionalCity->GetIncome(), s.income))
		{
			logger.WriteLineFormatted(LogLevel::Info, "Apply: SetIncome(%.0f)", s.income);
			pRegionalCity->SetIncome(s.income);
			changed++;
		}

		logger.WriteLine(LogLevel::Info, "Apply: commodity setters");

		for (size_t i = 0; i < kCommoditySlots; i++)
		{
			const int32_t commodity = static_cast<int32_t>(i);
			const CommodityStats& c = s.commodities[i];

			if (Differs(pRegionalCity->GetExported(commodity), c.exported))
			{
				pRegionalCity->SetExported(commodity, c.exported);
				changed++;
			}
			if (Differs(pRegionalCity->GetImported(commodity), c.imported))
			{
				pRegionalCity->SetImported(commodity, c.imported);
				changed++;
			}
			if (Differs(pRegionalCity->GetProduced(commodity), c.produced))
			{
				pRegionalCity->SetProduced(commodity, c.produced);
				changed++;
			}
			if (Differs(pRegionalCity->GetDemanded(commodity), c.demanded))
			{
				pRegionalCity->SetDemanded(commodity, c.demanded);
				changed++;
			}
			if (Differs(pRegionalCity->GetCostPerUnit(commodity), c.costPerUnit))
			{
				pRegionalCity->SetCostPerUnit(commodity, c.costPerUnit);
				changed++;
			}
		}

		logger.WriteLineFormatted(LogLevel::Info, "Apply: done, changed=%d", changed);
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
