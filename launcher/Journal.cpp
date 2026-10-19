/*
 * sc4-region-sync launcher
 *
 * Journal.cpp - see Journal.h.
 */

#include "Journal.h"
#include "Manifest.h"
#include <algorithm>

namespace
{
	bool ParseCoord(std::string_view s, int32_t& v)
	{
		uint64_t u = 0;
		if (!lan::ParseU64(s, u) || u > 9999) return false;
		v = static_cast<int32_t>(u);
		return true;
	}

	const lan::ManifestEntry* FindEntry(const lan::Manifest& m, std::string_view name)
	{
		if (name.empty()) return nullptr;
		for (const auto& e : m.files)
		{
			if (lan::EqualsNoCase(e.name, name)) return &e;
		}
		return nullptr;
	}
}

namespace lan
{
	std::string TileKey(Tile t)
	{
		return std::to_string(t.x) + "_" + std::to_string(t.z);
	}

	bool ParseTileKey(std::string_view s, Tile& t)
	{
		const size_t us = s.find('_');
		return us != std::string_view::npos && ParseCoord(s.substr(0, us), t.x) && ParseCoord(s.substr(us + 1), t.z);
	}

	std::string BuildJournalText(const Journal& j)
	{
		std::string out = "; sc4-region-sync LAN journal\n";
		for (const auto& [tile, e] : j.tiles)
		{
			out += TileKey(tile) + "=" + e.file + "|" + e.baseHash + "|" + std::to_string(e.baseCount) + "\n";
		}
		return out;
	}

	bool ParseJournalText(std::string_view text, Journal& j, std::string& error)
	{
		j = Journal{};
		KeyValues kv;
		regionsync::ParseKeyValues(text, kv);
		for (const auto& [key, value] : kv)
		{
			Tile t;
			const size_t a = value.find('|');
			const size_t b = a == std::string::npos ? a : value.find('|', a + 1);
			if (!ParseTileKey(key, t) || b == std::string::npos)
			{
				error = "bad journal line " + key;
				return false;
			}
			JournalEntry e;
			e.file = value.substr(0, a);
			e.baseHash = value.substr(a + 1, b - a - 1);
			const bool fileOk = e.file.empty() ? e.baseHash.empty() : IsValidRegionFileName(e.file) && IsLowerHex(e.baseHash, 64);
			if (!fileOk || !ParseU64(value.substr(b + 1), e.baseCount))
			{
				error = "bad journal line " + key;
				return false;
			}
			j.tiles[t] = std::move(e);
		}
		return true;
	}

	void UpdateJournalAfterSwap(Journal& j, const Manifest& m, const std::map<Tile, SaveCounter>& saves,
		const std::vector<Tile>& keptTiles)
	{
		std::vector<Tile> tiles;
		for (const auto& e : m.files) if (e.tile) tiles.push_back(*e.tile);
		for (const auto& [t, e] : j.tiles) tiles.push_back(t);
		for (const auto& [t, s] : saves) tiles.push_back(t);
		std::sort(tiles.begin(), tiles.end());
		tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());

		for (const Tile t : tiles)
		{
			if (std::find(keptTiles.begin(), keptTiles.end(), t) != keptTiles.end()) continue;
			JournalEntry& entry = j.tiles[t];
			const auto s = saves.find(t);
			const ManifestEntry* file = nullptr;
			for (const auto& e : m.files)
			{
				if (e.tile == t) { file = &e; break; }
			}
			if (!file && s != saves.end()) file = FindEntry(m, s->second.file);
			if (!file) file = FindEntry(m, entry.file);
			entry.file = file ? file->name : std::string();
			entry.baseHash = file ? file->hash : std::string();
			if (s != saves.end()) entry.baseCount = s->second.count;
		}
	}
}
