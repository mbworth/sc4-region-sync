/*
 * sc4-region-sync launcher
 *
 * Manifest.cpp - see Manifest.h.
 */

#include "Manifest.h"

namespace
{
	char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

	std::string LowerCopy(std::string_view s)
	{
		std::string out(s);
		for (char& c : out) c = Lower(c);
		return out;
	}

	bool EndsWithNoCase(std::string_view s, std::string_view suffix)
	{
		return s.size() >= suffix.size() && lan::EqualsNoCase(s.substr(s.size() - suffix.size()), suffix);
	}

	bool IsReservedDeviceName(std::string_view s)
	{
		std::string_view base = s.substr(0, s.find('.'));
		while (!base.empty() && base.back() == ' ') base.remove_suffix(1);
		for (std::string_view r : { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" })
		{
			if (lan::EqualsNoCase(base, r)) return true;
		}
		return base.size() == 4 && (lan::EqualsNoCase(base.substr(0, 3), "COM") || lan::EqualsNoCase(base.substr(0, 3), "LPT"))
			&& base[3] >= '0' && base[3] <= '9';
	}

	template <typename T>
	bool ParsePositive(std::string_view s, uint64_t max, T& v)
	{
		uint64_t u = 0;
		if (!lan::ParseU64(s, u) || u == 0 || u > max) return false;
		v = static_cast<T>(u);
		return true;
	}

	bool StripSuffix(std::string_view& s, std::string_view suffix)
	{
		if (s.size() <= suffix.size() || s.substr(s.size() - suffix.size()) != suffix) return false;
		s.remove_suffix(suffix.size());
		return true;
	}
}

namespace lan
{
	bool EqualsNoCase(std::string_view a, std::string_view b)
	{
		return regionsync::SameHost(a, b);
	}

	bool IsValidRegionName(std::string_view s)
	{
		if (s.empty() || s.size() > kMaxRegionNameLength || s == "." || s == "..") return false;
		if (s.front() == ' ' || s.front() == '.' || s.back() == ' ' || s.back() == '.') return false;
		for (char c : s)
		{
			if (c < 0x20 || c > 0x7E) return false;
			if (std::string_view("<>:\"/\\|?*~").find(c) != std::string_view::npos) return false;
		}
		return !IsReservedDeviceName(s);
	}

	bool IsValidRegionFileName(std::string_view s)
	{
		return IsValidRegionName(s)
			&& (EqualsNoCase(s, "region.ini") || EqualsNoCase(s, "config.bmp") || (s.size() > 4 && EndsWithNoCase(s, ".sc4")));
	}

	bool ParseSnapshotName(std::string_view s, int32_t sizeX, int32_t sizeZ, Tile& t)
	{
		return StripSuffix(s, ".snapshot") && ParseTileKey(s, t) && t.x < sizeX && t.z < sizeZ;
	}

	bool ParseStatusName(std::string_view s, uint32_t& pid, uint64_t& start)
	{
		if (!StripSuffix(s, ".status")) return false;
		const size_t dash = s.find('-');
		return dash != std::string_view::npos && ParsePositive(s.substr(0, dash), UINT32_MAX, pid)
			&& ParsePositive(s.substr(dash + 1), UINT64_MAX, start);
	}

	namespace
	{
		bool ValidateEntries(const Manifest& m, std::string& error)
		{
			if (!IsValidRegionName(m.region)) { error = "bad region name"; return false; }
			if (m.files.empty()) { error = "empty manifest"; return false; }
			if (m.files.size() > kMaxManifestFiles) { error = "too many files"; return false; }
			std::set<std::string> seen;
			for (const auto& e : m.files)
			{
				if (!IsValidRegionFileName(e.name)) { error = "bad file name"; return false; }
				if (e.size > kMaxManifestFileSize) { error = "file too large: " + e.name; return false; }
				if (!IsLowerHex(e.hash, 64)) { error = "bad hash: " + e.name; return false; }
				if (!seen.insert(LowerCopy(e.name)).second) { error = "duplicate name: " + e.name; return false; }
			}
			return true;
		}
	}

	bool BuildManifestText(const Manifest& m, std::string& text, std::string& data, std::string& error)
	{
		if (!ValidateEntries(m, error)) return false;
		Fields f;
		for (size_t i = 0; i < m.files.size(); i++)
		{
			const auto& e = m.files[i];
			f.Add("file." + std::to_string(i),
				e.name + "|" + std::to_string(e.size) + "|" + e.hash + "|" + (e.tile ? TileKey(*e.tile) : std::string()));
		}
		text = Fields().Add("region", m.region).Add("count", m.files.size()).Text();
		data = f.Text();
		return true;
	}

	bool ParseManifest(std::string_view text, std::string_view data, Manifest& m, std::string& error)
	{
		m = Manifest{};
		KeyValues header, kv;
		if (!ParseFields(text, header) || header.size() != 2 || !ParseFields(data, kv)) { error = "malformed text"; return false; }
		m.region = Get(header, "region");
		uint64_t count = 0;
		if (!ParseU64(Get(header, "count"), count) || count == 0 || count > kMaxManifestFiles)
		{
			error = "bad count";
			return false;
		}
		size_t fileKeys = 0;
		for (const auto& [key, value] : kv) if (key.rfind("file.", 0) == 0) fileKeys++;
		if (fileKeys != count || kv.size() != count) { error = "count mismatch"; return false; }
		for (size_t i = 0; i < count; i++)
		{
			const std::string value = Get(kv, "file." + std::to_string(i));
			const size_t a = value.find('|');
			const size_t b = a == std::string::npos ? a : value.find('|', a + 1);
			const size_t c = b == std::string::npos ? b : value.find('|', b + 1);
			ManifestEntry e;
			if (c == std::string::npos || !ParseU64(std::string_view(value).substr(a + 1, b - a - 1), e.size))
			{
				error = "bad file." + std::to_string(i);
				return false;
			}
			e.name = value.substr(0, a);
			e.hash = value.substr(b + 1, c - b - 1);
			const std::string tile = value.substr(c + 1);
			if (!tile.empty())
			{
				Tile t;
				if (!ParseTileKey(tile, t)) { error = "bad tile in file." + std::to_string(i); return false; }
				e.tile = t;
			}
			m.files.push_back(std::move(e));
		}
		return ValidateEntries(m, error);
	}

	DiffResult DiffRegion(const Manifest& m, const std::map<std::string, std::string>& localHashes,
		const std::map<Tile, SaveCounter>& saves, const Journal& j, const std::set<Tile>& saveCountFailed)
	{
		DiffResult r;
		std::set<Tile> kept = saveCountFailed;
		for (const auto& [t, s] : saves)
		{
			const auto je = j.tiles.find(t);
			if (s.count > (je != j.tiles.end() ? je->second.baseCount : 0u)) kept.insert(t);
		}

		// Lowercase names of every file that belongs to a kept tile.
		std::set<std::string> keptFiles;
		auto addKept = [&](const std::string& name) { if (!name.empty()) keptFiles.insert(LowerCopy(name)); };
		// A kept tile that names no file could own any local .sc4: keep every one that differs.
		bool keepUnnamed = false;
		for (const Tile t : kept)
		{
			bool named = false;
			auto claim = [&](const std::string& name) { named = named || !name.empty(); addKept(name); };
			if (const auto je = j.tiles.find(t); je != j.tiles.end()) claim(je->second.file);
			if (const auto s = saves.find(t); s != saves.end())
			{
				claim(s->second.file);
				claim(s->second.prevFile);
			}
			for (const auto& e : m.files)
			{
				if (e.tile == t) claim(e.name);
			}
			keepUnnamed = keepUnnamed || !named;
		}
		auto keepLocal = [&](const std::string& key) { return keptFiles.count(key) || (keepUnnamed && key.size() > 4 && key.ends_with(".sc4")); };

		std::map<std::string, std::pair<std::string, std::string>> local;   // lower -> (name, hash)
		for (const auto& [name, hash] : localHashes) local[LowerCopy(name)] = { name, hash };

		for (const auto& e : m.files)
		{
			const std::string key = LowerCopy(e.name);
			const auto l = local.find(key);
			DiffItem item{ e.name, l != local.end() ? l->second.first : std::string(), DiffAction::Same };
			if (keptFiles.count(key) || (l != local.end() && l->second.second != e.hash && keepLocal(key)))
			{
				item.action = DiffAction::KeepNotSent;
			}
			else if (l == local.end() || l->second.second != e.hash)
			{
				item.action = DiffAction::Download;
				r.downloadBytes += e.size;
			}
			if (l != local.end()) local.erase(l);
			r.items.push_back(std::move(item));
		}
		for (const auto& [key, value] : local)
		{
			r.items.push_back({ value.first, value.first, keepLocal(key) ? DiffAction::KeepNotSent : DiffAction::Archive });
		}
		r.keptTiles.assign(kept.begin(), kept.end());
		return r;
	}
}
