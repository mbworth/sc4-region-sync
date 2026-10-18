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
#include <algorithm>
#include <fstream>
#include <sstream>
#include <map>
#include <charconv>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>
#include <type_traits>
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
			std::conditional_t<std::is_unsigned_v<T>, unsigned long long, long long> value = 0;
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
		if (s.writer.Known() && IsValidWriterHost(s.writer.host))
		{
			out << "writerHost=" << s.writer.host << '\n';
			out << "writerPid=" << s.writer.pid << '\n';
			out << "writerStart=" << s.writer.start << '\n';
		}
		if (s.cityLoadedAt != 0)
		{
			out << "cityLoadedAt=" << s.cityLoadedAt << '\n';
		}
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
			out << "connectionsComplete=" << (s.connectionsComplete ? 1 : 0) << '\n';
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
			out << "dealsComplete=" << (s.dealsComplete ? 1 : 0) << '\n';
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
		// All three or none: a partial identity is unknown.
		s.writer = {};
		if (auto it = kv.find("writerHost"); it != kv.end() && IsValidWriterHost(it->second))
		{
			WriterIdentity w;
			w.host = it->second;
			if (Read(kv, "writerPid", w.pid) && Read(kv, "writerStart", w.start) && w.Known())
			{
				s.writer = std::move(w);
			}
		}
		s.cityLoadedAt = 0;
		Read(kv, "cityLoadedAt", s.cityLoadedAt);
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
				bool ok = Read(kv, p + "type", c.type);
				ok = Read(kv, p + "innerX", c.innerX) && ok;
				ok = Read(kv, p + "innerZ", c.innerZ) && ok;
				const bool cellXKnown = Read(kv, p + "cellX", c.cellX);
				c.cellKnown = Read(kv, p + "cellZ", c.cellZ) && cellXKnown;
				ok = Read(kv, p + "otherTileX", c.otherTileX) && ok;
				ok = Read(kv, p + "otherTileZ", c.otherTileZ) && ok;
				if (!ok) error += p + " keys missing; ";
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
				bool ok = Read(kv, p + "type", d.type);
				ok = Read(kv, p + "otherTileX", d.otherTileX) && ok;
				ok = Read(kv, p + "otherTileZ", d.otherTileZ) && ok;
				ok = Read(kv, p + "amount", d.amount) && ok;
				ok = Read(kv, p + "flags", d.flags) && ok;
				ok = Read(kv, p + "expiresIn", d.expiresIn) && ok;
				if (!ok) error += p + " keys missing; ";
			}
		}

		int32_t complete = 0;
		s.connectionsComplete = s.connectionsKnown && Read(kv, "connectionsComplete", complete) && complete != 0;
		complete = 0;
		s.dealsComplete = s.dealsKnown && Read(kv, "dealsComplete", complete) && complete != 0;
		// Any parse problem: a missing entry must not read as a removal.
		if (!error.empty())
		{
			s.connectionsComplete = false;
			s.dealsComplete = false;
		}

		return true;
	}

	namespace
	{
		// Publisher edge side (0 W, 1 N, 2 E, 3 S) and along-edge coordinate.
		bool PublisherLaneSide(const ConnectionRecord& c, int32_t cellsA, uint32_t& side, int32_t& along)
		{
			if (cellsA <= 0) return false;
			if (c.cellKnown)
			{
				if (!IsConnectionCellAdjacent(c.innerX, c.innerZ, c.cellX, c.cellZ, cellsA)) return false;
				side = c.cellX == -1 ? 0 : c.cellZ == -1 ? 1 : c.cellX == cellsA ? 2 : 3;
			}
			else
			{
				const int32_t w = cellsA - 1;
				const auto in = [w](int32_t v) { return v >= 0 && v <= w; };
				if (!in(c.innerX) || !in(c.innerZ)) return false;
				const bool edges[4] = { c.innerX == 0, c.innerZ == 0, c.innerX == w, c.innerZ == w };
				if (std::count(std::begin(edges), std::end(edges), true) != 1) return false;   // corner or interior
				side = static_cast<uint32_t>(std::find(std::begin(edges), std::end(edges), true) - std::begin(edges));
			}
			along = (side == 0 || side == 2) ? c.innerZ : c.innerX;
			return true;
		}
	}

	std::vector<std::pair<size_t, size_t>> PairDualLanes(const std::vector<ConnectionRecord>& lanes, int32_t cellsA)
	{
		struct Lane
		{
			uint32_t type, side;
			int32_t along;
			size_t index;
		};
		std::vector<Lane> v;
		for (size_t i = 0; i < lanes.size(); i++)
		{
			Lane l{ lanes[i].type, 0, 0, i };
			if (IsDualConnectionType(l.type) && PublisherLaneSide(lanes[i], cellsA, l.side, l.along)) v.push_back(l);
		}
		const auto key = [](const Lane& l) { return std::tuple(l.type, l.side, l.along); };
		std::sort(v.begin(), v.end(), [&](const Lane& x, const Lane& y) { return std::tuple(x.type, x.side, x.along, x.index) < std::tuple(y.type, y.side, y.along, y.index); });
		v.erase(std::unique(v.begin(), v.end(), [&](const Lane& x, const Lane& y) { return key(x) == key(y); }), v.end());

		std::vector<std::pair<size_t, size_t>> pairs;
		for (size_t i = 0; i + 1 < v.size();)
		{
			const Lane& lo = v[i];
			const Lane& hi = v[i + 1];
			if (lo.type == hi.type && lo.side == hi.side && hi.along == lo.along + 1)
			{
				pairs.emplace_back(lo.index, hi.index);
				i += 2;
			}
			else
			{
				i++;
			}
		}
		return pairs;
	}
}
