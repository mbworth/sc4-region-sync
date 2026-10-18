/*
 * sc4-region-sync
 *
 * NeighborSnapshot.cpp - key=value serialization of NeighborSnapshot.
 *
 * Writes go to a temporary file in the same directory and are then renamed
 * over the destination, so a reader never sees a half-written snapshot even
 * when two game instances are running at once.
 */

#include "NeighborSnapshot.h"
#include <fstream>
#include <sstream>
#include <map>
#include <charconv>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace
{
	template <typename T>
	bool ParseNumber(const std::string& text, T& out)
	{
		if constexpr (std::is_floating_point_v<T>)
		{
			char* end = nullptr;
			double value = std::strtod(text.c_str(), &end);
			if (end == text.c_str() || !(std::fabs(value) <= std::numeric_limits<T>::max()))
			{
				return false;
			}
			out = static_cast<T>(value);
			return true;
		}
		else
		{
			const char* first = text.data();
			const char* last = first + text.size();
			long long value = 0;
			auto result = std::from_chars(first, last, value);
			if (result.ec != std::errc() || !std::in_range<T>(value))
			{
				return false;
			}
			out = static_cast<T>(value);
			return true;
		}
	}

	// Only non-empty, non-comment lines of the form key=value are kept.
	bool LoadKeyValues(const std::filesystem::path& path, std::map<std::string, std::string>& out, std::string& error)
	{
		std::ifstream stream(path);
		if (!stream)
		{
			error = "Unable to open " + path.string();
			return false;
		}

		for (std::string line; std::getline(stream, line);)
		{
			if (line.empty() || line[0] == ';' || line[0] == '#')
			{
				continue;
			}

			const size_t eq = line.find('=');
			if (eq == std::string::npos)
			{
				continue;
			}

			out[line.substr(0, eq)] = line.substr(eq + 1);
		}

		return true;
	}

	// False when the key is absent or its value does not parse into T.
	template <typename T>
	bool Read(const std::map<std::string, std::string>& kv, const std::string& key, T& out)
	{
		auto it = kv.find(key);
		return it != kv.end() && ParseNumber(it->second, out);
	}

	// Reads a list count; appends to warning when it is bad or over the cap.
	size_t ReadCount(const std::map<std::string, std::string>& kv, const std::string& key, std::string& warning)
	{
		size_t n = 0;
		if (!Read(kv, key, n))
		{
			warning += key + " did not parse, list empty; ";
			return 0;
		}
		if (n > regionsync::kMaxListEntries)
		{
			warning += key + "=" + std::to_string(n) + " truncated to " + std::to_string(regionsync::kMaxListEntries) + "; ";
			return regionsync::kMaxListEntries;
		}
		return n;
	}
}

namespace regionsync
{
	std::string SnapshotFileName(int32_t regionX, int32_t regionZ)
	{
		return std::to_string(regionX) + "_" + std::to_string(regionZ) + ".snapshot";
	}

	bool WriteSnapshotFile(const std::filesystem::path& path, const NeighborSnapshot& s, std::string& error)
	{
		std::ostringstream out;
		out << "; sc4-region-sync neighbor snapshot\n";
		out << "schemaVersion=" << s.schemaVersion << '\n';
		out << "regionX=" << s.regionX << '\n';
		out << "regionZ=" << s.regionZ << '\n';
		out << "sizeX=" << s.sizeX << '\n';
		out << "sizeZ=" << s.sizeZ << '\n';
		out << "citySerial=" << s.citySerial << '\n';
		out << "cityName=" << s.cityName << '\n';
		out << "established=" << (s.established ? 1 : 0) << '\n';
		out << "writtenUnixTime=" << s.writtenUnixTime << '\n';
		out << "simDateNumber=" << s.simDateNumber << '\n';
		out << "simYear=" << s.simYear << '\n';
		out << "simMonth=" << s.simMonth << '\n';
		out << "simDay=" << s.simDay << '\n';
		out << "population=" << s.population << '\n';
		out << "commercialJobs=" << s.commercialJobs << '\n';
		out << "industrialJobs=" << s.industrialJobs << '\n';
		out << "workforcePercentage=" << s.workforcePercentage << '\n';
		out << "mayorRating=" << static_cast<int32_t>(s.mayorRating) << '\n';
		out << "budget=" << s.budget << '\n';
		out << "income=" << s.income << '\n';

		for (size_t i = 0; i < kCommoditySlots; i++)
		{
			const CommodityStats& c = s.commodities[i];
			out << "commodity." << i << ".exported=" << c.exported << '\n';
			out << "commodity." << i << ".imported=" << c.imported << '\n';
			out << "commodity." << i << ".produced=" << c.produced << '\n';
			out << "commodity." << i << ".demanded=" << c.demanded << '\n';
			out << "commodity." << i << ".costPerUnit=" << c.costPerUnit << '\n';
		}

		out << "rciGroupCount=" << s.rciGroupCount << '\n';
		for (size_t i = 0; i < s.rciGroupCount && i < kRCIGroupSlots; i++)
		{
			out << "rci." << i << ".groupId=" << s.rciGroups[i].groupId << '\n';
			out << "rci." << i << ".population=" << s.rciGroups[i].population << '\n';
		}

		if (s.connectionsKnown)
		{
			out << "connectionCount=" << s.connections.size() << '\n';
			for (size_t i = 0; i < s.connections.size(); i++)
			{
				const ConnectionRecord& c = s.connections[i];
				const std::string p = "connection." + std::to_string(i) + ".";
				out << p << "type=" << c.type << '\n';
				out << p << "innerX=" << c.innerX << '\n';
				out << p << "innerZ=" << c.innerZ << '\n';
				if (c.cellKnown)
				{
					out << p << "cellX=" << c.cellX << '\n';
					out << p << "cellZ=" << c.cellZ << '\n';
				}
				out << p << "otherTileX=" << c.otherTileX << '\n';
				out << p << "otherTileZ=" << c.otherTileZ << '\n';
			}
		}

		if (s.dealsKnown)
		{
			out << "dealCount=" << s.deals.size() << '\n';
			for (size_t i = 0; i < s.deals.size(); i++)
			{
				const DealRecord& d = s.deals[i];
				const std::string p = "deal." + std::to_string(i) + ".";
				out << p << "type=" << d.type << '\n';
				out << p << "otherTileX=" << d.otherTileX << '\n';
				out << p << "otherTileZ=" << d.otherTileZ << '\n';
				out << p << "amount=" << d.amount << '\n';
				out << p << "flags=" << d.flags << '\n';
				out << p << "expiresIn=" << d.expiresIn << '\n';
			}
		}

		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);

		const std::filesystem::path tempPath = path.string() + ".tmp";
		{
			std::ofstream stream(tempPath, std::ios::trunc);
			if (!stream)
			{
				error = "Unable to write " + tempPath.string();
				return false;
			}
			stream << out.str();
		}

		std::filesystem::rename(tempPath, path, ec);
		if (ec)
		{
			error = "Unable to rename " + tempPath.string() + " to " + path.string() + ": " + ec.message();
			return false;
		}

		return true;
	}

	bool ReadSnapshotFile(const std::filesystem::path& path, NeighborSnapshot& s, std::string& error)
	{
		error.clear();
		std::map<std::string, std::string> kv;
		if (!LoadKeyValues(path, kv, error))
		{
			return false;
		}

		Read(kv, "schemaVersion", s.schemaVersion);
		Read(kv, "regionX", s.regionX);
		Read(kv, "regionZ", s.regionZ);
		Read(kv, "sizeX", s.sizeX);
		Read(kv, "sizeZ", s.sizeZ);
		Read(kv, "citySerial", s.citySerial);
		if (auto it = kv.find("cityName"); it != kv.end())
		{
			s.cityName = it->second;
		}
		int32_t established = 0;
		Read(kv, "established", established);
		s.established = established != 0;
		Read(kv, "writtenUnixTime", s.writtenUnixTime);
		Read(kv, "simDateNumber", s.simDateNumber);
		Read(kv, "simYear", s.simYear);
		Read(kv, "simMonth", s.simMonth);
		Read(kv, "simDay", s.simDay);
		Read(kv, "population", s.population);
		Read(kv, "commercialJobs", s.commercialJobs);
		Read(kv, "industrialJobs", s.industrialJobs);
		Read(kv, "workforcePercentage", s.workforcePercentage);
		int32_t rating = 0;
		Read(kv, "mayorRating", rating);
		s.mayorRating = static_cast<int8_t>(rating);
		Read(kv, "budget", s.budget);
		Read(kv, "income", s.income);

		for (size_t i = 0; i < kCommoditySlots; i++)
		{
			const std::string prefix = "commodity." + std::to_string(i) + ".";
			Read(kv, prefix + "exported", s.commodities[i].exported);
			Read(kv, prefix + "imported", s.commodities[i].imported);
			Read(kv, prefix + "produced", s.commodities[i].produced);
			Read(kv, prefix + "demanded", s.commodities[i].demanded);
			Read(kv, prefix + "costPerUnit", s.commodities[i].costPerUnit);
		}

		size_t count = 0;
		Read(kv, "rciGroupCount", count);
		s.rciGroupCount = count < kRCIGroupSlots ? count : kRCIGroupSlots;
		for (size_t i = 0; i < s.rciGroupCount; i++)
		{
			const std::string prefix = "rci." + std::to_string(i) + ".";
			Read(kv, prefix + "groupId", s.rciGroups[i].groupId);
			Read(kv, prefix + "population", s.rciGroups[i].population);
		}

		s.connectionsKnown = kv.count("connectionCount") != 0;
		s.connections.clear();
		if (s.connectionsKnown)
		{
			s.connections.resize(ReadCount(kv, "connectionCount", error));
			for (size_t i = 0; i < s.connections.size(); i++)
			{
				ConnectionRecord& c = s.connections[i];
				const std::string p = "connection." + std::to_string(i) + ".";
				Read(kv, p + "type", c.type);
				Read(kv, p + "innerX", c.innerX);
				Read(kv, p + "innerZ", c.innerZ);
				const bool cellXKnown = Read(kv, p + "cellX", c.cellX);
				c.cellKnown = Read(kv, p + "cellZ", c.cellZ) && cellXKnown;
				Read(kv, p + "otherTileX", c.otherTileX);
				Read(kv, p + "otherTileZ", c.otherTileZ);
			}
		}

		s.dealsKnown = kv.count("dealCount") != 0;
		s.deals.clear();
		if (s.dealsKnown)
		{
			s.deals.resize(ReadCount(kv, "dealCount", error));
			for (size_t i = 0; i < s.deals.size(); i++)
			{
				DealRecord& d = s.deals[i];
				const std::string p = "deal." + std::to_string(i) + ".";
				Read(kv, p + "type", d.type);
				Read(kv, p + "otherTileX", d.otherTileX);
				Read(kv, p + "otherTileZ", d.otherTileZ);
				Read(kv, p + "amount", d.amount);
				Read(kv, p + "flags", d.flags);
				Read(kv, p + "expiresIn", d.expiresIn);
			}
		}

		return true;
	}
}
